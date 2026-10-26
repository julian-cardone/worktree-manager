# Removes the files listed in an install manifest, the record `cmake --install`
# writes of every file it installed. Run through the `uninstall` target:
#
#   cmake -DWT_INSTALL_MANIFEST=<build>/install_manifest.txt -P uninstall.cmake
#
# Only listed files are removed; directories, including ones the install
# created, are left alone because they may hold files something else put there.

if(NOT DEFINED WT_INSTALL_MANIFEST)
  message(FATAL_ERROR "uninstall: WT_INSTALL_MANIFEST is not set; nothing was removed.")
endif()

if(NOT EXISTS "${WT_INSTALL_MANIFEST}")
  message(FATAL_ERROR
    "uninstall: no install manifest at ${WT_INSTALL_MANIFEST}; nothing was removed.\n"
    "Either nothing was installed from this build directory, or it was already uninstalled.")
endif()

# One path per line. CMake can't record a path containing a semicolon in the
# manifest (it splits it into two lines), so neither can this.
file(STRINGS "${WT_INSTALL_MANIFEST}" installed_files)

# Validate every entry before removing any, so a bad manifest changes nothing.
set(present_files "")
foreach(installed_file IN LISTS installed_files)
  # WARNING: only absolute paths to files or symlinks are removed. A relative
  # path would resolve against wherever this runs, and a directory may hold
  # files this project never installed.
  if(NOT IS_ABSOLUTE "${installed_file}")
    message(FATAL_ERROR
      "uninstall: the manifest lists a relative path, ${installed_file}; nothing was removed.\n"
      "Check ${WT_INSTALL_MANIFEST}.")
  endif()
  if(IS_SYMLINK "${installed_file}")
    list(APPEND present_files "${installed_file}")
  elseif(IS_DIRECTORY "${installed_file}")
    message(FATAL_ERROR
      "uninstall: ${installed_file} is a directory, but the install put a file there; nothing was removed.\n"
      "Check what is at that path and remove it yourself if it is not needed.")
  elseif(EXISTS "${installed_file}")
    list(APPEND present_files "${installed_file}")
  else()
    message(STATUS "Already gone: ${installed_file}")
  endif()
endforeach()

foreach(installed_file IN LISTS present_files)
  message(STATUS "Removing: ${installed_file}")
  file(REMOVE "${installed_file}")
  if(IS_SYMLINK "${installed_file}" OR EXISTS "${installed_file}")
    message(FATAL_ERROR
      "uninstall: could not remove ${installed_file}.\n"
      "Files listed before it in ${WT_INSTALL_MANIFEST} were removed; fix the cause "
      "(often permissions) and run the uninstall again to remove the rest.")
  endif()
endforeach()

# The manifest has been fully acted on. Removing it means a later uninstall
# can't delete a file something else has since put at the same path.
file(REMOVE "${WT_INSTALL_MANIFEST}")
list(LENGTH present_files removed_count)
message(STATUS "Uninstalled ${removed_count} file(s).")
