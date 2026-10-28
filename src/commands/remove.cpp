#include "commands/remove.hpp"

#include "commands/task_worktree.hpp"
#include "error.hpp"
#include "git/git.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "layout/container.hpp"
#include "layout/task.hpp"
#include "ui/format.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace wt {

namespace fs = std::filesystem;

namespace {

// Fails unless wt created `worktree`: it must sit directly inside this
// repository's container, and the container must still be provably wt's.
// WARNING: this is what keeps wt from deleting a worktree it doesn't manage.
void check_managed(const Context& ctx,
                   const std::vector<git::Worktree>& worktrees,
                   const git::Worktree& worktree, const std::string& task) {
    const fs::path common = git::common_dir(ctx.repo);
    const fs::path container =
        layout::container_path(worktrees.front().path, common);
    if (!git::same_path(worktree.path.parent_path(), container)) {
        throw Error(ErrorKind::refused,
                    "the worktree for '" + task + "' at " +
                        worktree.path.string() +
                        " wasn't created by wt, so wt won't remove it");
    }

    // A container that's gone can't hold the worktree's files, so there is
    // nothing of anyone's to delete, only Git's record of the worktree.
    std::error_code ec;
    if (fs::symlink_status(container, ec).type() == fs::file_type::not_found) {
        return;
    }
    if (auto problem = layout::ownership_problem(container, common)) {
        throw Error(ErrorKind::refused,
                    container.string() +
                        " exists but wt can't confirm it manages it (" +
                        *problem + "), so wt won't remove anything from it");
    }
}

// "2 modified files" for a non-zero count.
void add_count(std::vector<std::string>& lines, std::size_t count,
               const std::string& what, const std::string& note = "") {
    if (count != 0) {
        lines.push_back(std::to_string(count) + " " + what +
                        (count == 1 ? " file" : " files") + note);
    }
}

// The count, then the first few paths, e.g. "1 untracked file" + "new.txt".
void add_listed(std::vector<std::string>& lines,
                const std::vector<std::string>& paths, const std::string& what,
                const std::string& note = "") {
    // Enough to spot what matters without flooding the terminal.
    constexpr std::size_t kMaxShown = 10;

    add_count(lines, paths.size(), what, note);
    const std::size_t shown = std::min(paths.size(), kMaxShown);
    for (std::size_t i = 0; i < shown; ++i) {
        lines.push_back("  " + ui::printable(paths[i]));
    }
    if (paths.size() > shown) {
        lines.push_back("  ... and " + std::to_string(paths.size() - shown) +
                        " more");
    }
}

// Says how to deal with the uncommitted changes, on stderr. Each kind needs
// different advice: plain `git stash` skips untracked files, and Git won't
// stash at all while conflicts are unresolved, so those come first.
void report_recovery(const Context& ctx, const git::Changes& changes) {
    const bool conflicts = changes.conflicts != 0;
    const bool tracked = changes.staged != 0 || changes.modified != 0;
    const bool untracked = !changes.untracked.empty();

    if (conflicts) {
        ctx.err << "Resolve the conflicts first, or abort the operation that "
                   "caused them.\n";
    }
    if (tracked) {
        ctx.err << (conflicts ? "Then commit or stash the other changes"
                              : "Commit or stash these changes")
                << (untracked ? " (`git stash -u` includes untracked files)"
                              : "")
                << ".\n";
    } else if (untracked) {
        ctx.err << (conflicts ? "Then commit" : "Commit")
                << " or delete the untracked files, or stash them with "
                   "`git stash -u`.\n";
    }
}

// Explains what removal would destroy, on stderr. Untracked, hidden, and
// ignored files are named, not just counted: Git has no copy of them or
// doesn't show them, and deciding to pass --delete-ignored means having seen
// whether something like a .env file is among them.
void report_blocked(const Context& ctx, const std::string& task,
                    const git::Changes& changes,
                    const std::vector<std::string>& hidden, bool show_ignored) {
    std::vector<std::string> lines;
    add_count(lines, changes.staged, "staged");
    add_count(lines, changes.modified, "modified");
    add_listed(lines, changes.untracked, "untracked");
    add_count(lines, changes.conflicts, "conflicted");
    add_listed(lines, hidden, "modified", " that Git doesn't show");
    if (show_ignored) {
        add_listed(lines, changes.ignored, "ignored");
    }

    ctx.err << error_prefix(ErrorKind::refused) << "cannot remove "
            << ui::printable(task) << "\n\n";
    for (const auto& line : lines) {
        ctx.err << "  " << line << '\n';
    }
    ctx.err << '\n';
    report_recovery(ctx, changes);
    if (!hidden.empty()) {
        ctx.err << "Git is set not to check the files it doesn't show "
                   "(assume-unchanged or skip-worktree), but would delete "
                   "them with the worktree; copy them somewhere safe, or "
                   "clear the flag with `git update-index "
                   "--no-assume-unchanged --no-skip-worktree -- <file>` and "
                   "commit or stash them.\n";
    }
    if (show_ignored && !changes.ignored.empty()) {
        ctx.err << "Ignored files are deleted with the worktree; move any you "
                   "need, then rerun with --delete-ignored.\n";
    }
}

// Explains, on stderr, why a worktree whose directory is missing isn't
// forgotten without being asked, and the ways out.
void report_missing(const Context& ctx, const git::Worktree& worktree,
                    const std::string& task, std::string_view command) {
    const std::string quoted = ui::printable(ui::shell_quote(task));
    ctx.err << error_prefix(ErrorKind::refused) << "nothing is at "
            << ui::printable(worktree.path.string())
            << ", where Git has the worktree for '" << ui::printable(task)
            << "' registered\n\n"
            << "It may have been moved, or be on a drive that isn't "
               "mounted.\n"
            << "If it was moved, `git worktree repair <new-path>` reconnects "
               "it.\n"
            << "If it's gone for good, `wt remove " << quoted
            << " --forget-missing` removes only Git's record of it and keeps "
               "the branch";
    if (command != "wt remove") {
        ctx.err << "; then rerun `" << command << ' ' << quoted
                << " --cleanup`";
    }
    ctx.err << ".\n";
}

void report_failure(const Context& ctx, const git::Result& result,
                    const fs::path& path) {
    ctx.err << error_prefix(ErrorKind::git)
            << "git worktree remove failed with exit code " << result.exit_code
            << '\n';
    std::istringstream lines(result.err);
    for (std::string line; std::getline(lines, line);) {
        ctx.err << "  " << ui::printable(line) << '\n';
    }
    std::error_code ec;
    if (fs::symlink_status(path, ec).type() != fs::file_type::not_found) {
        ctx.err << "wt: " << ui::printable(path.string())
                << " is still there\n";
    }
}

// Checks, after Git reports success, that it no longer lists the worktree and
// its directory is gone. Returns what is wrong, if anything.
// WARNING: wt reports a removal only once this passes.
std::optional<std::string> removal_problem(const Context& ctx,
                                           const fs::path& path) {
    for (const auto& worktree : git::list_worktrees(ctx.repo)) {
        if (git::same_path(worktree.path, path)) {
            return "Git still lists it as a worktree";
        }
    }
    std::error_code ec;
    if (fs::symlink_status(path, ec).type() != fs::file_type::not_found) {
        return "its directory is still there";
    }
    return std::nullopt;
}

// Fails unless `task` names a branch --delete-branch may delete, wherever it
// is checked out. wt keeps no record of the branches it creates, so nothing
// proves a branch is a task's; this rules out those that can't be.
// WARNING: this, with the checks on its worktree or repository, is what
// keeps --delete-branch from deleting a branch that isn't a task's.
void check_abandonable(const Context& ctx,
                       const std::vector<git::Worktree>& worktrees,
                       const std::string& task) {
    // Listed rather than looked up: on a case-insensitive filesystem Git
    // finds "Fix" when only "fix" exists, and deleting "Fix" deletes "fix".
    const auto branches = git::local_branches(ctx.repo);
    if (!git::is_valid_branch_name(ctx.repo, task) ||
        std::ranges::find(branches, task) == branches.end()) {
        throw Error(ErrorKind::user, "no branch named '" + task + "'");
    }
    for (const auto& name : git::integration_candidates(ctx.repo)) {
        if (layout::same_ignoring_case(name, task)) {
            throw Error(ErrorKind::refused,
                        "'" + task +
                            "' is, or could become, the integration branch, "
                            "so wt won't delete it");
        }
    }
    if (auto problem = layout::task_name_problem(task)) {
        throw Error(ErrorKind::refused, "'" + task + "' can't be a wt task (" +
                                            *problem +
                                            "), so wt won't delete it");
    }
    for (const auto& other : branches) {
        if (other != task && layout::same_ignoring_case(other, task)) {
            throw Error(ErrorKind::refused,
                        "'" + task + "' and '" + other +
                            "' differ only in case and may share a ref file, "
                            "so wt won't delete either");
        }
    }
    if (const auto where = git::rebasing_worktree(
            worktrees, git::common_dir(ctx.repo), task)) {
        throw Error(ErrorKind::refused,
                    "'" + task + "' is being rebased in " + where->string() +
                        "; finish or abort the rebase first");
    }
}

// Fails unless wt manages worktrees for this repository: its container must
// exist and be provably wt's. A branch without a worktree shows no sign of
// who created it, so that is all wt can confirm before deleting one.
// WARNING: keeps --delete-branch away from repositories wt doesn't manage.
void check_wt_repository(const Context& ctx,
                         const std::vector<git::Worktree>& worktrees,
                         const std::string& task) {
    const fs::path common = git::common_dir(ctx.repo);
    const fs::path container =
        layout::container_path(worktrees.front().path, common);
    std::error_code ec;
    if (fs::symlink_status(container, ec).type() == fs::file_type::not_found) {
        throw Error(ErrorKind::refused,
                    "'" + task +
                        "' has no worktree, and wt has never created one in "
                        "this repository, so wt can't tell it's a task and "
                        "won't delete it");
    }
    if (auto problem = layout::ownership_problem(container, common)) {
        throw Error(ErrorKind::refused,
                    container.string() +
                        " exists but wt can't confirm it manages it (" +
                        *problem + "), so wt won't delete '" + task + "'");
    }
}

// Removes the configuration Git keeps for the deleted branch `task`, such as
// its upstream, as `git branch -d` does, so a later branch of that name
// doesn't inherit it. Returns what went wrong, if anything.
std::optional<std::string> remove_branch_config(const Context& ctx,
                                                const std::string& task) {
    // Task names contain no regex syntax other than '.'. Variable names can't
    // contain '.', so anchoring the end keeps out the config of a branch
    // whose name only starts with the task's, e.g. "a.b" for "a".
    std::string pattern = "^branch\\.";
    for (const char c : task) {
        pattern += c == '.' ? std::string("\\.") : std::string(1, c);
    }
    pattern += "\\.[^.]+$";
    const git::Result query =
        git::run(ctx.repo, {"config", "--local", "--get-regexp", pattern});
    if (query.exit_code == 1) {
        return std::nullopt;  // there is none
    }
    if (query.exit_code != 0) {
        return git::error_message(query.err);
    }
    const git::Result removed = git::run(
        ctx.repo, {"config", "--local", "--remove-section", "branch." + task});
    if (removed.exit_code != 0) {
        return git::error_message(removed.err);
    }
    return std::nullopt;
}

// Says, on stderr, that the worktree of `task` was removed but its branch
// wasn't deleted, and how to finish.
void report_kept_branch(const Context& ctx, const std::string& task) {
    ctx.err << "wt: the worktree was removed, but branch "
            << ui::printable(task)
            << " was kept; once that's dealt with, delete it with `wt remove "
            << ui::printable(ui::shell_quote(task)) << " --delete-branch`\n";
}

// Deletes the branch of `task`, which must have no worktree, whether or not
// it has landed. Throws for a refusal, before deleting anything. Otherwise
// prints what was deleted and how to restore it and returns 0, or reports the
// failure to stderr and returns its exit code.
int abandon_branch(const Context& ctx, const std::string& task) {
    const std::string ref = "refs/heads/" + task;
    // WARNING: everything is read again here, after the worktree's removal;
    // only the branch as it is now, checked out nowhere, may be deleted.
    const auto worktrees = git::list_worktrees(ctx.repo);
    check_abandonable(ctx, worktrees, task);
    for (const auto& worktree : worktrees) {
        if (worktree.branch &&
            layout::same_ignoring_case(*worktree.branch, task)) {
            throw Error(ErrorKind::refused,
                        "'" + *worktree.branch + "' is checked out in " +
                            worktree.path.string() + ", so wt won't delete '" +
                            task + "'");
        }
    }
    const auto tip = git::resolve_commit(ctx.repo, ref);
    if (!tip) {
        throw Error(ErrorKind::refused,
                    "cannot read branch '" + task + "', so wt won't delete it");
    }

    const std::string shown_task = ui::printable(task);
    // WARNING: from here on the branch may be gone, so every failure is
    // reported here, never thrown.
    std::string problem;
    try {
        // WARNING: never `git branch -D`. Given the old value, Git deletes
        // the branch only if it is still at `tip`, so a commit added since
        // it was read is never lost unreported; --no-deref deletes this ref,
        // never one it points to.
        const git::Result result =
            git::run(ctx.repo, {"update-ref", "--no-deref", "-d", ref, *tip});
        if (result.exit_code != 0) {
            ctx.err << error_prefix(ErrorKind::git)
                    << "git update-ref failed with exit code "
                    << result.exit_code << '\n';
            std::istringstream lines(result.err);
            for (std::string line; std::getline(lines, line);) {
                ctx.err << "  " << ui::printable(line) << '\n';
            }
            if (git::ref_exists(ctx.repo, ref)) {
                ctx.err << "wt: branch " << shown_task << " was kept\n";
                return exit_code(ErrorKind::git);
            }
            problem = "git update-ref failed, but branch " + task +
                      " no longer exists";
        } else if (git::ref_exists(ctx.repo, ref)) {
            problem = "git update-ref reported success, but branch " + task +
                      " still exists";
        }
    } catch (const std::exception& e) {
        problem = std::string("wt could not tell whether branch ") + task +
                  " was deleted: " + e.what();
    }
    // A branch that may be gone gets its restore command either way.
    const std::string restore =
        "git branch " + ui::shell_quote(task) + " " + *tip;
    if (!problem.empty()) {
        ctx.err << error_prefix(ErrorKind::git) << ui::printable(problem)
                << "\nwt: the result could not be verified; check it with "
                   "`git branch --list "
                << ui::printable(ui::shell_quote(task)) << "`. If it's gone, `"
                << ui::printable(restore) << "` restores it.\n";
        return exit_code(ErrorKind::git);
    }

    ctx.out << "Deleted branch " << shown_task << " (was " << tip->substr(0, 7)
            << ")\n"
            << "To restore it: " << ui::printable(restore) << '\n';
    std::optional<std::string> config_problem;
    try {
        config_problem = remove_branch_config(ctx, task);
    } catch (const std::exception& e) {
        config_problem = e.what();
    }
    if (config_problem) {
        ctx.err << error_prefix(ErrorKind::git) << "branch " << shown_task
                << " was deleted, but its configuration wasn't removed: "
                << ui::printable(*config_problem) << '\n'
                << "Remove it with `git config --remove-section "
                << ui::printable(ui::shell_quote("branch." + task)) << "`.\n";
        return exit_code(ErrorKind::git);
    }
    return 0;
}

}  // namespace

RemovalCheck check_removable(const Context& ctx,
                             const std::vector<git::Worktree>& worktrees,
                             const git::Worktree& worktree,
                             const std::string& task, bool delete_ignored,
                             bool forget_missing, std::string_view command) {
    // WARNING: wt must never remove the main worktree, whatever branch it has
    // checked out; it is the user's repository itself, not a task. Git lists
    // it first, so its position, not a branch name, identifies it.
    if (&worktree == &worktrees.front()) {
        throw Error(ErrorKind::refused,
                    "'" + task +
                        "' is checked out in the main worktree, which wt "
                        "never removes");
    }
    check_managed(ctx, worktrees, worktree, task);
    // Removing it would leave the shell (or agent) in a deleted directory.
    if (git::worktree_contains(worktree, ctx.cwd)) {
        throw Error(ErrorKind::refused,
                    "you are inside the worktree for '" + task + "'; run " +
                        std::string(command) + " from somewhere else");
    }
    // Git would follow a symlink here and delete the files it points to.
    std::error_code ec;
    const auto type = fs::symlink_status(worktree.path, ec).type();
    if (type != fs::file_type::not_found) {
        if (ec) {
            throw Error(ErrorKind::refused, "cannot inspect " +
                                                worktree.path.string() + ": " +
                                                ec.message());
        }
        if (type != fs::file_type::directory) {
            throw Error(ErrorKind::refused,
                        worktree.path.string() +
                            " is not a directory, so wt won't remove it");
        }
    }
    if (worktree.locked) {
        throw Error(ErrorKind::refused,
                    "the worktree for '" + task +
                        "' is locked; if it's no longer needed, unlock it with "
                        "`git worktree unlock " +
                        ui::shell_quote(worktree.path.string()) + "`");
    }

    const auto status = git::worktree_status(worktree, /*count_ignored=*/true);
    // WARNING: the flag only ever forgets a record whose path is empty; it
    // must not remove a worktree that is there after all.
    if (forget_missing && status.status != git::Status::missing) {
        throw Error(ErrorKind::refused,
                    worktree.path.string() +
                        " is there, so --forget-missing doesn't apply");
    }
    switch (status.status) {
        case git::Status::unknown:
            throw Error(ErrorKind::refused,
                        "cannot read the status of " + worktree.path.string() +
                            ": " + status.detail + "; not removing it");
        // WARNING: a missing directory may have been moved or be on a drive
        // that isn't mounted, and forgetting the worktree deletes its index
        // and any operation state with Git's record, so it takes asking.
        case git::Status::missing:
            if (!forget_missing) {
                report_missing(ctx, worktree, task, command);
                return {.allowed = false, .missing = true};
            }
            return {.allowed = true, .missing = true};
        case git::Status::bare:
            // Only the main worktree can be bare, and it was refused above.
            throw Error(ErrorKind::internal, "cannot remove a bare repository");
        case git::Status::clean:
        case git::Status::dirty:
            break;
    }
    // WARNING: Git's status, and its own check in `git worktree remove`, skip
    // assume-unchanged and skip-worktree files, so their edits are only
    // caught here.
    const auto hidden = git::hidden_changes(worktree.path);
    const bool ignored_block =
        !status.changes.ignored.empty() && !delete_ignored;
    if (!status.changes.empty() || !hidden.empty() || ignored_block) {
        report_blocked(ctx, task, status.changes, hidden, !delete_ignored);
        return {.allowed = false, .missing = false};
    }
    // WARNING: its state (e.g. a merge message) lives with the worktree and
    // would go with it, even when the files are clean.
    if (auto operation = git::operation_in_progress(worktree.path)) {
        throw Error(ErrorKind::refused,
                    "the worktree for '" + task + "' is in the middle of " +
                        *operation + "; finish or abort it first");
    }
    return {.allowed = true, .missing = false};
}

int remove_worktree(const Context& ctx, const git::Worktree& worktree,
                    const RemovalCheck& check) {
    const std::string shown_path =
        ui::printable(ui::tilde_path(worktree.path, ctx.home));
    // WARNING: from here on Git may have removed something, so every failure
    // is reported here, never thrown; callers rely on an error that escapes
    // meaning nothing was removed.
    std::string problem;
    git::Result result{0, {}, {}};
    try {
        // WARNING: never pass --force. Git's own check for modified and
        // untracked files is the last line of defense if something changed
        // since ours. "--" and the absolute path keep the path from being
        // read as an option.
        result = git::run(ctx.repo,
                          {"worktree", "remove", "--", worktree.path.string()});
    } catch (const std::exception& e) {
        problem = std::string(
                      "wt could not tell whether git worktree remove "
                      "finished: ") +
                  e.what();
    }
    if (problem.empty() && result.exit_code != 0) {
        report_failure(ctx, result, worktree.path);
        return exit_code(ErrorKind::git);
    }
    if (problem.empty()) {
        try {
            if (auto found = removal_problem(ctx, worktree.path)) {
                problem = *found;
            }
        } catch (const std::exception& e) {
            problem = std::string("wt could not check the result: ") + e.what();
        }
        if (!problem.empty()) {
            problem = "git worktree remove reported success, but " + problem;
        }
    }
    if (!problem.empty()) {
        ctx.err << error_prefix(ErrorKind::git) << ui::printable(problem)
                << " (" << shown_path << ")\n"
                << "wt: the result could not be verified; inspect it with "
                   "`wt list` before retrying\n";
        return exit_code(ErrorKind::git);
    }

    if (check.missing) {
        ctx.out << "Worktree " << shown_path
                << " was already gone; removed Git's record of it\n";
    } else {
        ctx.out << "Removed worktree " << shown_path << '\n';
    }
    return 0;
}

int remove_command(const Context& ctx, const std::string& task,
                   bool delete_ignored, bool forget_missing,
                   bool delete_branch) {
    const auto worktrees = git::list_worktrees(ctx.repo);
    const git::Worktree* worktree = nullptr;
    if (delete_branch) {
        // Checked first, so a branch that may not be deleted keeps its
        // worktree too.
        check_abandonable(ctx, worktrees, task);
        worktree = find_branch_worktree(worktrees, task);
        if (worktree == nullptr) {
            if (forget_missing) {
                throw Error(ErrorKind::user,
                            "'" + task +
                                "' has no worktree, so --forget-missing "
                                "doesn't apply");
            }
            check_wt_repository(ctx, worktrees, task);
        }
    } else {
        worktree = &find_task_worktree(ctx.repo, worktrees, task);
    }

    if (worktree != nullptr) {
        const auto check =
            check_removable(ctx, worktrees, *worktree, task, delete_ignored,
                            forget_missing, "wt remove");
        if (!check.allowed) {
            ctx.err << "Nothing was removed.\n";
            return exit_code(ErrorKind::refused);
        }
        if (const int code = remove_worktree(ctx, *worktree, check);
            code != 0) {
            if (delete_branch) {
                ctx.err << "wt: branch " << ui::printable(task)
                        << " was kept\n";
            }
            return code;
        }
    }
    if (!delete_branch) {
        ctx.out << "Kept branch " << ui::printable(task) << '\n';
        return 0;
    }
    if (worktree == nullptr) {
        // Nothing has been removed yet, so a refusal may escape.
        return abandon_branch(ctx, task);
    }
    // WARNING: the worktree is gone now, so every failure is reported here
    // with that said, never thrown; app.cpp takes an error that escapes to
    // mean nothing was removed.
    try {
        return abandon_branch(ctx, task);
    } catch (const Error& e) {
        ctx.err << error_prefix(e.kind()) << ui::printable(e.what()) << '\n';
        report_kept_branch(ctx, task);
        return exit_code(e.kind());
    } catch (const std::exception& e) {
        ctx.err << error_prefix(ErrorKind::internal) << ui::printable(e.what())
                << '\n';
        report_kept_branch(ctx, task);
        return exit_code(ErrorKind::internal);
    }
}

}  // namespace wt
