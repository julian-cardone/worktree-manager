#include "commands/app.hpp"

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/sandbox.hpp"

#include <sstream>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

struct Result {
    int code;
    std::string out;
    std::string err;
};

// Runs wt in-process with a sandbox as its working directory.
Result run_wt(std::vector<const char*> args) {
    args.insert(args.begin(), "wt");
    wt::test::Sandbox box;
    std::ostringstream out, err;
    int code = wt::run(static_cast<int>(args.size()), args.data(),
                       wt::Context{box.work(), out, err, box.home()});
    return {code, out.str(), err.str()};
}

}  // namespace

TEST_CASE("make_app names the program wt") {
    auto app = wt::make_app();
    CHECK(app->get_name() == "wt");
    CHECK_THAT(app->help(), ContainsSubstring("Usage: wt"));
}

TEST_CASE("help flag prints usage to out and succeeds") {
    auto r = run_wt({"--help"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("Usage: wt"));
    CHECK_THAT(r.out, ContainsSubstring("--help"));
    CHECK_THAT(r.out, ContainsSubstring("--version"));
    CHECK(r.err.empty());
}

TEST_CASE("short help flag is an alias for the long one") {
    auto r = run_wt({"-h"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("Usage: wt"));
}

TEST_CASE("version flags print the version and succeed") {
    const std::string expected = "wt " + std::string(WT_VERSION) + "\n";
    for (const char* flag : {"--version", "-V"}) {
        INFO("flag: " << flag);
        auto r = run_wt({flag});
        CHECK(r.code == 0);
        CHECK(r.out == expected);
        CHECK(r.err.empty());
    }
}

TEST_CASE("no arguments prints help and succeeds") {
    auto r = run_wt({});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, StartsWith("wt - manage Git worktrees"));
    CHECK_THAT(r.out, ContainsSubstring("Usage: wt"));
}

TEST_CASE("unknown arguments fail with an error on err") {
    auto r = run_wt({"--bogus"});
    CHECK(r.code == static_cast<int>(CLI::ExitCodes::ExtrasError));
    CHECK(r.out.empty());
    CHECK_THAT(r.err, ContainsSubstring("--bogus"));
}

TEST_CASE("help lists the list command") {
    auto r = run_wt({"--help"});
    CHECK_THAT(r.out, ContainsSubstring("list"));
}

TEST_CASE("help lists the new command") {
    auto r = run_wt({"--help"});
    CHECK_THAT(r.out, ContainsSubstring("new"));
}

TEST_CASE("new --help describes the task argument") {
    auto r = run_wt({"new", "--help"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("task"));
}

TEST_CASE("help lists the status command") {
    auto r = run_wt({"--help"});
    CHECK_THAT(r.out, ContainsSubstring("status"));
}

TEST_CASE("status --help describes the task argument") {
    auto r = run_wt({"status", "--help"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("task"));
}

TEST_CASE("help lists the remove command") {
    auto r = run_wt({"--help"});
    CHECK_THAT(r.out, ContainsSubstring("remove"));
}

TEST_CASE("remove --help describes the task and --delete-ignored") {
    auto r = run_wt({"remove", "--help"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("task"));
    CHECK_THAT(r.out, ContainsSubstring("--delete-ignored"));
}

TEST_CASE("help lists the merge command") {
    auto r = run_wt({"--help"});
    CHECK_THAT(r.out, ContainsSubstring("merge"));
}

TEST_CASE("merge --help describes the task, --cleanup, and --delete-ignored") {
    auto r = run_wt({"merge", "--help"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("task"));
    CHECK_THAT(r.out, ContainsSubstring("--cleanup"));
    CHECK_THAT(r.out, ContainsSubstring("--delete-ignored"));
}
