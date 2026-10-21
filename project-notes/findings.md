# Findings

## `git worktree remove` deletes ignored files without asking

Without `--force`, Git refuses to remove a worktree with modified or untracked files, but silently deletes ignored ones (`.env` files, local config, build output).
`wt remove` counts ignored files and refuses unless `--delete-ignored` is given.

## `git worktree remove` follows a symlink at the worktree path

If the worktree's directory is replaced by a symlink to another copy of it, Git deletes the files the symlink points to, then fails with "Not a directory".
`wt remove` refuses any worktree path that isn't a real directory.

## `git worktree remove` allows removing the current worktree

Git removes the worktree the command runs from, leaving the shell in a deleted directory.
`wt remove` refuses when the working directory is inside the target worktree.

## `git merge` overwrites ignored files without asking

Git refuses to merge when an untracked file is in the way, but silently replaces an ignored one, e.g. a local `.env` when the merged branch tracks a `.env`.
`wt merge` refuses when the task's branch adds a file where the destination has an ignored file.

## Open question: should `wt remove` ever delete the branch?

`wt remove` keeps the task's branch in v1, so cleaning up a merged task still needs a raw `git branch -d`.
Whether `wt` should offer branch deletion at all, and with what safeguards, is undecided.
Squashing or rebasing a task onto `main` makes Git consider its branch unmerged, so `git branch -d` refuses and `-D` is needed even when nothing would be lost.
Any branch-deletion safeguard based on "merged into the integration branch" would hit the same problem; comparing content (e.g. trees) may be needed instead.
