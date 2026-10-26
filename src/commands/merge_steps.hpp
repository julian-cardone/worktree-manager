#pragma once

// The steps of `wt merge`, shared only among merge.cpp, merge_preflight.cpp
// and merge_cleanup.cpp. merge.cpp calls them in order; see merge_command and
// land there for where each check runs relative to the mutation it protects.

#include "commands/app.hpp"
#include "git/git.hpp"
#include "git/worktree.hpp"

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace wt {

// --- Preflight: is it safe to merge? (merge_preflight.cpp) ---------------

// Reports whether Git is configured to merge only by fast-forwarding, as
// `git config merge.ff only` does in repositories that keep a linear history.
bool fast_forward_only(const std::filesystem::path& dir);

// Fails if Git is configured to change how a merge into `base` runs: Git adds
// `branch.<base>.mergeOptions` to every merge into `base`, and uses
// `pull.twohead` as every merge's default strategy, not only git pull's.
// Some values (e.g. -Xours, ours) settle conflicts without stopping, and the
// task is still an ancestor afterwards, so nothing after the merge could tell.
void check_merge_config(const git::Worktree& dest, const std::string& base);

// Explains, on stderr, that `task` must be rebased before it can land.
void report_behind(const Context& ctx, const std::string& task,
                   const std::string& base, std::size_t behind,
                   const git::Worktree* source);

// Fails unless the integration branch's worktree can take a merge and, if it
// fails, be put back exactly as it was.
void check_destination(const git::Worktree& dest, const std::string& base);

// Fails if the task's worktree has changes the merge would leave out, which
// is almost always work someone forgot to commit, or is in the middle of an
// operation, so its branch may not hold the finished work yet.
void check_source(const git::Worktree& source, const std::string& task);

// Paths in `dest` that merging `task` would silently overwrite. Git refuses
// to overwrite untracked files, but replaces ignored ones (e.g. a .env file)
// without a word. The destination is clean, so anything on disk where the
// task adds a file is ignored. Can over-report (e.g. a file the merge would
// end up deleting), which only errs toward refusing.
std::set<std::string> ignored_in_the_way(const git::Worktree& dest,
                                         const std::string& before,
                                         const std::string& task);

// --- Cleanup after landing (merge_cleanup.cpp) ----------------------------

// Fails unless `task` could be removed with its branch once it has landed in
// `base`, so a predictable refusal comes before anything is merged. These
// are rechecked after landing, right before each step. Throws for most
// refusals; for files that would be lost, prints them and returns false.
bool check_cleanup(const Context& ctx,
                   const std::vector<git::Worktree>& worktrees,
                   const git::Worktree* source, const std::string& task,
                   const std::string& base, bool delete_ignored);

// Removes the worktree and branch of `task`, which has just landed in `base`.
// Each step rechecks what it depends on first. Never touches remote branches.
int clean_up(const Context& ctx, const std::string& task,
             const std::string& base, bool delete_ignored);

// --- Shared reporting (merge.cpp) -----------------------------------------

// Prints what a Git command wrote, indented, to stderr.
void print_git_output(const Context& ctx, const git::Result& result);

}  // namespace wt
