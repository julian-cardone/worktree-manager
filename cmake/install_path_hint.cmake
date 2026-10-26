# Runs at the end of `cmake --install`. Warns when the directory `wt` was
# installed into isn't on PATH, since `wt` then can't be run by name.
# WT_INSTALL_BINDIR is set by the install(CODE) call before this script.

# A DESTDIR install is staged for packaging, not run from where it lands.
if(NOT "$ENV{DESTDIR}" STREQUAL "")
  return()
endif()

if(IS_ABSOLUTE "${WT_INSTALL_BINDIR}")
  set(bindir "${WT_INSTALL_BINDIR}")
else()
  set(bindir "${CMAKE_INSTALL_PREFIX}/${WT_INSTALL_BINDIR}")
endif()
file(REAL_PATH "${bindir}" bindir)

# PATH entries are colon-separated on every platform wt supports.
string(REPLACE ":" ";" path_entries "$ENV{PATH}")
foreach(entry IN LISTS path_entries)
  if(entry STREQUAL "" OR NOT IS_DIRECTORY "${entry}")
    continue()
  endif()
  file(REAL_PATH "${entry}" entry)
  if(entry STREQUAL bindir)
    return()
  endif()
endforeach()

message(NOTICE
  "Note: wt was installed to ${bindir}, which is not on your PATH.\n"
  "Add that directory to PATH in your shell profile to run `wt` by name; "
  "see \"Installing\" in README.md.")
