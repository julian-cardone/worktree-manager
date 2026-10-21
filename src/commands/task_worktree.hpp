#pragma once

#include "git/worktree.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace wt {

// The worktree that has `branch` checked out, or null if none has. Throws if
// more than one has it.
const git::Worktree* find_branch_worktree(
    const std::vector<git::Worktree>& worktrees, const std::string& branch);

// Tasks are branch names, so a task's worktree is the one with that branch
// checked out. Throws with an explanation if no worktree, or more than one,
// has it.
const git::Worktree& find_task_worktree(
    const std::filesystem::path& dir,
    const std::vector<git::Worktree>& worktrees, const std::string& task);

}  // namespace wt
