#include "ui/format.hpp"

#include <catch2/catch_test_macros.hpp>

using wt::ui::format_table;
using wt::ui::printable;
using wt::ui::shell_quote;
using wt::ui::tilde_path;

TEST_CASE("printable replaces control characters") {
    CHECK(printable("feature/x") == "feature/x");
    CHECK(printable("a\nb\tc") == "a?b?c");
    CHECK(printable("\x1b[31mred") == "?[31mred");
    CHECK(printable("del\x7f") == "del?");
    CHECK(printable("caf\xc3\xa9") == "caf\xc3\xa9");  // UTF-8 is kept
}

TEST_CASE("shell_quote leaves plain words alone and quotes the rest") {
    CHECK(shell_quote("claude/fix-login") == "claude/fix-login");
    CHECK(shell_quote("v1.2_x+y@z") == "v1.2_x+y@z");
    CHECK(shell_quote("") == "''");
    CHECK(shell_quote("a b") == "'a b'");
    CHECK(shell_quote("$(rm -rf ~);x") == "'$(rm -rf ~);x'");
    CHECK(shell_quote("it's") == "'it'\\''s'");
}

TEST_CASE("tilde_path abbreviates paths inside home") {
    CHECK(tilde_path("/Users/me/code/wt", "/Users/me") == "~/code/wt");
    CHECK(tilde_path("/Users/me", "/Users/me") == "~");
    CHECK(tilde_path("/Users/me/code", "/Users/me/") == "~/code");
}

TEST_CASE("tilde_path leaves other paths alone") {
    CHECK(tilde_path("/Users/meow/code", "/Users/me") == "/Users/meow/code");
    CHECK(tilde_path("/opt/code", "/Users/me") == "/opt/code");
    CHECK(tilde_path("/Users/me/code", "") == "/Users/me/code");
    CHECK(tilde_path("/Users/me/code", "relative") == "/Users/me/code");
    CHECK(tilde_path("/code", "/") == "/code");
}

TEST_CASE("format_table aligns columns without trailing spaces") {
    CHECK(format_table({{"BRANCH", "STATUS", "PATH"},
                        {"main", "clean", "~/code/wt"},
                        {"feature-long", "dirty", "~/code/wt.worktrees/x"}}) ==
          "BRANCH        STATUS  PATH\n"
          "main          clean   ~/code/wt\n"
          "feature-long  dirty   ~/code/wt.worktrees/x\n");
}

TEST_CASE("format_table handles no rows") {
    CHECK(format_table({}).empty());
}
