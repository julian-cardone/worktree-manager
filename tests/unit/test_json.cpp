#include "ui/json.hpp"

#include <catch2/catch_test_macros.hpp>

#include "support/error_kind.hpp"

#include <string>

using wt::test::kind_is;
using wt::ui::Json;

namespace {

using namespace std::string_literals;

std::string str(std::string_view text) {
    return Json::string(text).text();
}

}  // namespace

TEST_CASE("Json::string quotes text and escapes what JSON requires") {
    CHECK(str("feature/x") == R"("feature/x")");
    CHECK(str("") == R"("")");
    CHECK(str(R"(a "b" \c)") == R"("a \"b\" \\c")");
    CHECK(str("/a b/c") == R"("/a b/c")");
}

TEST_CASE("Json::string escapes control characters instead of replacing them") {
    CHECK(str("a\nb\tc") == R"("a\u000ab\u0009c")");
    CHECK(str("\x1b[31m") == R"("\u001b[31m")");
    CHECK(str("\0x"s) == R"("\u0000x")");
    CHECK(str("del\x7f") == R"("del\u007f")");
}

TEST_CASE("Json::string keeps UTF-8 as it is") {
    CHECK(str("caf\xc3\xa9") == "\"caf\xc3\xa9\"");
    CHECK(str("\xe2\x9c\x93") == "\"\xe2\x9c\x93\"");          // U+2713
    CHECK(str("\xf0\x9f\x98\x80") == "\"\xf0\x9f\x98\x80\"");  // U+1F600
    CHECK(str("\xf4\x8f\xbf\xbf") == "\"\xf4\x8f\xbf\xbf\"");  // U+10FFFF
}

TEST_CASE("Json::string rejects text that isn't valid UTF-8") {
    const auto user = kind_is(wt::ErrorKind::user);
    for (const auto& bad : {
             "\xff"s,              // never valid
             "a\x80"s,             // stray continuation byte
             "\xc3"s,              // truncated
             "\xe2\x9c"s,          // truncated
             "\xc3x"s,             // missing continuation
             "\xc0\xaf"s,          // overlong '/'
             "\xe0\x80\xaf"s,      // overlong '/'
             "\xed\xa0\x80"s,      // surrogate U+D800
             "\xf4\x90\x80\x80"s,  // past U+10FFFF
         }) {
        CAPTURE(bad);
        CHECK_THROWS_MATCHES(Json::string(bad), wt::Error, user);
    }
}

TEST_CASE("Json builds compact documents with fields in the given order") {
    CHECK(Json::number(0).text() == "0");
    CHECK(Json::number(42).text() == "42");
    CHECK(Json::boolean(true).text() == "true");
    CHECK(Json::boolean(false).text() == "false");
    CHECK(Json::null().text() == "null");
    CHECK(Json::string_or_null(std::nullopt).text() == "null");
    CHECK(Json::string_or_null("x").text() == R"("x")");
    CHECK(Json::array({}).text() == "[]");
    CHECK(Json::object({}).text() == "{}");
    CHECK(
        Json::object({
                         {"z", Json::number(1)},
                         {"a", Json::array({Json::null(), Json::string("q")})},
                         {"m", Json::object({{"k", Json::boolean(false)}})},
                     })
            .text() == R"({"z":1,"a":[null,"q"],"m":{"k":false}})");
}
