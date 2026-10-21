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
    CHECK_THAT(r.err, ContainsSubstring("nothing was removed"));

    // Both are still there for the user to inspect.
    git(box, repo, {"show-ref", "--verify", "--quiet", "refs/heads/feature"});
    auto list = run_wt(box, repo, {"list"});
    CHECK_THAT(list.out, ContainsSubstring("/feature\n"));
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
