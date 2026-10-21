#include "git/git.hpp"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

extern char** environ;

namespace wt::git {

namespace {

void check(int rc, const char* what) {
    if (rc != 0) {
        throw std::system_error(rc, std::generic_category(), what);
    }
}

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    int get() const { return fd_; }
    void reset() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;
};

struct Pipe {
    Fd read;
    Fd write;
};

void make_pipe(int (&fds)[2]) {
    if (::pipe(fds) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe");
    }
    // Close-on-exec so the pipe only reaches the child through the dup2'd
    // stdout/stderr; a stray copy of a write end would keep reads from ending.
    for (int fd : fds) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
}

class FileActions {
public:
    FileActions() {
        check(::posix_spawn_file_actions_init(&actions_), "spawn");
    }
    ~FileActions() { ::posix_spawn_file_actions_destroy(&actions_); }
    FileActions(const FileActions&) = delete;
    FileActions& operator=(const FileActions&) = delete;

    posix_spawn_file_actions_t* get() { return &actions_; }

private:
    posix_spawn_file_actions_t actions_;
};

// Reads both pipes until EOF. Reading them together matters: if git fills
// one pipe while we block on the other, both processes deadlock.
void drain(int out_fd, int err_fd, std::string& out, std::string& err) {
    std::array<pollfd, 2> fds = {{{out_fd, POLLIN, 0}, {err_fd, POLLIN, 0}}};
    std::array<std::string*, 2> sinks = {&out, &err};
    int open = 2;
    char buf[4096];
    while (open > 0) {
        if (::poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "poll");
        }
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if (fds[i].fd < 0 || fds[i].revents == 0) {
                continue;
            }
            const ssize_t n = ::read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[i]->append(buf, static_cast<std::size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                fds[i].fd = -1;  // poll ignores negative descriptors
                --open;
            }
        }
    }
}

int wait_for(pid_t pid) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "waitpid");
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128 + WTERMSIG(status);  // shell convention for "killed by signal"
}

}  // namespace

Result run(const std::filesystem::path& dir,
           const std::vector<std::string>& args) {
    std::vector<std::string> argv_storage = {"git", "-C", dir.string()};
    argv_storage.insert(argv_storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : argv_storage) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    int out_fds[2];
    make_pipe(out_fds);
    Pipe out_pipe{Fd(out_fds[0]), Fd(out_fds[1])};
    int err_fds[2];
    make_pipe(err_fds);
    Pipe err_pipe{Fd(err_fds[0]), Fd(err_fds[1])};

    pid_t pid = 0;
    {
        FileActions actions;
        // No stdin, so git can never stop to wait for input.
        check(::posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO,
                                                 "/dev/null", O_RDONLY, 0),
              "spawn");
        check(::posix_spawn_file_actions_adddup2(
                  actions.get(), out_pipe.write.get(), STDOUT_FILENO),
              "spawn");
        check(::posix_spawn_file_actions_adddup2(
                  actions.get(), err_pipe.write.get(), STDERR_FILENO),
              "spawn");
        const int rc = ::posix_spawnp(&pid, "git", actions.get(), nullptr,
                                      argv.data(), environ);
        if (rc == ENOENT) {
            throw Error("git was not found on PATH; install Git to use wt");
        }
        if (rc != 0) {
            throw Error(std::string("cannot run git: ") + std::strerror(rc));
        }
    }
    out_pipe.write.reset();
    err_pipe.write.reset();

    Result result{0, {}, {}};
    try {
        drain(out_pipe.read.get(), err_pipe.read.get(), result.out, result.err);
    } catch (...) {
        wait_for(pid);  // don't leave a zombie behind
        throw;
    }
    result.exit_code = wait_for(pid);
    return result;
}

std::string error_message(std::string_view git_stderr) {
    std::string_view msg = git_stderr;
    while (!msg.empty() && (msg.front() == '\n' || msg.front() == ' ')) {
        msg.remove_prefix(1);
    }
    msg = msg.substr(0, msg.find('\n'));
    for (std::string_view prefix : {"fatal: ", "error: "}) {
        if (msg.starts_with(prefix)) {
            msg.remove_prefix(prefix.size());
            break;
        }
    }
    if (msg.empty()) {
        return "git failed without an error message";
    }
    return std::string(msg);
}

}  // namespace wt::git
