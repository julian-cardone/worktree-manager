#include "git/git.hpp"
#include "git/worktree.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using wt::git::parse_worktree_list;

namespace {

using namespace std::string_literals;

const std::string kHash = "0123456789abcdef0123456789abcdef01234567";

}  // namespace

TEST_CASE("parse_worktree_list reads a branch worktree") {
    const auto list = parse_worktree_list("worktree /repo\0HEAD "s + kHash +
                                          "\0branch refs/heads/main\0\0"s);
    REQUIRE(list.size() == 1);
    CHECK(list[0].path == "/repo");
    CHECK(list[0].head == kHash);
    CHECK(list[0].branch == "main");
    CHECK_FALSE(list[0].bare);
    CHECK_FALSE(list[0].detached);
    CHECK_FALSE(list[0].locked);
    CHECK_FALSE(list[0].prunable);
}

TEST_CASE("parse_worktree_list reads several worktrees in order") {
    const auto list = parse_worktree_list(
        "worktree /repo\0HEAD "s + kHash + "\0branch refs/heads/main\0\0"s +
        "worktree /repo.worktrees/feature-x\0HEAD "s + kHash +
        "\0branch refs/heads/feature/x\0\0"s +
        "worktree /repo.worktrees/detached\0HEAD "s + kHash +
        "\0detached\0\0"s);
    REQUIRE(list.size() == 3);
    CHECK(list[0].path == "/repo");
    CHECK(list[1].branch == "feature/x");
    CHECK(list[2].path == "/repo.worktrees/detached");
    CHECK(list[2].detached);
    CHECK_FALSE(list[2].branch.has_value());
}

TEST_CASE("parse_worktree_list reads bare, locked, and prunable flags") {
    const auto list = parse_worktree_list(
        "worktree /repo.git\0bare\0\0"s
        "worktree /a\0HEAD "s +
        kHash + "\0branch refs/heads/a\0locked on a USB drive\0\0"s +
        "worktree /b\0HEAD "s + kHash +
        "\0detached\0prunable gitdir file points to non-existent location\0\0"s);
    REQUIRE(list.size() == 3);
    CHECK(list[0].bare);
    CHECK(list[1].locked);
    CHECK_FALSE(list[1].prunable);
    CHECK(list[2].prunable);
    CHECK(list[2].detached);
}

TEST_CASE("parse_worktree_list keeps unusual characters in paths") {
    const auto list = parse_worktree_list(
        "worktree /my repo/with\nnewline\0HEAD "s + kHash + "\0detached\0\0"s);
    REQUIRE(list.size() == 1);
    CHECK(list[0].path == "/my repo/with\nnewline");
}

TEST_CASE("parse_worktree_list keeps refs outside refs/heads intact") {
    const auto list = parse_worktree_list("worktree /repo\0HEAD "s + kHash +
                                          "\0branch refs/other/x\0\0"s);
    REQUIRE(list.size() == 1);
    CHECK(list[0].branch == "refs/other/x");
}

TEST_CASE("parse_worktree_list ignores attributes it doesn't know") {
    const auto list = parse_worktree_list("worktree /repo\0HEAD "s + kHash +
                                          "\0future-flag yes\0\0"s);
    REQUIRE(list.size() == 1);
    CHECK(list[0].head == kHash);
}

TEST_CASE("parse_worktree_list tolerates a missing final terminator") {
    const auto list = parse_worktree_list("worktree /repo\0bare"s);
    REQUIRE(list.size() == 1);
    CHECK(list[0].bare);
}

TEST_CASE("parse_worktree_list handles empty output") {
    CHECK(parse_worktree_list("").empty());
}

TEST_CASE("parse_worktree_list rejects attributes outside a record") {
    CHECK_THROWS_AS(parse_worktree_list("HEAD "s + kHash + "\0\0"s),
                    wt::git::Error);
}

TEST_CASE("error_message keeps the first line of git's error") {
    using wt::git::error_message;
    CHECK(error_message("fatal: not a git repository\n") ==
          "not a git repository");
    CHECK(error_message("error: bad thing\nhint: try again\n") == "bad thing");
    CHECK(error_message("\nplain message") == "plain message");
    CHECK(error_message("") == "git failed without an error message");
}

TEST_CASE("find_worktree_containing picks the deepest enclosing worktree") {
    using wt::git::find_worktree_containing;
    using wt::git::Worktree;
    const std::vector<Worktree> list = {
        Worktree{.path = "/code/repo"},
        Worktree{.path = "/code/repo/.worktrees/nested"},
        Worktree{.path = "/code/repo.worktrees/feature"},
    };
    CHECK(find_worktree_containing(list, "/code/repo") == 0u);
    CHECK(find_worktree_containing(list, "/code/repo/src") == 0u);
    CHECK(find_worktree_containing(list, "/code/repo/.worktrees/nested/a") ==
          1u);
    CHECK(find_worktree_containing(list, "/code/repo.worktrees/feature") == 2u);
    CHECK_FALSE(find_worktree_containing(list, "/code/repo2").has_value());
    CHECK_FALSE(find_worktree_containing(list, "/elsewhere").has_value());
    CHECK_FALSE(find_worktree_containing({}, "/code/repo").has_value());
}

TEST_CASE("worktree_contains matches the worktree and paths inside it") {
    using wt::git::worktree_contains;
    const wt::git::Worktree worktree{.path = "/code/.repo.wt-1/feature"};
    CHECK(worktree_contains(worktree, "/code/.repo.wt-1/feature"));
    CHECK(worktree_contains(worktree, "/code/.repo.wt-1/feature/src/x"));
    CHECK(worktree_contains(worktree, "/code/.repo.wt-1/feature/src/.."));
    CHECK_FALSE(worktree_contains(worktree, "/code/.repo.wt-1"));
    CHECK_FALSE(worktree_contains(worktree, "/code/.repo.wt-1/feature-2"));
    CHECK_FALSE(worktree_contains(worktree, "/code/.repo.wt-1/feature/../b"));
}

TEST_CASE("parse_status counts nothing for a clean worktree") {
    CHECK(wt::git::parse_status("").empty());
}

TEST_CASE("parse_status counts staged and unstaged changes separately") {
    const auto changes = wt::git::parse_status(
        "1 M. N... 100644 100644 100644 a a staged.txt\0"s
        "1 .M N... 100644 100644 100644 a a modified.txt\0"s
        "1 MM N... 100644 100644 100644 a a both.txt\0"s
        "1 A. N... 000000 100644 100644 0 a added.txt\0"s
        "1 .D N... 100644 100644 000000 a a deleted.txt\0"s);
    CHECK(changes.staged == 3);
    CHECK(changes.modified == 3);
    CHECK(changes.untracked == 0);
    CHECK(changes.conflicts == 0);
    CHECK_FALSE(changes.empty());
}

TEST_CASE("parse_status skips a rename's original path") {
    // The original path looks like an untracked entry, but isn't one.
    const auto changes = wt::git::parse_status(
        "2 R. N... 100644 100644 100644 a a R100 new.txt\0? old.txt\0"s
        "? untracked.txt\0"s);
    CHECK(changes.staged == 1);
    CHECK(changes.modified == 0);
    CHECK(changes.untracked == 1);
}

TEST_CASE("parse_status counts conflicts and untracked files") {
    const auto changes = wt::git::parse_status(
        "u UU N... 100644 100644 100644 100644 a b c conflict.txt\0"s
        "? new.txt\0? dir/\0"s);
    CHECK(changes.conflicts == 1);
    CHECK(changes.untracked == 2);
    CHECK(changes.staged == 0);
}

TEST_CASE("parse_status skips headers and counts ignored files apart") {
    const auto changes = wt::git::parse_status(
        "# branch.oid 0123\0# branch.head main\0! build/\0! x.env\0"s);
    CHECK(changes.ignored == 2);
    // Ignored files aren't changes.
    CHECK(changes.empty());
}

TEST_CASE("parse_status rejects output it doesn't understand") {
    using wt::git::Error;
    using wt::git::parse_status;
    CHECK_THROWS_AS(parse_status("garbage\0"s), Error);
    CHECK_THROWS_AS(parse_status("1\0"s), Error);
    CHECK_THROWS_AS(parse_status("1 M\0"s), Error);
    CHECK_THROWS_AS(parse_status("? a\0\0"s), Error);
}

TEST_CASE("status_label names each status") {
    using wt::git::Status;
    using wt::git::status_label;
    CHECK(status_label(Status::clean) == "clean");
    CHECK(status_label(Status::dirty) == "dirty");
    CHECK(status_label(Status::bare) == "-");
    CHECK(status_label(Status::missing) == "missing");
    CHECK(status_label(Status::unknown) == "unknown");
}
