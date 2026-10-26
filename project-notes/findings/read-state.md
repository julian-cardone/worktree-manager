# Read-state findings

Why `wt status` and `wt list` describe worktrees the way they do.
See [findings.md](../findings.md) for how findings are organized.

## Addressed

### `wt status` and `wt list` misdescribed worktrees mid-operation

During a stopped `git rebase` in a task's worktree, `wt status <task>` said the branch "isn't checked out in any worktree", and `wt list` showed only a detached HEAD.
A worktree mid-bisect, cherry-pick, or `git am` showed `Status: clean` with no mention of the operation, although `wt merge` and `wt remove` refuse it.
`wt status` now finds the worktree whose rebase is rewriting the task (`rebasing_worktree`, confirmed by that worktree's own `git::rebased_branch`), and says when a `--update-refs` rebase elsewhere will only move it.
Both commands show `operation_in_progress`; when it can't be read, they show the state as unknown rather than leave a bare `clean`.

### Something other than a directory at a worktree's path isn't missing

`wt` used to call a worktree `missing` whenever its path wasn't a directory, including when a file or dangling symlink had replaced it.
`wt merge` treats a missing task worktree as having nothing left to lose, so it landed such a task without question.
Only a path with nothing at it is now `missing`; anything else is `unknown`, which `wt status` and `wt merge` refuse to treat as safe.
