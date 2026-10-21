# wt

`wt` is a small C++ CLI that takes the friction out of managing Git worktrees when you (and agents like Claude or Codex) are working on several things at once.

> Status: early development.
> `wt list`, `wt new`, and `wt status` work; switching and removing worktrees are not implemented yet.

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

The version is set by `project(wt VERSION ...)` in `CMakeLists.txt`.

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
`wt list` exits 1 outside a Git repository.

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

- the branch already exists locally;
- anything exists at the target path, including an empty directory, a file, or a dangling symlink (e.g. `a/b` and `a-b` would share a folder);
- Git still has a worktree registered at the target path, even one whose directory was deleted (`git worktree prune` clears those);
- the container exists but isn't verifiably owned by this repository.

If `origin/<task>` exists, `wt` warns but still creates a new local branch from the integration branch, without tracking it.

**If Git fails partway**, for example because a `post-checkout` hook exits non-zero after the worktree was created, `wt new` exits 1, shows Git's output, and lists the branch or directory left behind.
It never removes them itself.

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

## Project layout

```
CMakeLists.txt        # top-level build; produces build/wt
src/
  main.cpp            # entry point; delegates to wt::run
  commands/           # CLI definition and command handlers; commands take a wt::Context
  git/                # runs git and parses its output
  layout/             # where wt puts worktrees: task names and the owned container
  ui/                 # terminal output helpers (tables, path display)
tests/
  unit/               # Catch2 unit tests
  e2e/                # Catch2 tests that drive the built binary
  support/            # shared test helpers (e.g. the temp-directory sandbox)
```
