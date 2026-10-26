// End-to-end tests for wt's top-level command line: help, version, and argument
// handling.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/e2e.hpp"

using Catch::Matchers::ContainsSubstring;
using namespace wt::test;

TEST_CASE("wt binary exists") {
    CHECK(fs::is_regular_file(WT_BINARY_PATH));
}

TEST_CASE("wt --help prints usage and exits 0") {
    auto r = run_wt({"--help"});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("Usage: wt"));
    CHECK_THAT(r.out, ContainsSubstring("--version"));
    CHECK(r.err.empty());
}

TEST_CASE("wt --version prints the project version and exits 0") {
    auto r = run_wt({"--version"});
    CHECK(r.code == 0);
    CHECK(r.out == "wt " WT_VERSION "\n");
    CHECK(r.err.empty());
}

TEST_CASE("wt with no arguments prints help and exits 0") {
    auto r = run_wt({});
    CHECK(r.code == 0);
    CHECK_THAT(r.out, ContainsSubstring("Usage: wt"));
}

TEST_CASE("wt with an unknown flag exits non-zero and reports it on stderr") {
    auto r = run_wt({"--bogus"});
    CHECK(r.code != 0);
    CHECK(r.out.empty());
    CHECK_THAT(r.err, ContainsSubstring("--bogus"));
}

TEST_CASE("arguments reach wt verbatim, without shell interpretation") {
    auto r = run_wt({"--bogus 'quoted' $HOME;"});
    CHECK(r.code != 0);
    CHECK_THAT(r.err, ContainsSubstring("--bogus 'quoted' $HOME;"));
}
