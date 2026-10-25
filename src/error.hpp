#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace wt {

// Why a command didn't do what was asked. Each kind calls for a different
// response, so people tell them apart by the prefix on the error line and
// agents and scripts by the exit code.
enum class ErrorKind {
    // The request or setup needs fixing: an unknown task, an invalid name, a
    // misconfiguration, a path wt can't create.
    user,
    // wt deliberately stopped because going ahead could lose work or touch
    // something it doesn't own, or because it couldn't verify that it
    // wouldn't. Nothing is broken; the message says what is in the way.
    refused,
    // A merge conflicted. Resolving it is always left to a person.
    conflict,
    // A Git command that wt ran failed.
    git,
    // Something wt doesn't understand, e.g. unexpected Git output. Probably a
    // bug in wt.
    internal,
};

// A failure that knows its kind. Commands throw it, or print a detailed
// report themselves (starting with error_prefix) and return exit_code.
class Error : public std::runtime_error {
public:
    Error(ErrorKind kind, const std::string& message)
        : std::runtime_error(message), kind_(kind) {}

    ErrorKind kind() const { return kind_; }

private:
    ErrorKind kind_;
};

// WARNING: these are part of wt's interface, documented in README.md; agents
// and scripts branch on them. Don't renumber them.
constexpr int exit_code(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::user:
            return 1;
        case ErrorKind::refused:
            return 2;
        case ErrorKind::conflict:
            return 3;
        case ErrorKind::git:
            return 4;
        case ErrorKind::internal:
            return 5;
    }
    return 5;
}

// The start of the first line wt prints for an error of this kind.
constexpr std::string_view error_prefix(ErrorKind kind) {
    switch (kind) {
        case ErrorKind::user:
            return "wt: error: ";
        case ErrorKind::refused:
            return "wt: refused: ";
        case ErrorKind::conflict:
            return "wt: conflict: ";
        case ErrorKind::git:
            return "wt: git error: ";
        case ErrorKind::internal:
            return "wt: internal error: ";
    }
    return "wt: internal error: ";
}

}  // namespace wt
