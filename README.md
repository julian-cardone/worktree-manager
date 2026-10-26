# wt

`wt` is a small C++ CLI that takes the friction out of managing Git worktrees when you (and agents like Claude or Codex) are working on several things at once.
Each task gets its own branch and worktree, so tasks can run side by side without stashing or switching branches, and then land on the integration branch and get cleaned up safely.

`wt` owns the worktree lifecycle: creating, listing, inspecting, merging, and removing task worktrees, and the branch cleanup that goes with them.
It is not a Git replacement.
Staging, committing, rebasing, diffs, history, and everything else stay with Git, and `wt` never fetches, pushes, or changes remote branches.

`wt` runs on macOS and Linux.
Windows is not supported.

## Requirements

To run `wt`:

- macOS or Linux
- Git 2.36 or newer on `PATH`

To build it from source, also:

- A C++20 compiler (Apple Clang 15+, GCC 11+, or Clang 15+)
- CMake 3.24+
- Network access on the first configure, which fetches the dependencies

On macOS, `wt` links the system's iconv library, which every macOS install includes; nothing else needs installing.

Dependencies are pinned to release archives, verified by SHA256, and fetched automatically via CMake `FetchContent`:

| Library | Version | Used for |
| --- | --- | --- |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.4.2 | Argument parsing |
| [Catch2](https://github.com/catchorg/Catch2) | 3.7.1 | Tests |

## Installing

Both ways install a single file, `bin/wt`, under a prefix of your choice.
The examples use `~/.local`, which needs no administrator rights.

### From a release binary

Each [GitHub Release](https://github.com/julian-cardone/worktree-manager/releases) has a prebuilt binary for macOS on Apple silicon (`macos-arm64`), plus a `SHA256SUMS` file:

```sh
version=0.1.0
platform=macos-arm64
base=https://github.com/julian-cardone/worktree-manager/releases/download/v$version
curl -fLO "$base/wt-$version-$platform.tar.gz"
curl -fLO "$base/SHA256SUMS"
shasum -a 256 --check --ignore-missing SHA256SUMS
mkdir -p ~/.local
tar -xzf "wt-$version-$platform.tar.gz" --strip-components=1 -C ~/.local
wt --version
```

The archive contains only `wt-<version>-<platform>/bin/wt`, so this installs `~/.local/bin/wt`.
The macOS binary isn't notarized; downloading with `curl` as above avoids the quarantine flag a browser download would add, which makes macOS block it.
The binary is built on macOS 15 and uses the system's C and C++ runtime libraries, so it may not run on older versions.
On other platforms, such as Linux or Intel Macs, build from source.

To update, install the newer archive the same way; it replaces the old binary.
To uninstall, delete the binary: `rm ~/.local/bin/wt`.

### From source

```sh
git clone https://github.com/julian-cardone/worktree-manager.git
cd worktree-manager
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --install build --prefix "$HOME/.local"
```

Any prefix works; without `--prefix`, CMake uses `/usr/local`, which usually needs `sudo`.
Quote a prefix that contains spaces.
Release binaries are built and tested with these same commands from the release's tag (see [Releases](#releases)), so building a tag from source gives the same version of `wt`.

To update, pull, then rerun the build and install commands; the new binary replaces the old one.

To uninstall:

```sh
cmake --build build --target uninstall
```

This removes exactly the files listed in `build/install_manifest.txt`, which `cmake --install` writes each time it runs, and then removes that manifest.
It never removes directories or anything else in the prefix.
The manifest records only the most recent install from that build directory, so if you installed to more than one prefix, delete the copies the manifest doesn't list by hand, e.g. `rm "$HOME/.local/bin/wt"`.
The same applies if the build directory has since been deleted.

### Adding `wt` to `PATH`

A source install says so if the install directory isn't on your `PATH`.
Add it in your shell profile (for example `~/.zshrc` or `~/.bashrc`), then open a new shell:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

## Quick start

Run `wt` from anywhere inside a Git repository: the main checkout or any of its worktrees.

```sh
cd "$(wt new fix-login)"           # new branch fix-login from the integration branch, in its own worktree
# ...edit, then commit with Git as usual...
wt list                            # every worktree, with its branch and whether it has uncommitted changes
wt status fix-login                # uncommitted changes, and commits ahead of or behind the integration branch
cd -                               # back to the main checkout
wt merge fix-login --cleanup       # land it, then remove its worktree and delete its branch
```

`wt new` prints only the new worktree's path on stdout, so it composes with `cd`.
Worktrees are created in a hidden folder next to the main checkout (see [`wt new`](#wt-new-task)).
The integration branch is usually `main`; [`wt new`](#wt-new-task) explains how `wt` chooses it.

`--cleanup` refuses to remove the worktree you're running it from, so run it from another worktree.
Without `--cleanup`, `wt merge fix-login` lands the task and keeps its worktree and branch.
`wt remove fix-login` removes the worktree without merging, and always keeps the branch.
Both refuse rather than delete uncommitted work, and say what is in the way.

To keep the integration branch's history linear, set `git config merge.ff only`; `wt merge` then refuses a task that is behind and prints the `git rebase` command to bring it up to date.

## Commands

| Command | Description |
| --- | --- |
| `wt`, `wt -h`, `wt --help` | Print help. |
| `wt -V`, `wt --version` | Print the version (e.g. `wt 0.1.0`) and exit. |
| `wt list` | List every worktree of the current repository with its branch, status, and path. |
| `wt list --json` | The same, as JSON for agents and scripts (see [JSON output](#json-output)). |
| `wt new <task>` | Create branch `<task>` from the integration branch in a new worktree, and print its path. |
| `wt status <task>` | Summarize the task's worktree: HEAD, uncommitted changes, and commits ahead of or behind the integration branch. |
| `wt status <task> --json` | The same, as JSON for agents and scripts. |
| `wt merge <task>` | Merge the task's branch into the integration branch, following Git's merge settings; on a conflict, abort and list the conflicting files. |
| `wt merge <task> --cleanup` | Merge the task, then remove its worktree and delete its branch once it's proven to have landed. |
| `wt merge <task> --cleanup --delete-ignored` | The same, also deleting ignored files (such as `build/`) with the worktree. |
| `wt remove <task>` | Remove the task's worktree if nothing uncommitted would be lost. The branch is kept. |
| `wt remove <task> --delete-ignored` | The same, also deleting ignored files with the worktree. |
| `wt remove <task> --forget-missing` | Remove only Git's record of a worktree whose directory is gone. The branch is kept. |

`wt <command> --help` describes each command and its options.
A `<task>` is the name of the task's branch.

## Safety

`wt` is built to be safe to run unattended. In particular:

- **It never discards uncommitted work.**
  `wt remove` and `wt merge --cleanup` refuse a worktree with staged, modified, untracked, or conflicted files, or with an operation such as a rebase in progress, and list what is in the way.
  Ignored files (such as `.env` or `build/`) are deleted only if you pass `--delete-ignored`, after `wt` has listed them.
- **It never forces.**
  `wt` never passes `--force` to Git or resets a branch.
  `wt remove` never deletes a branch.
  Only `wt merge --cleanup` does, after proving the branch's tip is contained in the integration branch, and then only with `git branch -d`, so Git checks again.
- **It never resolves conflicts.**
  On a conflict, `wt merge` aborts the merge, checks that the integration branch is back where it was, and lists the conflicting files for you to resolve.
- **It only removes what it created.**
  `wt remove` refuses the main worktree, the worktree you're in, locked worktrees, and any worktree outside the folder `wt` created and verified as this repository's.
- **It checks its work.**
  After changing anything, `wt` reads the result back from Git and the filesystem, since a Git hook could have changed it.
  When something fails partway, it says what it changed and what was left behind, and never claims that nothing changed once it has started.
- **Git is the only state.**
  `wt` keeps no database of its own; besides the worktrees, the only file it writes is the `.wt-managed` marker that records which repository owns its worktree folder.

`--forget-missing` is the one way to drop a worktree without its directory, and only applies when nothing at all is left at the worktree's path, for example after deleting it by hand.

## Agents and scripts

Every `wt` command is non-interactive: it never prompts and never reads stdin, and the Git commands it runs get no stdin either, so they can't stop to wait for input.
`wt` prints no color or other terminal escapes.

**Output streams.**
Results go to stdout; errors, warnings, and progress notes go to stderr.

- `wt list --json` and `wt status <task> --json` print exactly one JSON document on stdout on success, and nothing on stdout on failure (see [JSON output](#json-output)).
- `wt new <task>` prints only the new worktree's absolute path on stdout; its `wt: created branch …` note goes to stderr.
- `wt merge` and `wt remove` print a short human-readable summary on stdout when they succeed, and have no `--json`; use the exit code to tell what happened.
- Without `--json`, `wt list` and `wt status` print tables meant for people, with paths under your home directory shortened to `~`; parse the JSON instead.

**Which repository.**
`wt` works on the repository containing the current directory.
If `GIT_DIR` is set, it decides which repository that is: from outside any repository, `GIT_DIR` alone names one (e.g. a bare repository of dotfiles); from inside one, it must point at the current directory's own Git directory, or `wt` refuses rather than guess.
The Git commands `wt` runs on a particular worktree ignore `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`, and similar variables, so `wt` checks the right worktree even when run from a Git hook, `git rebase --exec`, or a `!` alias.

### Errors and exit codes

When a command doesn't do what was asked, the first line on stderr and the exit code say why, so people and agents can tell a deliberate refusal from something that broke.
Scripts should treat any nonzero exit code as failure, and branch on a specific code only when they deliberately handle that category.

The message also says whether anything was changed.
When `wt new`, `wt remove`, or `wt merge` stops before changing anything, it says so (`No branch or worktree was created.`, `Nothing was removed.`, `Nothing was merged.`).
Once a command has started changing things, it never says that; it reports what it changed or left behind, or that it couldn't verify the result and where to look.

| Exit code | Prefix | Meaning | What to do |
| --- | --- | --- | --- |
| 0 | | Success. | |
| 1 | `wt: error:` | The request or setup needs fixing, e.g. no such task, an invalid name, or a missing integration branch. | Fix what the message names. |
| 2 | `wt: refused:` | `wt` deliberately stopped, because going ahead could lose work or touch something it doesn't own, or because it couldn't verify that it wouldn't. Nothing is broken. | Deal with what is in the way (the message lists it), then rerun. |
| 3 | `wt: conflict:` | A merge conflicted. `wt` aborted it; conflicts are never resolved for you. | Resolve the conflicts in the task's worktree, then rerun. |
| 4 | `wt: git error:` | A Git command `wt` ran failed, e.g. outside a repository or when a hook fails, or Git reported success but `wt` couldn't confirm the result (a hook may have changed it). | Act on Git's message; `wt` says what, if anything, was left behind. If the result couldn't be verified, inspect the state before retrying. |
| 5 | `wt: internal error:` | `wt` hit something it doesn't understand, such as unexpected Git output. Probably a bug in `wt`. | Check the affected worktrees with `git status` before retrying. |
| 100+ | | The command line couldn't be parsed (from CLI11), e.g. an unknown option. | See `wt --help`. |

#### Interrupted commands

`wt` installs no signal handlers, so Ctrl+C stops it along with the Git command it is running, and `wt` attempts no cleanup of its own.
Git cleans up its lock files, but may leave an operation in progress, such as a merge `wt merge` was about to abort, or the files partly updated.
The next `wt merge` or `wt remove` then refuses that worktree; inspect it with `git status`, finish or abort any operation with Git, then rerun.
If `wt merge --cleanup` is interrupted after the task has landed, rerunning it finishes the cleanup, or explains what is left to deal with first.

### JSON output

`wt list --json` and `wt status <task> --json` print the same information as their tables, as JSON for agents and scripts.
The field names and meanings below are part of `wt`'s interface: new fields may be added, so consumers should ignore ones they don't know, but existing ones won't be renamed or change meaning.

- On success, stdout is exactly one compact JSON document on one line, followed by a newline.
- On failure, stdout is empty; the exit code and stderr say why, as for any command.
- Warnings, such as a worktree whose status can't be read, go to stderr and never into the JSON.
- Paths are absolute and exactly as Git reports them: never shortened to `~`, and with control characters escaped rather than replaced.
- Strings are UTF-8. A branch name or path that isn't valid UTF-8 can't be represented, so the command fails (exit 1) rather than print a different one.
- Fields with nothing to report are `null` rather than left out.

The other commands have no `--json`; see [Output streams](#agents-and-scripts) for what they print.

**`wt list --json`** prints an object with a `worktrees` array, main worktree first, then in Git's order:

```json
{"worktrees":[{"path":"/Users/me/Dev/worktree-manager","branch":"main","head":"5f749c1…","status":"clean","operation":null,"current":true,"detached":false,"locked":false}]}
```

| Field | Meaning |
| --- | --- |
| `path` | The worktree's absolute path. |
| `branch` | The branch checked out, e.g. `feature/x`, or the branch being rebased there, since a rebase detaches HEAD until it finishes; `null` if otherwise detached, or bare. |
| `head` | The full commit id checked out; `null` for a bare repository or a branch with no commits. |
| `status` | `clean`, `dirty`, `missing`, `unknown`, or `bare`, as in [`wt list`](#wt-list). |
| `operation` | The operation Git has stopped in the middle of: `merge`, `rebase`, `am`, `cherry-pick`, `revert`, `cherry-pick-or-revert`, or `bisect`; `unknown` if `wt` can't tell (a warning says why); `null` if none, or the status is `missing`, `unknown`, or `bare`. |
| `current` | Whether you're running `wt` from this worktree or a directory inside it. |
| `detached` | Whether Git has HEAD detached, i.e. a commit rather than a branch checked out; true during a rebase. |
| `locked` | Whether the worktree is locked (`git worktree lock`). |

**`wt status <task> --json`** prints one object:

```json
{"branch":"feature-x","path":"/Users/me/Dev/.worktree-manager.wt-a13f92c4/feature-x","head":"3f2a9c1…","status":"dirty","operation":null,"changes":{"staged":1,"modified":2,"untracked":0,"conflicts":0},"integration":{"branch":"main","ahead":3,"behind":1}}
```

| Field | Meaning |
| --- | --- |
| `branch` | The task's branch, as given. |
| `path` | The worktree's absolute path. |
| `head` | The full commit id checked out; `null` for a branch with no commits. |
| `status` | `clean`, `dirty`, or `missing`, as in [`wt list`](#wt-list). If the status can't be read, the command fails instead. |
| `operation` | As for `wt list --json`, except that if `wt` can't tell, the command fails instead. |
| `changes` | `staged`, `modified`, `untracked`, and `conflicts`, counted as in [`wt status`](#wt-status-task); `null` when the worktree is missing. |
| `integration` | The integration branch's name as `branch`, and `ahead` and `behind` counts against it; `null` for the integration branch itself, a branch with no commits, or when there is no integration branch (a warning says why). |

## Command reference

### `wt list`

Works from the main checkout or any linked worktree, and always lists the main worktree first.
`*` marks the worktree you're in.
Paths under your home directory are shown with `~`.

```
  BRANCH              STATUS  PATH
* main                clean   ~/Dev/worktree-manager
  feature/x           dirty   ~/Dev/.worktree-manager.wt-a13f92c4/feature-x
  (detached 9744289)  clean   ~/Dev/experiment
  (rebasing fix-ui)   dirty (rebase)  ~/Dev/.worktree-manager.wt-a13f92c4/fix-ui
```

A rebase detaches HEAD until it finishes, so a worktree in the middle of one is shown as `(rebasing <branch>)` rather than detached.

| Status | Meaning |
| --- | --- |
| `clean` | No uncommitted changes. |
| `dirty` | Staged, unstaged, or untracked changes (ignored files don't count). |
| `missing` | Nothing exists at the worktree's path any more. Git still tracks it; `wt status <task>` explains how to reconnect or forget it. |
| `unknown` | The status couldn't be read, e.g. a file or dangling symlink replaced the directory; a warning on stderr explains why. |
| `-` | A bare repository, which has no working files. |

`clean` or `dirty` is followed by the operation Git has stopped in the middle of, if any: `(merge)`, `(rebase)`, `(git am)`, `(cherry-pick)`, `(revert)`, `(cherry-pick or revert)`, or `(bisect)`.
These can leave the files clean, and `wt merge` and `wt remove` refuse a worktree in the middle of one.
If `wt` can't tell, it shows `(operation unknown)` and warns on stderr.

Checking status never modifies a worktree: `wt` runs `git status` with `--no-optional-locks`, so it won't touch the index or contend with other Git processes.
Untracked files always count, even if `status.showUntrackedFiles` is set to `no`.
`wt list` fails with a Git error (exit 4) outside a Git repository.
For scripts, `wt list --json` prints the same as [JSON](#json-output).

### `wt new <task>`

```sh
cd "$(wt new claude/fix-login)"
```

Creates the branch `<task>` from the integration branch and checks it out in a new worktree.
The worktree's absolute path is the only thing printed on stdout, so the command composes with `cd` and scripts; messages and warnings go to stderr.
It works from the main checkout or any linked worktree, and always branches from the integration branch, not from the current one.

**Where worktrees go.**
Every worktree `wt` creates lives in one hidden container next to the main checkout:

```
~/Dev/worktree-manager/                       # main checkout
~/Dev/.worktree-manager.wt-a13f92c4/          # container
    .wt-managed                               # ownership marker
    feature-x/
    claude-fix-login/                         # task claude/fix-login
```

The suffix is derived from the path of the repository's Git directory, so it is stable for a repository and distinct between repositories.
Moving the repository changes the suffix; `wt` then starts a new container and leaves the old one untouched.
`.wt-managed` names the repository that owns the container.
`wt` only uses a container that is a real directory (not a symlink) with a marker naming the current repository; anything else at that path makes it stop with an error rather than reuse or overwrite it.
For a bare repository, the container sits next to it and drops the `.git` suffix (`repo.git` → `.repo.wt-…`).

**The integration branch** is the first of these that applies.
`wt` never fetches, and always starts from the local branch:

1. `git config wt.integrationBranch <branch>`, if set.
   It must name an existing local branch.
2. The branch `origin/HEAD` points to (set by `git clone`, or by `git remote set-head origin <branch>`).
   A local branch of the same name must exist.
3. A local `main`, then `master`.

If none applies, `wt new` fails rather than guessing.

**Task names** become the branch name as-is, and the worktree folder name with `/` replaced by `-`.
They may contain only letters, digits, `.`, `_`, `-`, and `/`, must be valid Git branch names, and are at most 100 characters.
They can't start with `-` or `.`, start or end with `/`, or contain `//` or `..`.

**Before creating anything**, `wt new` fails if:

- the branch already exists locally, even if it's a task's (`wt new` never reuses a branch);
- another branch makes the name impossible for Git, e.g. `payments` exists and the task is `payments/api`, or the reverse;
- a local branch would clash with it if case were ignored (`Fix` and `fix`, or `Fix` and `fix/login`), when Git reports a case-insensitive filesystem (`core.ignorecase`), however that branch is stored;
- anything exists at the target path, including an empty directory, a file, or a dangling symlink (e.g. `a/b` and `a-b` would share a folder);
- Git still has a worktree registered at the target path, even one whose directory was deleted (the error says how to clear it);
- the container exists but isn't verifiably owned by this repository.

The two branch checks are errors (exit 1).
The rest are refusals (exit 2): the task may be fine, but something already occupies its place, and `wt` won't reuse or overwrite what it can't prove it owns.

If `origin/<task>` exists, `wt` warns but still creates a new local branch from the integration branch, without tracking it.

**The starting point** is the integration branch's commit, read immediately before creating the worktree and named in the success message (`wt: created branch feature-x from main at 5f749c1 Fix x`).
`wt` hands Git that commit rather than the branch name, so the task starts exactly there even if the branch moves meanwhile, and Git never sets the integration branch as its upstream, even with `branch.autoSetupMerge=always`.

**If Git fails partway**, for example because a `post-checkout` hook exits non-zero after the worktree was created, `wt new` exits 4, shows Git's output, and reports the current state of the branch and the target path.
It never removes anything itself.
When the branch and worktree are exactly what `wt new` would have made, it says how to clean them up: `wt remove <task>` for the worktree, then `git branch -d <task>` for the branch.
Otherwise, for example when the branch has moved or the path holds something Git doesn't list as a worktree (perhaps created by another process meanwhile), it suggests no cleanup and asks you to inspect the state first.

**After Git succeeds**, `wt new` checks that Git lists a worktree at the target path with `<task>` checked out, starting at that commit, since a `post-checkout` hook could have switched, moved, or deleted it.
If not, it exits 4 with nothing on stdout, says what it found, and reports the current state the same way, removing nothing.

### `wt status <task>`

```
$ wt status feature-x
feature-x

Path:         ~/Dev/.worktree-manager.wt-a13f92c4/feature-x
HEAD:         3f2a9c1 Handle empty input
Status:       dirty
Staged:       1
Modified:     2
Untracked:    0
Conflicts:    0
Ahead main:   3
Behind main:  1
```

Finds the worktree that has branch `<task>` checked out, so it also works for worktrees `wt` didn't create, including the main checkout.
While `<task>` is being rebased, its HEAD is detached, so `wt status` finds the worktree rebasing it instead.
If `<task>` is only going to be moved by a `git rebase --update-refs` in another worktree, it says so and exits 1.
It gives the same answer from any worktree of the repository.

| Field | Meaning |
| --- | --- |
| `Status` | As in `wt list`. |
| `In progress` | The operation Git has stopped in the middle of, e.g. `a rebase` or `a bisect`; only shown when there is one. If `wt` can't tell, it shows `unknown`, says why, and exits 1. |
| `Staged` | Files with changes in the index. |
| `Modified` | Tracked files with changes not yet staged. A file with both staged and unstaged changes counts in both. |
| `Untracked` | Untracked files, as `git status` lists them: an untracked directory counts once. Ignored files don't count. |
| `Conflicts` | Files with unresolved merge conflicts. |
| `Ahead <base>`, `Behind <base>` | Commits on the task's branch that the local integration branch lacks, and the reverse. |

The integration branch is chosen as for `wt new`, and `wt` never fetches.
The ahead and behind lines are left out for the integration branch itself and for a branch with no commits.
If there is no integration branch, `wt status` warns and leaves them out.

If the worktree's directory is gone, the status is `missing` and the change counts are left out; ahead and behind still show, since they only depend on the branch.
A warning says how to recover: `git worktree repair <new-path>` if the directory was moved, otherwise `wt remove <task> --forget-missing`.
If the worktree is locked (e.g. it lives on a removable drive), the warning says Git keeps its record until it is unlocked.
If the status can't be read, `wt status` prints what it can and exits 1 (with `--json`, it prints nothing on stdout).
It also exits 1 if no worktree has the branch checked out.

### `wt merge <task>`

```
$ wt merge feature-x
✓ Merged feature-x into main
```

Merges branch `<task>` into the integration branch (chosen as for `wt new`) with a normal `git merge`, which fast-forwards when it can.
`wt` doesn't choose a merge strategy: Git's own settings, such as `merge.ff`, decide whether a merge commit is made.
The merge happens in the worktree that has the integration branch checked out, usually the main checkout; `wt` never switches a worktree to it.
It works from the main checkout or any other worktree of the repository, including the task's own.
The task's worktree and branch are kept, unless you pass `--cleanup` (see [below](#cleaning-up-after-landing)).
If the integration branch already has every commit on `<task>`, it says so and does nothing.

**It refuses, and changes nothing, if:**

- the integration branch's worktree has staged, modified, or untracked files, or is in the middle of a merge, rebase, `git am`, cherry-pick, revert, or bisect, even with clean files;
- the task's worktree has uncommitted changes, since the merge wouldn't include them, or is in the middle of one of those operations, since its branch may not hold the finished work yet (a task branch with no worktree merges fine);
- `<task>` or the integration branch is in the middle of a rebase in any worktree, or a `git rebase --update-refs` there will move `<task>`, since its branch still points at the commits being rewritten;
- `merge.ff` is set to `only` and `<task>` is behind the integration branch (see below);
- the merge would overwrite ignored files in the integration branch's worktree, i.e. the task's branch adds a file where one is ignored (such as `.env`), which Git would otherwise replace silently;
- there is something to merge and `branch.<integration-branch>.mergeOptions` or `pull.twohead` is set to anything, in any config file: Git adds those options to every merge into that branch, and uses `pull.twohead` as every merge's default strategy, not only `git pull`'s, and some values (such as `-Xours` or `ours`) resolve conflicts without stopping; `wt` doesn't override the repository's settings, so unset them where they are configured (`git config --show-origin --get-all <key>` shows where) to merge with `wt`;
- the integration branch isn't checked out in any worktree, or `<task>` is the integration branch.

**On a conflict**, `wt merge` aborts the merge with `git merge --abort`, checks that the integration branch's worktree is back exactly where it was, and lists the conflicting files:

```
$ wt merge feature-x
wt: conflict: feature-x conflicts with main

Conflicts:
- src/bar.cpp
- src/foo.cpp

main was restored; no merge remains in progress.
```

It never resolves conflicts itself.
Resolve them in the task's worktree, then run `wt merge` again.
Rebasing the task onto the integration branch (`git rebase main` in its worktree) keeps the integration branch's history linear.
Merging the integration branch into the task instead (`git merge main`) also works, but the landing then fast-forwards, so the integration branch's history runs through a "Merge branch 'main' into …" commit; set `merge.ff` to `false` if you work this way.

**For a linear history**, set `git config merge.ff only`.
`wt merge` then lands a task only by fast-forwarding, and if the task is behind, refuses before touching anything and says how to rebase it:

```
$ wt merge feature-x
wt: refused: cannot fast-forward main to feature-x

feature-x is 2 commits behind main, and merge.ff is set to "only".
Nothing was merged.
Rebase it onto main in its worktree, resolving any conflicts there:

  git -C /Users/me/Dev/.worktree-manager.wt-a13f92c4/feature-x rebase main

Then run `wt merge feature-x` again.
```

While that rebase is in progress, `wt merge` refuses the task until it is finished or aborted.

The clean-worktree check is what makes the abort safe: `git merge --abort` can only restore a worktree that had no uncommitted changes before the merge.
If Git stops mid-merge for another reason, such as a hook rejecting the merge commit, `wt merge` aborts the same way and shows Git's output.
If Git refuses to start the merge (for example, because `merge.verifySignatures` rejects the task's commits), it shows Git's output and nothing is changed.
With `rerere.autoupdate`, Git reapplies a recorded resolution but still stops the merge, so `wt merge` aborts it like any other.
Custom merge drivers set in `.gitattributes` are the repository's own policy, and `wt` leaves them to Git.
If the abort fails, or `wt merge` can't confirm the worktree is back where it was, it lists the conflicting files and warns that the merge may still be in progress; don't run other `wt` commands until you have inspected that worktree with the `git status` command it prints.
After Git reports success, `wt merge` checks that the integration branch contains both the task and the commit it was at before the merge, and that its worktree is on it, clean, and not mid-operation, since a `post-merge` hook runs after the merge and could undo it, add to it, or drop the branch's own commits.
If not, it says what is wrong and where to look, and leaves the merge in place.
Refusals exit 2, conflicts 3, and Git failures 4, including a merge Git reported but the check after it didn't find (see [Errors and exit codes](#errors-and-exit-codes)).

#### Cleaning up after landing

```
$ wt merge feature-x --cleanup
✓ Merged feature-x into main
Removed worktree ~/Dev/.worktree-manager.wt-a13f92c4/feature-x
Deleted branch feature-x (was 3f2a9c1)
```

`--cleanup` finishes the task once it has landed: it removes the task's worktree, with every check `wt remove` makes, and then deletes the task's local branch.
`wt remove` itself never deletes a branch; this is the only command that does.
`--delete-ignored` may be added, with the same meaning as for `wt remove`: it lets ignored files be deleted with the worktree, and never overrides any other check.

**Before merging**, `wt merge --cleanup` also refuses, and changes nothing, if the worktree couldn't be removed afterwards (for example, it has ignored files and `--delete-ignored` wasn't given, its directory is missing, or you are running `wt` from inside it), or if the branch has an upstream other than the integration branch that lacks its commits, since `git branch -d` would then refuse to delete it.

**The branch is deleted only once that's proven safe.**
After the worktree is gone, `wt` reads the state again and refuses unless the branch is checked out nowhere, isn't being rebased, and its current tip is contained in the integration branch.
It then deletes it with `git branch -d`, never `-D`, so Git checks again that it is merged, and confirms afterwards that the branch is gone.
A commit added to the branch after it landed, even in the moment between `wt`'s check and Git's, keeps it from being deleted.
`git branch -d` also removes the branch's configuration, such as its upstream.
Remote branches, such as `origin/<task>`, are never touched.

**If the task has already landed**, `wt merge --cleanup` finds nothing to merge and goes straight to the cleanup.
That also deletes branches whose worktrees were removed earlier with `wt remove`.
`--cleanup` never forgets a worktree whose directory is missing; run `wt remove <task> --forget-missing` first, then rerun it.

**If the cleanup fails after landing**, the merge stays, and `wt` reads what is left from Git and says how to finish:

```
$ wt merge feature-x --cleanup
✓ Merged feature-x into main
wt: refused: cannot remove feature-x

  1 untracked file
    notes.md

Commit or delete the untracked files, or stash them with `git stash -u`.

feature-x has landed in main, but the cleanup didn't finish:
- its worktree is still there (`wt list` shows it)
- its branch feature-x was kept

Once that's dealt with, finish the cleanup with:

  wt merge feature-x --cleanup
```

### `wt remove <task>`

```
$ wt remove feature-x
Removed worktree ~/Dev/.worktree-manager.wt-a13f92c4/feature-x
Kept branch feature-x
```

Removes the worktree that has branch `<task>` checked out, using `git worktree remove`.
The branch and its commits are always kept; to delete the branch once the task has landed, use `wt merge <task> --cleanup` instead.
It works from the main checkout or any other worktree of the repository.

**It refuses, and changes nothing, if the worktree has:**

- staged changes;
- unstaged changes to tracked files;
- untracked files (even if `status.showUntrackedFiles` is `no`);
- unresolved merge conflicts;
- edits to files Git has been told not to check (`assume-unchanged` or `skip-worktree`);
- ignored files, unless `--delete-ignored` is given.

```
$ wt remove feature-x
wt: refused: cannot remove feature-x

  1 untracked file
    notes.md
  2 ignored files
    .env
    build/

Commit or delete the untracked files, or stash them with `git stash -u`.
Ignored files are deleted with the worktree; move any you need, then rerun with --delete-ignored.
Nothing was removed.
```

Counts follow `git status`: an untracked or ignored directory counts once.
Untracked files are named (up to 10) as well, since Git has no copy of them.
The advice after the counts covers only the kinds of change present: plain `git stash` skips untracked files, and Git won't stash while conflicts are unresolved, so those are resolved or aborted first.
Git itself deletes ignored files (such as `.env` files or build output) with the worktree, so `wt` treats them as something to lose too, and names them (up to 10) so you can check what `--delete-ignored` would delete.
`--delete-ignored` lets them be deleted, but never overrides any other check.
`wt` never passes `--force` to Git, so Git's own check for modified and untracked files still applies.
Neither `git status` nor that check looks at files marked `assume-unchanged` or `skip-worktree` (e.g. local config kept out of commits, or a sparse checkout), so `wt` compares those itself and names any whose content differs from the index as modified files "that Git doesn't show".
One that is absent, as in a sparse checkout, loses nothing and doesn't count.

**It also refuses:**

- the main worktree;
- a worktree `wt` didn't create, i.e. one that isn't directly inside this repository's verified container (see `wt new`);
- the worktree you're running it from, including any directory inside it;
- a locked worktree (`git worktree unlock` it first);
- a worktree path that is a symlink or a file rather than a directory, since Git would follow a symlink and delete what it points to;
- a worktree whose status can't be read;
- a worktree in the middle of a merge, rebase, `git am`, cherry-pick, revert, or bisect, even with clean files, since that state is deleted with it.

**If nothing is at the worktree's path any more**, `wt remove` refuses, since the directory may have been moved or be on a drive that isn't mounted:

```
$ wt remove feature-x
wt: refused: nothing is at /Users/me/Dev/.worktree-manager.wt-a13f92c4/feature-x, where Git has the worktree for 'feature-x' registered

It may have been moved, or be on a drive that isn't mounted.
If it was moved, `git worktree repair <new-path>` reconnects it.
If it's gone for good, `wt remove feature-x --forget-missing` removes only Git's record of it and keeps the branch.
Nothing was removed.
```

`--forget-missing` removes only Git's record of the worktree, which also holds its index and any in-progress operation state, and keeps the branch.
It applies only when nothing at all is at the path: a worktree that is there, or a file or symlink in its place, is still refused, as is a locked worktree.
It also works if the whole container was deleted.
Other stale worktrees are left alone, unlike `git worktree prune`.

**If Git fails**, `wt remove` exits 4, shows Git's output, and says whether the worktree's directory is still there.
After Git reports success, `wt remove` checks that Git no longer lists the worktree and its directory is gone, and exits 4 if not.

## Development

Changes to `wt` itself follow [`AGENTS.md`](AGENTS.md).

### Building

```sh
cmake -S . -B build          # configure (add -DCMAKE_BUILD_TYPE=Release for an optimized build)
cmake --build build -j       # build; produces ./build/wt
```

Pass `-DWT_BUILD_TESTS=OFF` at configure time to skip building the tests (and fetching Catch2).

### Testing

```sh
ctest --test-dir build --output-on-failure
```

Every test runs in its own sandbox, so the suite can run in parallel with `-j`.

Tests live in `tests/`:

- `tests/unit/` — in-process tests against the `wt_core` library, each given a throwaway temp directory as its working directory.
- `tests/e2e/` — run the real `build/wt` binary as a subprocess and check its output and exit code.
  Each run happens in a throwaway temp directory with its own `HOME` and Git config, so tests can't touch your real repositories.
  Tests are grouped by command in `test_<command>.cpp`; `test_cli.cpp` and `test_repository.cpp` cover behavior common to all commands.
  Helpers shared between those files live in `tests/support/e2e.hpp`.
- `tests/install/` — installs the built binary into a throwaway prefix inside the build directory, runs it, and uninstalls it; also checks that uninstall refuses unsafe manifest entries.

Each suite can also be run directly, e.g. `./build/tests/wt_unit_tests` or `./build/tests/wt_e2e_tests`.

### Formatting

Code style is defined in `.clang-format`.
Format all sources with:

```sh
git ls-files -z '*.cpp' '*.hpp' | xargs -0 clang-format -i
```

Check formatting without changing files:

```sh
git ls-files -z '*.cpp' '*.hpp' | xargs -0 clang-format --dry-run --Werror
```

### Continuous integration

GitHub Actions (`.github/workflows/ci.yml`) runs the commands above on every push to `main` and every pull request.
It configures, builds, and runs the full test suite on Linux and macOS, and checks formatting with clang-format 23.
The tests need no Git setup on the runner, since they never read its Git config.

### Project layout

```
CMakeLists.txt        # top-level build; produces build/wt
cmake/                # install-time and uninstall scripts used by the build
scripts/              # release packaging script
src/
  main.cpp            # entry point; delegates to wt::run
  error.hpp           # wt::Error and its kinds, exit codes, and message prefixes
  commands/           # CLI definition and command handlers; commands take a wt::Context
  git/                # runs git and parses its output
  layout/             # where wt puts worktrees: task names and the owned container
  ui/                 # output helpers (tables, path display, JSON)
tests/
  unit/               # Catch2 unit tests
  e2e/                # Catch2 tests that drive the built binary
  install/            # CMake script testing install and uninstall
  support/            # shared test helpers (e.g. the temp-directory sandbox)
.github/workflows/    # CI (build, test, format check) and tag-triggered releases
```

## Releases

The current version is 0.1.0, the first release.

Release binaries are published on the [Releases page](https://github.com/julian-cardone/worktree-manager/releases) as an archive named `wt-<version>-<os>-<arch>.tar.gz` (currently only `macos-arm64`, e.g. `wt-0.1.0-macos-arm64.tar.gz`), with a `SHA256SUMS` file.
See [Installing](#from-a-release-binary) for how to install one.

### Cutting a release

The version has one source: `project(wt VERSION ...)` in `CMakeLists.txt`, which `wt --version` reports.

To release, land a commit that sets the new version on `main`, then tag that commit and push the tag:

```sh
git tag -a v0.1.0 -m "wt 0.1.0"
git push origin v0.1.0
```

Pushing a tag of the form `vX.Y.Z` runs `.github/workflows/release.yml`, which publishes a GitHub Release only if:

- the tagged commit is on `main`;
- the Release build passes the full test suite on macOS arm64;
- the built binary, and the copy in the archive, report exactly the tagged version;
- no release for the tag exists yet; an existing one is never modified.

The archive is built on the same macOS 15 (arm64) runner as CI, with a script you can also run locally:

```sh
scripts/package-release.sh     # configure, build, and test in build-release/, then package
```

It prints the archive's path, e.g. `build-release/wt-0.1.0-macos-arm64.tar.gz`.
`build-release/` is for packaging only; install from your usual build directory instead, since packaging removes `build-release/install_manifest.txt`.
