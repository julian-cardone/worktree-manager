// End-to-end tests for `wt remove --delete-branch`.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/e2e.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace wt::test;

namespace {

bool branch_exists(const Sandbox& box, const fs::path& repo,
                   const std::string& branch) {
    return !git_output(box, repo, {"branch", "--list", branch}).empty();
}

// A task with a commit main doesn't have, so its branch hasn't landed.
// Returns its worktree.
fs::path unmerged_task(const Sandbox& box, const fs::path& repo,
                       const std::string& task) {
    const fs::path path = wt_new(box, repo, task);
    commit_file(box, path, "work.txt", "work\n");
    return path;
}

std::string deleted(const std::string& task, const std::string& tip) {
    return "Deleted branch " + task + " (was " + tip.substr(0, 7) +
           ")\nTo restore it: git branch " + task + " " + tip + "\n";
}

}  // namespace

TEST_CASE("wt remove --delete-branch deletes an unmerged task's branch") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = unmerged_task(box, repo, "claude/abandon");
    const std::string tip = head_of(box, path);
    // Configuration goes with the branch, as with `git branch -d`.
    git(box, repo, {"config", "branch.claude/abandon.remote", "origin"});
    git(box, repo, {"config", "branch.claude/abandon.description", "x"});

    auto r = run_wt(box, repo, {"remove", "claude/abandon", "--delete-branch"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out == "Removed worktree " + path.string() + "\n" +
                       deleted("claude/abandon", tip));
    CHECK_FALSE(fs::exists(path));
    CHECK_FALSE(registered(box, repo, path));
    CHECK_FALSE(branch_exists(box, repo, "claude/abandon"));
    CHECK(git_output(box, repo, {"config", "--list", "--local"})
              .find("branch.claude/abandon.") == std::string::npos);
    CHECK(head_of(box, repo) != tip);

    // The printed command brings the work back.
    git(box, repo, {"branch", "claude/abandon", tip});
    CHECK(git_output(box, repo, {"rev-parse", "claude/abandon"}) == tip);
}

TEST_CASE("wt remove --delete-branch deletes a branch whose worktree is gone") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = unmerged_task(box, repo, "feature");
    const std::string tip = head_of(box, path);
    git(box, repo, {"branch", "other"});
    REQUIRE(run_wt(box, repo, {"remove", "feature"}).code == 0);
    REQUIRE(branch_exists(box, repo, "feature"));

    auto r = run_wt(box, repo, {"remove", "feature", "--delete-branch"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out == deleted("feature", tip));
    CHECK_FALSE(branch_exists(box, repo, "feature"));

    SECTION("repeating it fails and changes nothing") {
        r = run_wt(box, repo, {"remove", "feature", "--delete-branch"});
        CHECK(r.code == 1);
        CHECK(r.out.empty());
        CHECK(r.err ==
              "wt: error: no branch named 'feature'\nNothing was removed.\n");
        CHECK(branch_exists(box, repo, "other"));
        CHECK(branch_exists(box, repo, "main"));
    }
    SECTION("--forget-missing doesn't apply without a worktree") {
        git(box, repo, {"branch", "parked"});
        r = run_wt(box, repo,
                   {"remove", "parked", "--delete-branch", "--forget-missing"});
        CHECK(r.code == 1);
        CHECK(r.err ==
              "wt: error: 'parked' has no worktree, so --forget-missing "
              "doesn't apply\nNothing was removed.\n");
        CHECK(branch_exists(box, repo, "parked"));
    }
}

TEST_CASE("wt remove without --delete-branch still keeps an unmerged branch") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = unmerged_task(box, repo, "feature");
    const std::string tip = head_of(box, path);

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 0);
    CHECK(r.out ==
          "Removed worktree " + path.string() + "\nKept branch feature\n");
    CHECK(git_output(box, repo, {"rev-parse", "feature"}) == tip);

    // Without its worktree, the branch is still kept.
    r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 1);
    CHECK(git_output(box, repo, {"rev-parse", "feature"}) == tip);
}

TEST_CASE(
    "wt remove --delete-branch keeps everything when work would be lost") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    write_file(repo / ".gitignore", "*.env\n");
    git(box, repo, {"add", ".gitignore"});
    git(box, repo, {"commit", "-q", "-m", "ignore"});
    const fs::path path = unmerged_task(box, repo, "feature");
    std::string expected;

    SECTION("staged") {
        write_file(path / "staged.txt", "staged\n");
        git(box, path, {"add", "staged.txt"});
        expected = "  1 staged file\n";
    }
    SECTION("modified") {
        write_file(path / "README", "changed\n");
        expected = "  1 modified file\n";
    }
    SECTION("untracked") {
        write_file(path / "new.txt", "new\n");
        expected = "  1 untracked file\n    new.txt\n";
    }
    SECTION("conflicted") {
        commit_file(box, path, "README", "feature\n");
        commit_file(box, repo, "README", "main\n");
        REQUIRE(run_in(box, path, {find_on_path("git"), "merge", "-q", "main"})
                    .code != 0);
        expected = "  1 conflicted file\n";
    }
    SECTION("ignored") {
        write_file(path / "secret.env", "token\n");
        expected = "  1 ignored file\n    secret.env\n";
    }
    SECTION("hidden from Git") {
        git(box, path, {"update-index", "--assume-unchanged", "README"});
        write_file(path / "README", "local edit\n");
        expected = "  1 modified file that Git doesn't show\n    README\n";
    }
    const std::string tip = head_of(box, path);

    auto r = run_wt(box, repo, {"remove", "feature", "--delete-branch"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: refused: cannot remove feature\n\n" +
                                 expected + "\n"));
    CHECK_THAT(r.err, ContainsSubstring("Nothing was removed.\n"));
    CHECK(registered(box, repo, path));
    CHECK(fs::is_regular_file(path / "work.txt"));
    CHECK(git_output(box, repo, {"rev-parse", "refs/heads/feature"}) == tip);
}

TEST_CASE("wt remove --delete-branch doesn't bypass the operation check") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = unmerged_task(box, repo, "feature");
    stop_git_am(box, path);

    auto r = run_wt(box, repo, {"remove", "feature", "--delete-branch"});
    CHECK(r.code == 2);
    CHECK(r.err ==
          "wt: refused: the worktree for 'feature' is in the middle of a git "
          "am; finish or abort it first\nNothing was removed.\n");
    CHECK(registered(box, repo, path));
    CHECK(branch_exists(box, repo, "feature"));
}

TEST_CASE("wt remove --delete-branch refuses integration branches") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    wt_new(box, repo, "feature");
    std::string branch;

    SECTION("main, checked out in the main worktree") {
        branch = "main";
    }
    SECTION("master, even when main is the integration branch") {
        branch = "master";
        git(box, repo, {"branch", "master"});
    }
    SECTION("the configured integration branch") {
        branch = "develop";
        git(box, repo, {"branch", "develop"});
        git(box, repo, {"config", "wt.integrationBranch", "develop"});
    }
    SECTION("origin's default branch") {
        branch = "trunk";
        git(box, repo, {"branch", "trunk"});
        git(box, repo,
            {"update-ref", "refs/remotes/origin/trunk", "refs/heads/trunk"});
        git(box, repo,
            {"symbolic-ref", "refs/remotes/origin/HEAD",
             "refs/remotes/origin/trunk"});
    }

    auto r = run_wt(box, repo, {"remove", branch, "--delete-branch"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: refused: '" + branch +
                       "' is, or could become, the integration branch, so wt "
                       "won't delete it\nNothing was removed.\n");
    CHECK(branch_exists(box, repo, branch));
}

TEST_CASE("wt remove --delete-branch refuses a branch checked out elsewhere") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path container = wt_new(box, repo, "feature").parent_path();

    SECTION("in the main worktree") {
        git(box, repo, {"switch", "-q", "-c", "topic"});
        auto r = run_wt(box, repo, {"remove", "topic", "--delete-branch"});
        CHECK(r.code == 2);
        CHECK(r.err ==
              "wt: refused: 'topic' is checked out in the main worktree, "
              "which wt never removes\nNothing was removed.\n");
        CHECK(git_output(box, repo, {"branch", "--show-current"}) == "topic");
    }
    SECTION("in a worktree wt didn't create") {
        const fs::path other = box.work() / "elsewhere";
        git(box, repo,
            {"worktree", "add", "-q", "-b", "topic", other.string()});
        auto r = run_wt(box, repo, {"remove", "topic", "--delete-branch"});
        CHECK(r.code == 2);
        CHECK(r.err == "wt: refused: the worktree for 'topic' at " +
                           other.string() +
                           " wasn't created by wt, so wt won't remove "
                           "it\nNothing was removed.\n");
        check_untouched(box, repo, other, "topic");
    }
    SECTION("being rebased") {
        const fs::path path = unmerged_task(box, repo, "topic");
        commit_file(box, path, "README", "topic\n");
        commit_file(box, repo, "README", "main\n");
        REQUIRE(run_in(box, path, {find_on_path("git"), "rebase", "-q", "main"})
                    .code != 0);
        auto r = run_wt(box, repo, {"remove", "topic", "--delete-branch"});
        CHECK(r.code == 2);
        CHECK(r.err == "wt: refused: 'topic' is being rebased in " +
                           path.string() +
                           "; finish or abort the rebase first\nNothing was "
                           "removed.\n");
        CHECK(registered(box, repo, path));
        CHECK(branch_exists(box, repo, "topic"));
    }
    CHECK(fs::is_regular_file(container / ".wt-managed"));
}

TEST_CASE("wt remove --delete-branch refuses branches that can't be tasks") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    wt_new(box, repo, "feature");

    SECTION("a name wt wouldn't create") {
        git(box, repo, {"branch", "fix+1"});
        auto r = run_wt(box, repo, {"remove", "fix+1", "--delete-branch"});
        CHECK(r.code == 2);
        CHECK_THAT(r.err, StartsWith("wt: refused: 'fix+1' can't be a wt "
                                     "task (the task name may only contain"));
        CHECK_THAT(r.err, ContainsSubstring("), so wt won't delete it\n"
                                            "Nothing was removed.\n"));
        CHECK(branch_exists(box, repo, "fix+1"));
    }
    SECTION("a different spelling of a branch's case") {
        // On a case-insensitive filesystem Git would find "Feature" in
        // feature's ref file.
        auto r = run_wt(box, repo, {"remove", "Feature", "--delete-branch"});
        CHECK(r.code == 1);
        CHECK(r.err ==
              "wt: error: no branch named 'Feature'\nNothing was removed.\n");
        CHECK(branch_exists(box, repo, "feature"));
    }
    SECTION("a tag, not a branch") {
        git(box, repo, {"tag", "v1"});
        auto r = run_wt(box, repo, {"remove", "v1", "--delete-branch"});
        CHECK(r.code == 1);
        CHECK(r.err ==
              "wt: error: no branch named 'v1'\nNothing was removed.\n");
        CHECK(git_output(box, repo, {"tag", "--list", "v1"}) == "v1");
    }
}

TEST_CASE("wt remove --delete-branch refuses repositories wt doesn't manage") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"branch", "parked"});

    SECTION("wt has never created a worktree here") {
        auto r = run_wt(box, repo, {"remove", "parked", "--delete-branch"});
        CHECK(r.code == 2);
        CHECK(r.err ==
              "wt: refused: 'parked' has no worktree, and wt has never "
              "created one in this repository, so wt can't tell it's a task "
              "and won't delete it\nNothing was removed.\n");
    }
    SECTION("the container belongs to another repository") {
        const fs::path container = wt_new(box, repo, "feature").parent_path();
        write_file(container / ".wt-managed",
                   "wt-managed 1\nrepository /elsewhere/.git\n");
        auto r = run_wt(box, repo, {"remove", "parked", "--delete-branch"});
        CHECK(r.code == 2);
        CHECK_THAT(r.err, StartsWith("wt: refused: " + container.string() +
                                     " exists but wt can't confirm it "
                                     "manages it ("));
        CHECK_THAT(r.err, ContainsSubstring("so wt won't delete 'parked'\n"));
    }
    SECTION("the branch is another repository's") {
        const fs::path other = make_repo(box, box.work() / "other");
        wt_new(box, other, "feature");
        auto r = run_wt(box, other, {"remove", "parked", "--delete-branch"});
        CHECK(r.code == 1);
        CHECK(r.err ==
              "wt: error: no branch named 'parked'\nNothing was removed.\n");
    }
    CHECK(branch_exists(box, repo, "parked"));
}

TEST_CASE("wt remove --delete-branch keeps the branch if removal fails") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = unmerged_task(box, repo, "feature");
    const std::string tip = head_of(box, path);

    SECTION("Git fails to remove the worktree") {
        fs::create_directories(path / "locked");
        write_file(path / "locked" / "data", "data\n");
        git(box, path, {"add", "locked/data"});
        git(box, path, {"commit", "-q", "-m", "data"});
        const std::string locked_tip = head_of(box, path);
        fs::permissions(path / "locked", fs::perms::owner_write,
                        fs::perm_options::remove);
        auto r = run_wt(box, repo, {"remove", "feature", "--delete-branch"});
        fs::permissions(path / "locked", fs::perms::owner_write,
                        fs::perm_options::add);
        CHECK(r.code == 4);
        CHECK(r.out.empty());
        CHECK_THAT(r.err, StartsWith("wt: git error: git worktree remove "
                                     "failed with exit code "));
        CHECK_THAT(r.err, ContainsSubstring("wt: branch feature was kept\n"));
        CHECK(git_output(box, repo, {"rev-parse", "refs/heads/feature"}) ==
              locked_tip);
    }
    SECTION("the removal can't be verified") {
        auto r = run_wt(box, repo, {"remove", "feature", "--delete-branch"},
                        git_fails_after(box, " worktree remove "));
        CHECK(r.code == 4);
        CHECK(r.out.empty());
        CHECK_THAT(r.err, ContainsSubstring("the result could not be "
                                            "verified"));
        CHECK_THAT(r.err, ContainsSubstring("wt: branch feature was kept\n"));
        CHECK(git_output(box, repo, {"rev-parse", "refs/heads/feature"}) ==
              tip);
    }
    SECTION("the branch check fails after the worktree is gone") {
        // Every for-each-ref after the removal fails, so the recheck before
        // deleting the branch can't run.
        const std::string done = (box.root() / "removed").string();
        const Env env = git_shim(
            box, "case \" $* \" in *' worktree remove '*) : > '" + done +
                     "';; *' for-each-ref '*) if [ -e '" + done +
                     "' ]; then echo 'fatal: simulated failure' >&2; exit "
                     "128; fi;; esac\n");
        auto r =
            run_wt(box, repo, {"remove", "feature", "--delete-branch"}, env);
        CHECK(r.code == 4);
        CHECK(r.out == "Removed worktree " + path.string() + "\n");
        CHECK(r.err ==
              "wt: git error: simulated failure\n"
              "wt: the worktree was removed, but branch feature was kept; "
              "once that's dealt with, delete it with `wt remove feature "
              "--delete-branch`\n");
        CHECK_FALSE(registered(box, repo, path));
        CHECK(git_output(box, repo, {"rev-parse", "refs/heads/feature"}) ==
              tip);
    }
}

TEST_CASE("wt remove --delete-branch keeps a branch that moved meanwhile") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = unmerged_task(box, repo, "feature");
    REQUIRE(run_wt(box, repo, {"remove", "feature"}).code == 0);
    const std::string main_tip = head_of(box, repo);
    // Moves the branch just before wt deletes it, as another process could.
    const Env env =
        git_shim(box,
                 "case \" $* \" in *' update-ref '*) \"$REAL_GIT\" -C \"$2\" "
                 "branch -f feature main;; esac\n");

    auto r = run_wt(box, repo, {"remove", "feature", "--delete-branch"}, env);
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: git error: git update-ref failed with exit "
                          "code "));
    CHECK_THAT(r.err, ContainsSubstring("wt: branch feature was kept\n"));
    CHECK(git_output(box, repo, {"rev-parse", "refs/heads/feature"}) ==
          main_tip);
    CHECK(fs::exists(path.parent_path() / ".wt-managed"));
}

TEST_CASE("wt remove --delete-branch with --forget-missing") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = unmerged_task(box, repo, "feature");
    const std::string tip = head_of(box, path);
    fs::remove_all(path);

    SECTION("still refuses a missing worktree without it") {
        auto r = run_wt(box, repo, {"remove", "feature", "--delete-branch"});
        CHECK(r.code == 2);
        CHECK_THAT(r.err, StartsWith("wt: refused: nothing is at "));
        CHECK(registered(box, repo, path));
        CHECK(branch_exists(box, repo, "feature"));
    }
    SECTION("forgets the worktree, then deletes the branch") {
        auto r = run_wt(
            box, repo,
            {"remove", "feature", "--forget-missing", "--delete-branch"});
        CHECK(r.code == 0);
        CHECK(r.err.empty());
        CHECK(r.out == "Worktree " + path.string() +
                           " was already gone; removed Git's record of it\n" +
                           deleted("feature", tip));
        CHECK_FALSE(registered(box, repo, path));
        CHECK_FALSE(branch_exists(box, repo, "feature"));
    }
}

TEST_CASE("wt remove --delete-branch leaves other branches' configuration") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = unmerged_task(box, repo, "a");
    const std::string tip = head_of(box, path);
    // Its name starts with the task's, so its section looks like a's.
    git(box, repo, {"branch", "a.b"});
    git(box, repo, {"config", "branch.a.b.merge", "refs/heads/x"});

    SECTION("when the task has no configuration") {}
    SECTION("when the task has configuration too") {
        git(box, repo, {"config", "branch.a.remote", "origin"});
    }

    auto r = run_wt(box, repo, {"remove", "a", "--delete-branch"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out ==
          "Removed worktree " + path.string() + "\n" + deleted("a", tip));
    CHECK(git_output(box, repo, {"config", "branch.a.b.merge"}) ==
          "refs/heads/x");
    CHECK(git_output(box, repo, {"config", "--list", "--local"})
              .find("branch.a.remote") == std::string::npos);
    CHECK(branch_exists(box, repo, "a.b"));
}
