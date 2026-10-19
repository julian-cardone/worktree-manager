# wt

`wt` is a small C++ CLI that takes the friction out of managing Git worktrees when you (and agents like Claude or Codex) are working on several things at once.

> Status: project skeleton.
> `wt` builds and responds to `--help` / `--version`; no Git functionality yet.

## Requirements

- A C++20 compiler (Apple Clang 15+, GCC 11+, or Clang 15+)
- CMake 3.24+
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
Format changed files with `clang-format -i <files>`.

## Usage

| Command | Description |
| --- | --- |
| `wt` | Print help. |
| `wt -h`, `wt --help` | Print help and exit. |
| `wt -V`, `wt --version` | Print the version (e.g. `wt 0.1.0`) and exit. |

The version is set by `project(wt VERSION ...)` in `CMakeLists.txt`.

## Project layout

```
CMakeLists.txt        # top-level build; produces build/wt
src/
  main.cpp            # entry point; delegates to wt::run
  commands/           # CLI definition and command handlers; commands take a wt::Context
  git/                # Git integration (not yet implemented)
  ui/                 # terminal output helpers (not yet implemented)
tests/
  unit/               # Catch2 unit tests
  e2e/                # Catch2 tests that drive the built binary
  support/            # shared test helpers (e.g. the temp-directory sandbox)
```
