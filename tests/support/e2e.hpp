#pragma once

// Helpers shared by the end-to-end tests, which run the real wt binary
// (WT_BINARY_PATH) as a subprocess inside a Sandbox.

#include "support/sandbox.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
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

namespace wt::test {

namespace fs = std::filesystem;

struct Result {
    int code;
    std::string out;
    std::string err;
};

inline std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out << content;
}

// Inherits the parent environment, but points HOME and Git's config lookup at
// the sandbox so user config and repositories outside it are never touched.
// Inherited GIT_* variables are dropped: GIT_DIR or GIT_WORK_TREE (set, e.g.,
// when tests run from a Git hook) would point git at a real repository.
// LC_ALL=C keeps Git's messages in English, which some tests expect, whatever
// the developer's locale.
// `extra` adds variables on top, and must only point inside the sandbox.
using Env = std::vector<std::pair<std::string, std::string>>;

inline std::vector<std::string> sandbox_env(const Sandbox& box,
                                            const Env& extra) {
    Env overrides = {
        {"HOME", box.home().string()},
        {"XDG_CONFIG_HOME", (box.home() / ".config").string()},
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CEILING_DIRECTORIES", box.root().string()},
        {"LC_ALL", "C"},
        {"GIT_AUTHOR_NAME", "wt test"},
        {"GIT_AUTHOR_EMAIL", "wt-test@example.invalid"},
        {"GIT_COMMITTER_NAME", "wt test"},
        {"GIT_COMMITTER_EMAIL", "wt-test@example.invalid"},
    };
    overrides.insert(overrides.end(), extra.begin(), extra.end());

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
inline std::string find_on_path(const std::string& name) {
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
inline Result run_in(const Sandbox& box, const fs::path& dir,
                     std::vector<std::string> argv_storage,
                     const Env& extra_env = {}) {
    const std::string out_path = (box.root() / "stdout").string();
    const std::string err_path = (box.root() / "stderr").string();
    const std::string work_dir = dir.string();

    std::vector<char*> argv;
    for (auto& a : argv_storage) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage = sandbox_env(box, extra_env);
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

inline Result run_wt(const Sandbox& box, const fs::path& dir,
                     std::initializer_list<std::string> args,
                     const Env& extra_env = {}) {
    std::vector<std::string> argv = {WT_BINARY_PATH};
    argv.insert(argv.end(), args);
    return run_in(box, dir, std::move(argv), extra_env);
}

// Runs `wt <args>` in the work directory of a fresh sandbox.
inline Result run_wt(std::initializer_list<std::string> args) {
    Sandbox box;
    return run_wt(box, box.work(), args);
}

// Runs git for test setup; any failure fails the test.
inline void git(const Sandbox& box, const fs::path& dir,
                std::initializer_list<std::string> args) {
    static const std::string git_path = find_on_path("git");
    std::vector<std::string> argv = {git_path};
    argv.insert(argv.end(), args);
    const Result r = run_in(box, dir, std::move(argv));
    INFO("git stderr: " << r.err);
    REQUIRE(r.code == 0);
}

// Creates a repository at `path` with one commit on main.
inline fs::path make_repo(const Sandbox& box, const fs::path& path) {
    fs::create_directories(path.parent_path());
    git(box, path.parent_path(), {"init", "-q", "-b", "main", path.string()});
    write_file(path / "README", "hello\n");
    git(box, path, {"add", "README"});
    git(box, path, {"commit", "-q", "-m", "initial"});
    return path;
}

inline std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    std::stringstream in(text);
    for (std::string line; std::getline(in, line);) {
        result.push_back(line);
    }
    return result;
}

// Returns the `wt list` row for the worktree at `path` (the last column).
inline std::string row_for(const std::string& out, const std::string& path) {
    for (const auto& line : lines(out)) {
        if (line.ends_with("  " + path)) {
            return line;
        }
    }
    FAIL("no row for " << path << " in:\n" << out);
    return {};
}

// Runs git for test setup and returns its stdout without the final newline.
inline std::string git_output(const Sandbox& box, const fs::path& dir,
                              std::initializer_list<std::string> args) {
    static const std::string git_path = find_on_path("git");
    std::vector<std::string> argv = {git_path};
    argv.insert(argv.end(), args);
    const Result r = run_in(box, dir, std::move(argv));
    INFO("git stderr: " << r.err);
    REQUIRE(r.code == 0);
    std::string out = r.out;
    if (out.ends_with('\n')) {
        out.pop_back();
    }
    return out;
}

// Runs `wt new <task>`, requires success, and returns the printed path.
inline fs::path wt_new(const Sandbox& box, const fs::path& dir,
                       const std::string& task) {
    const Result r = run_wt(box, dir, {"new", task});
    INFO("wt stderr: " << r.err);
    REQUIRE(r.code == 0);
    REQUIRE(r.out.ends_with('\n'));
    return fs::path(r.out.substr(0, r.out.size() - 1));
}

// Environment that runs wt's git through a shell script that first runs
// `prelude`, in which "$REAL_GIT" is the real git and wt's arguments are
// `-C <dir> <args...>`, then the real git.
inline Env git_shim(const Sandbox& box, const std::string& prelude) {
    const fs::path bin = box.root() / "bin";
    fs::create_directories(bin);
    write_file(bin / "git", "#!/bin/sh\nREAL_GIT='" + find_on_path("git") +
                                "'\n" + prelude +
                                "exec \"$REAL_GIT\" \"$@\"\n");
    fs::permissions(bin / "git", fs::perms::owner_all);
    const char* path_env = std::getenv("PATH");
    return {{"PATH", bin.string() + ":" + (path_env ? path_env : "")}};
}

inline void commit_file(const Sandbox& box, const fs::path& dir,
                        const std::string& name, const std::string& content) {
    write_file(dir / name, content);
    git(box, dir, {"add", name});
    git(box, dir, {"commit", "-q", "-m", "change " + name});
}

// Returns the line of `out` starting with `label`, e.g. "Staged:".
inline std::string field(const std::string& out, const std::string& label) {
    for (const auto& line : lines(out)) {
        if (line.starts_with(label)) {
            return line;
        }
    }
    FAIL("no " << label << " line in:\n" << out);
    return {};
}

// Reports whether Git still has a worktree registered at `path`.
inline bool registered(const Sandbox& box, const fs::path& repo,
                       const fs::path& path) {
    // -z, so a path with a newline in it stays one field.
    std::stringstream in(
        git_output(box, repo, {"worktree", "list", "--porcelain", "-z"}));
    for (std::string field; std::getline(in, field, '\0');) {
        if (field == "worktree " + path.string()) {
            return true;
        }
    }
    return false;
}

// Checks that a refused `wt remove` left the worktree, its files, and its
// branch exactly as they were.
inline void check_untouched(const Sandbox& box, const fs::path& repo,
                            const fs::path& path, const std::string& branch) {
    CHECK(registered(box, repo, path));
    CHECK(fs::is_regular_file(path / "README"));
    CHECK(git_output(box, path, {"branch", "--show-current"}) == branch);
}

// Stops `git am` in the worktree at `dir` on a patch to a file that isn't
// there, which leaves the branch checked out and the files clean.
inline void stop_git_am(const Sandbox& box, const fs::path& dir) {
    const fs::path patch = box.work() / "missing.patch";
    write_file(patch,
               "From 0000000000000000000000000000000000000000 Mon Sep 17 "
               "00:00:00 2001\n"
               "From: Test <test@example.com>\n"
               "Subject: [PATCH] Change a missing file\n"
               "\n"
               "---\n"
               "diff --git a/missing.txt b/missing.txt\n"
               "--- a/missing.txt\n"
               "+++ b/missing.txt\n"
               "@@ -1 +1 @@\n"
               "-old\n"
               "+new\n");
    const Result r =
        run_in(box, dir, {find_on_path("git"), "am", "-q", patch.string()});
    REQUIRE(r.code != 0);
    REQUIRE(git_output(box, dir, {"status", "--porcelain"}).empty());
}

inline std::string head_of(const Sandbox& box, const fs::path& dir) {
    return git_output(box, dir, {"rev-parse", "HEAD"});
}

// Checks that a refused or failed merge left main's worktree exactly as it
// was: same HEAD, nothing changed, and no merge in progress.
inline void check_main_restored(const Sandbox& box, const fs::path& repo,
                                const std::string& before) {
    CHECK(head_of(box, repo) == before);
    CHECK(git_output(box, repo, {"status", "--porcelain"}).empty());
    CHECK_FALSE(fs::exists(repo / ".git" / "MERGE_HEAD"));
}

}  // namespace wt::test
