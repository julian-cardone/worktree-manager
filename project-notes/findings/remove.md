# Remove findings

Why `wt remove` (and the worktree removal in `wt merge --cleanup`) refuses what it does.
See [findings.md](../findings.md) for how findings are organized.

## Addressed

### `git worktree remove` deletes edits Git has been told not to look at

`git status` doesn't report changes to files marked `skip-worktree` (sparse checkouts, `git update-index --skip-worktree`) or `assume-unchanged`, a common way to keep local config edits out of commits.
`git worktree remove` checks the same way, so `wt remove` deleted such edits with exit code 0.
`wt remove`, and so `wt merge --cleanup`, now lists these files (`git ls-files -v`) and refuses when one's content differs from the index (`git::hidden_changes`); `--delete-ignored` doesn't override it.
Content is compared with `git hash-object`, which applies the same filters as `git add` and writes nothing, so line-ending conversion alone isn't a change; a symlink is compared by its target, and anything else in a file's place counts as changed.
A flagged file that is absent, as in a sparse checkout, loses nothing and is allowed, as is a flagged submodule's empty, uninitialized directory.
`wt status` still reports what Git reports, so it can call such a worktree clean; the check guards the deletion, not the read.

### `git worktree remove` deletes ignored files without asking

Without `--force`, Git refuses to remove a worktree with modified or untracked files, but silently deletes ignored ones (`.env` files, local config, build output).
`wt remove` names the ignored files it would delete and refuses unless `--delete-ignored` is given; `wt merge --cleanup` makes the same check before merging.
An ignored file created by another process between that check and `git worktree remove` is still deleted.
Git offers no lock covering both steps, so this is an accepted residual race: `wt` keeps the window short, never passes `--force`, and deliberately adds no locking or filesystem machinery of its own.

### `wt remove` deliberately has no force mode

`wt remove` never passes `--force` to Git and exposes no general force option.
Refusals are intended to preserve uncertain or valuable state and provide a safe recovery path instead.
`--delete-ignored` is deliberately narrow: it authorizes only ignored-file deletion and bypasses no other safety check.
`--forget-missing` is narrow in the same way: it authorizes only forgetting a worktree whose path is empty.
`--delete-branch` authorizes only deleting the task's branch once its worktree is gone; every check on the worktree still applies.
A general force mode should not be added speculatively.
Reconsider it only if repeated real-world use demonstrates a concrete workflow that cannot be supported safely through more specific behavior.

### A missing worktree directory may only be moved or unmounted

When nothing is at a worktree's path, it may have been deleted, moved by hand, or be on a drive that isn't mounted, and nothing tells these apart.
Forgetting Git's record of it also deletes its index (staged changes) and any in-progress operation state, and stops `git worktree repair` from reconnecting a moved directory.
`wt remove` used to forget such a worktree automatically, as `git worktree remove` does, and so did `wt merge --cleanup`.
`wt remove` now refuses and explains `git worktree repair`; only `--forget-missing` forgets the record, and only when nothing at all is at the path and the worktree isn't locked.
`wt merge --cleanup` never forgets it; the user runs `wt remove --forget-missing` first, keeping the acknowledgement in one place.

### `git worktree remove` follows a symlink at the worktree path

If the worktree's directory is replaced by a symlink to another copy of it, Git deletes the files the symlink points to, then fails with "Not a directory".
`wt remove` refuses any worktree path that isn't a real directory.

### `git worktree remove` allows removing the current worktree

Git removes the worktree the command runs from, leaving the shell in a deleted directory.
`wt remove` refuses when the working directory is inside the target worktree.

### Abandoning a task needed raw Git, and Git can't prove whose a branch is

`wt remove` kept branches and `wt merge --cleanup` deleted only landed ones, so abandoning a task meant `git branch -D`, which deletes whatever the branch points to by then and doesn't check what it is.
`wt remove --delete-branch` now does it: it checks the branch before removing anything, removes the worktree with every usual check, then rereads the state and deletes the branch with `git update-ref -d <ref> <tip>`, so a branch that moved after the check is kept rather than lost unreported, and prints the command that restores it.
A worktree inside the verified container is the only evidence that `wt` created a branch; without one, Git records nothing and `wt` keeps no database (`AGENTS.md`).
So without a worktree it refuses unless the repository's container exists and is verifiably its own, and in every case refuses the integration branch and every branch that could become it, names `wt new` wouldn't create, branches checked out or being rebased anywhere, and names that match another branch, or the given spelling's ref file, only ignoring case.
Any other branch named explicitly is accepted: an accepted limit, since recording ownership would add a second state store that can disagree with Git.
A task worktree with a detached HEAD isn't found by branch, so deleting its branch would have left a worktree `wt` can't remove; `--delete-branch` refuses while one is at the task's folder in the container.
