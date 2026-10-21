#include "commands/app.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <system_error>

int main(int argc, char** argv) {
    // This is the only place wt reads the process working directory and HOME;
    // commands receive them through Context. It fails if the directory was
    // deleted, e.g. when the shell is still sitting in a removed worktree.
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) {
        std::cerr << "wt: error: cannot determine the current directory: "
                  << ec.message() << '\n';
        return 1;
    }

    const char* home = std::getenv("HOME");
    return wt::run(argc, argv,
                   wt::Context{cwd, std::cout, std::cerr, home ? home : ""});
}
