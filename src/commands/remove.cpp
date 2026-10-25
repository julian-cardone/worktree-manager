#include "commands/remove.hpp"

#include "commands/task_worktree.hpp"
#include "error.hpp"
#include "git/git.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "layout/container.hpp"
#include "ui/format.hpp"

#include <algorithm>
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
               const std::string& what) {
    if (count != 0) {
        lines.push_back(std::to_string(count) + " " + what +
                        (count == 1 ? " file" : " files"));
    }
}

// The count, then the first few paths, e.g. "1 untracked file" + "new.txt".
void add_listed(std::vector<std::string>& lines,
                const std::vector<std::string>& paths,
                const std::string& what) {
    // Enough to spot what matters without flooding the terminal.
    constexpr std::size_t kMaxShown = 10;

    add_count(lines, paths.size(), what);
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

// Explains what removal would destroy, on stderr. Untracked and ignored files
// are named, not just counted: Git has no copy of them, and deciding to pass
// --delete-ignored means having seen whether something like a .env file is
// among them.
void report_blocked(const Context& ctx, const std::string& task,
                    const git::Changes& changes, bool show_ignored) {
    std::vector<std::string> lines;
    add_count(lines, changes.staged, "staged");
    add_count(lines, changes.modified, "modified");
    add_listed(lines, changes.untracked, "untracked");
    add_count(lines, changes.conflicts, "conflicted");
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
                        " is there, so --forget-missing doesn't apply; "
                        "nothing was removed");
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
    const bool ignored_block =
        !status.changes.ignored.empty() && !delete_ignored;
    if (!status.changes.empty() || ignored_block) {
        report_blocked(ctx, task, status.changes, !delete_ignored);
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
    // WARNING: never pass --force. Git's own check for modified and untracked
    // files is the last line of defense if something changed since ours.
    // "--" and the absolute path keep the path from being read as an option.
    const git::Result result = git::run(
        ctx.repo, {"worktree", "remove", "--", worktree.path.string()});
    if (result.exit_code != 0) {
        report_failure(ctx, result, worktree.path);
        return exit_code(ErrorKind::git);
    }
    if (auto problem = removal_problem(ctx, worktree.path)) {
        ctx.err << error_prefix(ErrorKind::git)
                << "git worktree remove reported success, but "
                << ui::printable(*problem) << " at " << shown_path << '\n'
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
                   bool delete_ignored, bool forget_missing) {
    const auto worktrees = git::list_worktrees(ctx.repo);
    const git::Worktree& worktree =
        find_task_worktree(ctx.repo, worktrees, task);
    const auto check =
        check_removable(ctx, worktrees, worktree, task, delete_ignored,
                        forget_missing, "wt remove");
    if (!check.allowed) {
        return exit_code(ErrorKind::refused);
    }
    if (const int code = remove_worktree(ctx, worktree, check); code != 0) {
        return code;
    }
    ctx.out << "Kept branch " << ui::printable(task) << '\n';
    return 0;
}

}  // namespace wt
