#include "commands/status.hpp"

#include "commands/task_worktree.hpp"
#include "error.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "ui/format.hpp"
#include "ui/json.hpp"

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

// What `wt status` reports, in either format.
struct TaskStatus {
    const git::Worktree& worktree;
    std::optional<std::string> head;
    git::StatusResult status;
    std::optional<git::Operation> operation;
    // Why wt couldn't tell whether an operation is in progress, if it couldn't.
    std::optional<std::string> operation_unknown;
    // Set when the branch was compared with the integration branch.
    std::optional<std::string> base;
    git::Divergence divergence = {};
};

bool has_changes(const git::StatusResult& status) {
    return status.status == git::Status::clean ||
           status.status == git::Status::dirty;
}

void print_summary(const Context& ctx, const std::string& task,
                   const TaskStatus& s) {
    std::vector<std::vector<std::string>> rows = {
        {"Path:", ui::printable(ui::tilde_path(s.worktree.path, ctx.home))},
        {"HEAD:", s.head ? ui::printable(git::commit_summary(ctx.repo, *s.head))
                         : "(no commits)"},
        {"Status:", std::string(git::status_label(s.status.status))},
    };
    if (s.operation_unknown) {
        rows.push_back({"In progress:", "unknown"});
    } else if (s.operation) {
        rows.push_back(
            {"In progress:", std::string(git::describe(*s.operation))});
    }
    if (has_changes(s.status)) {
        const auto& changes = s.status.changes;
        rows.push_back({"Staged:", std::to_string(changes.staged)});
        rows.push_back({"Modified:", std::to_string(changes.modified)});
        rows.push_back(
            {"Untracked:", std::to_string(changes.untracked.size())});
        rows.push_back({"Conflicts:", std::to_string(changes.conflicts)});
    }
    if (s.base) {
        const std::string name = ui::printable(*s.base);
        rows.push_back(
            {"Ahead " + name + ":", std::to_string(s.divergence.ahead)});
        rows.push_back(
            {"Behind " + name + ":", std::to_string(s.divergence.behind)});
    }
    ctx.out << ui::printable(task) << "\n\n" << ui::format_table(rows);
}

// WARNING: the field names and meanings are part of wt's interface,
// documented in README.md. Add fields rather than change existing ones.
void print_json(const Context& ctx, const std::string& task,
                const TaskStatus& s) {
    using ui::Json;
    const auto& changes = s.status.changes;
    const Json counts =
        has_changes(s.status)
            ? Json::object({
                  {"staged", Json::number(changes.staged)},
                  {"modified", Json::number(changes.modified)},
                  {"untracked", Json::number(changes.untracked.size())},
                  {"conflicts", Json::number(changes.conflicts)},
              })
            : Json::null();
    const Json integration =
        s.base ? Json::object({
                     {"branch", Json::string(*s.base)},
                     {"ahead", Json::number(s.divergence.ahead)},
                     {"behind", Json::number(s.divergence.behind)},
                 })
               : Json::null();
    ctx.out << Json::object(
                   {
                       {"branch", Json::string(task)},
                       {"path", Json::string(s.worktree.path.string())},
                       {"head", Json::string_or_null(s.head)},
                       {"status",
                        Json::string(git::status_name(s.status.status))},
                       {"operation",
                        s.operation
                            ? Json::string(git::operation_name(*s.operation))
                            : Json::null()},
                       {"changes", counts},
                       {"integration", integration},
                   })
                   .text()
            << '\n';
}

}  // namespace

int status_command(const Context& ctx, const std::string& task, bool json) {
    const auto worktrees = git::list_worktrees(ctx.repo);
    TaskStatus s{.worktree = find_status_worktree(ctx, worktrees, task)};
    s.head = git::head_commit(s.worktree);
    s.status = git::worktree_status(s.worktree);
    // An operation can leave the files clean, so the status alone doesn't
    // say the worktree is idle; `wt merge` and `wt remove` refuse it.
    if (has_changes(s.status)) {
        try {
            s.operation = git::current_operation(s.worktree.path);
        } catch (const Error& e) {
            s.operation_unknown = e.what();
        }
    }
    // Branch refs rather than the worktree's HEAD, so this works even when
    // the worktree's directory is gone.
    if (s.head) {
        if (const auto base = try_integration_branch(ctx);
            base && *base != task) {
            s.base = base;
            s.divergence = git::divergence(ctx.repo, "refs/heads/" + task,
                                           "refs/heads/" + *base);
        }
    }

    const bool unknown = s.status.status == git::Status::unknown;
    // JSON goes out only on success, so a script never mistakes a partial
    // report for a complete one.
    if (!json) {
        print_summary(ctx, task, s);
    } else if (!unknown && !s.operation_unknown) {
        print_json(ctx, task, s);
    }

    if (s.status.status == git::Status::missing) {
        report_missing(ctx, s.worktree, task);
    }
    if (unknown) {
        ctx.err << error_prefix(ErrorKind::user) << "cannot read the status of "
                << ui::printable(s.worktree.path.string()) << ": "
                << ui::printable(s.status.detail) << '\n';
        return exit_code(ErrorKind::user);
    }
    if (s.operation_unknown) {
        ctx.err << error_prefix(ErrorKind::user)
                << ui::printable(*s.operation_unknown) << '\n';
        return exit_code(ErrorKind::user);
    }
    return 0;
}

}  // namespace wt
