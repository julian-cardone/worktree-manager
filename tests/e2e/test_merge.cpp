// End-to-end tests for `wt merge`.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/e2e.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using namespace wt::test;

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
    CHECK(r.code == 3);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: conflict: feature-x conflicts with main\n"
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
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(
        r.err,
        StartsWith("wt: git error: git merge failed with exit code 1\n"));
    CHECK_THAT(r.err, ContainsSubstring("  hook says no\n"));
    CHECK_THAT(r.err,
               ContainsSubstring(
                   "\nmain was restored; no merge remains in progress.\n"));
    check_main_restored(box, repo, before);
    CHECK_FALSE(fs::exists(repo / "a.txt"));
}

TEST_CASE(
    "wt merge doesn't call main restored when it's no longer checked out") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, repo, "b.txt", "b\n");
    git(box, repo, {"branch", "other"});
    // After the abort, HEAD is at the same commit, but on another branch.
    const fs::path hook = repo / ".git" / "hooks" / "pre-merge-commit";
    write_file(hook,
               "#!/bin/sh\ngit symbolic-ref HEAD refs/heads/other\n"
               "exit 1\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: git error: the merge failed, and wt "
                                 "could not undo the merge: main's worktree "
                                 "no longer has main checked out\n"));
    CHECK_THAT(r.err, !ContainsSubstring("was restored"));
    CHECK(git_output(box, repo, {"symbolic-ref", "HEAD"}) ==
          "refs/heads/other");
}

namespace {

// Commits a.txt with different content on main and on a new task `task`, so
// merging the task conflicts there.
void make_conflict(const Sandbox& box, const fs::path& repo,
                   const std::string& task) {
    const fs::path path = wt_new(box, repo, task);
    commit_file(box, path, "a.txt", "task\n");
    commit_file(box, repo, "a.txt", "main\n");
}

}  // namespace

TEST_CASE("wt merge warns that a merge may remain when the abort fails") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    make_conflict(box, repo, "feature");
    const Env env = git_shim(box,
                             "if [ \"$3\" = merge ] && [ \"$4\" = --abort ]; "
                             "then\n"
                             "  echo 'fatal: abort broke' >&2\n"
                             "  exit 128\n"
                             "fi\n");

    auto r = run_wt(box, repo, {"merge", "feature"}, env);
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: git error: feature conflicts with main, and "
                          "wt could not undo the merge: git merge --abort "
                          "failed: abort broke\n"));
    CHECK_THAT(r.err, ContainsSubstring("\nConflicts:\n- a.txt\n"));
    CHECK_THAT(r.err,
               EndsWith("\nmain's worktree may still be in the middle of the "
                        "merge.\nDon't run other wt commands until you have "
                        "inspected it with `git -C " +
                        repo.string() + " status`.\n"));
    CHECK_THAT(r.err, !ContainsSubstring("restored"));
    CHECK(fs::exists(repo / ".git" / "MERGE_HEAD"));
}

TEST_CASE("wt merge still aborts when it cannot list the conflicts") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    make_conflict(box, repo, "feature");
    const std::string before = head_of(box, repo);
    const Env env = git_shim(box,
                             "if [ \"$3\" = diff ] && "
                             "[ \"$5\" = --diff-filter=U ]; then\n"
                             "  echo 'fatal: diff broke' >&2\n"
                             "  exit 128\n"
                             "fi\n");

    auto r = run_wt(box, repo, {"merge", "feature"}, env);
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(
        r.err,
        StartsWith("wt: git error: git merge failed with exit code 1\n"));
    CHECK_THAT(r.err, ContainsSubstring("CONFLICT"));
    CHECK_THAT(r.err, ContainsSubstring("\nwt could not list the conflicting "
                                        "files: diff broke\n"));
    CHECK_THAT(
        r.err,
        EndsWith("\nmain was restored; no merge remains in progress.\n"));
    check_main_restored(box, repo, before);
}

TEST_CASE("wt merge doesn't claim a restore it couldn't verify") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    make_conflict(box, repo, "feature");
    // The abort works, but every rev-parse after it fails.
    const fs::path aborted = box.root() / "aborted";
    const Env env =
        git_shim(box,
                 "if [ \"$3\" = merge ] && [ \"$4\" = --abort ]; then\n"
                 "  \"$REAL_GIT\" \"$@\" || exit\n"
                 "  : > '" +
                     aborted.string() +
                     "'\n"
                     "  exit 0\n"
                     "fi\n"
                     "if [ \"$3\" = rev-parse ] && [ -e '" +
                     aborted.string() +
                     "' ]; then\n"
                     "  echo 'fatal: rev-parse broke' >&2\n"
                     "  exit 128\n"
                     "fi\n");

    auto r = run_wt(box, repo, {"merge", "feature"}, env);
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: git error: feature conflicts with main, and "
                          "wt could not undo the merge: cannot verify the "
                          "abort: rev-parse broke\n"));
    CHECK_THAT(r.err, ContainsSubstring("\nConflicts:\n- a.txt\n"));
    CHECK_THAT(r.err, ContainsSubstring("may still be in the middle of the "
                                        "merge"));
    CHECK_THAT(r.err, !ContainsSubstring("restored"));
}

TEST_CASE("wt merge stops when it cannot tell whether a merge is in progress") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    make_conflict(box, repo, "feature");
    // The merge conflicts, but every rev-parse after it fails.
    const fs::path merged = box.root() / "merged";
    const Env env = git_shim(box,
                             "if [ \"$3\" = merge ]; then\n"
                             "  \"$REAL_GIT\" \"$@\"\n"
                             "  code=$?\n"
                             "  : > '" +
                                 merged.string() +
                                 "'\n"
                                 "  exit $code\n"
                                 "fi\n"
                                 "if [ \"$3\" = rev-parse ] && [ -e '" +
                                 merged.string() +
                                 "' ]; then\n"
                                 "  echo 'fatal: rev-parse broke' >&2\n"
                                 "  exit 128\n"
                                 "fi\n");

    auto r = run_wt(box, repo, {"merge", "feature"}, env);
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: git error: the merge failed, and wt could not "
                          "tell whether it is still in progress: rev-parse "
                          "broke\n"));
    CHECK_THAT(r.err, ContainsSubstring("may still be in the middle of the "
                                        "merge"));
    CHECK_THAT(r.err, !ContainsSubstring("restored"));
    // Unsure, wt left the merge alone rather than abort it.
    CHECK(fs::exists(repo / ".git" / "MERGE_HEAD"));
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
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: git error: git merge failed with exit code "));
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
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: git error: git merge reported success, but "
                          "main doesn't contain feature\n"));
    CHECK_THAT(r.err, ContainsSubstring(
                          "\nThe result could not be verified; "
                          "inspect main's worktree with `git -C " +
                          repo.string() + " status` before retrying.\n"));
    CHECK(head_of(box, repo) == before);
}

TEST_CASE("wt merge fails when Git succeeds but main loses its own commits") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, repo, "m.txt", "m\n");
    const std::string before = head_of(box, repo);
    // main contains the task afterwards, but no longer its own commit.
    const fs::path hook = repo / ".git" / "hooks" / "post-merge";
    write_file(hook, "#!/bin/sh\ngit reset -q --hard refs/heads/feature\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: git error: git merge reported success, "
                                 "but main no longer contains " +
                                 before.substr(0, 7) +
                                 ", where it was before the merge\n"));
    CHECK(head_of(box, repo) == head_of(box, path));
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
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: git error: git merge reported success, but "
                          "main's worktree is dirty\n"));
    // The merge itself happened and is left in place, along with the file.
    CHECK(head_of(box, repo) == head_of(box, path));
    CHECK(read_file(repo / "stray.txt") == "x\n");
}

TEST_CASE("wt merge fails when Git succeeds but main's worktree leaves main") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    // main contains the task, but its worktree is detached at the same commit.
    const fs::path hook = repo / ".git" / "hooks" / "post-merge";
    write_file(hook, "#!/bin/sh\ngit checkout -q --detach\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: git error: git merge reported success, but "
                          "main's worktree no longer has main checked out\n"));
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: cannot fast-forward main to second\n"
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
    CHECK(r.code == 2);
    CHECK_THAT(r.err, StartsWith("wt: refused: 'second' is being rebased in " +
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
    CHECK(r.code == 2);
    CHECK(r.err ==
          "wt: refused: cannot fast-forward main to feature\n"
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: refused: main's worktree at " + repo.string() +
                       " has uncommitted changes; commit or stash them "
                       "before merging into main\n");
    CHECK(head_of(box, repo) == before);
    CHECK_FALSE(fs::exists(repo / "a.txt"));
}

TEST_CASE("wt merge refuses when main is in the middle of an operation") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    // Most of these can leave main checked out and its files clean.
    std::string operation;
    SECTION("a merge") {
        // A merge whose changes are already resolved.
        write_file(repo / ".git" / "MERGE_HEAD", head_of(box, path) + "\n");
        operation = "a merge";
    }
    SECTION("a git am") {
        stop_git_am(box, repo);
        operation = "a git am";
    }
    SECTION("a cherry-pick or revert between steps") {
        // What a multi-commit cherry-pick leaves once a conflicted step has
        // been committed: no CHERRY_PICK_HEAD, only the remaining steps.
        fs::create_directory(repo / ".git" / "sequencer");
        write_file(repo / ".git" / "sequencer" / "todo",
                   "pick " + head_of(box, path) + "\n");
        operation = "a cherry-pick or revert";
    }
    SECTION("a bisect") {
        git(box, repo, {"bisect", "start"});
        operation = "a bisect";
    }
    SECTION("a conflicted merge") {
        // Git won't commit or stash this, so the operation must be named.
        commit_file(box, repo, "a.txt", "main\n");
        stop_conflicted_merge(box, repo, "feature");
        operation = "a merge";
    }
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 2);
    CHECK(r.err == "wt: refused: main's worktree at " + repo.string() +
                       " is in the middle of " + operation +
                       "; finish or abort it first\n");
    CHECK(head_of(box, repo) == before);
}

TEST_CASE("wt merge refuses when main is being rebased") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, repo, "b.txt", "b\n");
    const std::string main_before = head_of(box, repo);
    // Detaches main's worktree, so main looks checked out nowhere.
    const Result rebase = run_in(
        box, repo,
        {find_on_path("git"), "rebase", "-q", "--exec", "false", "HEAD~1"});
    REQUIRE(rebase.code != 0);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: the integration branch 'main' is being "
          "rebased in " +
              repo.string() +
              "; finish or abort the rebase, then merge again\n");
    CHECK(git_output(box, repo, {"rev-parse", "main"}) == main_before);
}

TEST_CASE("wt merge refuses when the task is in the middle of an operation") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    // Each leaves the task's files clean and its branch checked out, but
    // without the work the operation is still adding.
    std::string operation;
    SECTION("a merge") {
        commit_file(box, repo, "b.txt", "b\n");
        git(box, path, {"merge", "-q", "--no-commit", "-s", "ours", "main"});
        REQUIRE(git_output(box, path, {"status", "--porcelain"}).empty());
        operation = "a merge";
    }
    SECTION("a git am") {
        stop_git_am(box, path);
        operation = "a git am";
    }
    SECTION("a conflicted merge") {
        commit_file(box, repo, "a.txt", "main\n");
        stop_conflicted_merge(box, path, "main");
        operation = "a merge";
    }
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: the worktree for 'feature' is in the middle "
          "of " +
              operation + "; finish or abort it, then merge again\n");
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: the worktree for 'feature' has uncommitted changes, "
          "which the merge wouldn't include; commit or stash them first "
          "(`wt status feature` shows them)\n");
    CHECK(head_of(box, repo) == before);
    CHECK(read_file(path / "forgotten.txt") == "not committed\n");
}

TEST_CASE("wt merge lands a task whose directory was deleted") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    fs::remove_all(path);

    // Its commits are all on the branch, so nothing is left behind.
    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 0);
    CHECK(r.out == "✓ Merged feature into main\n");
    CHECK(read_file(repo / "a.txt") == "a\n");
}

TEST_CASE("wt merge refuses when something else is at the task's path") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    fs::remove_all(path);
    write_file(path, "keep me");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: refused: cannot read the status of " + path.string() +
                       ": it is not a directory\n");
    CHECK(head_of(box, repo) == before);
    CHECK(read_file(path) == "keep me");
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: refused: 'feature' is being rebased in "));
    CHECK_THAT(r.err, ContainsSubstring("; finish the rebase (`git rebase "
                                        "--continue`) or abort it (`git rebase "
                                        "--abort`), then merge again\n"));
    CHECK(git_output(box, repo, {"rev-parse", "feature"}) == task_before);
    CHECK(run_in(box, repo,
                 {find_on_path("git"), "merge-base", "--is-ancestor",
                  task_before, "main"})
              .code == 1);
}

TEST_CASE("wt merge refuses a task that git rebase --update-refs will move") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    // A second task stacked on the first, in its own worktree.
    const fs::path top = wt_new(box, repo, "top");
    git(box, top, {"rebase", "-q", "feature"});
    commit_file(box, top, "t.txt", "t\n");
    // --update-refs leaves alone branches checked out elsewhere.
    git(box, repo, {"worktree", "remove", path.string()});
    commit_file(box, repo, "b.txt", "b\n");
    const std::string task_before =
        git_output(box, repo, {"rev-parse", "feature"});

    // Rebasing top stops after rewriting feature's commit, before moving
    // feature, so feature is neither checked out nor named in head-name.
    const Result rebase = run_in(box, top,
                                 {find_on_path("git"), "rebase", "-q",
                                  "--update-refs", "--exec", "false", "main"});
    REQUIRE(rebase.code != 0);
    const fs::path state =
        fs::path(git_output(box, top, {"rev-parse", "--absolute-git-dir"})) /
        "rebase-merge";
    REQUIRE(read_file(state / "head-name") == "refs/heads/top\n");
    REQUIRE_THAT(read_file(state / "update-refs"),
                 StartsWith("refs/heads/feature\n" + task_before + "\n"));

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: refused: 'feature' is being rebased in " +
                       top.string() +
                       "; finish the rebase (`git rebase --continue`) or "
                       "abort it (`git rebase --abort`), then merge again\n");
    CHECK(git_output(box, repo, {"rev-parse", "feature"}) == task_before);
    CHECK(run_in(box, repo,
                 {find_on_path("git"), "merge-base", "--is-ancestor",
                  task_before, "main"})
              .code == 1);

    // Each --continue runs on to the next failing exec; the second finishes
    // the rebase and moves feature, which can then land.
    REQUIRE(
        run_in(box, top, {find_on_path("git"), "rebase", "--continue"}).code !=
        0);
    git(box, top, {"rebase", "--continue"});
    REQUIRE_FALSE(fs::exists(state));
    r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, StartsWith("✓ Merged feature into main\n"));
    CHECK(git_output(box, repo, {"rev-parse", "main"}) ==
          git_output(box, top, {"rev-parse", "HEAD~1"}));
}

TEST_CASE("wt merge refuses when --update-refs state can't be understood") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string before = head_of(box, repo);
    // A truncated entry might be the one naming feature.
    const fs::path state = repo / ".git" / "rebase-merge";
    fs::create_directories(state);
    write_file(state / "update-refs", "refs/heads/other\n");

    const auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 2);
    CHECK(r.err == "wt: refused: cannot understand " +
                       (state / "update-refs").string() + "\n");
    CHECK(head_of(box, repo) == before);
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: cannot merge feature into main\n"
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
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: git error: not a git repository"));
}

TEST_CASE("commands wt prints for pasting quote shell-sensitive paths") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "it's my repo");
    git(box, repo, {"config", "merge.ff", "only"});
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, repo, "b.txt", "b\n");

    auto r = run_wt(box, repo, {"merge", "feature"});
    CHECK(r.code == 2);
    const std::string quoted = "'" + path.parent_path().parent_path().string() +
                               "/.it'\\''s my repo.wt-";
    CHECK_THAT(r.err, ContainsSubstring("  git -C " + quoted));
    CHECK_THAT(r.err, ContainsSubstring("/feature' rebase main\n"));
}
