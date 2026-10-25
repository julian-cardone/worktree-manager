#pragma once

#include "error.hpp"

#include <catch2/matchers/catch_matchers_predicate.hpp>

#include <string>

namespace wt::test {

// Matches a wt::Error of the given kind, for CHECK_THROWS_MATCHES.
inline auto kind_is(ErrorKind kind) {
    return Catch::Matchers::Predicate<Error>(
        [kind](const Error& e) { return e.kind() == kind; },
        "is a wt::Error of kind " + std::to_string(static_cast<int>(kind)));
}

}  // namespace wt::test
