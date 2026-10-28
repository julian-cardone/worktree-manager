#pragma once

#include "commands/app.hpp"
#include "git/worktree.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace wt {

// `wt remove <task>`: removes the worktree wt created for <task>, refusing if
// that could lose anything not committed. Ignored files also block removal
// unless `delete_ignored` is set, and a worktree whose directory is missing
// unless `forget_missing` is. Keeps the branch unless `delete_branch` is set;
// then, once the worktree is verified gone, or if the task had none, deletes
// the branch whether or not it has landed.
int remove_command(const Context& ctx, const std::string& task,
                   bool delete_ignored, bool forget_missing,
                   bool delete_branch);

// Whether `wt remove` may remove a task's worktree, from check_removable.
struct RemovalCheck {
    bool allowed;  // if not, the reason has already been printed to stderr
    bool missing;  // the directory is gone; only Git's record of it is left
};

// Runs every check `wt remove` makes before removing `worktree`, the worktree
// for `task` among `worktrees`. Throws a wt::Error for most refusals; for
// files that would be lost, or a missing directory without `forget_missing`,
// prints why to stderr and returns allowed=false. `command` names the wt
// command for the user, e.g. "wt remove".
RemovalCheck check_removable(const Context& ctx,
                             const std::vector<git::Worktree>& worktrees,
                             const git::Worktree& worktree,
                             const std::string& task, bool delete_ignored,
                             bool forget_missing, std::string_view command);

// Removes `worktree`, which check_removable has just allowed, and confirms
// it is gone. Prints what was removed to stdout and returns 0, or reports the
// failure to stderr and returns its exit code. Never deletes the branch.
int remove_worktree(const Context& ctx, const git::Worktree& worktree,
                    const RemovalCheck& check);

}  // namespace wt
