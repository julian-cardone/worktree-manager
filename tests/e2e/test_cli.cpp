// End-to-end tests: run the real wt binary as a subprocess.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/sandbox.hpp"

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
// `extra` adds variables on top, and must only point inside the sandbox.
using Env = std::vector<std::pair<std::string, std::string>>;

std::vector<std::string> sandbox_env(const Sandbox& box, const Env& extra) {
    Env overrides = {
        {"HOME", box.home().string()},
        {"XDG_CONFIG_HOME", (box.home() / ".config").string()},
        {"GIT_CONFIG_NOSYSTEM", "1"},
        {"GIT_CEILING_DIRECTORIES", box.root().string()},
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

Result run_wt(const Sandbox& box, const fs::path& dir,
              std::initializer_list<std::string> args,
              const Env& extra_env = {}) {
    std::vector<std::string> argv = {WT_BINARY_PATH};
    argv.insert(argv.end(), args);
    return run_in(box, dir, std::move(argv), extra_env);
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

// ---------------------------------------------------------------------------
// wt new
// ---------------------------------------------------------------------------

namespace {

// Runs git for test setup and returns its stdout without the final newline.
std::string git_output(const Sandbox& box, const fs::path& dir,
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
fs::path wt_new(const Sandbox& box, const fs::path& dir,
                const std::string& task) {
    const Result r = run_wt(box, dir, {"new", task});
    INFO("wt stderr: " << r.err);
    REQUIRE(r.code == 0);
    REQUIRE(r.out.ends_with('\n'));
    return fs::path(r.out.substr(0, r.out.size() - 1));
}

// Returns where wt puts `repo`'s container, making sure it doesn't exist yet.
fs::path expected_container(const Sandbox& box, const fs::path& repo) {
    const fs::path probe = wt_new(box, repo, "probe");
    git(box, repo, {"worktree", "remove", probe.string()});
    git(box, repo, {"branch", "-D", "probe"});
    const fs::path container = probe.parent_path();
    fs::remove_all(container);
    return container;
}

// Every entry directly inside `dir`, so tests can check nothing appeared.
std::vector<fs::path> entries(const fs::path& dir) {
    std::vector<fs::path> result;
    for (const auto& entry : fs::directory_iterator(dir)) {
        result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

}  // namespace

TEST_CASE("wt new creates a worktree in the repository's container") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");

    auto r = run_wt(box, repo, {"new", "feature-x"});
    CHECK(r.code == 0);
    CHECK(r.err == "wt: created branch feature-x from main\n");
    REQUIRE(r.out.ends_with("/feature-x\n"));
    const fs::path path(r.out.substr(0, r.out.size() - 1));

    // stdout is exactly the absolute path, so `cd "$(wt new x)"` works.
    CHECK(path.is_absolute());
    CHECK(lines(r.out).size() == 1);
    const fs::path container = path.parent_path();
    CHECK(container.parent_path() == box.work());
    CHECK_THAT(container.filename().string(), StartsWith(".repo.wt-"));
    CHECK_THAT(read_file(container / ".wt-managed"),
               ContainsSubstring("repository " + (repo / ".git").string()));

    CHECK(git_output(box, path, {"branch", "--show-current"}) == "feature-x");
    CHECK(git_output(box, path, {"rev-parse", "HEAD"}) ==
          git_output(box, repo, {"rev-parse", "main"}));
    CHECK(fs::is_regular_file(path / "README"));

    auto list = run_wt(box, repo, {"list"});
    CHECK_THAT(row_for(list.out, path.string()), StartsWith("  feature-x"));
}

TEST_CASE("wt new reuses the container and maps slashes to dashes") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path first = wt_new(box, repo, "feature-x");
    const fs::path second = wt_new(box, repo, "claude/fix-login");

    CHECK(second.parent_path() == first.parent_path());
    CHECK(second.filename() == "claude-fix-login");
    CHECK(git_output(box, second, {"branch", "--show-current"}) ==
          "claude/fix-login");
    // Only the repository and its one container sit in the parent.
    CHECK(entries(box.work()) ==
          std::vector<fs::path>{first.parent_path(), repo});
}

TEST_CASE("wt new from a linked worktree still starts from main") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path first = wt_new(box, repo, "first");
    write_file(first / "wip.txt", "wip\n");
    git(box, first, {"add", "wip.txt"});
    git(box, first, {"commit", "-q", "-m", "wip"});
    fs::create_directories(first / "sub");

    const fs::path second = wt_new(box, first / "sub", "second");
    CHECK(second.parent_path() == first.parent_path());
    CHECK(git_output(box, second, {"rev-parse", "HEAD"}) ==
          git_output(box, repo, {"rev-parse", "main"}));
    CHECK_FALSE(fs::exists(second / "wip.txt"));
}

TEST_CASE("wt new starts from the local branch, never fetching") {
    Sandbox box;
    const fs::path upstream = make_repo(box, box.work() / "upstream");
    const fs::path clone = box.work() / "clone";
    git(box, box.work(), {"clone", "-q", upstream.string(), clone.string()});
    write_file(upstream / "later.txt", "later\n");
    git(box, upstream, {"add", "later.txt"});
    git(box, upstream, {"commit", "-q", "-m", "later"});
    const std::string before =
        git_output(box, clone, {"rev-parse", "origin/main"});

    const fs::path path = wt_new(box, clone, "feature");
    CHECK(git_output(box, path, {"rev-parse", "HEAD"}) ==
          git_output(box, clone, {"rev-parse", "main"}));
    CHECK(git_output(box, clone, {"rev-parse", "origin/main"}) == before);
}

TEST_CASE("wt new uses wt.integrationBranch when it's set") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"branch", "develop"});
    git(box, repo, {"config", "wt.integrationBranch", "develop"});

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 0);
    CHECK(r.err == "wt: created branch feature from develop\n");
}

TEST_CASE("wt new fails when wt.integrationBranch names a missing branch") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"config", "wt.integrationBranch", "nope"});

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               ContainsSubstring("wt.integrationBranch is set to 'nope'"));
    CHECK(entries(box.work()) == std::vector<fs::path>{repo});
}

TEST_CASE("wt new follows origin's default branch") {
    Sandbox box;
    const fs::path upstream = make_repo(box, box.work() / "upstream");
    git(box, upstream, {"branch", "trunk"});
    const fs::path clone = box.work() / "clone";
    git(box, box.work(), {"clone", "-q", upstream.string(), clone.string()});
    git(box, clone, {"remote", "set-head", "origin", "trunk"});

    // origin/HEAD wins over a local main, but only as a local branch.
    auto r = run_wt(box, clone, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK_THAT(r.err, ContainsSubstring("origin's default branch is 'trunk'"));
    CHECK(entries(box.work()) == (std::vector<fs::path>{clone, upstream}));

    git(box, clone, {"branch", "trunk", "origin/trunk"});
    r = run_wt(box, clone, {"new", "feature"});
    CHECK(r.code == 0);
    CHECK(r.err == "wt: created branch feature from trunk\n");
}

TEST_CASE("wt new falls back to master") {
    Sandbox box;
    const fs::path repo = box.work() / "repo";
    git(box, box.work(), {"init", "-q", "-b", "master", repo.string()});
    write_file(repo / "README", "hello\n");
    git(box, repo, {"add", "README"});
    git(box, repo, {"commit", "-q", "-m", "initial"});

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 0);
    CHECK(r.err == "wt: created branch feature from master\n");
}

TEST_CASE("wt new refuses to guess an integration branch") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"branch", "-m", "main", "trunk"});

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK_THAT(r.err, ContainsSubstring("no local main or master branch"));
    CHECK(entries(box.work()) == std::vector<fs::path>{repo});
}

TEST_CASE("wt new fails in a repository with no commits") {
    Sandbox box;
    const fs::path repo = box.work() / "empty";
    git(box, box.work(), {"init", "-q", "-b", "main", repo.string()});

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK_THAT(r.err, ContainsSubstring("no commits yet"));
    CHECK(entries(box.work()) == std::vector<fs::path>{repo});
}

TEST_CASE("wt new outside a repository fails without creating anything") {
    Sandbox box;
    auto r = run_wt(box, box.work(), {"new", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: not a git repository"));
    CHECK(entries(box.work()).empty());
    CHECK(entries(box.root()).size() == 4);  // home, work, stdout, stderr
}

TEST_CASE("wt new rejects invalid task names without creating anything") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    // The last few pass wt's own checks but are rejected by Git.
    for (const std::string task :
         {"../escape", "a/../../b", "/abs", "has space", "semi;colon",
          "$(touch pwned)", "HEAD", ".hidden", "a.lock", "end.", "a/b.lock"}) {
        INFO("task: " << task);
        auto r = run_wt(box, repo, {"new", task});
        CHECK(r.code == 1);
        CHECK(r.out.empty());
        CHECK_THAT(r.err, StartsWith("wt: error: invalid task name"));
    }
    // A leading dash is caught by argument parsing or by wt.
    auto r = run_wt(box, repo, {"new", "--", "-rf"});
    CHECK(r.code == 1);
    CHECK_THAT(r.err, ContainsSubstring("can't start with '-'"));

    CHECK(entries(box.work()) == std::vector<fs::path>{repo});
    CHECK(git_output(box, repo, {"branch", "--format=%(refname:short)"}) ==
          "main");
}

TEST_CASE("wt new requires a task name") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    auto r = run_wt(box, repo, {"new"});
    CHECK(r.code != 0);
    CHECK_THAT(r.err, ContainsSubstring("task"));
}

TEST_CASE("wt new refuses a branch that already exists") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"branch", "feature"});

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: error: branch 'feature' already exists\n");
}

TEST_CASE("wt new refuses a target path that already exists") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path container = wt_new(box, repo, "first").parent_path();
    const fs::path target = container / "feature";

    SECTION("directory with files") {
        fs::create_directory(target);
        write_file(target / "data", "keep me");
    }
    SECTION("empty directory") {
        fs::create_directory(target);
    }
    SECTION("file") {
        write_file(target, "keep me");
    }
    SECTION("dangling symlink") {
        fs::create_symlink(box.work() / "nowhere", target);
    }

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK(r.err == "wt: error: " + target.string() + " already exists\n");
    CHECK_FALSE(fs::exists(target / ".git"));
    if (fs::is_regular_file(target / "data")) {
        CHECK(read_file(target / "data") == "keep me");
    }
    CHECK(git_output(box, repo, {"branch", "--list", "feature"}).empty());
}

TEST_CASE("wt new refuses two tasks that share a folder") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path first = wt_new(box, repo, "a/b");

    auto r = run_wt(box, repo, {"new", "a-b"});
    CHECK(r.code == 1);
    CHECK(r.err == "wt: error: " + first.string() +
                       " is already the worktree for branch a/b\n");
}

TEST_CASE("wt new refuses a path Git still has registered") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path container = wt_new(box, repo, "first").parent_path();
    const fs::path target = container / "feature";
    git(box, repo, {"worktree", "add", "-q", "--detach", target.string()});
    fs::remove_all(target);

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK_THAT(r.err, ContainsSubstring("already has a worktree registered") &&
                          ContainsSubstring("run `git worktree prune`"));
    CHECK_FALSE(fs::exists(target));
}

TEST_CASE("wt new refuses a container it can't prove it owns") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path container = expected_container(box, repo);
    const fs::path elsewhere = box.work() / "elsewhere";
    fs::create_directory(elsewhere);

    SECTION("directory without a marker") {
        fs::create_directory(container);
        write_file(container / "theirs", "keep me");
    }
    SECTION("marker for another repository") {
        fs::create_directory(container);
        write_file(container / ".wt-managed",
                   "wt-managed 1\nrepository /somewhere/else/.git\n");
    }
    SECTION("symlink to an owned-looking directory") {
        // A copy of a real marker, so only the symlink check can stop it.
        const fs::path real = wt_new(box, repo, "real").parent_path();
        fs::create_directory(elsewhere / "c");
        fs::copy_file(real / ".wt-managed", elsewhere / "c" / ".wt-managed");
        git(box, repo, {"worktree", "remove", (real / "real").string()});
        fs::remove_all(real);
        fs::create_directory_symlink(elsewhere / "c", container);
    }
    SECTION("regular file") {
        write_file(container, "keep me");
    }

    const auto before_work = entries(box.work());
    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: " + container.string()));
    CHECK(entries(box.work()) == before_work);
    CHECK_FALSE(fs::exists(elsewhere / "c" / "feature"));
    if (fs::is_regular_file(container / "theirs")) {
        CHECK(read_file(container / "theirs") == "keep me");
        CHECK_FALSE(fs::exists(container / ".wt-managed"));
    }
    CHECK(git_output(box, repo, {"branch", "--list", "feature"}).empty());
}

TEST_CASE("wt new warns when the branch already exists on origin") {
    Sandbox box;
    const fs::path upstream = make_repo(box, box.work() / "upstream");
    git(box, upstream, {"branch", "feature"});
    const fs::path clone = box.work() / "clone";
    git(box, box.work(), {"clone", "-q", upstream.string(), clone.string()});

    auto r = run_wt(box, clone, {"new", "feature"});
    CHECK(r.code == 0);
    CHECK_THAT(r.err, StartsWith("wt: warning: origin/feature already exists"));
}

TEST_CASE("wt new reports what git leaves behind and removes nothing") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path hook = repo / ".git" / "hooks" / "post-checkout";
    write_file(hook, "#!/bin/sh\necho hook broke >&2\nexit 3\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(
        r.err,
        StartsWith("wt: error: git worktree add failed with exit code 3"));
    CHECK_THAT(r.err, ContainsSubstring("  hook broke\n"));
    CHECK_THAT(r.err, ContainsSubstring("  branch feature\n"));
    CHECK_THAT(r.err,
               ContainsSubstring(
                   "nothing was removed; if you don't need them, remove the "
                   "worktree with `wt remove feature`, then delete the branch "
                   "with `git branch -d feature`\n"));

    // Both are still there for the user to inspect.
    git(box, repo, {"show-ref", "--verify", "--quiet", "refs/heads/feature"});
    auto list = run_wt(box, repo, {"list"});
    CHECK_THAT(list.out, ContainsSubstring("/feature\n"));

    // The suggested cleanup works.
    auto removed = run_wt(box, repo, {"remove", "feature"});
    CHECK(removed.code == 0);
    git(box, repo, {"branch", "-d", "feature"});
    CHECK(git_output(box, repo, {"branch", "--list", "feature"}).empty());
}

TEST_CASE("wt new fails when Git succeeds but the worktree isn't the task's") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    // Before the hook, which would also fire for expected_container's probe.
    const fs::path target = expected_container(box, repo) / "feature";
    // Runs in the new worktree after Git has made it. The switch runs the
    // hook again, which must then do nothing.
    const fs::path hook = repo / ".git" / "hooks" / "post-checkout";
    write_file(hook,
               "#!/bin/sh\n[ \"$(git branch --show-current)\" = feature ] || "
               "exit 0\ngit switch -q -c other\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    // Nothing for `cd "$(wt new feature)"` to follow.
    CHECK(r.out.empty());
    CHECK(r.err == "wt: error: git worktree add reported success, but " +
                       target.string() +
                       " has branch other checked out, not feature\n"
                       "wt: nothing was removed; `wt list` shows what is "
                       "there\n");
    // Left as the hook made it, for the user to inspect.
    CHECK(git_output(box, target, {"branch", "--show-current"}) == "other");
}

TEST_CASE("wt new works with a bare repository") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path bare = box.work() / "bare.git";
    git(box, box.work(),
        {"clone", "-q", "--bare", repo.string(), bare.string()});

    const fs::path path = wt_new(box, bare, "feature");
    CHECK(path.parent_path().parent_path() == box.work());
    CHECK_THAT(path.parent_path().filename().string(), StartsWith(".bare.wt-"));
    CHECK(git_output(box, path, {"branch", "--show-current"}) == "feature");
}

TEST_CASE("wt new handles spaces in the repository path") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "my repo");
    const fs::path path = wt_new(box, repo, "feature");
    CHECK_THAT(path.parent_path().filename().string(),
               StartsWith(".my repo.wt-"));
    CHECK(fs::is_regular_file(path / "README"));
}

// ---------------------------------------------------------------------------
// wt status
// ---------------------------------------------------------------------------

namespace {

void commit_file(const Sandbox& box, const fs::path& dir,
                 const std::string& name, const std::string& content) {
    write_file(dir / name, content);
    git(box, dir, {"add", name});
    git(box, dir, {"commit", "-q", "-m", "change " + name});
}

// Returns the line of `out` starting with `label`, e.g. "Staged:".
std::string field(const std::string& out, const std::string& label) {
    for (const auto& line : lines(out)) {
        if (line.starts_with(label)) {
            return line;
        }
    }
    FAIL("no " << label << " line in:\n" << out);
    return {};
}

}  // namespace

TEST_CASE("wt status summarizes a clean task worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature-x");
    const std::string head =
        git_output(box, repo, {"log", "-1", "--format=%h %s", "main"});

    auto r = run_wt(box, repo, {"status", "feature-x"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out ==
          "feature-x\n"
          "\n"
          "Path:         " +
              path.string() +
              "\n"
              "HEAD:         " +
              head +
              "\n"
              "Status:       clean\n"
              "Staged:       0\n"
              "Modified:     0\n"
              "Untracked:    0\n"
              "Conflicts:    0\n"
              "Ahead main:   0\n"
              "Behind main:  0\n");
}

TEST_CASE(
    "wt status counts changes and commits against the integration branch") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature-x");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, path, "b.txt", "b\n");
    commit_file(box, path, "c.txt", "c\n");
    commit_file(box, repo, "main.txt", "main\n");

    write_file(path / "staged.txt", "staged\n");
    git(box, path, {"add", "staged.txt"});
    write_file(path / "README", "changed\n");
    write_file(path / "a.txt", "changed\n");
    write_file(path / "untracked.txt", "new\n");

    auto r = run_wt(box, repo, {"status", "feature-x"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(field(r.out, "HEAD:") ==
          "HEAD:         " +
              git_output(box, path, {"log", "-1", "--format=%h %s"}));
    CHECK(field(r.out, "Status:") == "Status:       dirty");
    CHECK(field(r.out, "Staged:") == "Staged:       1");
    CHECK(field(r.out, "Modified:") == "Modified:     2");
    CHECK(field(r.out, "Untracked:") == "Untracked:    1");
    CHECK(field(r.out, "Conflicts:") == "Conflicts:    0");
    CHECK(field(r.out, "Ahead main:") == "Ahead main:   3");
    CHECK(field(r.out, "Behind main:") == "Behind main:  1");
}

TEST_CASE("wt status reports merge conflicts") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "README", "feature\n");
    commit_file(box, repo, "README", "main\n");
    const Result merge =
        run_in(box, path, {find_on_path("git"), "merge", "-q", "main"});
    REQUIRE(merge.code != 0);

    auto r = run_wt(box, repo, {"status", "feature"});
    CHECK(r.code == 0);
    CHECK(field(r.out, "Status:") == "Status:       dirty");
    CHECK(field(r.out, "Conflicts:") == "Conflicts:    1");
    CHECK(field(r.out, "Staged:") == "Staged:       0");
}

TEST_CASE("wt status gives the same answer from any worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path feature = wt_new(box, repo, "claude/fix");
    // A worktree wt didn't create is found by its branch too.
    const fs::path other = box.work() / "elsewhere";
    git(box, repo, {"worktree", "add", "-q", "-b", "other", other.string()});
    fs::create_directories(feature / "src");
    write_file(feature / "wip.txt", "wip\n");

    const auto expected = run_wt(box, repo, {"status", "claude/fix"});
    REQUIRE(expected.code == 0);
    CHECK(lines(expected.out).at(0) == "claude/fix");
    CHECK(field(expected.out, "Untracked:") == "Untracked:    1");
    for (const fs::path& from : {feature / "src", other}) {
        INFO("running from " << from);
        auto r = run_wt(box, from, {"status", "claude/fix"});
        CHECK(r.code == 0);
        CHECK(r.out == expected.out);
    }
    auto r = run_wt(box, feature, {"status", "other"});
    CHECK(r.code == 0);
    CHECK(field(r.out, "Path:") == "Path:         " + other.string());
}

TEST_CASE("wt status on the integration branch leaves out ahead and behind") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    auto r = run_wt(box, repo, {"status", "main"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(field(r.out, "Status:") == "Status:     clean");
    CHECK_THAT(r.out,
               !ContainsSubstring("Ahead") && !ContainsSubstring("Behind"));
}

TEST_CASE("wt status fails for a task without a worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");

    auto r = run_wt(box, repo, {"status", "nope"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: no worktree for task 'nope'; `wt list` shows them all\n");

    git(box, repo, {"branch", "parked"});
    r = run_wt(box, repo, {"status", "parked"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: branch 'parked' isn't checked out in any worktree\n");

    r = run_wt(box, repo, {"status", "--", "-x"});
    CHECK(r.code == 1);
    CHECK_THAT(r.err, StartsWith("wt: error: no worktree for task '-x'"));
}

TEST_CASE("wt status reports a worktree whose directory was deleted") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "gone");
    commit_file(box, path, "a.txt", "a\n");
    fs::remove_all(path);

    auto r = run_wt(box, repo, {"status", "gone"});
    CHECK(r.code == 0);
    CHECK(field(r.out, "Status:") == "Status:       missing");
    CHECK_THAT(r.out, !ContainsSubstring("Staged:"));
    CHECK(field(r.out, "Ahead main:") == "Ahead main:   1");
    CHECK_THAT(r.err,
               StartsWith("wt: warning: the worktree's directory is gone"));
}

TEST_CASE("wt status fails when the worktree's status can't be read") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "broken");
    fs::remove(path / ".git");

    auto r = run_wt(box, repo, {"status", "broken"});
    CHECK(r.code == 1);
    CHECK(field(r.out, "Status:") == "Status:       unknown");
    CHECK_THAT(r.out, !ContainsSubstring("Staged:"));
    CHECK_THAT(r.err, StartsWith("wt: error: cannot read the status of " +
                                 path.string()));
}

TEST_CASE("wt status handles a repository with no commits") {
    Sandbox box;
    const fs::path repo = box.work() / "empty";
    git(box, box.work(), {"init", "-q", "-b", "main", repo.string()});
    write_file(repo / "new.txt", "new\n");

    auto r = run_wt(box, repo, {"status", "main"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(field(r.out, "HEAD:") == "HEAD:       (no commits)");
    CHECK(field(r.out, "Untracked:") == "Untracked:  1");
}

TEST_CASE("wt status warns, but still reports, without an integration branch") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"branch", "-m", "main", "trunk"});

    auto r = run_wt(box, repo, {"status", "trunk"});
    CHECK(r.code == 0);
    CHECK(field(r.out, "Status:") == "Status:     clean");
    CHECK_THAT(r.out, !ContainsSubstring("Ahead"));
    CHECK_THAT(r.err,
               StartsWith("wt: warning: not comparing with an integration "
                          "branch: cannot tell which branch to start from"));
}

TEST_CASE("wt status and wt list see untracked files Git config hides") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"config", "status.showUntrackedFiles", "no"});
    write_file(repo / "new.txt", "new\n");

    auto r = run_wt(box, repo, {"status", "main"});
    CHECK(field(r.out, "Status:") == "Status:     dirty");
    CHECK(field(r.out, "Untracked:") == "Untracked:  1");
    auto list = run_wt(box, repo, {"list"});
    CHECK_THAT(row_for(list.out, repo.string()), StartsWith("* main    dirty"));
}

TEST_CASE("wt status sanitizes commit subjects") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"commit", "-q", "--allow-empty", "-m", "evil\x1b[31m"});

    auto r = run_wt(box, repo, {"status", "main"});
    CHECK(r.code == 0);
    CHECK_THAT(field(r.out, "HEAD:"), ContainsSubstring(" evil?[31m"));
    CHECK(r.out.find('\x1b') == std::string::npos);
}

TEST_CASE("wt status outside a repository fails with a clear error") {
    Sandbox box;
    auto r = run_wt(box, box.work(), {"status", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: not a git repository"));
}

TEST_CASE("wt status requires a task name") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    auto r = run_wt(box, repo, {"status"});
    CHECK(r.code != 0);
    CHECK_THAT(r.err, ContainsSubstring("task"));
}

// --- wt remove ---

namespace {

// Reports whether Git still has a worktree registered at `path`.
bool registered(const Sandbox& box, const fs::path& repo,
                const fs::path& path) {
    const auto list =
        lines(git_output(box, repo, {"worktree", "list", "--porcelain"}));
    return std::ranges::find(list, "worktree " + path.string()) != list.end();
}

// Checks that a refused `wt remove` left the worktree, its files, and its
// branch exactly as they were.
void check_untouched(const Sandbox& box, const fs::path& repo,
                     const fs::path& path, const std::string& branch) {
    CHECK(registered(box, repo, path));
    CHECK(fs::is_regular_file(path / "README"));
    CHECK(git_output(box, path, {"branch", "--show-current"}) == branch);
}

}  // namespace

TEST_CASE("wt remove removes a clean worktree and keeps its branch") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "claude/fix-login");
    commit_file(box, path, "a.txt", "a\n");
    const std::string head = git_output(box, path, {"rev-parse", "HEAD"});

    auto r = run_wt(box, repo, {"remove", "claude/fix-login"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out == "Removed worktree " + path.string() +
                       "\n"
                       "Kept branch claude/fix-login\n");
    CHECK_FALSE(fs::exists(path));
    CHECK_FALSE(registered(box, repo, path));
    CHECK(git_output(box, repo, {"rev-parse", "refs/heads/claude/fix-login"}) ==
          head);
    // The container stays, so the next `wt new` reuses it.
    CHECK(fs::is_regular_file(path.parent_path() / ".wt-managed"));

    // The task can be started again from scratch once the branch is gone.
    git(box, repo, {"branch", "-D", "claude/fix-login"});
    CHECK(wt_new(box, repo, "claude/fix-login") == path);
}

TEST_CASE("wt remove refuses a worktree with uncommitted changes") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature-x");
    commit_file(box, path, "a.txt", "a\n");
    write_file(path / "README", "changed\n");
    write_file(path / "a.txt", "changed\n");
    write_file(path / "new.txt", "new\n");

    auto r = run_wt(box, repo, {"remove", "feature-x"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: cannot remove feature-x\n"
          "\n"
          "  2 modified files\n"
          "  1 untracked file\n"
          "\n"
          "Commit, stash, or remove these changes first.\n");
    check_untouched(box, repo, path, "feature-x");
    CHECK(read_file(path / "a.txt") == "changed\n");
    CHECK(read_file(path / "new.txt") == "new\n");
}

TEST_CASE("wt remove refuses each kind of uncommitted change") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    std::string expected;

    SECTION("staged") {
        write_file(path / "staged.txt", "staged\n");
        git(box, path, {"add", "staged.txt"});
        // Staged and then deleted from disk: only the index has it.
        fs::remove(path / "staged.txt");
        expected = "  1 staged file\n  1 modified file\n";
    }
    SECTION("unstaged deletion") {
        fs::remove(path / "README");
        fs::create_directories(path / "dir");
        write_file(path / "dir" / "README", "x\n");
        expected = "  1 modified file\n  1 untracked file\n";
    }
    SECTION("untracked directory") {
        fs::create_directories(path / "notes" / "deep");
        write_file(path / "notes" / "deep" / "idea.md", "idea\n");
        expected = "  1 untracked file\n";
    }
    SECTION("untracked file hidden by Git config") {
        git(box, repo, {"config", "status.showUntrackedFiles", "no"});
        write_file(path / "new.txt", "new\n");
        expected = "  1 untracked file\n";
    }
    SECTION("merge conflict") {
        commit_file(box, path, "README", "feature\n");
        commit_file(box, repo, "README", "main\n");
        REQUIRE(run_in(box, path, {find_on_path("git"), "merge", "-q", "main"})
                    .code != 0);
        expected = "  1 conflicted file\n";
    }

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: error: cannot remove feature\n\n" + expected +
                       "\nCommit, stash, or remove these changes first.\n");
    CHECK(registered(box, repo, path));
    CHECK(fs::is_directory(path));
}

TEST_CASE("wt remove refuses ignored files unless told to delete them") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    write_file(repo / ".gitignore", "*.env\nbuild/\n");
    git(box, repo, {"add", ".gitignore"});
    git(box, repo, {"commit", "-q", "-m", "ignore"});
    const fs::path path = wt_new(box, repo, "feature");
    write_file(path / "secret.env", "token\n");
    fs::create_directories(path / "build" / "obj");
    write_file(path / "build" / "obj" / "a.o", "o\n");

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: cannot remove feature\n"
          "\n"
          "  2 ignored files\n"
          "    build/\n"
          "    secret.env\n"
          "\n"
          "Ignored files are deleted with the worktree; move any you need, "
          "then rerun with --delete-ignored.\n");
    CHECK(read_file(path / "secret.env") == "token\n");
    CHECK(registered(box, repo, path));

    SECTION("changes are reported alongside ignored files") {
        write_file(path / "new.txt", "new\n");
        r = run_wt(box, repo, {"remove", "feature"});
        CHECK(r.code == 1);
        CHECK(r.err ==
              "wt: error: cannot remove feature\n"
              "\n"
              "  1 untracked file\n"
              "  2 ignored files\n"
              "    build/\n"
              "    secret.env\n"
              "\n"
              "Commit, stash, or remove these changes first.\n"
              "Ignored files are deleted with the worktree; move any you "
              "need, then rerun with --delete-ignored.\n");

        // --delete-ignored never lets uncommitted changes through.
        r = run_wt(box, repo, {"remove", "--delete-ignored", "feature"});
        CHECK(r.code == 1);
        CHECK(r.err ==
              "wt: error: cannot remove feature\n"
              "\n"
              "  1 untracked file\n"
              "\n"
              "Commit, stash, or remove these changes first.\n");
        CHECK(read_file(path / "new.txt") == "new\n");
        CHECK(read_file(path / "secret.env") == "token\n");
    }
    SECTION("a long list of ignored files is cut short") {
        for (int i = 0; i < 10; ++i) {
            write_file(path / ("k" + std::to_string(i) + ".env"), "key\n");
        }
        r = run_wt(box, repo, {"remove", "feature"});
        CHECK(r.code == 1);
        CHECK(r.err ==
              "wt: error: cannot remove feature\n"
              "\n"
              "  12 ignored files\n"
              "    build/\n"
              "    k0.env\n"
              "    k1.env\n"
              "    k2.env\n"
              "    k3.env\n"
              "    k4.env\n"
              "    k5.env\n"
              "    k6.env\n"
              "    k7.env\n"
              "    k8.env\n"
              "    ... and 2 more\n"
              "\n"
              "Ignored files are deleted with the worktree; move any you "
              "need, then rerun with --delete-ignored.\n");
    }
    SECTION("--delete-ignored removes them") {
        r = run_wt(box, repo, {"remove", "feature", "--delete-ignored"});
        CHECK(r.code == 0);
        CHECK(r.err.empty());
        CHECK_FALSE(fs::exists(path));
        CHECK_FALSE(registered(box, repo, path));
    }
}

TEST_CASE("wt checks the task worktree even when GIT_DIR points elsewhere") {
    // Git exports these to hooks, `git rebase --exec` commands, and `!`
    // aliases. They override `git -C`, so wt's checks would read the main
    // checkout instead of the task's worktree.
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    write_file(repo / ".gitignore", "*.env\n");
    git(box, repo, {"add", ".gitignore"});
    git(box, repo, {"commit", "-q", "-m", "ignore"});
    const fs::path path = wt_new(box, repo, "feature");
    write_file(path / "secret.env", "token\n");
    write_file(path / "new.txt", "new\n");
    const Env hook_env = {
        {"GIT_DIR", (repo / ".git").string()},
        {"GIT_WORK_TREE", repo.string()},
        {"GIT_INDEX_FILE", (repo / ".git" / "index").string()},
    };

    auto r = run_wt(box, repo, {"status", "feature"}, hook_env);
    CHECK(r.code == 0);
    CHECK(field(r.out, "Untracked:") == "Untracked:    1");

    fs::remove(path / "new.txt");
    r = run_wt(box, repo, {"remove", "feature"}, hook_env);
    CHECK(r.code == 1);
    CHECK_THAT(r.err, ContainsSubstring("1 ignored file\n    secret.env\n"));
    CHECK(read_file(path / "secret.env") == "token\n");
    check_untouched(box, repo, path, "feature");
}

TEST_CASE("wt remove cleans up a worktree whose directory is gone") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "gone");
    commit_file(box, path, "a.txt", "a\n");

    SECTION("directory deleted") {
        fs::remove_all(path);
    }
    SECTION("whole container deleted") {
        fs::remove_all(path.parent_path());
    }

    auto r = run_wt(box, repo, {"remove", "gone"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out == "Worktree " + path.string() +
                       " was already gone; removed Git's record of it\n"
                       "Kept branch gone\n");
    CHECK_FALSE(registered(box, repo, path));
    CHECK(git_output(box, repo, {"log", "-1", "--format=%s", "gone"}) ==
          "change a.txt");
}

TEST_CASE("wt remove refuses the main worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path linked = wt_new(box, repo, "feature");

    for (const fs::path& from : {repo, linked}) {
        INFO("running from " << from);
        auto r = run_wt(box, from, {"remove", "main"});
        CHECK(r.code == 1);
        CHECK(r.out.empty());
        CHECK(r.err ==
              "wt: error: 'main' is checked out in the main worktree, which "
              "wt never removes\n");
    }
    CHECK(fs::is_regular_file(repo / "README"));
}

TEST_CASE("wt remove refuses a worktree wt didn't create") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path other = box.work() / "elsewhere";
    git(box, repo, {"worktree", "add", "-q", "-b", "other", other.string()});
    // Even inside the container, only direct children are wt's.
    const fs::path container = wt_new(box, repo, "feature").parent_path();
    const fs::path nested = container / "deeper" / "nested";
    git(box, repo, {"worktree", "add", "-q", "-b", "nested", nested.string()});

    for (const auto& [task, path] :
         {std::pair{"other", other}, std::pair{"nested", nested}}) {
        INFO("task " << task);
        auto r = run_wt(box, repo, {"remove", task});
        CHECK(r.code == 1);
        CHECK(r.out.empty());
        CHECK(r.err == "wt: error: the worktree for '" + std::string(task) +
                           "' at " + path.string() +
                           " wasn't created by wt, so wt won't remove it\n");
        check_untouched(box, repo, path, task);
    }
}

TEST_CASE("wt remove refuses a container it can't prove it owns") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    const fs::path marker = path.parent_path() / ".wt-managed";

    SECTION("marker missing") {
        fs::remove(marker);
    }
    SECTION("marker for another repository") {
        write_file(marker, "wt-managed 1\nrepository /elsewhere/.git\n");
    }

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: error: " + path.parent_path().string() +
                          " exists but wt can't confirm it manages it ("));
    check_untouched(box, repo, path, "feature");
}

TEST_CASE("wt remove refuses the worktree it's run from") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    fs::create_directories(path / "src");

    for (const fs::path& from : {path, path / "src"}) {
        INFO("running from " << from);
        auto r = run_wt(box, from, {"remove", "feature"});
        CHECK(r.code == 1);
        CHECK(r.out.empty());
        CHECK(r.err ==
              "wt: error: you are inside the worktree for 'feature'; run wt "
              "remove from somewhere else\n");
        check_untouched(box, repo, path, "feature");
    }

    // Any other worktree of the repository works.
    const fs::path other = wt_new(box, repo, "other");
    auto r = run_wt(box, other, {"remove", "feature"});
    CHECK(r.code == 0);
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("wt remove refuses a locked worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    git(box, repo,
        {"worktree", "lock", "--reason", "on a USB drive", path.string()});

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: the worktree for 'feature' is locked; if it's no longer "
          "needed, unlock it with `git worktree unlock " +
              path.string() + "`\n");
    check_untouched(box, repo, path, "feature");
}

TEST_CASE("wt remove refuses a worktree in the middle of a merge") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, repo, "a.txt", "a\n");
    // "ours" keeps the files as they are, so only MERGE_HEAD shows the merge.
    git(box, path, {"merge", "-q", "--no-commit", "-s", "ours", "main"});
    REQUIRE(git_output(box, path, {"status", "--porcelain"}).empty());

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: the worktree for 'feature' is in the middle of a merge; "
          "finish or abort it first\n");
    check_untouched(box, repo, path, "feature");
}

TEST_CASE("wt remove refuses a worktree path that isn't a real directory") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    // Git itself would delete the files a symlink here points to.
    const fs::path moved = box.work() / "moved";
    fs::rename(path, moved);

    SECTION("symlink") {
        fs::create_directory_symlink(moved, path);
    }
    SECTION("file") {
        write_file(path, "keep me");
    }

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: error: " + path.string() +
                       " is not a directory, so wt won't remove it\n");
    CHECK(registered(box, repo, path));
    CHECK(read_file(moved / "README") == "hello\n");
    if (fs::is_regular_file(fs::symlink_status(path))) {
        CHECK(read_file(path) == "keep me");
    }
}

TEST_CASE("wt remove refuses a worktree whose status can't be read") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "broken");
    fs::remove(path / ".git");

    auto r = run_wt(box, repo, {"remove", "broken"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: error: cannot read the status of " + path.string() +
                       ": its .git file is missing; not removing it\n");
    CHECK(fs::is_regular_file(path / "README"));
    CHECK(registered(box, repo, path));
}

TEST_CASE("wt remove reports Git's failure and what is left") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    fs::create_directories(path / "locked");
    write_file(path / "locked" / "data", "data\n");
    git(box, path, {"add", "locked/data"});
    git(box, path, {"commit", "-q", "-m", "data"});
    // Git can't delete files inside a directory it can't write to.
    fs::permissions(path / "locked", fs::perms::owner_write,
                    fs::perm_options::remove);

    auto r = run_wt(box, repo, {"remove", "feature"});
    fs::permissions(path / "locked", fs::perms::owner_write,
                    fs::perm_options::add);
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: git worktree remove failed with "
                                 "exit code "));
    CHECK_THAT(r.err,
               ContainsSubstring("wt: " + path.string() + " is still there\n"));
    CHECK(read_file(path / "locked" / "data") == "data\n");
}

TEST_CASE("wt remove fails for a task without a worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"branch", "parked"});

    auto r = run_wt(box, repo, {"remove", "nope"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: no worktree for task 'nope'; `wt list` shows them all\n");

    r = run_wt(box, repo, {"remove", "parked"});
    CHECK(r.code == 1);
    CHECK(r.err ==
          "wt: error: branch 'parked' isn't checked out in any worktree\n");
    CHECK(git_output(box, repo, {"branch", "--list", "parked"}) == "  parked");
}

TEST_CASE("wt remove handles spaces in the repository path") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "my repo");
    const fs::path path = wt_new(box, repo, "feature");

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 0);
    CHECK_FALSE(fs::exists(path));
    CHECK_FALSE(registered(box, repo, path));
}

TEST_CASE("wt remove outside a repository fails with a clear error") {
    Sandbox box;
    auto r = run_wt(box, box.work(), {"remove", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: not a git repository"));
}

TEST_CASE("wt remove requires a task name") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    auto r = run_wt(box, repo, {"remove"});
    CHECK(r.code != 0);
    CHECK_THAT(r.err, ContainsSubstring("task"));
}

// --- wt merge ---

namespace {

std::string head_of(const Sandbox& box, const fs::path& dir) {
    return git_output(box, dir, {"rev-parse", "HEAD"});
}

// Checks that a refused or failed merge left main's worktree exactly as it
// was: same HEAD, nothing changed, and no merge in progress.
void check_main_restored(const Sandbox& box, const fs::path& repo,
                         const std::string& before) {
    CHECK(head_of(box, repo) == before);
    CHECK(git_output(box, repo, {"status", "--porcelain"}).empty());
    CHECK_FALSE(fs::exists(repo / ".git" / "MERGE_HEAD"));
}

}  // namespace

TEST_CASE("wt merge fast-forwards main and keeps the task") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "claude/fix-login");
    commit_file(box, path, "a.txt", "a\n");

    // Run from the task's own worktree, as an agent would.
    auto r = run_wt(box, path, {"merge", "claude/fix-login"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out == "✓ Merged claude/fix-login into main\n");
    CHECK(head_of(box, repo) == head_of(box, path));
    CHECK(read_file(repo / "a.txt") == "a\n");
    CHECK(git_output(box, repo, {"status", "--porcelain"}).empty());
    CHECK(fs::is_regular_file(path / "a.txt"));
    CHECK(git_output(box, path, {"branch", "--show-current"}) ==
          "claude/fix-login");
}

TEST_CASE("wt merge creates a merge commit when main has moved on") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, repo, "b.txt", "b\n");
    const std::string main_before = head_of(box, repo);
    const std::string task_head = head_of(box, path);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 0);
    CHECK(r.out == "✓ Merged feature into main\n");
    CHECK(git_output(box, repo, {"log", "-1", "--format=%P"}) ==
          main_before + " " + task_head);
    CHECK(git_output(box, repo, {"log", "-1", "--format=%s"}) ==
          "Merge branch 'feature' into main");
    CHECK(read_file(repo / "a.txt") == "a\n");
}

TEST_CASE("wt merge merges the branch, not a tag of the same name") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    git(box, repo, {"tag", "feature"});
    commit_file(box, path, "a.txt", "a\n");

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 0);
    CHECK(head_of(box, repo) == head_of(box, path));
}

TEST_CASE("wt merge merges a branch that has no worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string task_head = head_of(box, path);
    git(box, repo, {"worktree", "remove", path.string()});

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 0);
    CHECK(head_of(box, repo) == task_head);
}

TEST_CASE("wt merge does nothing when main already has the task's commits") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    wt_new(box, repo, "feature");
    commit_file(box, repo, "b.txt", "b\n");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out == "main already contains feature; nothing to merge\n");
    check_main_restored(box, repo, before);
}

TEST_CASE("wt merge aborts a conflicting merge and lists the conflicts") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature-x");
    fs::create_directories(path / "src");
    write_file(path / "src" / "foo.cpp", "task\n");
    write_file(path / "src" / "bar.cpp", "task\n");
    write_file(path / "README", "task\n");
    write_file(path / "clean.txt", "merges fine\n");
    git(box, path, {"add", "."});
    git(box, path, {"commit", "-q", "-m", "task"});
    fs::create_directories(repo / "src");
    write_file(repo / "src" / "foo.cpp", "main\n");
    write_file(repo / "src" / "bar.cpp", "main\n");
    git(box, repo, {"add", "."});
    git(box, repo, {"commit", "-q", "-m", "main"});
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature-x"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: merge blocked: feature-x conflicts with main\n"
          "\n"
          "Conflicts:\n"
          "- src/bar.cpp\n"
          "- src/foo.cpp\n"
          "\n"
          "main was restored; no merge remains in progress.\n");
    check_main_restored(box, repo, before);
    CHECK(read_file(repo / "src" / "foo.cpp") == "main\n");
    CHECK(read_file(repo / "README") == "hello\n");
    CHECK_FALSE(fs::exists(repo / "clean.txt"));
    CHECK(read_file(path / "src" / "foo.cpp") == "task\n");
}

TEST_CASE("wt merge aborts a merge Git stops without conflicts") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, repo, "b.txt", "b\n");
    const fs::path hook = repo / ".git" / "hooks" / "pre-merge-commit";
    write_file(hook, "#!/bin/sh\necho 'hook says no' >&2\nexit 1\n");
    fs::permissions(hook, fs::perms::owner_exec, fs::perm_options::add);
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: error: git merge failed with exit code 1\n"));
    CHECK_THAT(r.err, ContainsSubstring("  hook says no\n"));
    CHECK_THAT(r.err,
               ContainsSubstring(
                   "\nmain was restored; no merge remains in progress.\n"));
    check_main_restored(box, repo, before);
    CHECK_FALSE(fs::exists(repo / "a.txt"));
}

TEST_CASE("wt merge reports a merge Git refuses to start") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, repo, "b.txt", "b\n");
    // wt doesn't read branch-specific merge options, so Git refuses for it.
    git(box, repo, {"config", "branch.main.mergeOptions", "--ff-only"});
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: error: git merge failed with exit code "));
    CHECK_THAT(r.err, ContainsSubstring("\nmain is unchanged.\n"));
    check_main_restored(box, repo, before);
}

TEST_CASE("wt merge fails when Git succeeds but main lacks the task") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string before = head_of(box, repo);
    // Runs after Git has merged, and undoes the fast-forward.
    const fs::path hook = repo / ".git" / "hooks" / "post-merge";
    write_file(hook, "#!/bin/sh\ngit reset -q --hard HEAD~1\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: git merge reported success, but "
                                 "main doesn't contain feature\n"));
    CHECK_THAT(r.err, ContainsSubstring("\nInspect main's worktree with `git "
                                        "-C " +
                                        repo.string() + " status`.\n"));
    CHECK(head_of(box, repo) == before);
}

TEST_CASE("wt merge fails when Git succeeds but leaves main's worktree dirty") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const fs::path hook = repo / ".git" / "hooks" / "post-merge";
    write_file(hook, "#!/bin/sh\necho x > stray.txt\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: git merge reported success, but "
                                 "main's worktree is dirty\n"));
    // The merge itself happened and is left in place, along with the file.
    CHECK(head_of(box, repo) == head_of(box, path));
    CHECK(read_file(repo / "stray.txt") == "x\n");
}

TEST_CASE("wt merge explains how to land a task when merge.ff is only") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"config", "merge.ff", "only"});
    const fs::path first = wt_new(box, repo, "first");
    const fs::path second = wt_new(box, repo, "second");
    commit_file(box, first, "a.txt", "first\n");
    commit_file(box, second, "a.txt", "second\n");
    commit_file(box, second, "b.txt", "b\n");

    // The first task to finish fast-forwards main.
    auto r = run_wt(box, repo, {"merge", "first"});
    CHECK(r.code == 0);
    CHECK(r.out == "✓ Merged first into main\n");
    const std::string before = head_of(box, repo);
    CHECK(before == head_of(box, first));

    // The second is now behind, so it can't be fast-forwarded. Run from the
    // task's own worktree, as an agent would: the setting is shared by all
    // worktrees.
    r = run_wt(box, second, {"merge", "second"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: cannot fast-forward main to second\n"
          "\n"
          "second is 1 commit behind main, and merge.ff is set to "
          "\"only\".\n"
          "Rebase it onto main in its worktree, resolving any "
          "conflicts there:\n"
          "\n"
          "  git -C " +
              second.string() +
              " rebase main\n"
              "\n"
              "Then run `wt merge second` again.\n");
    check_main_restored(box, repo, before);

    // Rebasing resolves the conflict in the task's worktree, and main stays
    // linear: a fast-forward, with no merge commits.
    const Result rebase =
        run_in(box, second, {find_on_path("git"), "rebase", "-q", "main"});
    REQUIRE(rebase.code != 0);
    r = run_wt(box, repo, {"merge", "second"});
    CHECK(r.code == 1);
    CHECK_THAT(r.err, StartsWith("wt: error: 'second' is being rebased in " +
                                 second.string() + "; "));
    write_file(second / "a.txt", "first and second\n");
    git(box, second, {"add", "a.txt"});
    git(box, second, {"-c", "core.editor=true", "rebase", "--continue"});

    r = run_wt(box, second, {"merge", "second"});
    CHECK(r.code == 0);
    CHECK(r.out == "✓ Merged second into main\n");
    CHECK(head_of(box, repo) == head_of(box, second));
    CHECK(read_file(repo / "a.txt") == "first and second\n");
    CHECK(git_output(box, repo, {"rev-list", "--merges", "main"}).empty());
}

TEST_CASE("wt merge explains how to land a behind task without a worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"config", "merge.ff", "only"});
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    git(box, repo, {"worktree", "remove", path.string()});
    commit_file(box, repo, "b.txt", "b\n");
    commit_file(box, repo, "c.txt", "c\n");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.err ==
          "wt: error: cannot fast-forward main to feature\n"
          "\n"
          "feature is 2 commits behind main, and merge.ff is set to "
          "\"only\".\n"
          "feature has no worktree; rebase it onto main wherever you "
          "check it out.\n"
          "Then run `wt merge feature` again.\n");
    check_main_restored(box, repo, before);
}

TEST_CASE("wt merge refuses when main's worktree isn't clean") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string before = head_of(box, repo);

    SECTION("modified") {
        write_file(repo / "README", "changed\n");
    }
    SECTION("staged") {
        write_file(repo / "new.txt", "new\n");
        git(box, repo, {"add", "new.txt"});
    }
    SECTION("untracked") {
        write_file(repo / "new.txt", "new\n");
    }

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: error: main's worktree at " + repo.string() +
                       " has uncommitted changes; commit or stash them "
                       "before merging into main\n");
    CHECK(head_of(box, repo) == before);
    CHECK_FALSE(fs::exists(repo / "a.txt"));
}

TEST_CASE("wt merge refuses when main is in the middle of another merge") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    // A merge whose changes are already resolved can leave the files clean.
    write_file(repo / ".git" / "MERGE_HEAD", head_of(box, path) + "\n");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.err == "wt: error: main's worktree at " + repo.string() +
                       " is in the middle of a merge; finish or abort it "
                       "first\n");
    CHECK(head_of(box, repo) == before);
}

TEST_CASE("wt merge refuses when the task has uncommitted changes") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    write_file(path / "forgotten.txt", "not committed\n");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: the worktree for 'feature' has uncommitted changes, "
          "which the merge wouldn't include; commit or stash them first "
          "(`wt status feature` shows them)\n");
    CHECK(head_of(box, repo) == before);
    CHECK(read_file(path / "forgotten.txt") == "not committed\n");
}

TEST_CASE("wt merge refuses a task that is being rebased") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, repo, "b.txt", "b\n");
    const std::string task_before =
        git_output(box, repo, {"rev-parse", "feature"});

    // Mid-rebase, the worktree is detached and the branch still points at the
    // old commits, so merging it would land work that is being rewritten.
    SECTION("stopped with a clean worktree") {
        const Result r = run_in(
            box, path,
            {find_on_path("git"), "rebase", "-q", "--exec", "false", "main"});
        REQUIRE(r.code != 0);
    }
    SECTION("stopped at a conflict") {
        commit_file(box, repo, "a.txt", "main\n");
        const Result r =
            run_in(box, path, {find_on_path("git"), "rebase", "-q", "main"});
        REQUIRE(r.code != 0);
    }
    SECTION("in the main worktree") {
        // A task without its own worktree can be rebased from anywhere.
        git(box, repo, {"worktree", "remove", path.string()});
        const Result r = run_in(box, repo,
                                {find_on_path("git"), "rebase", "-q", "--exec",
                                 "false", "main", "feature"});
        REQUIRE(r.code != 0);
        // Leave main checked out somewhere, so only the rebase is in the way.
        git(box, repo,
            {"worktree", "add", "-q", (box.work() / "main").string(), "main"});
    }

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: 'feature' is being rebased in "));
    CHECK_THAT(r.err, ContainsSubstring("; finish the rebase (`git rebase "
                                        "--continue`) or abort it (`git rebase "
                                        "--abort`), then merge again\n"));
    CHECK(git_output(box, repo, {"rev-parse", "feature"}) == task_before);
    CHECK(run_in(box, repo,
                 {find_on_path("git"), "merge-base", "--is-ancestor",
                  task_before, "main"})
              .code == 1);
}

TEST_CASE("wt merge refuses to overwrite ignored files in main's worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    commit_file(box, repo, ".gitignore", ".env\nbuild\n");
    const fs::path path = wt_new(box, repo, "feature");
    fs::create_directories(path / "build");
    write_file(path / ".env", "from the task\n");
    write_file(path / "build" / "out", "from the task\n");
    git(box, path, {"add", "-f", ".env", "build/out"});
    git(box, path, {"commit", "-q", "-m", "track ignored files"});
    write_file(repo / ".env", "SECRET=mine\n");
    write_file(repo / "build", "a file where the task has a directory\n");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: cannot merge feature into main\n"
          "\n"
          "Merging would overwrite these ignored files in main's worktree:\n"
          "- .env\n"
          "- build\n"
          "\n"
          "Move them out of the way first.\n");
    CHECK(head_of(box, repo) == before);
    CHECK(read_file(repo / ".env") == "SECRET=mine\n");

    // Ignored files elsewhere don't get in the way.
    fs::remove(repo / ".env");
    fs::remove(repo / "build");
    fs::create_directories(repo / "build");
    write_file(repo / "build" / "other", "mine\n");
    r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 0);
    CHECK(read_file(repo / "build" / "other") == "mine\n");
}

TEST_CASE("wt merge needs main checked out somewhere") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    git(box, repo, {"switch", "-q", "--detach"});

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.err ==
          "wt: error: the integration branch 'main' isn't checked out in any "
          "worktree; check it out to merge into it\n");
    CHECK(git_output(box, repo, {"rev-parse", "main"}) != head_of(box, path));
}

TEST_CASE("wt merge rejects the integration branch and unknown branches") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");

    auto r = run_wt(box, repo, {"merge", "main"});
    CHECK(r.code == 1);
    CHECK(r.err ==
          "wt: error: 'main' is the integration branch; there is "
          "nothing to merge it into\n");

    for (const std::string task : {"nope", "-x", "a..b"}) {
        r = run_wt(box, repo, {"merge", "--", task});
        CHECK(r.code == 1);
        CHECK(r.err == "wt: error: no branch named '" + task + "'\n");
    }
}

TEST_CASE("wt merge outside a repository fails with a clear error") {
    Sandbox box;
    auto r = run_wt(box, box.work(), {"merge", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: not a git repository"));
}
