// End-to-end tests for `wt new`.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/e2e.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using Catch::Matchers::StartsWith;
using namespace wt::test;

namespace {

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
    CHECK(r.err == "wt: created branch feature-x from main at " +
                       git_output(box, repo, {"rev-parse", "--short", "main"}) +
                       " initial\n");
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
    CHECK_THAT(r.err,
               StartsWith("wt: created branch feature from develop at "));
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
    CHECK_THAT(r.err, StartsWith("wt: created branch feature from trunk at "));
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
    CHECK_THAT(r.err, StartsWith("wt: created branch feature from master at "));
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
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: git error: not a git repository"));
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

    const std::string tip = git_output(box, repo, {"rev-parse", "feature"});

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: branch 'feature' already exists; wt new only "
          "creates new branches, so choose another task name\nNo branch or "
          "worktree was created.\n");
    CHECK(entries(box.work()) == std::vector<fs::path>{repo});
    CHECK(git_output(box, repo, {"rev-parse", "feature"}) == tip);
}

TEST_CASE("wt new says where an existing branch is checked out") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path path = wt_new(box, repo, "feature");

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: branch 'feature' already exists and is "
          "checked out at " +
              path.string() +
              "; wt new only creates new branches, so choose "
              "another task name\nNo branch or worktree was created.\n");
}

TEST_CASE("wt new refuses a branch Git can't create next to an existing one") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    std::string task;
    std::string existing;

    SECTION("existing branch is a directory of the task") {
        existing = "payments";
        task = "payments/api";
    }
    SECTION("task is a directory of an existing branch") {
        existing = "payments/api/v2";
        task = "payments";
    }
    git(box, repo, {"branch", existing});

    auto r = run_wt(box, repo, {"new", task});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err == "wt: error: cannot create branch '" + task +
                       "' because branch '" + existing +
                       "' exists, and Git can't have both; choose another "
                       "task name\nNo branch or worktree was created.\n");
    CHECK(entries(box.work()) == std::vector<fs::path>{repo});
}

TEST_CASE("wt new starts from the integration branch's exact commit") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    // Starting from the commit, not the branch name, also keeps Git from
    // making the task track the integration branch.
    git(box, repo, {"config", "branch.autoSetupMerge", "always"});

    const fs::path path = wt_new(box, repo, "feature");
    CHECK(git_output(box, path, {"rev-parse", "HEAD"}) ==
          git_output(box, repo, {"rev-parse", "main"}));
    CHECK(git_output(
              box, repo,
              {"for-each-ref", "--format=%(upstream)", "refs/heads/feature"})
              .empty());
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
    CHECK(r.code == 2);
    CHECK(r.err ==
          "wt: refused: cannot create the worktree: " + target.string() +
              " already exists; wt never reuses or overwrites it, so "
              "move it away or choose another task name\nNo branch or worktree "
              "was created.\n");
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
    CHECK(r.code == 2);
    CHECK(r.err == "wt: refused: " + first.string() +
                       " is already the worktree for branch a/b\nNo branch or "
                       "worktree was created.\n");
}

// Whether `dir` is on a case-insensitive filesystem, like macOS's default.
bool case_insensitive(const fs::path& dir) {
    const fs::path probe = dir / "case-probe";
    write_file(probe, "");
    const bool result = fs::exists(dir / "CASE-PROBE");
    fs::remove(probe);
    return result;
}

TEST_CASE("wt new names the branch a task differs from only in case") {
    Sandbox box;
    if (!case_insensitive(box.work())) {
        SKIP("needs a case-insensitive filesystem");
    }
    const fs::path repo = make_repo(box, box.work() / "repo");
    wt_new(box, repo, "fix");

    auto r = run_wt(box, repo, {"new", "Fix"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: task 'Fix' conflicts with existing branch 'fix' (names "
          "differ only in case); choose another task name\nNo branch or "
          "worktree was created.\n");
    CHECK(git_output(box, repo, {"branch", "--format=%(refname:short)"}) ==
          "fix\nmain");
}

TEST_CASE("wt new refuses a packed branch that differs only in case") {
    Sandbox box;
    if (!case_insensitive(box.work())) {
        SKIP("needs a case-insensitive filesystem");
    }
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path container = wt_new(box, repo, "fix").parent_path();
    REQUIRE(run_wt(box, repo, {"remove", "fix"}).code == 0);
    git(box, repo, {"pack-refs", "--all"});

    auto r = run_wt(box, repo, {"new", "Fix"});
    CHECK(r.code == 1);
    CHECK(r.out.empty());
    CHECK(r.err ==
          "wt: error: task 'Fix' conflicts with existing branch 'fix' (names "
          "differ only in case); choose another task name\nNo branch or "
          "worktree was created.\n");
    CHECK(
        git_output(box, repo, {"show-ref", "--heads"}).find("refs/heads/Fix") ==
        std::string::npos);
    CHECK(git_output(box, repo, {"branch", "--format=%(refname:short)"}) ==
          "fix\nmain");
    CHECK_FALSE(fs::exists(container / "fix"));
}

TEST_CASE("wt new allows case variants when Git says case matters") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"branch", "fix"});
    // Packed, so a case-insensitive filesystem can hold both refs too.
    git(box, repo, {"pack-refs", "--all"});
    git(box, repo, {"config", "core.ignorecase", "false"});

    wt_new(box, repo, "Fix");
    CHECK(git_output(box, repo, {"branch", "--format=%(refname:short)"}) ==
          "Fix\nfix\nmain");
}

TEST_CASE("wt new refuses a branch nested under a case variant") {
    Sandbox box;
    struct Case {
        std::string existing;
        std::string task;
        bool pack;
    };
    for (const Case& c :
         {Case{"Fix", "fix/login", true}, Case{"Fix", "fix/login", false},
          Case{"fix/login", "Fix", true}, Case{"fix/login", "Fix", false}}) {
        INFO("existing: " << c.existing << ", task: " << c.task
                          << ", packed: " << c.pack);
        const fs::path parent = box.work() / (c.task + (c.pack ? "-p" : "-l"));
        const fs::path repo = make_repo(box, parent / "repo");
        git(box, repo, {"config", "core.ignorecase", "true"});
        // Without reflogs, Git itself doesn't catch a packed clash.
        git(box, repo, {"config", "core.logAllRefUpdates", "false"});
        git(box, repo, {"branch", c.existing});
        if (c.pack) {
            git(box, repo, {"pack-refs", "--all"});
        }

        auto r = run_wt(box, repo, {"new", c.task});
        CHECK(r.code == 1);
        CHECK(r.out.empty());
        CHECK(r.err == "wt: error: cannot create branch '" + c.task +
                           "' because branch '" + c.existing +
                           "' exists, and Git can't have both when case is "
                           "ignored; choose another task name\nNo branch or "
                           "worktree was created.\n");
        CHECK(git_output(box, repo,
                         {"for-each-ref", "--format=%(refname:strip=2)",
                          "refs/heads/"}) == c.existing + "\nmain");
        CHECK(entries(parent) == std::vector<fs::path>{repo});
    }
}

TEST_CASE("wt new leaves nested case variants to Git when case matters") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    git(box, repo, {"config", "core.ignorecase", "false"});
    // Packed and without reflogs, so a case-insensitive filesystem can hold
    // both refs too.
    git(box, repo, {"config", "core.logAllRefUpdates", "false"});

    std::string existing;
    std::string task;
    SECTION("existing branch above the task") {
        existing = "Fix";
        task = "fix/login";
    }
    SECTION("existing branch below the task") {
        existing = "fix/login";
        task = "Fix";
    }
    git(box, repo, {"branch", existing});
    git(box, repo, {"pack-refs", "--all"});

    wt_new(box, repo, task);
    CHECK(git_output(box, repo,
                     {"for-each-ref", "--format=%(refname:strip=2)",
                      "refs/heads/"}) == "Fix\nfix/login\nmain");
}

TEST_CASE("wt new names the folder a task differs from only in case") {
    Sandbox box;
    if (!case_insensitive(box.work())) {
        SKIP("needs a case-insensitive filesystem");
    }
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path first = wt_new(box, repo, "a/fix");

    SECTION("another task's worktree") {
        auto r = run_wt(box, repo, {"new", "a-Fix"});
        CHECK(r.code == 2);
        CHECK(r.err == "wt: refused: " + first.string() +
                           " is already the worktree for branch a/fix (folder "
                           "names differ only in case)\nNo branch or worktree "
                           "was created.\n");
    }
    SECTION("an unrelated directory") {
        const fs::path existing = first.parent_path() / "feature";
        fs::create_directory(existing);
        auto r = run_wt(box, repo, {"new", "Feature"});
        CHECK(r.code == 2);
        CHECK(r.err ==
              "wt: refused: cannot create the worktree: " + existing.string() +
                  " already exists (folder names differ only in "
                  "case); wt never reuses or overwrites it, so move "
                  "it away or choose another task name\nNo branch or worktree "
                  "was created.\n");
    }
    CHECK(git_output(box, repo, {"branch", "--format=%(refname:short)"}) ==
          "a/fix\nmain");
}

TEST_CASE("wt new refuses a path Git still has registered") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path container = wt_new(box, repo, "first").parent_path();
    const fs::path target = container / "feature";
    git(box, repo, {"worktree", "add", "-q", "--detach", target.string()});
    fs::remove_all(target);

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 2);
    CHECK_THAT(r.err, ContainsSubstring("already has a worktree registered") &&
                          EndsWith("`git worktree prune` forgets it\nNo branch "
                                   "or worktree was created.\n"));
    CHECK_FALSE(fs::exists(target));
}

TEST_CASE("wt new names the task to remove when its path is registered") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    // Both tasks map to the same folder, so the second finds the first's.
    const fs::path target = wt_new(box, repo, "a/b");
    fs::remove_all(target);

    auto r = run_wt(box, repo, {"new", "a-b"});
    CHECK(r.code == 2);
    CHECK_THAT(r.err, EndsWith("although its directory is gone; "
                               "`wt remove a/b --forget-missing` forgets "
                               "it\nNo branch or worktree was created.\n"));
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
    CHECK(r.code == 2);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: refused: " + container.string()));
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
    // Before the hook, which would also fire for expected_container's probe.
    const fs::path target = expected_container(box, repo) / "feature";
    const fs::path hook = repo / ".git" / "hooks" / "post-checkout";
    write_file(hook, "#!/bin/sh\necho hook broke >&2\nexit 3\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(
        r.err,
        StartsWith("wt: git error: git worktree add failed with exit code 3"));
    CHECK_THAT(r.err, ContainsSubstring("  hook broke\n"));
    CHECK_THAT(
        r.err,
        EndsWith("wt: wt new did not complete; current state:\n"
                 "  branch feature: exists, at the commit wt started it from\n"
                 "  " +
                 target.string() +
                 ": a worktree with branch feature checked out\n"
                 "wt: no cleanup was attempted; if you don't need them, "
                 "remove the worktree with `wt remove feature`, then delete "
                 "the branch with `git branch -d feature`\n"));

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
    CHECK(r.code == 4);
    // Nothing for `cd "$(wt new feature)"` to follow.
    CHECK(r.out.empty());
    CHECK(r.err == "wt: git error: git worktree add reported success, but " +
                       target.string() +
                       " has branch other checked out, not feature\n"
                       "wt: wt new did not complete; current state:\n"
                       "  branch feature: exists, at the commit wt started "
                       "it from\n"
                       "  " +
                       target.string() +
                       ": a worktree with branch other checked out\n"
                       "wt: no cleanup was attempted; wt can't tell how all "
                       "of this came about, so check it with `wt list` and "
                       "`git branch` before removing anything\n");
    // Left as the hook made it, for the user to inspect.
    CHECK(git_output(box, target, {"branch", "--show-current"}) == "other");
}

TEST_CASE("wt new doesn't claim nothing was created when it can't check") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path target = expected_container(box, repo) / "feature";

    auto r = run_wt(box, repo, {"new", "feature"},
                    git_fails_after(box, " worktree add "));
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, StartsWith("wt: git error: git worktree add reported "
                                 "success, but wt could not check the "
                                 "result: simulated failure\n"));
    CHECK_THAT(r.err, ContainsSubstring("no cleanup was attempted; check the "
                                        "branch and worktree with `wt list` "
                                        "and `git branch`\n"));
    CHECK_THAT(r.err, !ContainsSubstring("No branch or worktree was created"));
    CHECK(registered(box, repo, target));
    CHECK(git_output(box, target, {"branch", "--show-current"}) == "feature");
}

TEST_CASE("wt new fails when Git succeeds but the branch has moved") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path target = expected_container(box, repo) / "feature";
    const std::string start = git_output(box, repo, {"rev-parse", "main"});
    const fs::path hook = repo / ".git" / "hooks" / "post-checkout";
    write_file(hook,
               "#!/bin/sh\n[ \"$(git branch --show-current)\" = feature ] || "
               "exit 0\ngit commit -q --allow-empty -m moved\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    const std::string moved = git_output(box, repo, {"rev-parse", "feature"});
    CHECK(r.err ==
          "wt: git error: git worktree add reported success, but "
          "branch feature is at " +
              moved + ", not at " + start +
              "\nwt: wt new did not complete; current state:\n"
              "  branch feature: exists, but is at " +
              moved + ", not at " + start + "\n  " + target.string() +
              ": a worktree with branch feature checked out\n"
              "wt: no cleanup was attempted; wt can't tell how all of this "
              "came about, so check it with `wt list` and `git branch` "
              "before removing anything\n");
}

TEST_CASE("wt new never offers to clean up what it didn't create") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path target = expected_container(box, repo) / "feature";
    // Stands in for another process taking the path after wt checked it:
    // Git creates the branch, then finds the directory in its way.
    const fs::path hook = repo / ".git" / "hooks" / "reference-transaction";
    write_file(hook,
               "#!/bin/sh\n[ \"$1\" = committed ] || exit 0\n"
               "mkdir -p \"$TARGET\" && echo theirs > \"$TARGET/file\"\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r =
        run_wt(box, repo, {"new", "feature"}, {{"TARGET", target.string()}});
    CHECK(r.code == 4);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, ContainsSubstring("already exists\n"));
    CHECK_THAT(
        r.err,
        EndsWith("wt: wt new did not complete; current state:\n"
                 "  branch feature: exists, at the commit wt started it from\n"
                 "  " +
                 target.string() +
                 ": exists, but Git doesn't list it as a worktree\n"
                 "wt: no cleanup was attempted; wt can't tell how all of this "
                 "came about, so check it with `wt list` and `git branch` "
                 "before removing anything\n"));
    CHECK(read_file(target / "file") == "theirs\n");
}

TEST_CASE("wt new says when a failure left nothing behind") {
    Sandbox box;
    const fs::path repo = make_repo(box, box.work() / "repo");
    const fs::path target = expected_container(box, repo) / "feature";
    // Rejecting the transaction stops Git before it creates the branch.
    const fs::path hook = repo / ".git" / "hooks" / "reference-transaction";
    write_file(hook, "#!/bin/sh\n[ \"$1\" = prepared ] && exit 1\nexit 0\n");
    fs::permissions(hook, fs::perms::owner_all);

    auto r = run_wt(box, repo, {"new", "feature"});
    CHECK(r.code == 4);
    CHECK_THAT(r.err, EndsWith("wt: wt new did not complete; current state:\n"
                               "  branch feature: doesn't exist\n"
                               "  " +
                               target.string() +
                               ": doesn't exist\n"
                               "wt: nothing was left behind\n"));
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
