#include "commands/new.hpp"

#include "git/git.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "layout/container.hpp"
#include "layout/task.hpp"
#include "ui/format.hpp"

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

bool path_exists(const fs::path& path) {
    std::error_code ec;
    // symlink_status so a dangling symlink still counts as taken.
    const fs::file_status st = fs::symlink_status(path, ec);
    if (st.type() == fs::file_type::not_found) {
        return false;
    }
    if (ec) {
        throw std::runtime_error("cannot inspect " + path.string() + ": " +
                                 ec.message());
    }
    return true;
}

// Fails if the target path is already taken in any way Git or the filesystem
// would notice. Git checks too, but its errors are less clear. Only call this
// once the container is verified, so wt never looks inside one it doesn't own.
void check_target_free(const std::vector<git::Worktree>& worktrees,
                       const fs::path& target) {
    const bool exists = path_exists(target);
    for (const auto& worktree : worktrees) {
        if (worktree.path != target) {
            continue;
        }
        if (exists) {
            throw std::runtime_error(
                target.string() + " is already the worktree for " +
                (worktree.branch ? "branch " + *worktree.branch
                                 : std::string("a detached HEAD")));
        }
        throw std::runtime_error(
            "Git already has a worktree registered at " + target.string() +
            " although its directory is gone; run `git worktree prune` to "
            "forget it");
    }
    if (exists) {
        throw std::runtime_error(target.string() + " already exists");
    }
}

// Git can fail after creating the branch or directory (e.g. when a
// post-checkout hook fails). Say exactly what was left behind, but don't
// remove it: it may already hold the hook's output, and cleanup that guesses
// wrong would lose more than it saves.
void report_failure(const Context& ctx, const git::Result& result,
                    const std::string& branch, const fs::path& target) {
    ctx.err << "wt: error: git worktree add failed with exit code "
            << result.exit_code << '\n';
    std::istringstream lines(result.err);
    for (std::string line; std::getline(lines, line);) {
        ctx.err << "  " << ui::printable(line) << '\n';
    }

    bool branch_left = false;
    bool path_left = false;
    try {
        branch_left = git::ref_exists(ctx.cwd, "refs/heads/" + branch);
        path_left = path_exists(target);
    } catch (const std::exception& e) {
        ctx.err << "wt: warning: cannot check what git left behind: "
                << ui::printable(e.what()) << '\n';
        return;
    }
    if (!branch_left && !path_left) {
        return;
    }
    ctx.err << "wt: git did not finish, but left behind:\n";
    if (branch_left) {
        ctx.err << "  branch " << ui::printable(branch) << '\n';
    }
    if (path_left) {
        ctx.err << "  worktree " << ui::printable(target.string()) << '\n';
    }
    ctx.err << "wt: nothing was removed; if you don't need them, remove them "
               "with `git worktree remove` and `git branch -d`\n";
}

}  // namespace

int new_command(const Context& ctx, const std::string& task) {
    if (auto problem = layout::task_name_problem(task)) {
        throw std::runtime_error("invalid task name '" + task +
                                 "': " + *problem);
    }
    // Also confirms we're inside a repository, with Git's own error if not.
    const auto worktrees = git::list_worktrees(ctx.cwd);
    if (worktrees.empty()) {
        throw std::runtime_error("git reported no worktrees");
    }
    if (!git::is_valid_branch_name(ctx.cwd, task)) {
        throw std::runtime_error("invalid task name '" + task +
                                 "': Git doesn't accept it as a branch name");
    }
    const std::string& branch = task;
    const std::string base = git::integration_branch(ctx.cwd);

    // Always the main worktree, even when run from a linked one, so every
    // worktree of the repository ends up in the same container.
    const fs::path common = git::common_dir(ctx.cwd);
    const fs::path container =
        layout::container_path(worktrees.front().path, common);
    const fs::path target = container / layout::task_folder_name(task);

    if (git::ref_exists(ctx.cwd, "refs/heads/" + branch)) {
        throw std::runtime_error("branch '" + branch + "' already exists");
    }
    layout::prepare_container(container, common);
    check_target_free(worktrees, target);

    if (git::ref_exists(ctx.cwd, "refs/remotes/origin/" + branch)) {
        ctx.err << "wt: warning: origin/" << ui::printable(branch)
                << " already exists; the new branch starts from "
                << ui::printable(base) << " and doesn't track it\n";
    }

    // "--" and the full ref keep the path and base from being read as
    // options or as an ambiguous name (e.g. a tag also called "main").
    const git::Result result =
        git::run(ctx.cwd, {"worktree", "add", "-b", branch, "--",
                           target.string(), "refs/heads/" + base});
    if (result.exit_code != 0) {
        report_failure(ctx, result, branch, target);
        return 1;
    }

    ctx.err << "wt: created branch " << ui::printable(branch) << " from "
            << ui::printable(base) << '\n';
    // Unsanitized on purpose: `cd "$(wt new foo)"` needs the exact path.
    ctx.out << target.string() << '\n';
    return 0;
}

}  // namespace wt
