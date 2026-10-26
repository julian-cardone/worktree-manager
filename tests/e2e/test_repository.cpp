// End-to-end tests for how wt finds the repository and worktrees it acts on:
// GIT_DIR and the other variables Git exports, and paths Git spells
// differently.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/e2e.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using namespace wt::test;

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
    CHECK(r.code == 2);
    CHECK_THAT(r.err, ContainsSubstring("1 ignored file\n    secret.env\n"));
    CHECK(read_file(path / "secret.env") == "token\n");
    check_untouched(box, repo, path, "feature");
}

TEST_CASE("wt run from git rebase --exec checks the right worktree") {
    // Git sets GIT_DIR to the rebasing worktree's own Git directory here, so
    // it agrees with the current directory; wt must still not let it redirect
    // the checks it runs on another worktree.
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    write_file(repo / ".gitignore", "*.env\n");
    git(box, repo, {"add", ".gitignore"});
    git(box, repo, {"commit", "-q", "-m", "ignore"});
    const fs::path other = wt_new(box, repo, "other");
    write_file(other / "secret.env", "token\n");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const fs::path log = box.work() / "exec.log";
    const std::string exec =
        std::string("'") + WT_BINARY_PATH + "' remove other >'" + log.string() +
        "' 2>&1; echo \"exit=$?\" >>'" + log.string() + "'";

    const Result r =
        run_in(box, path,
               {find_on_path("git"), "rebase", "-q", "--exec", exec, "HEAD~1"});
    INFO("git stderr: " << r.err);
    REQUIRE(r.code == 0);
    const std::string output = read_file(log);
    CHECK_THAT(output, ContainsSubstring("1 ignored file\n    secret.env\n"));
    CHECK_THAT(output, EndsWith("exit=2\n"));
    CHECK(read_file(other / "secret.env") == "token\n");
    check_untouched(box, repo, other, "other");
}

TEST_CASE("wt uses GIT_DIR to find a bare repository from outside it") {
    // E.g. dotfiles kept in a bare repository and managed with GIT_DIR and
    // GIT_WORK_TREE set, from a directory that isn't in any repository.
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path bare = box.work() / "dots.git";
    git(box, box.work(),
        {"clone", "-q", "--bare", repo.string(), bare.string()});
    const fs::path elsewhere = box.work() / "elsewhere";
    fs::create_directories(elsewhere);
    const Env env = {{"GIT_DIR", bare.string()},
                     {"GIT_WORK_TREE", elsewhere.string()}};

    auto r = run_wt(box, elsewhere, {"list"}, env);
    INFO("wt stderr: " << r.err);
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring(bare.string()));

    r = run_wt(box, elsewhere, {"new", "feature"}, env);
    INFO("wt stderr: " << r.err);
    REQUIRE(r.code == 0);
    const fs::path path(r.out.substr(0, r.out.size() - 1));
    CHECK(path.parent_path().parent_path() == box.work());
    CHECK(git_output(box, path, {"branch", "--show-current"}) == "feature");

    // A relative GIT_DIR is read against the current directory, as Git does.
    r = run_wt(box, box.work(), {"status", "feature"},
               {{"GIT_DIR", "dots.git"}});
    INFO("wt stderr: " << r.err);
    CHECK(r.code == 0);

    // Without GIT_DIR there is no repository here.
    CHECK(run_wt(box, elsewhere, {"list"}).code == 4);
}

TEST_CASE("wt refuses when GIT_DIR and the current directory disagree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path other_repo = make_repo(box, box.work() / "other");

    auto r = run_wt(box, repo, {"new", "feature"},
                    {{"GIT_DIR", (other_repo / ".git").string()}});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: refused: GIT_DIR points at "));
    CHECK_THAT(r.err, ContainsSubstring("; unset GIT_DIR, or run wt from the "
                                        "worktree it points at\n"));
    for (const auto& dir : {repo, other_repo}) {
        CHECK(git_output(box, dir, {"branch", "--list", "feature"}).empty());
    }

    // Another worktree of the same repository is just as ambiguous.
    const fs::path path = wt_new(box, repo, "task");
    r = run_wt(box, path, {"remove", "task"},
               {{"GIT_DIR", (repo / ".git").string()}});
    CHECK(r.code == 2);
    CHECK_THAT(r.err, StartsWith("wt: refused: GIT_DIR points at "));
    check_untouched(box, repo, path, "task");
}

TEST_CASE("wt reports a GIT_DIR that isn't a Git directory") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");

    const auto r = run_wt(box, repo, {"list"},
                          {{"GIT_DIR", (box.work() / "nope").string()}});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: error: GIT_DIR is set to '" +
                                 (box.work() / "nope").string() +
                                 "', which isn't a Git directory: "));
    CHECK_THAT(r.err, EndsWith("; unset GIT_DIR or point it at one\n"));
}

TEST_CASE("wt manages worktrees under a decomposed Unicode folder name") {
    Sandbox box;
    const fs::path parent =
        box.work() / "cafe\xcc\x81";  // e + combining accent
    const fs::path repo = make_repo(box, parent / "re\xcc\x81po");
    const fs::path path = wt_new(box, repo, "feature");
    CHECK(fs::is_regular_file(path / "README"));
    CHECK(git_output(box, path, {"branch", "--show-current"}) == "feature");

    auto r = run_wt(box, path, {"remove", "feature"});
    CHECK(r.code == 2);
    CHECK(r.err ==
          "wt: refused: you are inside the worktree for 'feature'; run wt "
          "remove from somewhere else\nNothing was removed.\n");
    CHECK(fs::is_regular_file(path / "README"));

    r = run_wt(box, repo, {"remove", "feature"});
    INFO("wt stderr: " << r.err);
    CHECK(r.code == 0);
    CHECK_FALSE(fs::exists(path));
}
