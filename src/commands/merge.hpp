#pragma once

#include "commands/app.hpp"

#include <string>

namespace wt {

// `wt merge <task>`: merges branch <task> into the integration branch, in the
// worktree that has the integration branch checked out. Refuses unless that
// worktree is clean. On a conflict, aborts the merge, checks the worktree is
// back where it was, and lists the conflicting files. Refuses a task that is
// being rebased, and explains how to rebase a task that is behind when Git is
// set to fast-forward only. Never resolves conflicts and never removes the
// task's worktree or branch.
int merge_command(const Context& ctx, const std::string& task);

}  // namespace wt
