#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace wt::git {

// The repository's shared Git directory as an absolute, symlink-free path:
// the main worktree's .git, or a bare repository itself. Throws Error if
// `dir` isn't in a repository.
std::filesystem::path common_dir(const std::filesystem::path& dir);

// Reports whether `ref` (a full name, e.g. "refs/heads/main") exists.
bool ref_exists(const std::filesystem::path& dir, std::string_view ref);

// Reports whether Git accepts `name` as a branch name, exactly as written.
bool is_valid_branch_name(const std::filesystem::path& dir,
                          std::string_view name);

// The local branch new work should start from. Tries, in order: the
// wt.integrationBranch config, the branch origin/HEAD points to, then local
// main or master. Never fetches. Throws Error, rather than guessing, if a
// configured branch is missing or none of these exists.
std::string integration_branch(const std::filesystem::path& dir);

}  // namespace wt::git
