// End-to-end tests: run the real wt binary as a subprocess.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/sandbox.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

extern char** environ;

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using wt::test::Sandbox;

namespace {

namespace fs = std::filesystem;

struct Result {
    int code;
    std::string out;
    std::string err;
};

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out << content;
}

// Inherits the parent environment, but points HOME and Git's config lookup at
// the sandbox so user config and repositories outside it are never touched.
// Inherited GIT_* variables are dropped: GIT_DIR or GIT_WORK_TREE (set, e.g.,
// when tests run from a Git hook) would point git at a real repository.
std::vector<std::string> sandbox_env(const Sandbox& box) {
    const std::vector<std::pair<std::string, std::string>> overrides = {
        {"HOME", box.home().string()},
        {"XDG_CONFIG_HOME", (box.home() / ".config").string()},
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CEILING_DIRECTORIES", box.root().string()},
        {"GIT_AUTHOR_NAME", "wt test"},
        {"GIT_AUTHOR_EMAIL", "wt-test@example.invalid"},
        {"GIT_COMMITTER_NAME", "wt test"},
        {"GIT_COMMITTER_EMAIL", "wt-test@example.invalid"},
    };

    std::vector<std::string> env;
    for (char** e = environ; *e != nullptr; ++e) {
        const std::string entry(*e);
        bool overridden = entry.starts_with("GIT_");
        for (const auto& [key, value] : overrides) {
            if (entry.starts_with(key + "=")) {
                overridden = true;
                break;
            }
        }
        if (!overridden) {
            env.push_back(entry);
        }
    }
    for (const auto& [key, value] : overrides) {
        env.push_back(key + "=" + value);
    }
    return env;
}

// Resolves a program name against PATH, since execve doesn't search it.
std::string find_on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::stringstream dirs(path ? path : "");
    std::string dir;
    while (std::getline(dirs, dir, ':')) {
        const fs::path candidate = fs::path(dir.empty() ? "." : dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            return candidate.string();
        }
    }
    FAIL("cannot find " << name << " on PATH");
    return {};
}

// Runs `argv` in `dir` inside the sandbox, capturing stdout, stderr, and exit
// code. Arguments are passed straight to execve, so no shell quoting is
// involved.
Result run_in(const Sandbox& box, const fs::path& dir,
              std::vector<std::string> argv_storage) {
    const std::string out_path = (box.root() / "stdout").string();
    const std::string err_path = (box.root() / "stderr").string();
    const std::string work_dir = dir.string();

    std::vector<char*> argv;
    for (auto& a : argv_storage) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage = sandbox_env(box);
    std::vector<char*> envp;
    for (auto& e : env_storage) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Child: only async-signal-safe calls from here until exec.
        const int out_fd =
            ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int err_fd =
            ::open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out_fd < 0 || err_fd < 0 || ::dup2(out_fd, STDOUT_FILENO) < 0 ||
            ::dup2(err_fd, STDERR_FILENO) < 0 ||
            ::chdir(work_dir.c_str()) != 0) {
            ::_exit(127);
        }
        ::execve(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        REQUIRE(errno == EINTR);
    }
    REQUIRE(WIFEXITED(status));

    return {WEXITSTATUS(status), read_file(out_path), read_file(err_path)};
}

Result run_wt(const Sandbox& box, const fs::path& dir,
              std::initializer_list<std::string> args) {
    std::vector<std::string> argv = {WT_BINARY_PATH};
    argv.insert(argv.end(), args);
    return run_in(box, dir, std::move(argv));
}

// Runs `wt <args>` in the work directory of a fresh sandbox.
Result run_wt(std::initializer_list<std::string> args) {
    Sandbox box;
    return run_wt(box, box.work(), args);
}

// Runs git for test setup; any failure fails the test.
void git(const Sandbox& box, const fs::path& dir,
         std::initializer_list<std::string> args) {
    static const std::string git_path = find_on_path("git");
    std::vector<std::string> argv = {git_path};
    argv.insert(argv.end(), args);
    const Result r = run_in(box, dir, std::move(argv));
    INFO("git stderr: " << r.err);
    REQUIRE(r.code == 0);
}

// Creates a repository at `path` with one commit on main.
fs::path make_repo(const Sandbox& box, const fs::path& path) {
    fs::create_directories(path.parent_path());
    git(box, path.parent_path(), {"init", "-q", "-b", "main", path.string()});
    write_file(path / "README", "hello\n");
    git(box, path, {"add", "README"});
    git(box, path, {"commit", "-q", "-m", "initial"});
    return path;
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    std::stringstream in(text);
    for (std::string line; std::getline(in, line);) {
        result.push_back(line);
    }
    return result;
}

// Returns the `wt list` row for the worktree at `path` (the last column).
std::string row_for(const std::string& out, const std::string& path) {
    for (const auto& line : lines(out)) {
        if (line.ends_with("  " + path)) {
            return line;
        }
    }
    FAIL("no row for " << path << " in:\n" << out);
    return {};
}

}  // namespace

TEST_CASE("wt binary exists") {
    CHECK(fs::is_regular_file(WT_BINARY_PATH));
}

TEST_CASE("wt --help prints usage and exits 0") {
    auto r = run_wt({"--help"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("Usage: wt"));
    CHECK_THAT(r.out, ContainsSubstring("--version"));
    CHECK(r.err.empty());
}

TEST_CASE("wt --version prints the project version and exits 0") {
    auto r = run_wt({"--version"});
    CHECK(r.code == 0);
    CHECK(r.out == "wt " WT_VERSION "\n");
    CHECK(r.err.empty());
}

TEST_CASE("wt with no arguments prints help and exits 0") {
    auto r = run_wt({});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("Usage: wt"));
}

TEST_CASE("wt with an unknown flag exits non-zero and reports it on stderr") {
    auto r = run_wt({"--bogus"});
    CHECK(r.code != 0);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, ContainsSubstring("--bogus"));
}

TEST_CASE("arguments reach wt verbatim, without shell interpretation") {
    auto r = run_wt({"--bogus 'quoted' $HOME;"});
    CHECK(r.code != 0);
    CHECK_THAT(r.err, ContainsSubstring("--bogus 'quoted' $HOME;"));
}

TEST_CASE("wt list outside a repository fails with a clear error") {
    Sandbox box;
    auto r = run_wt(box, box.work(), {"list"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: not a git repository"));
}

TEST_CASE("wt list shows a clean main worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    auto r = run_wt(box, repo, {"list"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    const auto out = lines(r.out);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "  BRANCH  STATUS  PATH");
    CHECK(out[1] == "* main    clean   " + repo.string());
}

TEST_CASE("wt list reports uncommitted changes as dirty") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");

    SECTION("untracked file") {
        write_file(repo / "new.txt", "new\n");
    }
    SECTION("modified tracked file") {
        write_file(repo / "README", "changed\n");
    }
    SECTION("staged change") {
        write_file(repo / "staged.txt", "staged\n");
        git(box, repo, {"add", "staged.txt"});
    }

    auto r = run_wt(box, repo, {"list"});
    CHECK(r.code == 0);
    CHECK_THAT(row_for(r.out, repo.string()), StartsWith("* main    dirty"));
}

TEST_CASE("wt list doesn't count ignored files as changes") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    write_file(repo / ".git" / "info" / "exclude", "build/\n");
    fs::create_directory(repo / "build");
    write_file(repo / "build" / "out.o", "obj\n");
    auto r = run_wt(box, repo, {"list"});
    CHECK_THAT(row_for(r.out, repo.string()), ContainsSubstring("clean"));
}

TEST_CASE("wt list shows every worktree, from any of them") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path wts = box.work() / "repo.worktrees";
    const fs::path feature = wts / "feature-x";
    const fs::path detached = wts / "detached";
    git(box, repo,
        {"worktree", "add", "-q", "-b", "feature/x", feature.string()});
    git(box, repo, {"worktree", "add", "-q", "--detach", detached.string()});
    write_file(feature / "wip.txt", "wip\n");

    for (const fs::path& from : {repo, feature, detached / "."}) {
        INFO("running from " << from);
        auto r = run_wt(box, from, {"list"});
        CHECK(r.code == 0);
        CHECK(r.err.empty());
        const auto out = lines(r.out);
        REQUIRE(out.size() == 4);
        CHECK(out[1].ends_with(repo.string()));  // main worktree comes first
        CHECK_THAT(row_for(r.out, repo.string()), ContainsSubstring("main "));
        CHECK_THAT(
            row_for(r.out, feature.string()),
            ContainsSubstring("feature/x ") && ContainsSubstring(" dirty "));
        CHECK_THAT(
            row_for(r.out, detached.string()),
            ContainsSubstring("(detached ") && ContainsSubstring(" clean "));

        // Exactly one row, the one being run from, is marked.
        const fs::path here = from == detached / "." ? detached : from;
        CHECK_THAT(row_for(r.out, here.string()), StartsWith("* "));
        int marked = 0;
        for (const auto& line : out) {
            marked += line.starts_with("* ") ? 1 : 0;
        }
        CHECK(marked == 1);
    }
}

TEST_CASE("wt list reports a worktree whose directory was deleted") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path gone = box.work() / "repo.worktrees" / "gone";
    git(box, repo, {"worktree", "add", "-q", "-b", "gone", gone.string()});
    fs::remove_all(gone);

    auto r = run_wt(box, repo, {"list"});
    CHECK(r.code == 0);
    CHECK_THAT(row_for(r.out, gone.string()), StartsWith("  gone    missing"));
    CHECK_THAT(row_for(r.out, repo.string()), ContainsSubstring("clean"));
}

TEST_CASE("wt list warns when a worktree's status can't be read") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path broken = box.work() / "repo.worktrees" / "broken";
    git(box, repo, {"worktree", "add", "-q", "-b", "broken", broken.string()});
    fs::remove(broken / ".git");

    auto r = run_wt(box, repo, {"list"});
    CHECK(r.code == 0);
    CHECK_THAT(row_for(r.out, broken.string()), ContainsSubstring("unknown"));
    CHECK_THAT(r.err, StartsWith("wt: warning: cannot read the status of " +
                                 broken.string()));
}

TEST_CASE("wt list handles a bare repository") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path bare = box.work() / "bare.git";
    const fs::path linked = box.work() / "bare-main";
    git(box, box.work(),
        {"clone", "-q", "--bare", repo.string(), bare.string()});
    git(box, bare, {"worktree", "add", "-q", linked.string(), "main"});

    auto r = run_wt(box, bare, {"list"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK_THAT(row_for(r.out, bare.string()), StartsWith("* (bare)  -"));
    CHECK_THAT(row_for(r.out, linked.string()), StartsWith("  main    clean"));
}

TEST_CASE("wt list handles a repository with no commits") {
    Sandbox box;
    const fs::path repo = box.work() / "empty";
    git(box, box.work(), {"init", "-q", "-b", "main", repo.string()});
    auto r = run_wt(box, repo, {"list"});
    CHECK(r.code == 0);
    CHECK_THAT(row_for(r.out, repo.string()), StartsWith("* main    clean"));
}

TEST_CASE("wt list handles spaces in paths") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "my repo");
    auto r = run_wt(box, repo, {"list"});
    CHECK(r.code == 0);
    CHECK_THAT(row_for(r.out, repo.string()), StartsWith("* main    clean"));
}

TEST_CASE("wt list abbreviates paths under HOME with ~") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.home() / "code" / "repo");
    auto r = run_wt(box, repo, {"list"});
    CHECK(r.code == 0);
    CHECK(lines(r.out).at(1) == "* main    clean   ~/code/repo");
}

TEST_CASE("wt list rejects unexpected arguments") {
    auto r = run_wt({"list", "extra"});
    CHECK(r.code != 0);
    CHECK_THAT(r.err, ContainsSubstring("extra"));
}

TEST_CASE("wt list marks the current worktree from a subdirectory") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path feature = box.work() / "repo.worktrees" / "feature";
    git(box, repo,
        {"worktree", "add", "-q", "-b", "feature", feature.string()});
    fs::create_directories(feature / "src" / "deep");

    auto r = run_wt(box, feature / "src" / "deep", {"list"});
    CHECK(r.code == 0);
    CHECK_THAT(row_for(r.out, feature.string()), StartsWith("* feature"));
    CHECK_THAT(row_for(r.out, repo.string()), StartsWith("  main"));
}

TEST_CASE("wt list marks a worktree nested inside the main checkout") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    write_file(repo / ".git" / "info" / "exclude", ".worktrees/\n");
    const fs::path nested = repo / ".worktrees" / "nested";
    git(box, repo, {"worktree", "add", "-q", "-b", "nested", nested.string()});

    auto r = run_wt(box, nested, {"list"});
    CHECK(r.code == 0);
    CHECK_THAT(row_for(r.out, nested.string()), StartsWith("* nested"));
    CHECK_THAT(row_for(r.out, repo.string()), StartsWith("  main"));
}
