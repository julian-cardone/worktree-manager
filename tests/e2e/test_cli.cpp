// End-to-end tests: run the real wt binary as a subprocess.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/sandbox.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

extern char** environ;

using Catch::Matchers::ContainsSubstring;
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

// Inherits the parent environment, but points HOME and Git's config lookup at
// the sandbox so user config and repositories outside it are never touched.
std::vector<std::string> sandbox_env(const Sandbox& box) {
    const std::vector<std::pair<std::string, std::string>> overrides = {
        {"HOME", box.home().string()},
        {"XDG_CONFIG_HOME", (box.home() / ".config").string()},
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CEILING_DIRECTORIES", box.root().string()},
    };

    std::vector<std::string> env;
    for (char** e = environ; *e != nullptr; ++e) {
        const std::string entry(*e);
        bool overridden = false;
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

// Runs `wt <args>` in a fresh sandbox, capturing stdout, stderr, and exit code.
// Arguments are passed straight to execve, so no shell quoting is involved.
Result run_wt(std::initializer_list<std::string> args) {
    Sandbox box;
    const std::string out_path = (box.root() / "stdout").string();
    const std::string err_path = (box.root() / "stderr").string();
    const std::string work_dir = box.work().string();

    std::vector<std::string> argv_storage = {WT_BINARY_PATH};
    argv_storage.insert(argv_storage.end(), args);
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
