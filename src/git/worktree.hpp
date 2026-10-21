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

// Uncommitted changes, counted the way `git status` lists them: a file with
// both staged and unstaged changes counts in both, and an untracked directory
// counts once.
struct Changes {
    std::size_t staged = 0;
    std::size_t modified = 0;  // unstaged changes to tracked files
    std::size_t untracked = 0;
    std::size_t conflicts = 0;

    bool empty() const {
        return staged == 0 && modified == 0 && untracked == 0 && conflicts == 0;
    }
};

struct StatusResult {
    Status status;
    std::string detail;  // why the status is unknown
    Changes changes;     // only filled in when clean or dirty
};

// Parses `git worktree list --porcelain -z` output.
std::vector<Worktree> parse_worktree_list(std::string_view porcelain);

// Parses `git status --porcelain=v2 -z` output.
Changes parse_status(std::string_view porcelain);

// Lists every worktree of the repository containing `dir`, main worktree
// first. Throws Error if `dir` isn't in a repository.
std::vector<Worktree> list_worktrees(const std::filesystem::path& dir);

// Returns the index of the worktree containing `dir`, preferring the deepest
// match so a worktree nested inside another is still found.
std::optional<std::size_t> find_worktree_containing(
    const std::vector<Worktree>& worktrees, const std::filesystem::path& dir);

// Reports whether the worktree has uncommitted changes, including untracked
// files, and counts them. Never modifies the worktree or its index.
StatusResult worktree_status(const Worktree& worktree);

// How wt shows a status, e.g. "dirty"; "-" for a bare repository.
std::string_view status_label(Status status);

}  // namespace wt::git
