// End-to-end tests for `wt merge --cleanup`.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/e2e.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using namespace wt::test;

namespace {

// Runs git and returns its result, for checks that expect it to fail.
Result git_result(const Sandbox& box, const fs::path& dir,
                  std::initializer_list<std::string> args) {
    static const std::string git_path = find_on_path("git");
    std::vector<std::string> argv = {git_path};
    argv.insert(argv.end(), args);
    return run_in(box, dir, std::move(argv));
}

bool branch_exists(const Sandbox& box, const fs::path& repo,
                   const std::string& branch) {
    return git_result(
               box, repo,
               {"rev-parse", "--verify", "--quiet", "refs/heads/" + branch})
               .code == 0;
}

void install_hook(const fs::path& repo, const std::string& name,
                  const std::string& script) {
    const fs::path hook = repo / ".git" / "hooks" / name;
    write_file(hook, "#!/bin/sh\n" + script);
    fs::permissions(hook, fs::perms::owner_all);
}

// Shell commands that add a commit to `branch` without touching any worktree,
// as another process might while wt runs.
std::string add_commit_to(const std::string& branch) {
    const std::string ref = "refs/heads/" + branch;
    return "c=$(git commit-tree -p " + ref + " -m late '" + ref +
           "^{tree}') && git update-ref " + ref + " \"$c\"\n";
}

}  // namespace

TEST_CASE(
    "wt merge --cleanup lands the task, then removes its worktree and "
    "branch") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "claude/fix-login");
    commit_file(box, path, "a.txt", "a\n");
    const std::string task_head = head_of(box, path);
    git(box, repo,
        {"update-ref", "refs/remotes/origin/claude/fix-login", task_head});

    auto r = run_wt(box, repo, {"merge", "claude/fix-login", "--cleanup"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out ==
          "✓ Merged claude/fix-login into main\n"
          "Removed worktree " +
              path.string() +
              "\n"
              "Deleted branch claude/fix-login (was " +
              task_head.substr(0, 7) + ")\n");
    CHECK(head_of(box, repo) == task_head);
    CHECK_FALSE(fs::exists(path));
    CHECK_FALSE(registered(box, repo, path));
    CHECK_FALSE(branch_exists(box, repo, "claude/fix-login"));
    // Remote branches are never touched.
    CHECK(git_output(box, repo,
                     {"rev-parse", "refs/remotes/origin/claude/fix-login"}) ==
          task_head);
    // The task can start over under the same name.
    CHECK(wt_new(box, repo, "claude/fix-login") == path);
}

TEST_CASE("wt merge --cleanup deletes a branch landed with a merge commit") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    commit_file(box, repo, "b.txt", "b\n");

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 0);
    CHECK(git_output(box, repo, {"log", "-1", "--format=%s"}) ==
          "Merge branch 'feature' into main");
    CHECK_FALSE(fs::exists(path));
    CHECK_FALSE(branch_exists(box, repo, "feature"));
}

TEST_CASE(
    "wt merge --cleanup refuses before merging a task checked out in the "
    "main worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"switch", "-q", "-c", "foo"});
    commit_file(box, repo, "a.txt", "a\n");
    const std::string foo = head_of(box, repo);
    // main has to be checked out somewhere for wt merge to land into it.
    const fs::path integration = box.work() / "integration";
    git(box, repo, {"worktree", "add", "-q", integration.string(), "main"});
    const std::string before = head_of(box, integration);

    auto r = run_wt(box, integration, {"merge", "foo", "--cleanup"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: 'foo' is checked out in the main worktree, which wt "
          "never removes\n"
          "Nothing was merged.\n");
    check_main_restored(box, integration, before);
    check_untouched(box, repo, repo, "foo");
    CHECK(head_of(box, repo) == foo);
    CHECK(fs::is_regular_file(repo / "a.txt"));
}

TEST_CASE(
    "wt merge --cleanup refuses before merging when run from the task's "
    "worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, path, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: you are inside the worktree for 'feature'; run wt "
          "merge from somewhere else\n"
          "Nothing was merged.\n");
    check_main_restored(box, repo, before);
    check_untouched(box, repo, path, "feature");
}

TEST_CASE(
    "wt merge --cleanup refuses ignored files unless told to delete "
    "them") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    write_file(repo / ".gitignore", "build/\n");
    git(box, repo, {"add", ".gitignore"});
    git(box, repo, {"commit", "-q", "-m", "ignore"});
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    fs::create_directories(path / "build");
    write_file(path / "build" / "a.o", "o\n");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: cannot remove feature\n"
          "\n"
          "  1 ignored file\n"
          "    build/\n"
          "\n"
          "Ignored files are deleted with the worktree; move any you need, "
          "then rerun with --delete-ignored.\n"
          "Nothing was merged.\n");
    check_main_restored(box, repo, before);
    check_untouched(box, repo, path, "feature");

    SECTION("--delete-ignored lets them go") {
        r = run_wt(box, repo,
                   {"merge", "feature", "--cleanup", "--delete-ignored"});
        CHECK(r.code == 0);
        CHECK(r.err.empty());
        CHECK_FALSE(fs::exists(path));
        CHECK_FALSE(branch_exists(box, repo, "feature"));
    }

    SECTION("--delete-ignored never overrides other changes") {
        write_file(path / "notes.txt", "keep\n");
        r = run_wt(box, repo,
                   {"merge", "feature", "--cleanup", "--delete-ignored"});
        CHECK(r.code == 2);
        CHECK_THAT(r.err, ContainsSubstring("has uncommitted changes"));
        check_main_restored(box, repo, before);
        CHECK(read_file(path / "build" / "a.o") == "o\n");
        CHECK(branch_exists(box, repo, "feature"));
    }
}

TEST_CASE("wt merge accepts --delete-ignored only with --cleanup") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature", "--delete-ignored"});
    CHECK(r.code >= 100);
    CHECK_THAT(r.err, ContainsSubstring("--cleanup"));
    check_main_restored(box, repo, before);
}

TEST_CASE(
    "wt merge --cleanup deletes an already-landed branch without a "
    "worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string task_head = head_of(box, path);
    REQUIRE(run_wt(box, repo, {"merge", "feature"}).code == 0);
    REQUIRE(run_wt(box, repo, {"remove", "feature"}).code == 0);
    commit_file(box, repo, "b.txt", "b\n");
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out ==
          "main already contains feature; nothing to merge\n"
          "Deleted branch feature (was " +
              task_head.substr(0, 7) + ")\n");
    CHECK_FALSE(branch_exists(box, repo, "feature"));
    check_main_restored(box, repo, before);
}

TEST_CASE("wt merge --cleanup refuses a worktree whose directory is gone") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string task_head = head_of(box, path);
    fs::remove_all(path);
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: refused: nothing is at " + path.string()) &&
                   EndsWith("If it's gone for good, `wt remove feature "
                            "--forget-missing` removes only Git's record of "
                            "it and keeps the branch; then rerun `wt merge "
                            "feature --cleanup`.\nNothing was merged.\n"));
    CHECK(head_of(box, repo) == before);
    CHECK(registered(box, repo, path));
    CHECK(branch_exists(box, repo, "feature"));

    // Forgetting it explicitly is what lets the cleanup go ahead.
    REQUIRE(run_wt(box, repo, {"remove", "feature", "--forget-missing"}).code ==
            0);
    r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 0);
    CHECK(r.out == "✓ Merged feature into main\nDeleted branch feature (was " +
                       task_head.substr(0, 7) + ")\n");
    CHECK_FALSE(branch_exists(box, repo, "feature"));
}

TEST_CASE(
    "wt merge --cleanup reports a cleanup that fails after landing, and "
    "a rerun finishes it") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string task_head = head_of(box, path);
    // Something writes into the task's worktree after the checks before the
    // merge have passed.
    install_hook(repo, "post-merge",
                 "echo late > '" + (path / "late.txt").string() + "'\n");

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 2);
    CHECK(r.out == "✓ Merged feature into main\n");
    CHECK(r.err ==
          "wt: refused: cannot remove feature\n"
          "\n"
          "  1 untracked file\n"
          "    late.txt\n"
          "\n"
          "Commit or delete the untracked files, or stash them with "
          "`git stash -u`.\n"
          "\n"
          "feature has landed in main, but the cleanup didn't finish:\n"
          "- its worktree is still there (`wt list` shows it)\n"
          "- its branch feature was kept\n"
          "\n"
          "Once that's dealt with, finish the cleanup with:\n"
          "\n"
          "  wt merge feature --cleanup\n");
    CHECK(head_of(box, repo) == task_head);
    CHECK(read_file(path / "late.txt") == "late\n");
    CHECK(branch_exists(box, repo, "feature"));

    fs::remove(repo / ".git" / "hooks" / "post-merge");
    fs::remove(path / "late.txt");
    r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out ==
          "main already contains feature; nothing to merge\n"
          "Removed worktree " +
              path.string() +
              "\n"
              "Deleted branch feature (was " +
              task_head.substr(0, 7) + ")\n");
    CHECK_FALSE(fs::exists(path));
    CHECK_FALSE(branch_exists(box, repo, "feature"));
}

TEST_CASE(
    "wt merge --cleanup reports what is left, not what it expected to "
    "leave") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");

    SECTION("a task whose worktree was removed earlier") {
        REQUIRE(run_wt(box, repo, {"remove", "feature"}).code == 0);
        install_hook(repo, "post-merge", add_commit_to("feature"));

        auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
        CHECK(r.code == 2);
        CHECK(r.out == "✓ Merged feature into main\n");
        CHECK_THAT(r.err, ContainsSubstring(
                              "feature has landed in main, but the cleanup "
                              "didn't finish:\n"
                              "- it has no worktree\n"
                              "- its branch feature was kept\n"));
    }
    SECTION("a branch deleted before wt could check") {
        auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"},
                        git_fails_after(box, " branch -d "));
        CHECK(r.code == 4);
        CHECK_THAT(r.err, EndsWith("\nfeature has landed in main, but the "
                                   "cleanup didn't finish, and wt could not "
                                   "check what is left: simulated failure\n"
                                   "Check it with `wt list` and `git branch` "
                                   "before retrying.\n"));
        CHECK_THAT(r.err, !ContainsSubstring("was kept"));
        CHECK_FALSE(branch_exists(box, repo, "feature"));
    }
}

TEST_CASE("wt merge --cleanup keeps a branch that moved after landing") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string task_head = head_of(box, path);
    install_hook(repo, "post-merge", add_commit_to("feature"));

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    const std::string moved =
        git_output(box, repo, {"rev-parse", "refs/heads/feature"});
    CHECK(r.code == 2);
    CHECK(r.out ==
          "✓ Merged feature into main\n"
          "Removed worktree " +
              path.string() + "\n");
    CHECK(r.err ==
          "wt: refused: 'feature' now has commits that main doesn't "
          "(it is at " +
              moved.substr(0, 7) +
              "), so wt won't delete it\n"
              "\n"
              "feature has landed in main, but the cleanup didn't "
              "finish:\n"
              "- it has no worktree\n"
              "- its branch feature was kept\n"
              "\n"
              "Once that's dealt with, finish the cleanup with:\n"
              "\n"
              "  wt merge feature --cleanup\n");
    CHECK(git_output(box, repo, {"rev-parse", "refs/heads/feature^"}) ==
          task_head);
    CHECK(head_of(box, repo) == task_head);
}

TEST_CASE(
    "wt merge --cleanup leaves it to git branch -d to catch a branch "
    "moving at the last moment") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    const std::string task_head = head_of(box, path);
    // A git in front of the real one moves the branch just before
    // `git branch -d` runs, after wt's own check that it's contained.
    const fs::path bin = box.root() / "bin";
    fs::create_directories(bin);
    const std::string real_git = find_on_path("git");
    write_file(bin / "git",
               "#!/bin/sh\n"
               "if [ \"$3\" = branch ] && [ \"$4\" = -d ]; then\n"
               "  c=$('" +
                   real_git +
                   "' -C \"$2\" commit-tree -p "
                   "refs/heads/feature -m race 'refs/heads/feature^{tree}') "
                   "|| exit 1\n"
                   "  '" +
                   real_git +
                   "' -C \"$2\" update-ref refs/heads/feature "
                   "\"$c\" || exit 1\n"
                   "fi\n"
                   "exec '" +
                   real_git + "' \"$@\"\n");
    fs::permissions(bin / "git", fs::perms::owner_all);
    const char* path_env = std::getenv("PATH");
    const Env env = {{"PATH", bin.string() + ":" + (path_env ? path_env : "")}};

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"}, env);
    CHECK(r.code == 4);
    CHECK(r.out ==
          "✓ Merged feature into main\n"
          "Removed worktree " +
              path.string() + "\n");
    CHECK_THAT(r.err, StartsWith("wt: git error: git branch -d failed with "
                                 "exit code 1\n"));
    CHECK_THAT(r.err, ContainsSubstring("not fully merged"));
    CHECK_THAT(r.err, ContainsSubstring("- its branch feature was kept\n"));
    CHECK(git_output(box, repo, {"rev-parse", "refs/heads/feature^"}) ==
          task_head);
}

TEST_CASE("wt merge --cleanup removes the branch's config with it") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    git(box, repo, {"branch", "--set-upstream-to=main", "feature"});
    git(box, repo, {"config", "branch.feature.description", "notes"});

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK_FALSE(branch_exists(box, repo, "feature"));
    CHECK(git_result(box, repo,
                     {"config", "--get-regexp", "^branch\\.feature\\."})
              .code == 1);
}

TEST_CASE(
    "wt merge --cleanup refuses before merging when the upstream lacks "
    "the task") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    commit_file(box, path, "a.txt", "a\n");
    git(box, repo, {"branch", "other"});
    git(box, repo, {"branch", "--set-upstream-to=other", "feature"});
    const std::string before = head_of(box, repo);

    auto r = run_wt(box, repo, {"merge", "feature", "--cleanup"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: 'feature' has commits its upstream refs/heads/other "
          "doesn't, so Git wouldn't delete the branch once it lands; update "
          "the upstream or unset it (`git branch --unset-upstream feature`), "
          "then rerun\n"
          "Nothing was merged.\n");
    check_main_restored(box, repo, before);
    check_untouched(box, repo, path, "feature");
}
