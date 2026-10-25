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
// both staged and unstaged changes counts in both, and an untracked or ignored
// directory is listed once, with a trailing '/'.
struct Changes {
    std::size_t staged = 0;
    std::size_t modified = 0;  // unstaged changes to tracked files
    std::vector<std::string> untracked;
    std::size_t conflicts = 0;
    std::vector<std::string> ignored;  // only listed when asked for

    // Ignored files aren't changes, so they don't count here.
    bool empty() const {
        return staged == 0 && modified == 0 && untracked.empty() &&
               conflicts == 0;
    }
};

struct StatusResult {
    Status status;
    std::string detail;  // why the status is unknown
    Changes changes;     // only filled in when clean or dirty
};

// Parses `git worktree list --porcelain -z` output. Throws an internal
// wt::Error if it isn't in the expected format.
std::vector<Worktree> parse_worktree_list(std::string_view porcelain);

// Parses `git status --porcelain=v2 -z` output. Throws an internal wt::Error
// if it isn't in the expected format.
Changes parse_status(std::string_view porcelain);

// Lists every worktree of the repository containing `dir`, main worktree
// first. Throws Error if `dir` isn't in a repository.
std::vector<Worktree> list_worktrees(const std::filesystem::path& dir);

// Returns the index of the worktree containing `dir`, preferring the deepest
// match so a worktree nested inside another is still found.
std::optional<std::size_t> find_worktree_containing(
    const std::vector<Worktree>& worktrees, const std::filesystem::path& dir);

// Reports whether `dir` is the worktree's directory or lies inside it.
bool worktree_contains(const Worktree& worktree,
                       const std::filesystem::path& dir);

// The worktree in which `branch` (a short name) is being rebased, if any.
// Mid-rebase, the worktree is detached and the branch still points at its old
// commits, so nothing in `git worktree list` shows the branch is in use.
// `common` is the repository's shared Git directory. Throws a refused
// wt::Error if a worktree's rebase state can't be read, since a branch that
// might be mid-rebase isn't safe to use.
std::optional<std::filesystem::path> rebasing_worktree(
    const std::vector<Worktree>& worktrees, const std::filesystem::path& common,
    std::string_view branch);

// The operation Git has stopped in the middle of in the worktree at `dir`,
// e.g. "a merge", or nothing. These can leave the files clean, so a clean
// status alone doesn't mean the worktree is idle.
std::optional<std::string> operation_in_progress(
    const std::filesystem::path& dir);

// Reports whether the worktree has uncommitted changes, including untracked
// files, and counts them; with `count_ignored`, also lists ignored files.
// Never modifies the worktree or its index.
StatusResult worktree_status(const Worktree& worktree,
                             bool count_ignored = false);

// How wt shows a status, e.g. "dirty"; "-" for a bare repository.
std::string_view status_label(Status status);

}  // namespace wt::git
