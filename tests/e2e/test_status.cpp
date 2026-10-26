// End-to-end tests for `wt status`.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/e2e.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace wt::test;

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
    CHECK(r.err ==
          "wt: warning: the worktree's directory is gone; if it was moved, "
          "`git worktree repair <new-path>` reconnects it, otherwise "
          "`wt remove gone --forget-missing` makes Git forget it and keeps "
          "the branch\n");
}

TEST_CASE("wt status says a locked worktree whose directory is gone is kept") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "offline");
    git(box, repo,
        {"worktree", "lock", "--reason", "on a USB drive", path.string()});
    fs::remove_all(path);

    auto r = run_wt(box, repo, {"status", "offline"});
    CHECK(r.code == 0);
    CHECK(field(r.out, "Status:") == "Status:       missing");
    CHECK(r.err ==
          "wt: warning: the worktree's directory is gone, but it is locked, "
          "so Git keeps its record; if it's no longer needed, unlock it with "
          "`git worktree unlock " +
              path.string() + "`\n");
}

TEST_CASE("wt status fails when something else is at the worktree's path") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "replaced");
    fs::remove_all(path);
    write_file(path, "keep me");

    auto r = run_wt(box, repo, {"status", "replaced"});
    CHECK(r.code == 1);
    CHECK(field(r.out, "Status:") == "Status:       unknown");
    CHECK(r.err == "wt: error: cannot read the status of " + path.string() +
                       ": it is not a directory\n");
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

TEST_CASE("wt status finds a task's worktree while it is being rebased") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "task\n");
    commit_file(box, repo, "a.txt", "main\n");
    // Stops at the conflict, with HEAD detached until the rebase finishes.
    REQUIRE(
        run_in(box, path, {find_on_path("git"), "rebase", "-q", "main"}).code !=
        0);

    auto r = run_wt(box, repo, {"status", "feature"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(field(r.out, "Path:") == "Path:         " + path.string());
    CHECK(field(r.out, "Status:") == "Status:       dirty");
    CHECK(field(r.out, "In progress:") == "In progress:  a rebase");
    CHECK(field(r.out, "Conflicts:") == "Conflicts:    1");
    // Still measured from the branch, which keeps its old commits until then.
    CHECK(field(r.out, "Ahead main:") == "Ahead main:   1");
}

TEST_CASE("wt status says a rebase elsewhere will move the branch") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const fs::path top = wt_new(box, repo, "top");
    git(box, top, {"rebase", "-q", "feature"});
    commit_file(box, top, "t.txt", "t\n");
    // --update-refs leaves alone branches checked out elsewhere.
    git(box, repo, {"worktree", "remove", path.string()});
    commit_file(box, repo, "b.txt", "b\n");
    REQUIRE(run_in(box, top,
                   {find_on_path("git"), "rebase", "-q", "--update-refs",
                    "--exec", "false", "main"})
                .code != 0);

    auto r = run_wt(box, repo, {"status", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: branch 'feature' isn't checked out in any "
          "worktree, but a rebase in " +
              top.string() + " will move it when it finishes\n");

    // The worktree rebasing top is still top's.
    r = run_wt(box, repo, {"status", "top"});
    CHECK(r.code == 0);
    CHECK(field(r.out, "Path:") == "Path:         " + top.string());
    CHECK(field(r.out, "In progress:") == "In progress:  a rebase");
}

TEST_CASE("wt status shows an operation that left the files clean") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    std::string operation;

    SECTION("bisect") {
        git(box, path, {"bisect", "start"});
        operation = "a bisect";
    }
    SECTION("git am") {
        stop_git_am(box, path);
        operation = "a git am";
    }

    auto r = run_wt(box, repo, {"status", "feature"});
    CHECK(r.code == 0);
    CHECK(field(r.out, "Status:") == "Status:       clean");
    CHECK(field(r.out, "In progress:") == "In progress:  " + operation);
}

TEST_CASE("wt status fails when it can't tell whether an operation is on") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    const Env failing =
        git_shim(box,
                 "case \" $* \" in *' --git-path rebase-merge '*) "
                 "echo 'fatal: simulated failure' >&2; exit 128;; esac\n");

    auto r = run_wt(box, repo, {"status", "feature"}, failing);
    CHECK(r.code == 1);
    CHECK(field(r.out, "Status:") == "Status:       clean");
    CHECK(field(r.out, "In progress:") == "In progress:  unknown");
    CHECK(r.err == "wt: error: cannot check whether " + path.string() +
                       " is in the middle of an operation: simulated "
                       "failure\n");
}

TEST_CASE("wt status outside a repository fails with a clear error") {
    Sandbox box;
    auto r = run_wt(box, box.work(), {"status", "feature"});
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: git error: not a git repository"));
}

TEST_CASE("wt status requires a task name") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    auto r = run_wt(box, repo, {"status"});
    CHECK(r.code != 0);
    CHECK_THAT(r.err, ContainsSubstring("task"));
}
