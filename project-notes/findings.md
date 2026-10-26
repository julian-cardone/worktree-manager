# Findings

## Open

Known gaps that still need action.

None right now.

## Addressed

Findings `wt` now handles.
Each explains why a check or rule exists; keep it while that check or rule does, and revisit it before relaxing one.

### Interrupted commands are handled by the next invocation, not a signal handler

Ctrl+C reaches both `wt` and the Git command it is running, and Git already cleans up its lock files and leaves any operation it can't finish recorded in the worktree.
Cleanup from a signal handler (e.g. running `git merge --abort`) would mutate state that hasn't been re-validated, at the moment it is least understood.
`wt` therefore keeps the default signal dispositions, and the commands that change an existing worktree (`wt merge`, `wt remove`) refuse one that is mid-operation (`git::operation_in_progress`).
`wt merge` checks for the operation before uncommitted changes, so a conflicted merge left by an interrupted `wt merge` is named, rather than reported as changes to commit or stash, which Git refuses while conflicts remain.
Don't add signal-based rollback without proving it safe against every intermediate state.

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
A general force mode should not be added speculatively.
Reconsider it only if repeated real-world use demonstrates a concrete workflow that cannot be supported safely through more specific behavior.

### Something other than a directory at a worktree's path isn't missing

`wt` used to call a worktree `missing` whenever its path wasn't a directory, including when a file or dangling symlink had replaced it.
`wt merge` treats a missing task worktree as having nothing left to lose, so it landed such a task without question.
Only a path with nothing at it is now `missing`; anything else is `unknown`, which `wt status` and `wt merge` refuse to treat as safe.

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

### Inherited `GIT_DIR` silently redirects every check

Git exports `GIT_DIR`, `GIT_WORK_TREE`, and `GIT_INDEX_FILE` to hooks, `git rebase --exec` commands, and `!` aliases, and they override `git -C`.
`wt` run from there checked the main checkout instead of the task's worktree, so `wt status` called a dirty worktree clean and `wt remove` deleted its ignored files without asking.
`git::run` now drops Git's per-repository variables (`git rev-parse --local-env-vars`, minus the config ones) before running Git.
An explicit `GIT_DIR` is still honored when choosing the repository, and `wt` refuses when it disagrees with the current directory.

### Newline-separated Git output misreads paths that contain newlines

`operation_in_progress` asked `git rev-parse` for all its state paths at once and split the answer on newlines.
Under a repository path containing a newline, the paths no longer lined up, so `wt remove` deleted a worktree in the middle of a bisect.
Values from Git commands are now read with `-z` where Git offers it, and otherwise one per command (`git::single_value`), never as a first line.

### Git rewrites the Unicode in paths it is given on macOS

With `core.precomposeunicode` (the macOS default), Git stores the path passed to `git worktree add` with its Unicode precomposed (NFC), while the filesystem keeps the name as created, and the main worktree's path comes back with the on-disk bytes.
Under a folder whose name is decomposed (NFD), `wt new` created the worktree and then reported failure because Git didn't list it at the path wt gave, and `wt remove` refused it as not created by wt.
Fixing only that comparison would also have let the "you are inside this worktree" check miss, since the working directory and Git's path differ in the same way.
`wt` now compares paths with `git::same_path`, which precomposes both sides as Git does (iconv `UTF-8-MAC`) on macOS only, where the filesystem treats both forms as one name; it stays lexical and doesn't follow symlinks.

### `git merge` overwrites ignored files without asking

Git refuses to merge when an untracked file is in the way, but silently replaces an ignored one, e.g. a local `.env` when the merged branch tracks a `.env`.
`wt merge` refuses when the task's branch adds a file where the destination has an ignored file.

### Branch names that differ only in case share ref files

On a case-insensitive filesystem, loose refs are files, so `Fix` and `fix` are one file, and `Fix` and `fix/login` need `refs/heads/Fix` as both a file and a directory.
Git catches this only by accident: a loose ref or reflog happens to be in the way, but a packed ref with reflogs off (`core.logAllRefUpdates=false`, the default in bare repositories) is not, so Git creates the second branch, and later updates to either then fail or shadow the other.
When Git reports `core.ignorecase`, `wt new` refuses a branch that equals, contains, or is contained by an existing one ignoring case, and names the existing branch.

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

### Git's exit code doesn't prove the operation happened

Hooks run after Git has done its work and can undo or change it without Git failing: a `post-checkout` hook can switch a new worktree to another branch, and a `post-merge` hook can reset the merge away.
`wt` used to report success on exit code 0 alone, so `wt merge` printed "✓ Merged" for a task `main` didn't contain, and `wt new` printed the path of a worktree on the wrong branch.
`wt new`, `wt merge`, and `wt remove` now confirm the result from Git's and the filesystem's current state before reporting success (see `AGENTS.md` section 3).
Matching HEAD's commit isn't enough: a hook can point HEAD at another branch, or detach it, at the same commit, so `wt merge` also checks the integration branch's worktree still has that branch checked out, after a successful merge and before calling a failed one undone.
Containing the task isn't enough either: a hook can reset the integration branch to the task's tip, dropping its own commits, so `wt merge` also checks it still contains its pre-merge commit.

### A failed `wt new` can't tell what it created from what was already there

`wt new` checks that the branch and path are free, but another process can take either before `git worktree add` runs, and Git then fails with that state in place.
`wt new` used to call whatever existed afterwards "left behind" by Git and suggest `wt remove` and `git branch -d` for it, including a directory that wasn't a worktree at all.
It now reports the full current state, removes nothing, and suggests cleanup only when the branch is still at the commit `wt` started it from and the path is absent or a worktree with the task's branch checked out.

### A failed check during conflict rollback hid an in-progress merge

`wt merge` listed the conflicting files before running `git merge --abort`, so if listing them failed, the abort never ran and the merge was left in progress behind a bare Git error.
A failed `MERGE_HEAD` or `HEAD` read after the abort was reported the same way, without saying the merge might remain.
`wt merge` now aborts even when the conflicts can't be listed, counts any check it can't run as a failed rollback, and when it can't verify the rollback says the integration branch's worktree may still be mid-merge and not to run other `wt` commands until it has been inspected.

### Ancestry is enough to prove a landed branch is safe to delete

`wt merge` never squashes, so after it lands a task (by fast-forward or merge commit) the task's tip is always an ancestor of the integration branch.
`wt merge --cleanup` relies on that: it deletes the branch only when its current tip is contained in the integration branch, then lets `git branch -d` check again, so a branch moved in between is kept.
Squashed or cherry-picked branches landed outside `wt` fail the check and are kept; comparing content to delete those is deliberately not done.
`git branch -d` also checks against the branch's upstream when one exists, so `--cleanup` refuses up front when an upstream other than the integration branch lacks the task's commits.

### Git's messages may be translated

Git localizes its error messages, including the `fatal: ` and `error: ` prefixes, so matching their English text fails silently in other locales.
`wt` used to recognize a Git too old for `worktree list -z` by its "unknown switch" message; in a German locale it showed Git's raw usage error instead of saying Git 2.36 is required.
`wt` now decides from exit codes and machine-readable output (`--porcelain`, `-z`, `--format`, `--quiet`) and uses Git's messages only as diagnostics; the old-Git check keys on the usage-error exit code 129, and on Git having exited rather than been killed, since a SIGHUP is reported as 129 too.
The e2e tests pin `LC_ALL=C`, since some assert Git's English diagnostics.
