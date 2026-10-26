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

// Explains how to recover a worktree whose directory is gone. It may have been
// deleted, moved, or be on a drive that isn't mounted, and wt can't tell
// which, so this names the ways out rather than choosing one.
void report_missing(const Context& ctx, const git::Worktree& worktree,
                    const std::string& task) {
    ctx.err << "wt: warning: the worktree's directory is gone";
    // A lock is how Git keeps a worktree on a removable drive; neither Git
    // nor `wt remove` forgets it until it's unlocked.
    if (worktree.locked) {
        ctx.err << ", but it is locked, so Git keeps its record; if it's no "
                   "longer needed, unlock it with `git worktree unlock "
                << ui::printable(ui::shell_quote(worktree.path.string()))
                << "`\n";
        return;
    }
    ctx.err << "; if it was moved, `git worktree repair <new-path>` "
               "reconnects it, otherwise `wt remove "
            << ui::printable(ui::shell_quote(task))
            << " --forget-missing` makes Git forget it and keeps the branch\n";
}

// The worktree for `task`: the one with its branch checked out or, since a
// rebase detaches HEAD until it finishes, the one rebasing it.
const git::Worktree& find_status_worktree(
    const Context& ctx, const std::vector<git::Worktree>& worktrees,
    const std::string& task) {
    if (const auto* found = find_branch_worktree(worktrees, task)) {
        return *found;
    }
    if (git::is_valid_branch_name(ctx.repo, task)) {
        if (const auto where = git::rebasing_worktree(
                worktrees, git::common_dir(ctx.repo), task)) {
            for (const auto& worktree : worktrees) {
                if (git::same_path(worktree.path, *where) &&
                    git::rebased_branch(worktree) == task) {
                    return worktree;
                }
            }
            // `git rebase --update-refs` there will move it.
            throw Error(ErrorKind::user,
                        "branch '" + task +
                            "' isn't checked out in any worktree, but a "
                            "rebase in " +
                            where->string() + " will move it when it finishes");
        }
    }
    return find_task_worktree(ctx.repo, worktrees, task);
}

}  // namespace

int status_command(const Context& ctx, const std::string& task) {
    const auto worktrees = git::list_worktrees(ctx.repo);
    const git::Worktree& worktree = find_status_worktree(ctx, worktrees, task);
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
    const bool readable = status.status == git::Status::clean ||
                          status.status == git::Status::dirty;
    // An operation can leave the files clean, so the status alone doesn't
    // say the worktree is idle; `wt merge` and `wt remove` refuse it.
    std::optional<std::string> operation_unknown;
    if (readable) {
        try {
            if (const auto operation =
                    git::operation_in_progress(worktree.path)) {
                rows.push_back({"In progress:", *operation});
            }
        } catch (const Error& e) {
            operation_unknown = e.what();
            rows.push_back({"In progress:", "unknown"});
        }
    }
    if (readable) {
        const auto& changes = status.changes;
        rows.push_back({"Staged:", std::to_string(changes.staged)});
        rows.push_back({"Modified:", std::to_string(changes.modified)});
        rows.push_back(
            {"Untracked:", std::to_string(changes.untracked.size())});
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
        report_missing(ctx, worktree, task);
    }
    if (status.status == git::Status::unknown) {
        ctx.err << error_prefix(ErrorKind::user) << "cannot read the status of "
                << ui::printable(worktree.path.string()) << ": "
                << ui::printable(status.detail) << '\n';
        return exit_code(ErrorKind::user);
    }
    if (operation_unknown) {
        ctx.err << error_prefix(ErrorKind::user)
                << ui::printable(*operation_unknown) << '\n';
        return exit_code(ErrorKind::user);
    }
    return 0;
}

}  // namespace wt
