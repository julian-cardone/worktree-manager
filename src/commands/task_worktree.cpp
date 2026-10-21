#include "commands/task_worktree.hpp"

#include "git/repo.hpp"

#include <stdexcept>

namespace wt {

const git::Worktree* find_branch_worktree(
    const std::vector<git::Worktree>& worktrees, const std::string& branch) {
    const git::Worktree* found = nullptr;
    for (const auto& worktree : worktrees) {
        if (worktree.branch != branch) {
            continue;
        }
        // Only possible if someone forced Git to check the branch out twice.
        if (found != nullptr) {
            throw std::runtime_error("branch '" + branch +
                                     "' is checked out in more than one "
                                     "worktree; `wt list` shows them");
        }
        found = &worktree;
    }
    return found;
}

const git::Worktree& find_task_worktree(
    const std::filesystem::path& dir,
    const std::vector<git::Worktree>& worktrees, const std::string& task) {
    if (const auto* found = find_branch_worktree(worktrees, task)) {
        return *found;
    }
    if (git::is_valid_branch_name(dir, task) &&
        git::ref_exists(dir, "refs/heads/" + task)) {
        throw std::runtime_error("branch '" + task +
                                 "' isn't checked out in any worktree");
    }
    throw std::runtime_error("no worktree for task '" + task +
                             "'; `wt list` shows them all");
}

}  // namespace wt
