# wt usage notes

Friction found while using `wt` to develop `wt`.

## `wt` can't delete a merged task's branch

`wt merge` lands a task and `wt remove` removes its worktree, but `wt` has no way to delete the merged branch, so finished task branches accumulate.
