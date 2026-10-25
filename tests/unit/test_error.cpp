#include "error.hpp"
#include "git/git.hpp"

#include <CLI/CLI.hpp>
#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>

namespace {

constexpr wt::ErrorKind kAllKinds[] = {
    wt::ErrorKind::user, wt::ErrorKind::refused, wt::ErrorKind::conflict,
    wt::ErrorKind::git, wt::ErrorKind::internal};

}  // namespace

TEST_CASE("each error kind has its own exit code") {
    std::set<int> codes;
    for (const auto kind : kAllKinds) {
        const int code = wt::exit_code(kind);
        INFO("kind " << static_cast<int>(kind) << " exits " << code);
        CHECK(code > 0);
        // CLI11 reports parse errors with codes from 100 up.
        CHECK(code < static_cast<int>(CLI::ExitCodes::IncorrectConstruction));
        codes.insert(code);
    }
    CHECK(codes.size() == std::size(kAllKinds));
}

TEST_CASE("exit codes match the documented values") {
    CHECK(wt::exit_code(wt::ErrorKind::user) == 1);
    CHECK(wt::exit_code(wt::ErrorKind::refused) == 2);
    CHECK(wt::exit_code(wt::ErrorKind::conflict) == 3);
    CHECK(wt::exit_code(wt::ErrorKind::git) == 4);
    CHECK(wt::exit_code(wt::ErrorKind::internal) == 5);
}

TEST_CASE("each error kind has its own prefix") {
    std::set<std::string_view> prefixes;
    for (const auto kind : kAllKinds) {
        const auto prefix = wt::error_prefix(kind);
        CHECK(prefix.starts_with("wt: "));
        prefixes.insert(prefix);
    }
    CHECK(prefixes.size() == std::size(kAllKinds));
}

TEST_CASE("git::Error is a git error") {
    const wt::git::Error e("boom");
    CHECK(e.kind() == wt::ErrorKind::git);
    CHECK(std::string(e.what()) == "boom");
}
