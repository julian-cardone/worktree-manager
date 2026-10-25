#include "commands/app.hpp"

#include "commands/list.hpp"
#include "commands/merge.hpp"
#include "commands/new.hpp"
#include "commands/remove.hpp"
#include "commands/status.hpp"
#include "error.hpp"
#include "ui/format.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <ostream>
#include <string>

namespace wt {

namespace {

int report_error(const Context& ctx, ErrorKind kind, const char* message) {
    // Messages can quote branch names and paths, so sanitize them.
    ctx.err << error_prefix(kind) << ui::printable(message) << '\n';
    if (kind == ErrorKind::internal) {
        ctx.err << "wt: this is probably a bug in wt; check the state of the "
                   "affected worktrees with `git status` before retrying\n";
    }
    return exit_code(kind);
}

}  // namespace

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
    app->add_subcommand("status",
                        "Show a task's worktree: its HEAD, uncommitted "
                        "changes, and commits ahead of or behind the "
                        "integration branch")
        ->add_option("task", "Task name, i.e. the worktree's branch")
        ->required();
    auto* remove = app->add_subcommand(
        "remove",
        "Remove a task's worktree if it has no uncommitted changes; the "
        "branch is kept");
    remove->add_option("task", "Task name, i.e. the worktree's branch")
        ->required();
    remove->add_flag("--delete-ignored",
                     "Remove the worktree even if it holds ignored files, "
                     "deleting them");
    app->add_subcommand("merge",
                        "Merge a task's branch into the integration branch; "
                        "on a conflict, abort and list the conflicting files")
        ->add_option("task", "Task name, i.e. the branch to merge")
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
        if (auto* sub = app->get_subcommand("remove"); sub->parsed()) {
            return remove_command(
                ctx, sub->get_option("task")->as<std::string>(),
                sub->get_option("--delete-ignored")->as<bool>());
        }
        if (auto* sub = app->get_subcommand("merge"); sub->parsed()) {
            return merge_command(ctx,
                                 sub->get_option("task")->as<std::string>());
        }
        if (auto* sub = app->get_subcommand("status"); sub->parsed()) {
            return status_command(ctx,
                                  sub->get_option("task")->as<std::string>());
        }
    } catch (const CLI::ParseError& e) {
        return app->exit(e, ctx.out, ctx.err);
    } catch (const Error& e) {
        // Commands report most failures by throwing.
        return report_error(ctx, e.kind(), e.what());
    } catch (const std::exception& e) {
        // Anything else wasn't anticipated, e.g. a failing system call; say
        // so rather than abort with no useful message.
        return report_error(ctx, ErrorKind::internal, e.what());
    }
    return 0;
}

}  // namespace wt
