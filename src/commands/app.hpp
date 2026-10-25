#pragma once

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace CLI {
class App;
}

namespace wt {

inline constexpr std::string_view kVersion = WT_VERSION;

// Everything a command needs from its environment. Commands must use this
// rather than process-global state (std::filesystem::current_path(),
// std::cout, getenv("HOME"), ...) so tests can run them in-process against an
// isolated sandbox.
struct Context {
    std::filesystem::path cwd;
    std::ostream& out;
    std::ostream& err;
    std::filesystem::path home;  // empty if unknown; only used for display
    // The GIT_DIR wt was started with, if any. It only decides which
    // repository wt works on; see git::invoked_repository.
    std::optional<std::string> git_dir = std::nullopt;
    // Where repository-wide Git commands run, set by run(): usually `cwd`.
    // `cwd` stays the user's location, e.g. for "the worktree you're in".
    std::filesystem::path repo = {};
};

// Builds the top-level `wt` command-line definition.
std::unique_ptr<CLI::App> make_app();

// Parses argv and runs the requested command. Returns the process exit code.
int run(int argc, const char* const* argv, const Context& ctx);

}  // namespace wt
