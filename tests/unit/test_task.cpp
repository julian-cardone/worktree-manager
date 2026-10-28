#include "layout/task.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using wt::layout::same_ignoring_case;
using wt::layout::task_folder_name;
using wt::layout::task_name_problem;

TEST_CASE("task_name_problem accepts ordinary branch names") {
    for (const char* name :
         {"feature-x", "claude/fix-login", "a", "v1.2_rc", "A/b/C-9", "x.y"}) {
        INFO("name: " << name);
        CHECK_FALSE(task_name_problem(name).has_value());
    }
}

TEST_CASE("task_name_problem rejects unsafe or ambiguous names") {
    for (const char* name :
         {"",          "-x",          "--force", "/abs",        "trailing/",
          "a//b",      "..",          "../evil", "a/../b",      "a..b",
          ".hidden",   ".wt-managed", "HEAD",    "has space",   "tab\there",
          "new\nline", "semi;colon",  "dollar$", "back\\slash", "star*",
          "q?",        "at@{-1}",     "tilde~1", "caret^",      "colon:",
          "quote'",    "caf\xc3\xa9"}) {
        INFO("name: " << name);
        CHECK(task_name_problem(name).has_value());
    }
}

TEST_CASE("task_name_problem limits the length") {
    CHECK_FALSE(task_name_problem(std::string(100, 'a')).has_value());
    CHECK(task_name_problem(std::string(101, 'a')).has_value());
}

TEST_CASE("task_folder_name turns slashes into dashes") {
    CHECK(task_folder_name("feature-x") == "feature-x");
    CHECK(task_folder_name("claude/fix-login") == "claude-fix-login");
    CHECK(task_folder_name("a/b/c") == "a-b-c");
}

TEST_CASE("same_ignoring_case compares branch names ignoring ASCII case") {
    CHECK(same_ignoring_case("Fix/Login", "fix/login"));
    CHECK(same_ignoring_case("MAIN", "main"));
    CHECK_FALSE(same_ignoring_case("main", "main2"));
    CHECK_FALSE(same_ignoring_case("fix", "fix/login"));
    // Only ASCII is folded, as with task names.
    CHECK_FALSE(same_ignoring_case("caf\xc3\xa9", "CAF\xc3\x89"));
}
