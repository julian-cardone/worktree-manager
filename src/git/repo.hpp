#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace wt::git {

// The repository's shared Git directory as an absolute, symlink-free path:
// the main worktree's .git, or a bare repository itself. Throws Error if
// `dir` isn't in a repository.
std::filesystem::path common_dir(const std::filesystem::path& dir);

// Reports whether `ref` (a full name, e.g. "refs/heads/main") exists.
bool ref_exists(const std::filesystem::path& dir, std::string_view ref);

// The commit `name` (e.g. "HEAD", "MERGE_HEAD") resolves to in the worktree at
// `dir`, or nothing if it doesn't exist there.
std::optional<std::string> resolve_commit(const std::filesystem::path& dir,
                                          std::string_view name);

// Reports whether `ancestor` is `commit` or one of its ancestors.
bool is_ancestor(const std::filesystem::path& dir, std::string_view ancestor,
                 std::string_view commit);

// Reports whether Git accepts `name` as a branch name, exactly as written.
bool is_valid_branch_name(const std::filesystem::path& dir,
                          std::string_view name);

// The first line of `commit`'s message, after its short id: "5f749c1 Fix x".
std::string commit_summary(const std::filesystem::path& dir,
                           std::string_view commit);

struct Divergence {
    std::size_t ahead;   // commits on the ref that the base lacks
    std::size_t behind;  // commits on the base that the ref lacks
};

// Compares two commits, e.g. "refs/heads/feature" against "refs/heads/main".
Divergence divergence(const std::filesystem::path& dir, std::string_view ref,
                      std::string_view base);

// The local branch new work should start from. Tries, in order: the
// wt.integrationBranch config, the branch origin/HEAD points to, then local
// main or master. Never fetches. Throws a user wt::Error, rather than guessing,
// if a configured branch is missing or none of these exists, and Error if Git
// fails.
std::string integration_branch(const std::filesystem::path& dir);

}  // namespace wt::git
