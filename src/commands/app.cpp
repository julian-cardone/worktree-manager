#include "commands/app.hpp"

#include "commands/list.hpp"
#include "commands/new.hpp"
#include "ui/format.hpp"

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
    app->require_subcommand(0, 1);
    app->add_subcommand(
        "list",
        "List the repository's worktrees and whether each has uncommitted "
        "changes");
    app->add_subcommand("new",
                        "Create a branch and worktree for a task, starting "
                        "from the integration branch")
        ->add_option("task",
                     "Task name, used as the branch name (e.g. feature-x or "
                     "claude/fix-login)")
        ->required();
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
        if (app->got_subcommand("list")) {
            return list_command(ctx);
        }
        if (auto* sub = app->get_subcommand("new"); sub->parsed()) {
            return new_command(ctx, sub->get_option("task")->as<std::string>());
        }
    } catch (const CLI::ParseError& e) {
        return app->exit(e, ctx.out, ctx.err);
    } catch (const std::exception& e) {
        // Commands report failures (e.g. git::Error) by throwing; this also
        // keeps anything unexpected from aborting with no useful message.
        // Messages can quote branch names and paths, so sanitize them.
        ctx.err << "wt: error: " << ui::printable(e.what()) << '\n';
        return 1;
    }
    return 0;
}

}  // namespace wt
