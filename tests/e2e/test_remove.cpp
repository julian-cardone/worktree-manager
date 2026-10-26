// End-to-end tests for `wt remove`.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/e2e.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace wt::test;

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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: cannot remove feature-x\n"
          "\n"
          "  2 modified files\n"
          "  1 untracked file\n"
          "    new.txt\n"
          "\n"
          "Commit or stash these changes (`git stash -u` includes untracked "
          "files).\nNothing was removed.\n");
    check_untouched(box, repo, path, "feature-x");
    CHECK(read_file(path / "a.txt") == "changed\n");
    CHECK(read_file(path / "new.txt") == "new\n");
}

TEST_CASE("wt remove refuses each kind of uncommitted change") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    const std::string untracked_advice =
        "Commit or delete the untracked files, or stash them with "
        "`git stash -u`.\n";
    const std::string conflict_advice =
        "Resolve the conflicts first, or abort the operation that caused "
        "them.\n";
    // Leaves README conflicted in the task's worktree.
    const auto conflict = [&] {
        commit_file(box, path, "README", "feature\n");
        commit_file(box, repo, "README", "main\n");
        REQUIRE(run_in(box, path, {find_on_path("git"), "merge", "-q", "main"})
                    .code != 0);
    };
    std::string expected;

    SECTION("staged") {
        write_file(path / "staged.txt", "staged\n");
        git(box, path, {"add", "staged.txt"});
        // Staged and then deleted from disk: only the index has it.
        fs::remove(path / "staged.txt");
        expected =
            "  1 staged file\n  1 modified file\n\n"
            "Commit or stash these changes.\n";
    }
    SECTION("unstaged deletion") {
        fs::remove(path / "README");
        fs::create_directories(path / "dir");
        write_file(path / "dir" / "README", "x\n");
        expected =
            "  1 modified file\n  1 untracked file\n    dir/\n\n"
            "Commit or stash these changes (`git stash -u` includes untracked "
            "files).\n";
    }
    SECTION("untracked directory") {
        fs::create_directories(path / "notes" / "deep");
        write_file(path / "notes" / "deep" / "idea.md", "idea\n");
        expected = "  1 untracked file\n    notes/\n\n" + untracked_advice;
    }
    SECTION("untracked file hidden by Git config") {
        git(box, repo, {"config", "status.showUntrackedFiles", "no"});
        write_file(path / "new.txt", "new\n");
        expected = "  1 untracked file\n    new.txt\n\n" + untracked_advice;
    }
    SECTION("merge conflict") {
        conflict();
        expected = "  1 conflicted file\n\n" + conflict_advice;
    }
    SECTION("merge conflict and an untracked file") {
        conflict();
        write_file(path / "new.txt", "new\n");
        expected =
            "  1 untracked file\n    new.txt\n  1 conflicted file\n\n" +
            conflict_advice +
            "Then commit or delete the untracked files, or stash them with "
            "`git stash -u`.\n";
    }
    SECTION("merge conflict with staged and untracked files") {
        // The merge stages main's other.txt alongside the conflict.
        commit_file(box, repo, "other.txt", "other\n");
        conflict();
        write_file(path / "new.txt", "new\n");
        expected =
            "  1 staged file\n  1 untracked file\n    new.txt\n"
            "  1 conflicted file\n\n" +
            conflict_advice +
            "Then commit or stash the other changes (`git stash -u` includes "
            "untracked files).\n";
    }

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: refused: cannot remove feature\n\n" + expected +
                       "Nothing was removed.\n");
    CHECK(registered(box, repo, path));
    CHECK(fs::is_directory(path));
}

TEST_CASE("wt remove cuts a long list of untracked files short") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    for (int i = 0; i < 12; ++i) {
        write_file(path / ("n" + std::to_string(i) + ".txt"), "new\n");
    }

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: cannot remove feature\n"
          "\n"
          "  12 untracked files\n"
          "    n0.txt\n"
          "    n1.txt\n"
          "    n10.txt\n"
          "    n11.txt\n"
          "    n2.txt\n"
          "    n3.txt\n"
          "    n4.txt\n"
          "    n5.txt\n"
          "    n6.txt\n"
          "    n7.txt\n"
          "    ... and 2 more\n"
          "\n"
          "Commit or delete the untracked files, or stash them with "
          "`git stash -u`.\nNothing was removed.\n");
    check_untouched(box, repo, path, "feature");
    CHECK(read_file(path / "n11.txt") == "new\n");
}

TEST_CASE("wt remove refuses edits to files Git doesn't check for changes") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    commit_file(box, repo, "local.cfg", "shared\n");
    fs::create_symlink("README", repo / "link");
    git(box, repo, {"add", "link"});
    git(box, repo, {"commit", "-q", "-m", "link"});
    const fs::path path = wt_new(box, repo, "feature");
    const std::string hidden_advice =
        "Git is set not to check the files it doesn't show (assume-unchanged "
        "or skip-worktree), but would delete them with the worktree; copy "
        "them somewhere safe, or clear the flag with `git update-index "
        "--no-assume-unchanged --no-skip-worktree -- <file>` and commit or "
        "stash them.\n";
    const std::string one_file =
        "  1 modified file that Git doesn't show\n    local.cfg\n";
    std::string listed;
    std::string advice = hidden_advice;

    SECTION("assume-unchanged") {
        git(box, path, {"update-index", "--assume-unchanged", "local.cfg"});
        write_file(path / "local.cfg", "mine\n");
        listed = one_file;
    }
    SECTION("skip-worktree") {
        git(box, path, {"update-index", "--skip-worktree", "local.cfg"});
        write_file(path / "local.cfg", "mine\n");
        listed = one_file;
    }
    SECTION("both flags, alongside an untracked file") {
        git(box, path,
            {"update-index", "--assume-unchanged", "--skip-worktree",
             "local.cfg", "README"});
        write_file(path / "local.cfg", "mine\n");
        write_file(path / "README", "mine too\n");
        write_file(path / "new.txt", "new\n");
        listed =
            "  1 untracked file\n    new.txt\n"
            "  2 modified files that Git doesn't show\n    README\n"
            "    local.cfg\n";
        advice =
            "Commit or delete the untracked files, or stash them with "
            "`git stash -u`.\n" +
            hidden_advice;
    }
    SECTION("a directory where Git has the file") {
        git(box, path, {"update-index", "--skip-worktree", "local.cfg"});
        fs::remove(path / "local.cfg");
        fs::create_directories(path / "local.cfg");
        write_file(path / "local.cfg" / "x", "x\n");
        listed = one_file;
    }
    SECTION("a retargeted symlink") {
        git(box, path, {"update-index", "--skip-worktree", "link"});
        fs::remove(path / "link");
        fs::create_symlink("local.cfg", path / "link");
        listed = "  1 modified file that Git doesn't show\n    link\n";
    }
    // Git's status, and so its own check before removing, misses every one.
    REQUIRE(
        git_output(box, path, {"status", "--porcelain", "--untracked-files=no"})
            .empty());

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: refused: cannot remove feature\n\n" + listed + "\n" +
                       advice + "Nothing was removed.\n");
    CHECK(registered(box, repo, path));
    CHECK(fs::exists(path / "local.cfg"));
}

TEST_CASE(
    "wt remove allows files Git doesn't check when they are unchanged or "
    "absent") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    // Line ending conversion makes crlf.txt's bytes on disk differ from the
    // index, while what Git would add is the same.
    write_file(repo / ".gitattributes", "crlf.txt text eol=crlf\n");
    write_file(repo / "crlf.txt", "line\n");
    commit_file(box, repo, "local.cfg", "shared\n");
    commit_file(box, repo, "sparse.txt", "elsewhere\n");
    fs::create_symlink("README", repo / "link");
    git(box, repo, {"add", ".gitattributes", "crlf.txt", "link"});
    git(box, repo, {"commit", "-q", "-m", "more"});
    const fs::path path = wt_new(box, repo, "feature");
    REQUIRE(read_file(path / "crlf.txt") == "line\r\n");

    git(box, path,
        {"update-index", "--assume-unchanged", "local.cfg", "crlf.txt"});
    git(box, path, {"update-index", "--skip-worktree", "sparse.txt", "link"});
    // A sparse checkout leaves skip-worktree files out of the worktree.
    fs::remove(path / "sparse.txt");

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK_FALSE(fs::exists(path));
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: cannot remove feature\n"
          "\n"
          "  2 ignored files\n"
          "    build/\n"
          "    secret.env\n"
          "\n"
          "Ignored files are deleted with the worktree; move any you need, "
          "then rerun with --delete-ignored.\nNothing was removed.\n");
    CHECK(read_file(path / "secret.env") == "token\n");
    CHECK(registered(box, repo, path));

    SECTION("changes are reported alongside ignored files") {
        write_file(path / "new.txt", "new\n");
        r = run_wt(box, repo, {"remove", "feature"});
        CHECK(r.code == 2);
        CHECK(
            r.err ==
            "wt: refused: cannot remove feature\n"
            "\n"
            "  1 untracked file\n"
            "    new.txt\n"
            "  2 ignored files\n"
            "    build/\n"
            "    secret.env\n"
            "\n"
            "Commit or delete the untracked files, or stash them with "
            "`git stash -u`.\n"
            "Ignored files are deleted with the worktree; move any you "
            "need, then rerun with --delete-ignored.\nNothing was removed.\n");

        // --delete-ignored never lets uncommitted changes through.
        r = run_wt(box, repo, {"remove", "--delete-ignored", "feature"});
        CHECK(r.code == 2);
        CHECK(r.err ==
              "wt: refused: cannot remove feature\n"
              "\n"
              "  1 untracked file\n"
              "    new.txt\n"
              "\n"
              "Commit or delete the untracked files, or stash them with "
              "`git stash -u`.\nNothing was removed.\n");
        CHECK(read_file(path / "new.txt") == "new\n");
        CHECK(read_file(path / "secret.env") == "token\n");
    }
    SECTION("a long list of ignored files is cut short") {
        for (int i = 0; i < 10; ++i) {
            write_file(path / ("k" + std::to_string(i) + ".env"), "key\n");
        }
        r = run_wt(box, repo, {"remove", "feature"});
        CHECK(r.code == 2);
        CHECK(
            r.err ==
            "wt: refused: cannot remove feature\n"
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
            "need, then rerun with --delete-ignored.\nNothing was removed.\n");
    }
    SECTION("--delete-ignored removes them") {
        r = run_wt(box, repo, {"remove", "feature", "--delete-ignored"});
        CHECK(r.code == 0);
        CHECK(r.err.empty());
        CHECK_FALSE(fs::exists(path));
        CHECK_FALSE(registered(box, repo, path));
    }
}

TEST_CASE("wt remove refuses to forget a worktree whose directory is gone") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "gone");
    // It may only have been moved; Git's record is what lets it be repaired.
    const fs::path moved = box.work() / "moved";
    fs::rename(path, moved);

    auto r = run_wt(box, repo, {"remove", "gone"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: nothing is at " + path.string() +
              ", where Git has the worktree for 'gone' registered\n\n"
              "It may have been moved, or be on a drive that isn't mounted.\n"
              "If it was moved, `git worktree repair <new-path>` reconnects "
              "it.\n"
              "If it's gone for good, `wt remove gone --forget-missing` "
              "removes only Git's record of it and keeps the branch.\nNothing "
              "was removed.\n");
    CHECK(registered(box, repo, path));

    git(box, repo, {"worktree", "repair", moved.string()});
    CHECK(git_output(box, moved, {"branch", "--show-current"}) == "gone");
}

TEST_CASE(
    "wt remove --forget-missing forgets a worktree whose directory is "
    "gone") {
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

    auto r = run_wt(box, repo, {"remove", "gone", "--forget-missing"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out == "Worktree " + path.string() +
                       " was already gone; removed Git's record of it\n"
                       "Kept branch gone\n");
    CHECK_FALSE(registered(box, repo, path));
    CHECK(git_output(box, repo, {"log", "-1", "--format=%s", "gone"}) ==
          "change a.txt");
}

TEST_CASE("wt remove --forget-missing only applies when nothing is there") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    const fs::path moved = box.work() / "moved";
    std::string err;

    SECTION("the worktree is there") {
        err = "wt: refused: " + path.string() +
              " is there, so --forget-missing doesn't apply\n"
              "Nothing was removed.\n";
    }
    SECTION("a file is there") {
        fs::rename(path, moved);
        write_file(path, "keep me");
        err = "wt: refused: " + path.string() +
              " is not a directory, so wt won't remove it\n"
              "Nothing was removed.\n";
    }
    SECTION("a dangling symlink is there") {
        fs::rename(path, moved);
        fs::create_directory_symlink(box.work() / "nowhere", path);
        err = "wt: refused: " + path.string() +
              " is not a directory, so wt won't remove it\n"
              "Nothing was removed.\n";
    }

    auto r = run_wt(box, repo, {"remove", "feature", "--forget-missing"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == err);
    CHECK(registered(box, repo, path));
    CHECK(fs::exists(fs::symlink_status(path)));
}

TEST_CASE("wt remove --forget-missing keeps a locked worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "offline");
    git(box, repo, {"worktree", "lock", path.string()});
    fs::remove_all(path);

    auto r = run_wt(box, repo, {"remove", "offline", "--forget-missing"});
    CHECK(r.code == 2);
    CHECK_THAT(r.err,
               StartsWith("wt: refused: the worktree for 'offline' is locked"));
    CHECK(registered(box, repo, path));
}

TEST_CASE("wt remove refuses the main worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path linked = wt_new(box, repo, "feature");

    // The refusal must follow the worktree, not the name "main".
    std::string branch;
    SECTION("on main") {
        branch = "main";
    }
    SECTION("on another branch") {
        branch = "foo";
        git(box, repo, {"switch", "-q", "-c", "foo"});
    }

    for (const fs::path& from : {repo, linked}) {
        INFO("running from " << from);
        auto r = run_wt(box, from, {"remove", branch});
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == "wt: refused: '" + branch +
                           "' is checked out in the main worktree, which wt "
                           "never removes\nNothing was removed.\n");
    }
    check_untouched(box, repo, repo, branch);
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
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err == "wt: refused: the worktree for '" + std::string(task) +
                           "' at " + path.string() +
                           " wasn't created by wt, so wt won't remove "
                           "it\nNothing was removed.\n");
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: refused: " + path.parent_path().string() +
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
        CHECK(r.code == 2);
        CHECK(r.out.empty());
        CHECK(r.err ==
              "wt: refused: you are inside the worktree for 'feature'; run wt "
              "remove from somewhere else\nNothing was removed.\n");
        check_untouched(box, repo, path, "feature");
    }

    // Any other worktree of the repository works.
    const fs::path other = wt_new(box, repo, "other");
    auto r = run_wt(box, other, {"remove", "feature"});
    CHECK(r.code == 0);
    CHECK_FALSE(fs::exists(path));
}

// On macOS, Git precomposes the Unicode in the path wt gives it, so the
// worktree it lists can differ in bytes from the folder on disk.
TEST_CASE("wt remove refuses a locked worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    git(box, repo,
        {"worktree", "lock", "--reason", "on a USB drive", path.string()});

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(
        r.err ==
        "wt: refused: the worktree for 'feature' is locked; if it's no longer "
        "needed, unlock it with `git worktree unlock " +
            path.string() + "`\nNothing was removed.\n");
    check_untouched(box, repo, path, "feature");
}

TEST_CASE("wt remove refuses a worktree in the middle of an operation") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    std::string operation;
    SECTION("a merge") {
        commit_file(box, repo, "a.txt", "a\n");
        // "ours" keeps the files as they are, so only MERGE_HEAD shows it.
        git(box, path, {"merge", "-q", "--no-commit", "-s", "ours", "main"});
        REQUIRE(git_output(box, path, {"status", "--porcelain"}).empty());
        operation = "a merge";
    }
    SECTION("a git am") {
        stop_git_am(box, path);
        operation = "a git am";
    }

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: refused: the worktree for 'feature' is in the middle "
          "of " +
              operation + "; finish or abort it first\nNothing was removed.\n");
    check_untouched(box, repo, path, "feature");
}

TEST_CASE(
    "wt remove sees an operation in progress under a path with a newline") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "new\nline" / "repo");
    const fs::path path = wt_new(box, repo, "feature");
    // A bisect leaves the branch checked out, so only its state file shows it.
    git(box, path, {"bisect", "start"});

    auto r = run_wt(box, repo, {"remove", "feature"});
    CHECK(r.code == 2);
    CHECK(r.err ==
          "wt: refused: the worktree for 'feature' is in the middle of a "
          "bisect; finish or abort it first\nNothing was removed.\n");
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: refused: " + path.string() +
                       " is not a directory, so wt won't remove it\nNothing "
                       "was removed.\n");
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: refused: cannot read the status of " + path.string() +
                       ": its .git file is missing; not removing it\nNothing "
                       "was removed.\n");
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
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err,
               StartsWith("wt: git error: git worktree remove failed with "
                          "exit code "));
    CHECK_THAT(r.err,
               ContainsSubstring("wt: " + path.string() + " is still there\n"));
    CHECK(read_file(path / "locked" / "data") == "data\n");
}

TEST_CASE("wt remove doesn't claim nothing was removed when it can't check") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");

    auto r = run_wt(box, repo, {"remove", "feature"},
                    git_fails_after(box, " worktree remove "));
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: git error: git worktree remove reported success, but wt could "
          "not check the result: simulated failure (" +
              path.string() +
              ")\n"
              "wt: the result could not be verified; inspect it with `wt "
              "list` before retrying\n");
    CHECK_FALSE(registered(box, repo, path));
    CHECK_FALSE(fs::exists(path));
}

TEST_CASE("wt remove fails for a task without a worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"branch", "parked"});

    auto r = run_wt(box, repo, {"remove", "nope"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: no worktree for task 'nope'; `wt list` shows them "
          "all\nNothing was removed.\n");

    r = run_wt(box, repo, {"remove", "parked"});
    CHECK(r.code == 1);
    CHECK(r.err ==
          "wt: error: branch 'parked' isn't checked out in any "
          "worktree\nNothing was removed.\n");
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
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: git error: not a git repository"));
}

TEST_CASE("wt remove requires a task name") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    auto r = run_wt(box, repo, {"remove"});
    CHECK(r.code != 0);
    CHECK_THAT(r.err, ContainsSubstring("task"));
}
