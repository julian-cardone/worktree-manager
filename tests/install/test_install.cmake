# Tests the install rules and cmake/uninstall.cmake. Run by CTest as
#
#   cmake -DWT_BUILD_DIR=... -DWT_UNINSTALL_SCRIPT=... -DWT_TEST_DIR=... -P test_install.cmake
#
# Everything is created under WT_TEST_DIR, a throwaway directory in the build
# tree, so nothing outside it is installed or removed.

foreach(var WT_BUILD_DIR WT_UNINSTALL_SCRIPT WT_TEST_DIR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is not set")
  endif()
endforeach()

file(REMOVE_RECURSE "${WT_TEST_DIR}")

function(check condition_description)
  if(NOT (${ARGN}))
    message(FATAL_ERROR "FAILED: ${condition_description}")
  endif()
endfunction()

# Runs the uninstall script against `manifest`; sets `rc` and `output` in the caller.
function(run_uninstall manifest)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DWT_INSTALL_MANIFEST=${manifest}" -P "${WT_UNINSTALL_SCRIPT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  set(rc "${result}" PARENT_SCOPE)
  set(output "${out}${err}" PARENT_SCOPE)
endfunction()

# --- Install, run, and uninstall the real binary ------------------------------
# CMAKE_INSTALL_LOCAL_ONLY makes the install write install_local_manifest.txt
# instead of install_manifest.txt, so the test can't change what a developer's
# own `uninstall` target would remove.
set(prefix "${WT_TEST_DIR}/real prefix")
set(local_manifest "${WT_BUILD_DIR}/install_local_manifest.txt")
file(WRITE "${prefix}/bin/unrelated" "not installed by wt\n")

foreach(attempt first reinstall)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DCMAKE_INSTALL_PREFIX=${prefix}" -DCMAKE_INSTALL_LOCAL_ONLY=ON
      -P "${WT_BUILD_DIR}/cmake_install.cmake"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
  check("${attempt} install succeeds: ${out}${err}" rc EQUAL 0)
  check("${attempt} install warns that the prefix is not on PATH: ${err}"
    err MATCHES "which is not on your PATH")
  file(STRINGS "${local_manifest}" installed)
  check("${attempt} install lists only bin/wt, got: ${installed}"
    installed STREQUAL "${prefix}/bin/wt")

  execute_process(COMMAND "${prefix}/bin/wt" --version RESULT_VARIABLE rc OUTPUT_VARIABLE out)
  check("installed wt runs after ${attempt} install" rc EQUAL 0 AND out MATCHES "^wt ")
endforeach()

# The PATH hint stays quiet once the directory is on PATH.
set(saved_path "$ENV{PATH}")
set(ENV{PATH} "${prefix}/bin:$ENV{PATH}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DCMAKE_INSTALL_PREFIX=${prefix}" -DCMAKE_INSTALL_LOCAL_ONLY=ON
    -P "${WT_BUILD_DIR}/cmake_install.cmake"
  RESULT_VARIABLE rc
  ERROR_VARIABLE err)
set(ENV{PATH} "${saved_path}")
check("install with prefix on PATH succeeds" rc EQUAL 0)
check("no PATH hint when the prefix is on PATH: ${err}" NOT err MATCHES "not on your PATH")

run_uninstall("${local_manifest}")
check("uninstall succeeds: ${output}" rc EQUAL 0)
check("uninstall removes wt" NOT EXISTS "${prefix}/bin/wt")
check("uninstall keeps files it did not install" EXISTS "${prefix}/bin/unrelated")
check("uninstall keeps directories" IS_DIRECTORY "${prefix}/bin")
check("uninstall removes the manifest it acted on" NOT EXISTS "${local_manifest}")

run_uninstall("${local_manifest}")
check("a second uninstall refuses: ${output}" NOT rc EQUAL 0 AND output MATCHES "no install manifest")

# --- Manifest edge cases --------------------------------------------------------
set(root "${WT_TEST_DIR}/edge cases")
set(manifest "${root}/install_manifest.txt")

# Removes listed files and symlinks, tolerating ones already gone.
file(WRITE "${root}/prefix/bin/plain file" "")
file(WRITE "${root}/link target" "")
file(CREATE_LINK "${root}/link target" "${root}/prefix/bin/link" SYMBOLIC)
file(CREATE_LINK "${root}/does not exist" "${root}/prefix/bin/dangling" SYMBOLIC)
file(WRITE "${manifest}"
  "${root}/prefix/bin/plain file\n${root}/prefix/bin/link\n"
  "${root}/prefix/bin/dangling\n${root}/prefix/bin/already gone\n")
run_uninstall("${manifest}")
check("uninstall of edge-case manifest succeeds: ${output}" rc EQUAL 0)
check("removes a path with spaces" NOT EXISTS "${root}/prefix/bin/plain file")
check("removes a symlink" NOT IS_SYMLINK "${root}/prefix/bin/link")
check("keeps a symlink's target" EXISTS "${root}/link target")
check("removes a dangling symlink" NOT IS_SYMLINK "${root}/prefix/bin/dangling")
check("reports files already gone: ${output}" output MATCHES "Already gone: [^\n]*already gone")

# A directory where a file was installed stops the uninstall before anything is removed.
file(WRITE "${root}/prefix/bin/kept" "")
file(MAKE_DIRECTORY "${root}/prefix/bin/now a directory")
file(WRITE "${manifest}" "${root}/prefix/bin/kept\n${root}/prefix/bin/now a directory\n")
run_uninstall("${manifest}")
check("refuses a directory entry: ${output}" NOT rc EQUAL 0 AND output MATCHES "is a directory")
check("removes nothing when an entry is a directory" EXISTS "${root}/prefix/bin/kept")
check("keeps the directory" IS_DIRECTORY "${root}/prefix/bin/now a directory")
check("keeps the manifest after refusing" EXISTS "${manifest}")

# A relative path stops the uninstall before anything is removed.
file(WRITE "${manifest}" "${root}/prefix/bin/kept\nbin/wt\n")
run_uninstall("${manifest}")
check("refuses a relative entry: ${output}" NOT rc EQUAL 0 AND output MATCHES "relative path")
check("removes nothing when an entry is relative" EXISTS "${root}/prefix/bin/kept")

file(REMOVE_RECURSE "${WT_TEST_DIR}")
message(STATUS "All install tests passed")
