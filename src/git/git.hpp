#pragma once

#include <filesystem>
#include <stdexcept>
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
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Runs `git -C <dir> <args...>` and captures its output. Arguments are passed
// straight to exec, never through a shell. Throws Error if git can't be run.
Result run(const std::filesystem::path& dir,
           const std::vector<std::string>& args);

// Turns git's stderr into a one-line message, dropping the "fatal: " prefix.
std::string error_message(std::string_view git_stderr);

}  // namespace wt::git
