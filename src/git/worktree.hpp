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

// Reports whether `a` and `b` are the same path, however Git and the
// filesystem spelled it. On macOS, Git stores the paths it is given with their
// Unicode precomposed (core.precomposeunicode), while the filesystem returns
// names as they were created and treats both forms as one name, so a path wt
// passes to Git can come back as different bytes. Doesn't resolve symlinks.
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b);

// Returns the index of the worktree containing `dir`, preferring the deepest
// match so a worktree nested inside another is still found.
std::optional<std::size_t> find_worktree_containing(
    const std::vector<Worktree>& worktrees, const std::filesystem::path& dir);

// Reports whether `dir` is the worktree's directory or lies inside it.
bool worktree_contains(const Worktree& worktree,
                       const std::filesystem::path& dir);

// The worktree in which `branch` (a short name) is being rebased, if any,
// either as the branch rebased or as one `git rebase --update-refs` will move.
// Mid-rebase, the branch still points at its old commits and isn't checked
// out anywhere, so nothing in `git worktree list` shows the branch is in use.
// `common` is the repository's shared Git directory. Throws a refused
// wt::Error if a worktree's rebase state can't be read, since a branch that
// might be mid-rebase isn't safe to use.
std::optional<std::filesystem::path> rebasing_worktree(
    const std::vector<Worktree>& worktrees, const std::filesystem::path& common,
    std::string_view branch);

// The branch (a short name) an interrupted rebase in `worktree` is rewriting,
// or nothing if no rebase is in progress there or it is rebasing a detached
// HEAD. A rebase detaches HEAD until it finishes, so this is how to tell
// which branch the worktree is working on. Throws a refused wt::Error if the
// rebase state can't be read.
std::optional<std::string> rebased_branch(const Worktree& worktree);

// An operation Git can stop in the middle of.
enum class Operation {
    merge,
    rebase,
    am,
    cherry_pick,
    revert,
    // A cherry-pick or revert of several commits, between steps; Git's state
    // doesn't say which.
    sequencer,
    bisect,
};

// The operation Git has stopped in the middle of in the worktree at `dir`, or
// nothing. These can leave the files clean, so a clean status alone doesn't
// mean the worktree is idle. Throws a refused wt::Error if that can't be
// determined.
std::optional<Operation> current_operation(const std::filesystem::path& dir);

// current_operation, described for messages, e.g. "a merge" or "a git am".
std::optional<std::string> operation_in_progress(
    const std::filesystem::path& dir);

// How messages name an operation, e.g. "a git am".
std::string_view describe(Operation operation);

// An operation's name in wt's JSON output, e.g. "am".
// WARNING: part of wt's interface, documented in README.md; agents and scripts
// branch on these. Don't rename them.
std::string_view operation_name(Operation operation);

// Reports whether the worktree has uncommitted changes, including untracked
// files, and counts them; with `count_ignored`, also lists ignored files.
// Never modifies the worktree or its index.
StatusResult worktree_status(const Worktree& worktree,
                             bool count_ignored = false);

// Files in the worktree at `dir` that Git has been told not to check for
// changes (assume-unchanged or skip-worktree) and whose content there differs
// from the index. `git status`, and Git's own check before removing a
// worktree, skip these files, so edits to them would be lost without a word.
// A file that is absent isn't listed: the index still has its content.
// Never modifies the worktree or its index, and stores no Git objects (though
// a clean filter, e.g. Git LFS, may cache what it reads). Throws a refused
// wt::Error if this can't be determined.
std::vector<std::string> hidden_changes(const std::filesystem::path& dir);

// How wt shows a status, e.g. "dirty"; "-" for a bare repository.
std::string_view status_label(Status status);

// A status's name in wt's JSON output, e.g. "bare".
// WARNING: part of wt's interface, documented in README.md; agents and scripts
// branch on these. Don't rename them.
std::string_view status_name(Status status);

// The commit the worktree has checked out, or nothing for a bare repository or
// a branch with no commits yet.
std::optional<std::string> head_commit(const Worktree& worktree);

}  // namespace wt::git
