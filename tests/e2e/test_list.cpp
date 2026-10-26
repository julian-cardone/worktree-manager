// End-to-end tests for `wt list`.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/e2e.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;
using namespace wt::test;

TEST_CASE("wt list outside a repository fails with a clear error") {
    Sandbox box;
    auto r = run_wt(box, box.work(), {"list"});
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: git error: not a git repository"));
}

TEST_CASE("wt explains that Git is too old in any language") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    // How Git before 2.36 rejects `worktree list -z` in a German locale.
    const Env env = git_shim(box,
                             "if [ \"$3\" = worktree ] && [ \"$4\" = list ]; "
                             "then\n"
                             "  echo \"Fehler: unbekannter Schalter 'z'\" >&2\n"
                             "  exit 129\n"
                             "fi\n");

    auto r = run_wt(box, repo, {"list"}, env);
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: error: wt requires Git 2.36 or newer\n");
}

TEST_CASE("wt doesn't blame the Git version when SIGHUP kills git") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    // Killed by SIGHUP, which wt also sees as 129.
    const Env env = git_shim(box,
                             "if [ \"$3\" = worktree ] && [ \"$4\" = list ]; "
                             "then\n"
                             "  kill -HUP $$\n"
                             "fi\n");

    auto r = run_wt(box, repo, {"list"}, env);
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: git error: "));
    CHECK_THAT(r.err, !ContainsSubstring("2.36"));
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

TEST_CASE("wt list shows operations in progress") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path rebasing = wt_new(box, repo, "feature");
    commit_file(box, rebasing, "a.txt", "task\n");
    commit_file(box, repo, "a.txt", "main\n");
    REQUIRE(run_in(box, rebasing, {find_on_path("git"), "rebase", "-q", "main"})
                .code != 0);
    const fs::path bisecting = wt_new(box, repo, "other");
    git(box, bisecting, {"bisect", "start"});
    const fs::path detached = box.work() / "detached";
    git(box, repo,
        {"worktree", "add", "-q", "--detach", detached.string(), "main"});

    auto r = run_wt(box, repo, {"list"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    // A rebase detaches HEAD, but the worktree is still the task's.
    CHECK_THAT(row_for(r.out, rebasing.string()),
               StartsWith("  (rebasing feature)  dirty (rebase)  "));
    CHECK_THAT(row_for(r.out, bisecting.string()),
               StartsWith("  other               clean (bisect)  "));
    CHECK_THAT(
        row_for(r.out, detached.string()),
        StartsWith("  (detached " +
                   git_output(box, repo, {"rev-parse", "--short=7", "main"}) +
                   ")  clean  "));
}

TEST_CASE("wt list doesn't call a worktree idle when it can't tell") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const Env failing =
        git_shim(box,
                 "case \" $* \" in *' --git-path rebase-merge '*) "
                 "echo 'fatal: simulated failure' >&2; exit 128;; esac\n");

    auto r = run_wt(box, repo, {"list"}, failing);
    CHECK(r.code == 0);
    CHECK_THAT(row_for(r.out, repo.string()),
               StartsWith("* main    clean (operation unknown)  "));
    CHECK(r.err == "wt: warning: cannot check whether " + repo.string() +
                       " is in the middle of an operation: simulated "
                       "failure\n");
}

TEST_CASE("wt list doesn't call a path with something else at it missing") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = box.work() / "repo.worktrees" / "replaced";
    git(box, repo, {"worktree", "add", "-q", "-b", "replaced", path.string()});
    fs::remove_all(path);

    SECTION("file") {
        write_file(path, "keep me");
    }
    SECTION("dangling symlink") {
        fs::create_directory_symlink(box.work() / "nowhere", path);
    }

    auto r = run_wt(box, repo, {"list"});
    CHECK(r.code == 0);
    CHECK_THAT(row_for(r.out, path.string()), ContainsSubstring(" unknown "));
    CHECK(r.err == "wt: warning: cannot read the status of " + path.string() +
                       ": it is not a directory\n");
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

TEST_CASE("wt list --json describes every worktree") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path wts = box.work() / "repo.worktrees";
    const fs::path feature = wts / "feature-x";
    // After feature-x: Git lists linked worktrees by name, after the main one.
    const fs::path detached = wts / "other";
    git(box, repo,
        {"worktree", "add", "-q", "-b", "feature/x", feature.string()});
    git(box, repo, {"worktree", "add", "-q", "--detach", detached.string()});
    git(box, repo, {"worktree", "lock", detached.string()});
    write_file(feature / "wip.txt", "wip\n");
    const std::string head = git_output(box, repo, {"rev-parse", "HEAD"});

    auto r = run_wt(box, feature, {"list", "--json"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    CHECK(r.out ==
          R"({"worktrees":[)"
          R"({"path":")" +
              repo.string() + R"(","branch":"main","head":")" + head +
              R"(","status":"clean","operation":null,"current":false,)"
              R"("detached":false,"locked":false},)"
              R"({"path":")" +
              feature.string() + R"(","branch":"feature/x","head":")" + head +
              R"(","status":"dirty","operation":null,"current":true,)"
              R"("detached":false,"locked":false},)"
              R"({"path":")" +
              detached.string() + R"(","branch":null,"head":")" + head +
              R"(","status":"clean","operation":null,"current":false,)"
              R"("detached":true,"locked":true}]})"
              "\n");
}

TEST_CASE("wt list --json prints paths exactly, without ~ or sanitizing") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.home() / "my \"repo\"\\\nx");

    auto r = run_wt(box, repo, {"list", "--json"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out,
               StartsWith(R"({"worktrees":[{"path":")" + box.home().string() +
                          R"(/my \"repo\"\\\u000ax","branch":"main",)"));

    // The table still shortens and sanitizes it.
    r = run_wt(box, repo, {"list"});
    CHECK(lines(r.out).at(1) == "* main    clean   ~/my \"repo\"\\?x");
}

TEST_CASE("wt list --json reports missing and unreadable worktrees") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path wts = box.work() / "repo.worktrees";
    const fs::path gone = wts / "gone";
    const fs::path broken = wts / "unreadable";  // listed after "gone"
    git(box, repo, {"worktree", "add", "-q", "-b", "gone", gone.string()});
    git(box, repo, {"worktree", "add", "-q", "-b", "broken", broken.string()});
    fs::remove_all(gone);
    fs::remove(broken / ".git");
    const std::string head = git_output(box, repo, {"rev-parse", "HEAD"});

    auto r = run_wt(box, repo, {"list", "--json"});
    CHECK(r.code == 0);
    CHECK(r.out ==
          R"({"worktrees":[)"
          R"({"path":")" +
              repo.string() + R"(","branch":"main","head":")" + head +
              R"(","status":"clean","operation":null,"current":true,)"
              R"("detached":false,"locked":false},)"
              R"({"path":")" +
              gone.string() + R"(","branch":"gone","head":")" + head +
              R"(","status":"missing","operation":null,"current":false,)"
              R"("detached":false,"locked":false},)"
              R"({"path":")" +
              broken.string() + R"(","branch":"broken","head":")" + head +
              R"(","status":"unknown","operation":null,"current":false,)"
              R"("detached":false,"locked":false}]})"
              "\n");
    // Warnings go to stderr, never into the JSON.
    CHECK_THAT(r.err, StartsWith("wt: warning: cannot read the status of " +
                                 broken.string()));
}

TEST_CASE("wt list --json has no head for a branch with no commits") {
    Sandbox box;
    const fs::path repo = box.work() / "empty";
    git(box, box.work(), {"init", "-q", "-b", "main", repo.string()});

    auto r = run_wt(box, repo, {"list", "--json"});
    CHECK(r.code == 0);
    CHECK(
        r.out ==
        R"({"worktrees":[{"path":")" + repo.string() +
            R"(","branch":"main","head":null,"status":"clean",)"
            R"("operation":null,"current":true,"detached":false,"locked":false}]})"
            "\n");
}

TEST_CASE("wt list --json handles a bare repository") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path bare = box.work() / "bare.git";
    git(box, box.work(),
        {"clone", "-q", "--bare", repo.string(), bare.string()});

    auto r = run_wt(box, bare, {"list", "--json"});
    CHECK(r.code == 0);
    CHECK(
        r.out ==
        R"({"worktrees":[{"path":")" + bare.string() +
            R"(","branch":null,"head":null,"status":"bare",)"
            R"("operation":null,"current":true,"detached":false,"locked":false}]})"
            "\n");
}

TEST_CASE("wt list --json prints nothing on stdout when it fails") {
    Sandbox box;
    SECTION("outside a repository") {
        auto r = run_wt(box, box.work(), {"list", "--json"});
        CHECK(r.code == 4);
        CHECK(r.out.empty());
        CHECK_THAT(r.err, StartsWith("wt: git error: "));
    }
    SECTION("a branch name JSON can't carry") {
        const fs::path repo = box.work() / "repo";
        git(box, box.work(), {"init", "-q", "-b", "main", repo.string()});
        git(box, repo, {"symbolic-ref", "HEAD", "refs/heads/caf\xff"});
        auto r = run_wt(box, repo, {"list", "--json"});
        CHECK(r.code == 1);
        CHECK(r.out.empty());
        CHECK(r.err ==
              "wt: error: cannot print 'caf\xff' as JSON: it is not valid "
              "UTF-8\n");
    }
}

TEST_CASE("wt list --json reports operations in progress") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path rebasing = wt_new(box, repo, "feature");
    commit_file(box, rebasing, "a.txt", "task\n");
    commit_file(box, repo, "a.txt", "main\n");
    REQUIRE(run_in(box, rebasing, {find_on_path("git"), "rebase", "-q", "main"})
                .code != 0);
    const fs::path bisecting = wt_new(box, repo, "other");
    git(box, bisecting, {"bisect", "start"});

    auto r = run_wt(box, repo, {"list", "--json"});
    CHECK(r.code == 0);
    CHECK(r.err.empty());
    // A rebase detaches HEAD, but the worktree is still the task's.
    CHECK_THAT(r.out,
               ContainsSubstring(
                   R"({"path":")" + rebasing.string() +
                   R"(","branch":"feature","head":")" +
                   git_output(box, rebasing, {"rev-parse", "HEAD"}) +
                   R"(","status":"dirty","operation":"rebase",)"
                   R"("current":false,"detached":true,"locked":false})"));
    CHECK_THAT(r.out,
               ContainsSubstring(
                   R"({"path":")" + bisecting.string() +
                   R"(","branch":"other","head":")" +
                   git_output(box, bisecting, {"rev-parse", "HEAD"}) +
                   R"(","status":"clean","operation":"bisect",)"
                   R"("current":false,"detached":false,"locked":false})"));
}

TEST_CASE("wt list --json says when it can't tell if an operation is running") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const Env failing =
        git_shim(box,
                 "case \" $* \" in *' --git-path rebase-merge '*) "
                 "echo 'fatal: simulated failure' >&2; exit 128;; esac\n");

    auto r = run_wt(box, repo, {"list", "--json"}, failing);
    CHECK(r.code == 0);
    CHECK_THAT(r.out,
               ContainsSubstring(R"("status":"clean","operation":"unknown",)"));
    CHECK_THAT(r.err, StartsWith("wt: warning: cannot check whether "));
}
