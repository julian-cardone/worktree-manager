#include "commands/remove.hpp"

#include "commands/task_worktree.hpp"
#include "git/git.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "layout/container.hpp"
#include "ui/format.hpp"

#include <algorithm>
#include <filesystem>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
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
    const fs::path common = git::common_dir(ctx.cwd);
    const fs::path container =
        layout::container_path(worktrees.front().path, common);
    if (worktree.path.parent_path() != container) {
        throw std::runtime_error(
            "the worktree for '" + task + "' at " + worktree.path.string() +
            " wasn't created by wt, so wt won't remove it");
    }

    // A container that's gone can't hold the worktree's files, so there is
    // nothing of anyone's to delete, only Git's record of the worktree.
    std::error_code ec;
    if (fs::symlink_status(container, ec).type() == fs::file_type::not_found) {
        return;
    }
    if (auto problem = layout::ownership_problem(container, common)) {
        throw std::runtime_error(
            container.string() +
            " exists but wt can't confirm it manages it (" + *problem +
            "), so wt won't remove anything from it");
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

// Explains what removal would destroy, on stderr. Ignored files are named,
// not just counted, so deciding to pass --delete-ignored means having seen
// whether something like a .env file is among them.
void report_blocked(const Context& ctx, const std::string& task,
                    const git::Changes& changes, bool show_ignored) {
    // Enough to spot what matters without flooding the terminal.
    constexpr std::size_t kMaxIgnoredShown = 10;

    std::vector<std::string> lines;
    add_count(lines, changes.staged, "staged");
    add_count(lines, changes.modified, "modified");
    add_count(lines, changes.untracked, "untracked");
    add_count(lines, changes.conflicts, "conflicted");
    if (show_ignored) {
        add_count(lines, changes.ignored.size(), "ignored");
    }

    ctx.err << "wt: error: cannot remove " << ui::printable(task) << "\n\n";
    for (const auto& line : lines) {
        ctx.err << "  " << line << '\n';
    }
    if (show_ignored) {
        const std::size_t shown =
            std::min(changes.ignored.size(), kMaxIgnoredShown);
        for (std::size_t i = 0; i < shown; ++i) {
            ctx.err << "    " << ui::printable(changes.ignored[i]) << '\n';
        }
        if (changes.ignored.size() > shown) {
            ctx.err << "    ... and " << changes.ignored.size() - shown
                    << " more\n";
        }
    }
    ctx.err << '\n';
    if (!changes.empty()) {
        ctx.err << "Commit, stash, or remove these changes first.\n";
    }
    if (show_ignored && !changes.ignored.empty()) {
        ctx.err << "Ignored files are deleted with the worktree; move any you "
                   "need, then rerun with --delete-ignored.\n";
    }
}

void report_failure(const Context& ctx, const git::Result& result,
                    const fs::path& path) {
    ctx.err << "wt: error: git worktree remove failed with exit code "
            << result.exit_code << '\n';
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

}  // namespace

int remove_command(const Context& ctx, const std::string& task,
                   bool delete_ignored) {
    const auto worktrees = git::list_worktrees(ctx.cwd);
    const git::Worktree& worktree =
        find_task_worktree(ctx.cwd, worktrees, task);
    const std::string shown_path =
        ui::printable(ui::tilde_path(worktree.path, ctx.home));

    // Git lists the main worktree first; it is the repository itself.
    if (&worktree == &worktrees.front()) {
        throw std::runtime_error("'" + task +
                                 "' is checked out in the main worktree, "
                                 "which wt never removes");
    }
    check_managed(ctx, worktrees, worktree, task);
    // Removing it would leave the shell (or agent) in a deleted directory.
    if (git::worktree_contains(worktree, ctx.cwd)) {
        throw std::runtime_error("you are inside the worktree for '" + task +
                                 "'; run wt remove from somewhere else");
    }
    // Git would follow a symlink here and delete the files it points to.
    std::error_code ec;
    const auto type = fs::symlink_status(worktree.path, ec).type();
    if (type != fs::file_type::not_found) {
        if (ec) {
            throw std::runtime_error("cannot inspect " +
                                     worktree.path.string() + ": " +
                                     ec.message());
        }
        if (type != fs::file_type::directory) {
            throw std::runtime_error(
                worktree.path.string() +
                " is not a directory, so wt won't remove it");
        }
    }
    if (worktree.locked) {
        throw std::runtime_error(
            "the worktree for '" + task +
            "' is locked; if it's no longer needed, unlock it with "
            "`git worktree unlock " +
            worktree.path.string() + "`");
    }

    const auto status = git::worktree_status(worktree, /*count_ignored=*/true);
    switch (status.status) {
        case git::Status::unknown:
            throw std::runtime_error("cannot read the status of " +
                                     worktree.path.string() + ": " +
                                     status.detail + "; not removing it");
        case git::Status::missing:
            break;
        case git::Status::bare:
            // Only the main worktree can be bare, and it was refused above.
            throw std::runtime_error("cannot remove a bare repository");
        case git::Status::clean:
        case git::Status::dirty: {
            const bool ignored_block =
                !status.changes.ignored.empty() && !delete_ignored;
            if (!status.changes.empty() || ignored_block) {
                report_blocked(ctx, task, status.changes, !delete_ignored);
                return 1;
            }
            break;
        }
    }

    // WARNING: never pass --force. Git's own check for modified and untracked
    // files is the last line of defense if something changed since ours.
    // "--" and the absolute path keep the path from being read as an option.
    const git::Result result =
        git::run(ctx.cwd, {"worktree", "remove", "--", worktree.path.string()});
    if (result.exit_code != 0) {
        report_failure(ctx, result, worktree.path);
        return 1;
    }

    if (status.status == git::Status::missing) {
        ctx.out << "Worktree " << shown_path
                << " was already gone; removed Git's record of it\n";
    } else {
        ctx.out << "Removed worktree " << shown_path << '\n';
    }
    ctx.out << "Kept branch " << ui::printable(task) << '\n';
    return 0;
}

}  // namespace wt
