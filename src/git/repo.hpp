#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wt::git {

// Where repository-wide Git commands should run for wt started in `cwd` with
// the given GIT_DIR. Without GIT_DIR, that's `cwd`. Outside any repository,
// GIT_DIR alone names one (as it does for Git), and its shared Git directory
// is returned. Inside one, GIT_DIR must be the current directory's own Git
// directory: anything else is a refused wt::Error rather than a guess, and a
// GIT_DIR that isn't a Git directory is a user wt::Error.
std::filesystem::path invoked_repository(
    const std::filesystem::path& cwd,
    const std::optional<std::string>& git_dir);

// The repository's shared Git directory as an absolute, symlink-free path:
// the main worktree's .git, or a bare repository itself. Throws Error if
// `dir` isn't in a repository.
std::filesystem::path common_dir(const std::filesystem::path& dir);

// Reports whether `ref` (a full name, e.g. "refs/heads/main") exists.
bool ref_exists(const std::filesystem::path& dir, std::string_view ref);

// An existing local branch that keeps `branch` from being created because one
// name is a directory of the other (e.g. "a" and "a/b"), or nothing.
std::optional<std::string> conflicting_branch(const std::filesystem::path& dir,
                                              std::string_view branch);

// Every local branch, by its name without "refs/heads/".
std::vector<std::string> local_branches(const std::filesystem::path& dir);

// Reports whether Git treats file names in the repository as case-insensitive
// (core.ignorecase, which Git sets by probing when it creates the repository).
bool ignores_case(const std::filesystem::path& dir);

// The commit `name` (e.g. "HEAD", "MERGE_HEAD") resolves to in the worktree at
// `dir`, or nothing if it doesn't exist there.
std::optional<std::string> resolve_commit(const std::filesystem::path& dir,
                                          std::string_view name);

// The branch HEAD points to in the worktree at `dir`, as a full ref (e.g.
// "refs/heads/main"), or nothing if HEAD is detached.
std::optional<std::string> head_branch(const std::filesystem::path& dir);

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

// Every branch name integration_branch could choose, whether or not it exists
// or would be chosen now: the configured one, origin's default, main and
// master. Throws Error if Git fails.
std::vector<std::string> integration_candidates(
    const std::filesystem::path& dir);

}  // namespace wt::git
