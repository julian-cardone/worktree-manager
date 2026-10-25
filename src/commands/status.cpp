#include "commands/status.hpp"

#include "commands/task_worktree.hpp"
#include "error.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "ui/format.hpp"

#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace wt {

namespace {

// The integration branch, or nothing (with a warning) if there isn't one.
// Not having one, or Git failing to name it, only costs the ahead/behind
// counts, so it isn't fatal.
std::optional<std::string> try_integration_branch(const Context& ctx) {
    try {
        return git::integration_branch(ctx.repo);
    } catch (const Error& e) {
        ctx.err << "wt: warning: not comparing with an integration branch: "
                << ui::printable(e.what()) << '\n';
        return std::nullopt;
    }
}

}  // namespace

int status_command(const Context& ctx, const std::string& task) {
    const auto worktrees = git::list_worktrees(ctx.repo);
    const git::Worktree& worktree =
        find_task_worktree(ctx.repo, worktrees, task);
    // Git reports an all-zero HEAD for a branch with no commits yet.
    const bool unborn =
        worktree.head.find_first_not_of('0') == std::string::npos;
    const auto status = git::worktree_status(worktree);

    std::vector<std::vector<std::string>> rows = {
        {"Path:", ui::printable(ui::tilde_path(worktree.path, ctx.home))},
        {"HEAD:",
         unborn ? "(no commits)"
                : ui::printable(git::commit_summary(ctx.repo, worktree.head))},
        {"Status:", std::string(git::status_label(status.status))},
    };
    if (status.status == git::Status::clean ||
        status.status == git::Status::dirty) {
        const auto& changes = status.changes;
        rows.push_back({"Staged:", std::to_string(changes.staged)});
        rows.push_back({"Modified:", std::to_string(changes.modified)});
        rows.push_back({"Untracked:", std::to_string(changes.untracked)});
        rows.push_back({"Conflicts:", std::to_string(changes.conflicts)});
    }
    // Branch refs rather than the worktree's HEAD, so this works even when
    // the worktree's directory is gone.
    if (!unborn) {
        if (const auto base = try_integration_branch(ctx);
            base && *base != task) {
            const auto d = git::divergence(ctx.repo, "refs/heads/" + task,
                                           "refs/heads/" + *base);
            const std::string name = ui::printable(*base);
            rows.push_back({"Ahead " + name + ":", std::to_string(d.ahead)});
            rows.push_back({"Behind " + name + ":", std::to_string(d.behind)});
        }
    }

    ctx.out << ui::printable(task) << "\n\n" << ui::format_table(rows);

    if (status.status == git::Status::missing) {
        ctx.err << "wt: warning: the worktree's directory is gone; "
                   "`git worktree prune` makes Git forget it\n";
    }
    if (status.status == git::Status::unknown) {
        ctx.err << error_prefix(ErrorKind::user) << "cannot read the status of "
                << ui::printable(worktree.path.string()) << ": "
                << ui::printable(status.detail) << '\n';
        return exit_code(ErrorKind::user);
    }
    return 0;
}

}  // namespace wt
