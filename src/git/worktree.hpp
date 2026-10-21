#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wt::git {

struct Worktree {
    std::filesystem::path path;
    std::string head;  // commit id; all zeros on an unborn branch
    std::optional<std::string> branch;  // short name, e.g. "feature/x"
    bool bare = false;
    bool detached = false;
    bool locked = false;
    bool prunable = false;  // Git considers the worktree stale
};

enum class Status { clean, dirty, bare, missing, unknown };

struct StatusResult {
    Status status;
    std::string detail;  // why the status is unknown
};

// Parses `git worktree list --porcelain -z` output.
std::vector<Worktree> parse_worktree_list(std::string_view porcelain);

// Lists every worktree of the repository containing `dir`, main worktree
// first. Throws Error if `dir` isn't in a repository.
std::vector<Worktree> list_worktrees(const std::filesystem::path& dir);

// Returns the index of the worktree containing `dir`, preferring the deepest
// match so a worktree nested inside another is still found.
std::optional<std::size_t> find_worktree_containing(
    const std::vector<Worktree>& worktrees, const std::filesystem::path& dir);

// Reports whether the worktree has uncommitted changes, including untracked
// files. Never modifies the worktree or its index.
StatusResult worktree_status(const Worktree& worktree);

}  // namespace wt::git
