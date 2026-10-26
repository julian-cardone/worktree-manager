# Git runtime findings

How `wt` runs Git and reads its results, across all commands.
See [findings.md](../findings.md) for how findings are organized.

## Addressed

### Interrupted commands are handled by the next invocation, not a signal handler

Ctrl+C reaches both `wt` and the Git command it is running; Git cleans up its lock files and records most operations it can't finish in the worktree.
Cleanup from a signal handler (e.g. running `git merge --abort`) would mutate state that hasn't been re-validated, at the moment it is least understood.
`wt` therefore keeps the default signal dispositions, and the commands that change an existing worktree (`wt merge`, `wt remove`) refuse one that is mid-operation (`git::operation_in_progress`).
`wt merge` checks for the operation before uncommitted changes, so a conflicted merge left by an interrupted `wt merge` is named, rather than reported as changes to commit or stash, which Git refuses while conflicts remain.
Git interrupted while updating files (e.g. mid-checkout in a fast-forward) can leave them partly updated with no operation recorded; `wt` then refuses them as uncommitted changes, which is safe, though its commit-or-stash advice doesn't fit.
Don't add signal-based rollback without proving it safe against every intermediate state.

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

### Git's messages may be translated

Git localizes its error messages, including the `fatal: ` and `error: ` prefixes, so matching their English text fails silently in other locales.
`wt` used to recognize a Git too old for `worktree list -z` by its "unknown switch" message; in a German locale it showed Git's raw usage error instead of saying Git 2.36 is required.
`wt` now decides from exit codes and machine-readable output (`--porcelain`, `-z`, `--format`, `--quiet`) and uses Git's messages only as diagnostics; the old-Git check keys on the usage-error exit code 129, and on Git having exited rather than been killed, since a SIGHUP is reported as 129 too.
The e2e tests pin `LC_ALL=C`, since some assert Git's English diagnostics.
