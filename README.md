# wt

`wt` is a small C++ CLI that takes the friction out of managing Git worktrees when you (and agents like Claude or Codex) are working on several things at once.

> Status: early development.
> `wt list` works; creating, switching, and removing worktrees are not implemented yet.

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

The version is set by `project(wt VERSION ...)` in `CMakeLists.txt`.

### `wt list`

Works from the main checkout or any linked worktree, and always lists the main worktree first.
`*` marks the worktree you're in.
Paths under your home directory are shown with `~`.

```
  BRANCH              STATUS  PATH
* main                clean   ~/Dev/worktree-manager
  feature/x           dirty   ~/Dev/worktree-manager.worktrees/feature-x
  (detached 9744289)  clean   ~/Dev/worktree-manager.worktrees/experiment
```

| Status | Meaning |
| --- | --- |
| `clean` | No uncommitted changes. |
| `dirty` | Staged, unstaged, or untracked changes (ignored files don't count). |
| `missing` | The worktree's directory no longer exists. Git still tracks it until `git worktree prune`. |
| `unknown` | The status couldn't be read; a warning on stderr explains why. |
| `-` | A bare repository, which has no working files. |

Checking status never modifies a worktree: `wt` runs `git status` with `--no-optional-locks`, so it won't touch the index or contend with other Git processes.
`wt list` exits 1 outside a Git repository.

## Project layout

```
CMakeLists.txt        # top-level build; produces build/wt
src/
  main.cpp            # entry point; delegates to wt::run
  commands/           # CLI definition and command handlers; commands take a wt::Context
  git/                # runs git and parses its output
  ui/                 # terminal output helpers (tables, path display)
tests/
  unit/               # Catch2 unit tests
  e2e/                # Catch2 tests that drive the built binary
  support/            # shared test helpers (e.g. the temp-directory sandbox)
```
