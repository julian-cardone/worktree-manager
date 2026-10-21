cmake_minimum_required(VERSION 3.24)

project(wt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WT_BUILD_TESTS "Build the wt test suite" ${PROJECT_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# Dependencies (pinned, fetched at configure time)
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(CLI11
  URL      https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.4.2.tar.gz
  URL_HASH SHA256=f2d893a65c3b1324c50d4e682c0cdc021dd0477ae2c048544f39eed6654b699a)

FetchContent_MakeAvailable(CLI11)

# Shared warning flags for everything we build (but not fetched dependencies).
add_library(wt_warnings INTERFACE)
target_compile_options(wt_warnings INTERFACE -Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------
# wt_core: everything except main(), so tests can link against it
# ---------------------------------------------------------------------------
add_library(wt_core STATIC
  src/commands/app.cpp
  src/commands/list.cpp
  src/commands/new.cpp
  src/commands/status.cpp
  src/git/git.cpp
  src/git/repo.cpp
  src/git/worktree.cpp
  src/layout/container.cpp
  src/layout/task.cpp
  src/ui/format.cpp)

target_include_directories(wt_core PUBLIC src)
target_compile_definitions(wt_core PUBLIC WT_VERSION="${PROJECT_VERSION}")
target_link_libraries(wt_core
  PUBLIC CLI11::CLI11
  PRIVATE wt_warnings)

# ---------------------------------------------------------------------------
# wt executable -> build/wt
# ---------------------------------------------------------------------------
add_executable(wt src/main.cpp)
target_link_libraries(wt PRIVATE wt_core wt_warnings)
set_target_properties(wt PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(WT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
