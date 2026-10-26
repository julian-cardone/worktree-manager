FetchContent_Declare(Catch2
  URL      https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
  URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c)
FetchContent_MakeAvailable(Catch2)

include(Catch)

# Unit tests: exercise wt_core in-process.
add_executable(wt_unit_tests
  unit/test_app.cpp
  unit/test_container.cpp
  unit/test_error.cpp
  unit/test_format.cpp
  unit/test_json.cpp
  unit/test_task.cpp
  unit/test_worktree.cpp)
target_link_libraries(wt_unit_tests PRIVATE wt_core wt_warnings Catch2::Catch2WithMain)
target_include_directories(wt_unit_tests PRIVATE .)
catch_discover_tests(wt_unit_tests TEST_PREFIX "unit: ")

# E2E tests: spawn the real build/wt binary.
add_executable(wt_e2e_tests
  e2e/test_cli.cpp
  e2e/test_list.cpp
  e2e/test_merge.cpp
  e2e/test_merge_cleanup.cpp
  e2e/test_new.cpp
  e2e/test_remove.cpp
  e2e/test_repository.cpp
  e2e/test_status.cpp)
target_link_libraries(wt_e2e_tests PRIVATE wt_warnings Catch2::Catch2WithMain)
target_compile_definitions(wt_e2e_tests PRIVATE
  WT_BINARY_PATH="$<TARGET_FILE:wt>"
  WT_VERSION="${PROJECT_VERSION}")
target_include_directories(wt_e2e_tests PRIVATE .)
add_dependencies(wt_e2e_tests wt)
catch_discover_tests(wt_e2e_tests TEST_PREFIX "e2e: ")
