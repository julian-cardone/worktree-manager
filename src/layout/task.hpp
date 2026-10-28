#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wt::layout {

// Explains why `task` can't be used as a branch name, or returns nothing if
// it can. Deliberately stricter than Git: the name also becomes a directory
// name and a command-line argument. Callers must still confirm the result
// with Git (git::is_valid_branch_name).
std::optional<std::string> task_name_problem(std::string_view task);

// The worktree directory name for a valid task: "claude/fix" -> "claude-fix".
std::string task_folder_name(std::string_view task);

// Whether two branch names are equal ignoring ASCII case, as their loose ref
// files are on a case-insensitive filesystem. Task names are ASCII, so ASCII
// case folding is enough to compare one with any branch name.
bool same_ignoring_case(std::string_view a, std::string_view b);

}  // namespace wt::layout
