#pragma once

#include "error.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wt::git {

struct Result {
    int exit_code;
    std::string out;
    std::string err;
};

// A Git failure worth reporting to the user as-is.
class Error : public wt::Error {
public:
    explicit Error(const std::string& message)
        : wt::Error(ErrorKind::git, message) {}
};

// Runs `git -C <dir> <args...>` and captures its output. Arguments are passed
// straight to exec, never through a shell. Inherited variables that would
// point git at another repository (GIT_DIR, GIT_WORK_TREE, ...) are dropped,
// so `dir` alone decides; invoked_repository (repo.hpp) is where wt honors an
// explicit GIT_DIR. Throws Error if git can't be run.
Result run(const std::filesystem::path& dir,
           const std::vector<std::string>& args);

// The one value a Git query printed: its output minus the newline that ends
// it. Unlike its first line, this keeps a value with a newline in it (e.g. a
// path) whole, so it can't silently become a different value.
std::string single_value(std::string_view out);

// Turns git's stderr into a one-line message, dropping the "fatal: " prefix.
std::string error_message(std::string_view git_stderr);

}  // namespace wt::git
