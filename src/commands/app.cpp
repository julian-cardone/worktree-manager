#include "commands/app.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <ostream>
#include <string>

namespace wt {

std::unique_ptr<CLI::App> make_app() {
    auto app = std::make_unique<CLI::App>(
        "wt - manage Git worktrees for parallel work", "wt");
    app->set_version_flag("-V,--version", "wt " + std::string(kVersion),
                          "Print the wt version and exit");
    return app;
}

int run(int argc, const char* const* argv, const Context& ctx) {
    auto app = make_app();

    // With no arguments, show usage rather than silently doing nothing.
    if (argc <= 1) {
        ctx.out << app->help();
        return 0;
    }

    try {
        app->parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app->exit(e, ctx.out, ctx.err);
    } catch (const std::exception& e) {
        // Last line of defense: report the failure instead of letting
        // std::terminate abort the process with no useful message.
        ctx.err << "wt: error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}

}  // namespace wt
