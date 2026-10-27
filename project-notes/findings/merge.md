# Merge findings

Why `wt merge` and `wt merge --cleanup` refuse, verify, roll back, and delete branches the way they do, including the post-mutation verification every mutating command follows.
See [findings.md](../findings.md) for how findings are organized.

## Addressed

### Merge configuration let `wt merge` resolve conflicts

Git applies `branch.<name>.mergeOptions` to every `git merge` run on that branch, and uses `pull.twohead` as the default strategy of every `git merge`, not only `git pull`.
With `-Xours` in the integration branch's `mergeOptions`, or `pull.twohead=ours`, a conflicting task merged with "✓ Merged" and the task's change was silently dropped; the task is still an ancestor, so the post-merge check passed and `--cleanup` would delete its branch.
`wt merge` now refuses whenever either setting has any value and there is something to merge.
It deliberately doesn't override them with `-c` or `-s`, which would silently ignore the repository's configuration, and doesn't sort values into safe and unsafe, which could get one wrong.
`rerere.autoupdate` reapplies a recorded resolution but still stops the merge, so `wt` aborts it as usual.
Custom merge drivers in `.gitattributes` are repository policy and out of scope.

### `git merge` overwrites ignored files without asking

Git refuses to merge when an untracked file is in the way, but silently replaces an ignored one, e.g. a local `.env` when the merged branch tracks a `.env`.
`wt merge` refuses when the task's branch adds a file where the destination has an ignored file.

### A branch being rebased looks free and stale

Mid-rebase, the worktree is detached and the branch still points at its old commits, so `git worktree list` doesn't show the branch as checked out anywhere.
`wt merge` used to skip the task's uncommitted-changes check and land the pre-rebase commits.
It now reads each worktree's rebase state (`rebase-merge`/`rebase-apply` `head-name`, as Git itself does) and refuses.

### Branches `git rebase --update-refs` will move look free and stale too

`git rebase --update-refs` also moves other branches that point into the rebased commits, and records them in the rebasing worktree's `rebase-merge/update-refs`, not in `head-name`.
Until the rebase finishes, such a branch still points at its old commits and isn't checked out anywhere, so `wt merge` landed its pre-rebase commits and `--cleanup` then failed in `git branch -d`, which, like the rest of Git, treats the branch as in use.
The rebase-state check now also reads `update-refs` (as Git itself does) and refuses for a branch listed there, and refuses when the file can't be parsed.

### Some stopped operations leave no ref and no changes

`git am` stopped on a patch, a multi-commit cherry-pick or revert between steps, and a bisect leave the branch checked out and the files clean, with only files in the worktree's Git directory (`rebase-apply`, `sequencer`, `BISECT_LOG`) to show it.
`wt` looked only for `MERGE_HEAD`, `CHERRY_PICK_HEAD`, and `REVERT_HEAD`, so `wt merge` merged into `main` mid-`git am` (after which `git am --abort` refused to rewind), and landed a task whose own worktree was mid-operation.
`wt` now also checks those paths (via `git rev-parse --git-path`, as `git status` does), and `wt merge` requires the task's worktree, like the integration branch's, to be clean and idle.

### Merging `main` into a task makes `main`'s history run backwards

When a task has merged `main` in, landing it fast-forwards, so `main`'s first-parent history goes through "Merge branch 'main' into …".
`wt` follows Git's merge settings rather than picking a strategy; this repository uses `merge.ff=only` and rebases tasks (see `docs/agent-workflows/landing-tasks.md`).
`wt merge` explains the rebase step when `merge.ff=only` blocks a task that is behind.

### A failed check after a change looked like nothing had happened

`wt new`, `wt remove`, and `wt merge` verify Git's result after mutating, but an exception from that verification (or from `git::run` after Git started) escaped to the top level as a bare one-line error.
A `wt merge` whose `post-merge` hook broke the repository's config printed only `wt: git error: bad config line`, although `main` had moved.
`wt merge --cleanup` also reported "its worktree is still there" and "its branch was kept" from flags rather than Git, which was wrong for a task with no worktree or a branch deleted before the check failed.
Each command now reports every failure from its first mutation on itself and says the result is unverified, and the cleanup report reads what is left from Git.
That makes an error that escapes a command proof nothing was changed, so `app.cpp` says so (`Nothing was removed.` and so on); a new mutation must keep that true.

### Git's exit code doesn't prove the operation happened

Hooks run after Git has done its work and can undo or change it without Git failing: a `post-checkout` hook can switch a new worktree to another branch, and a `post-merge` hook can reset the merge away.
`wt` used to report success on exit code 0 alone, so `wt merge` printed "✓ Merged" for a task `main` didn't contain, and `wt new` printed the path of a worktree on the wrong branch.
`wt new`, `wt merge`, and `wt remove` now confirm the result from Git's and the filesystem's current state before reporting success (see `AGENTS.md` section 2).
Matching HEAD's commit isn't enough: a hook can point HEAD at another branch, or detach it, at the same commit, so `wt merge` also checks the integration branch's worktree still has that branch checked out, after a successful merge and before calling a failed one undone.
Containing the task isn't enough either: a hook can reset the integration branch to the task's tip, dropping its own commits, so `wt merge` also checks it still contains its pre-merge commit.

### A failed check during conflict rollback hid an in-progress merge

`wt merge` listed the conflicting files before running `git merge --abort`, so if listing them failed, the abort never ran and the merge was left in progress behind a bare Git error.
A failed `MERGE_HEAD` or `HEAD` read after the abort was reported the same way, without saying the merge might remain.
`wt merge` now aborts even when the conflicts can't be listed, counts any check it can't run as a failed rollback, and when it can't verify the rollback says the integration branch's worktree may still be mid-merge and not to run other `wt` commands until it has been inspected.

### Ancestry is enough to prove a landed branch is safe to delete

`wt merge` never squashes, so after it lands a task (by fast-forward or merge commit) the task's tip is always an ancestor of the integration branch.
`wt merge --cleanup` relies on that: it deletes the branch only when its current tip is contained in the integration branch, then lets `git branch -d` check again, so a branch moved in between is kept.
Squashed or cherry-picked branches landed outside `wt` fail the check and are kept; comparing content to delete those is deliberately not done.
`git branch -d` also checks against the branch's upstream when one exists, so `--cleanup` refuses up front when an upstream other than the integration branch lacks the task's commits.
