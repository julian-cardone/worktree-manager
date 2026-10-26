#pragma once

#include "commands/app.hpp"

#include <string>

namespace wt {

// `wt merge <task>`: merges branch <task> into the integration branch, in the
// worktree that has the integration branch checked out. Refuses unless that
// worktree and the task's, if it has one, are clean and not in the middle of
// an operation. On a conflict, aborts the merge, checks the worktree is
// back where it was, and lists the conflicting files. Refuses a task that is
// being rebased, and explains how to rebase a task that is behind when Git is
// set to fast-forward only. Never resolves conflicts, so it refuses while Git
// is configured with merge options or a default strategy that could.
//
// Only with `cleanup` does it then remove the task's worktree, with the
// checks `wt remove` makes (`delete_ignored` as there), and delete its
// branch, once proven to be contained in the integration branch. Both are
// checked before merging too, so a predictable refusal changes nothing. A
// task that has already landed goes straight to the cleanup, so rerunning
// finishes a cleanup that stopped partway.
int merge_command(const Context& ctx, const std::string& task, bool cleanup,
                  bool delete_ignored);

}  // namespace wt
