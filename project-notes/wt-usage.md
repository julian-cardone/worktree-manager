# wt usage notes

Friction found while using `wt` to develop `wt`.

## No way to land a task

`wt` can create a task's worktree, but has no command to bring a finished task into the integration branch.
Landing `wt new` on `main` needed raw `git commit`, `git switch main`, and `git merge --ff-only`.
`wt remove` now removes the worktree, but the merged branch still has to be deleted with raw `git branch -d`.
