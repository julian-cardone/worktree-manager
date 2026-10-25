# wt

`wt` is a small C++ CLI that takes the friction out of managing Git worktrees when you (and agents like Claude or Codex) are working on several things at once.

> Status: early development.
> `wt list`, `wt new`, `wt status`, `wt merge`, and `wt remove` work; switching worktrees is not implemented yet.

## Requirements

- A C++20 compiler (Apple Clang 15+, GCC 11+, or Clang 15+)
- CMake 3.24+
- Git 2.36+ on `PATH` at runtime
- Network access on first configure (dependencies are fetched by CMake)

Dependencies are pinned to release archives, verified by SHA256, and fetched automatically via CMake `FetchContent`:

| Library | Version | Used for |
| --- | --- | --- |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.4.2 | Argument parsing |
| [Catch2](https://github.com/catchorg/Catch2) | 3.7.1 | Tests |

## Building

```sh
cmake -S . -B build          # configure (add -DCMAKE_BUILD_TYPE=Release for an optimized build)
cmake --build build -j       # build; produces ./build/wt
```

Pass `-DWT_BUILD_TESTS=OFF` at configure time to skip building the tests (and fetching Catch2).

## Testing

```sh
ctest --test-dir build --output-on-failure
```

Tests live in `tests/`:

- `tests/unit/` — in-process tests against the `wt_core` library, each given a throwaway temp directory as its working directory.
- `tests/e2e/` — run the real `build/wt` binary as a subprocess and check its output and exit code.
  Each run happens in a throwaway temp directory with its own `HOME` and Git config, so tests can't touch your real repositories.

Each suite can also be run directly, e.g. `./build/tests/wt_unit_tests` or `./build/tests/wt_e2e_tests`.

## Formatting

Code style is defined in `.clang-format`.
Format all sources with:

```sh
git ls-files '*.cpp' '*.hpp' | xargs clang-format -i
```

## Usage

| Command | Description |
| --- | --- |
| `wt` | Print help. |
| `wt -h`, `wt --help` | Print help and exit. |
| `wt -V`, `wt --version` | Print the version (e.g. `wt 0.1.0`) and exit. |
| `wt list` | List every worktree of the current repository with its branch, status, and path. |
| `wt new <task>` | Create branch `<task>` from the integration branch in a new worktree, and print its path. |
| `wt status <task>` | Summarize the task's worktree: HEAD, uncommitted changes, and commits ahead of or behind the integration branch. |
| `wt merge <task>` | Merge the task's branch into the integration branch, following Git's merge settings; on a conflict, abort and list the conflicting files. |
| `wt merge <task> --cleanup` | Merge the task, then remove its worktree and delete its branch once it's proven to have landed. |
| `wt remove <task>` | Remove the task's worktree if nothing uncommitted would be lost. The branch is kept. |

The version is set by `project(wt VERSION ...)` in `CMakeLists.txt`.

`wt` works on the repository containing the current directory.
If `GIT_DIR` is set, it decides which repository that is: from outside any repository, `GIT_DIR` alone names one (e.g. a bare repository of dotfiles); from inside one, it must point at the current directory's own Git directory, or `wt` refuses rather than guess.
The Git commands `wt` runs on a particular worktree ignore `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE`, and similar variables, so `wt` checks the right worktree even when run from a Git hook, `git rebase --exec`, or a `!` alias.

### Errors and exit codes

When a command doesn't do what was asked, the first line on stderr and the exit code say why, so people and agents can tell a deliberate refusal from something that broke.
Scripts should treat any nonzero exit code as failure, and branch on a specific code only when they deliberately handle that category.
Earlier builds exited 1 for every failure; the categories were introduced as a deliberate breaking change while `wt` is in early development.


| Exit code | Prefix | Meaning | What to do |
| --- | --- | --- | --- |
| 0 | | Success. | |
| 1 | `wt: error:` | The request or setup needs fixing, e.g. no such task, an invalid name, or a missing integration branch. | Fix what the message names. |
| 2 | `wt: refused:` | `wt` deliberately stopped, because going ahead could lose work or touch something it doesn't own, or because it couldn't verify that it wouldn't. Nothing is broken. | Deal with what is in the way (the message lists it), then rerun. |
| 3 | `wt: conflict:` | A merge conflicted. `wt` aborted it; conflicts are never resolved for you. | Resolve the conflicts in the task's worktree, then rerun. |
| 4 | `wt: git error:` | A Git command `wt` ran failed, e.g. outside a repository or when a hook fails, or Git reported success but `wt` couldn't confirm the result (a hook may have changed it). | Act on Git's message; `wt` says what, if anything, was left behind. If the result couldn't be verified, inspect the state before retrying. |
| 5 | `wt: internal error:` | `wt` hit something it doesn't understand, such as unexpected Git output. Probably a bug in `wt`. | Check the affected worktrees with `git status` before retrying. |
| 100+ | | The command line couldn't be parsed (from CLI11). | See `wt --help`. |

### `wt list`

Works from the main checkout or any linked worktree, and always lists the main worktree first.
`*` marks the worktree you're in.
Paths under your home directory are shown with `~`.

```
  BRANCH              STATUS  PATH
* main                clean   ~/Dev/worktree-manager
  feature/x           dirty   ~/Dev/.worktree-manager.wt-a13f92c4/feature-x
  (detached 9744289)  clean   ~/Dev/experiment
```

| Status | Meaning |
| --- | --- |
| `clean` | No uncommitted changes. |
| `dirty` | Staged, unstaged, or untracked changes (ignored files don't count). |
| `missing` | The worktree's directory no longer exists. Git still tracks it until `git worktree prune`. |
| `unknown` | The status couldn't be read; a warning on stderr explains why. |
| `-` | A bare repository, which has no working files. |

Checking status never modifies a worktree: `wt` runs `git status` with `--no-optional-locks`, so it won't touch the index or contend with other Git processes.
Untracked files always count, even if `status.showUntrackedFiles` is set to `no`.
`wt list` fails with a Git error (exit 4) outside a Git repository.

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
- Git still has a worktree registered at the target path, even one whose directory was deleted (`git worktree prune` clears those);
- the container exists but isn't verifiably owned by this repository.

The two branch checks are errors (exit 1).
The rest are refusals (exit 2): the task may be fine, but something already occupies its place, and `wt` won't reuse or overwrite what it can't prove it owns.

If `origin/<task>` exists, `wt` warns but still creates a new local branch from the integration branch, without tracking it.

**The starting point** is the integration branch's commit, read immediately before creating the worktree and named in the success message (`wt: created branch feature-x from main at 5f749c1 Fix x`).
`wt` hands Git that commit rather than the branch name, so the task starts exactly there even if the branch moves meanwhile, and Git never sets the integration branch as its upstream, even with `branch.autoSetupMerge=always`.

**If Git fails partway**, for example because a `post-checkout` hook exits non-zero after the worktree was created, `wt new` exits 4, shows Git's output, and lists the branch or directory left behind.
It never removes them itself, but says how: `wt remove <task>` for the worktree, then `git branch -d <task>` for the branch.

**After Git succeeds**, `wt new` checks that Git lists a worktree at the target path with `<task>` checked out, starting at that commit, since a `post-checkout` hook could have switched, moved, or deleted it.
If not, it exits 4 with nothing on stdout, says what it found, and removes nothing.

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
It gives the same answer from any worktree of the repository.

| Field | Meaning |
| --- | --- |
| `Status` | As in `wt list`. |
| `Staged` | Files with changes in the index. |
| `Modified` | Tracked files with changes not yet staged. A file with both staged and unstaged changes counts in both. |
| `Untracked` | Untracked files, as `git status` lists them: an untracked directory counts once. Ignored files don't count. |
| `Conflicts` | Files with unresolved merge conflicts. |
| `Ahead <base>`, `Behind <base>` | Commits on the task's branch that the local integration branch lacks, and the reverse. |

The integration branch is chosen as for `wt new`, and `wt` never fetches.
The ahead and behind lines are left out for the integration branch itself and for a branch with no commits.
If there is no integration branch, `wt status` warns and leaves them out.

If the worktree's directory is gone, the status is `missing` and the change counts are left out; ahead and behind still show, since they only depend on the branch.
If the status can't be read, `wt status` prints what it can and exits 1.
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

- the integration branch's worktree has staged, modified, or untracked files, or is in the middle of a merge, cherry-pick, or revert;
- the task's worktree has uncommitted changes, since the merge wouldn't include them (a task branch with no worktree merges fine);
- `<task>` is in the middle of a rebase in any worktree, or a `git rebase --update-refs` there will move it, since its branch still points at the commits being rewritten;
- `merge.ff` is set to `only` and `<task>` is behind the integration branch (see below);
- the merge would overwrite ignored files in the integration branch's worktree, i.e. the task's branch adds a file where one is ignored (such as `.env`), which Git would otherwise replace silently;
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
Rebase it onto main in its worktree, resolving any conflicts there:

  git -C /Users/me/Dev/.worktree-manager.wt-a13f92c4/feature-x rebase main

Then run `wt merge feature-x` again.
```

While that rebase is in progress, `wt merge` refuses the task until it is finished or aborted.

The clean-worktree check is what makes the abort safe: `git merge --abort` can only restore a worktree that had no uncommitted changes before the merge.
If Git stops mid-merge for another reason, such as a hook rejecting the merge commit, `wt merge` aborts the same way and shows Git's output.
If Git refuses to start the merge (for example, because of a branch's `mergeOptions`), it shows Git's output and nothing is changed.
If the abort or the check after it fails, `wt merge` says what is wrong and where to look with `git status`.
After Git reports success, `wt merge` checks that the integration branch contains the task, and that its worktree is on it, clean, and not mid-operation, since a `post-merge` hook runs after the merge and could undo or add to it.
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

**Before merging**, `wt merge --cleanup` also refuses, and changes nothing, if the worktree couldn't be removed afterwards (for example, it has ignored files and `--delete-ignored` wasn't given, or you are running `wt` from inside it), or if the branch has an upstream other than the integration branch that lacks its commits, since `git branch -d` would then refuse to delete it.

**The branch is deleted only once that's proven safe.**
After the worktree is gone, `wt` reads the state again and refuses unless the branch is checked out nowhere, isn't being rebased, and its current tip is contained in the integration branch.
It then deletes it with `git branch -d`, never `-D`, so Git checks again that it is merged, and confirms afterwards that the branch is gone.
A commit added to the branch after it landed, even in the moment between `wt`'s check and Git's, keeps it from being deleted.
`git branch -d` also removes the branch's configuration, such as its upstream.
Remote branches, such as `origin/<task>`, are never touched.

**If the task has already landed**, `wt merge --cleanup` finds nothing to merge and goes straight to the cleanup.
That also deletes branches whose worktrees were removed earlier with `wt remove`.

**If the cleanup fails after landing**, the merge stays, and `wt` says what was left and how to finish:

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
```

Counts follow `git status`: an untracked or ignored directory counts once.
Untracked files are named (up to 10) as well, since Git has no copy of them.
The advice after the counts covers only the kinds of change present: plain `git stash` skips untracked files, and Git won't stash while conflicts are unresolved, so those are resolved or aborted first.
Git itself deletes ignored files (such as `.env` files or build output) with the worktree, so `wt` treats them as something to lose too, and names them (up to 10) so you can check what `--delete-ignored` would delete.
`--delete-ignored` lets them be deleted, but never overrides any other check.
`wt` never passes `--force` to Git, so Git's own check for modified and untracked files still applies.

**It also refuses:**

- the main worktree;
- a worktree `wt` didn't create, i.e. one that isn't directly inside this repository's verified container (see `wt new`);
- the worktree you're running it from, including any directory inside it;
- a locked worktree (`git worktree unlock` it first);
- a worktree path that is a symlink or a file rather than a directory, since Git would follow a symlink and delete what it points to;
- a worktree whose status can't be read;
- a worktree in the middle of a merge, cherry-pick, or revert, even with clean files, since that state is deleted with it.

**If the worktree's directory is already gone**, `wt remove` only removes Git's record of it, and says so.
This also works if the whole container was deleted.
Other stale worktrees are left alone, unlike `git worktree prune`.

**If Git fails**, `wt remove` exits 4, shows Git's output, and says whether the worktree's directory is still there.
After Git reports success, `wt remove` checks that Git no longer lists the worktree and its directory is gone, and exits 4 if not.

## Project layout

```
CMakeLists.txt        # top-level build; produces build/wt
src/
  main.cpp            # entry point; delegates to wt::run
  error.hpp           # wt::Error and its kinds, exit codes, and message prefixes
  commands/           # CLI definition and command handlers; commands take a wt::Context
  git/                # runs git and parses its output
  layout/             # where wt puts worktrees: task names and the owned container
  ui/                 # terminal output helpers (tables, path display)
tests/
  unit/               # Catch2 unit tests
  e2e/                # Catch2 tests that drive the built binary
  support/            # shared test helpers (e.g. the temp-directory sandbox)
```
