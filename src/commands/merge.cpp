#include "commands/merge.hpp"

#include "commands/merge_steps.hpp"
#include "commands/task_worktree.hpp"
#include "error.hpp"
#include "git/git.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "ui/format.hpp"

#include <exception>
#include <filesystem>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace wt {

namespace fs = std::filesystem;

void print_git_output(const Context& ctx, const git::Result& result) {
    for (const std::string* text : {&result.err, &result.out}) {
        std::istringstream lines(*text);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty()) {
                ctx.err << "  " << ui::printable(line) << '\n';
            }
        }
    }
}

namespace {

// Files with unresolved conflicts in the worktree at `dir`.
std::vector<std::string> conflicted_files(const fs::path& dir) {
    const git::Result r =
        git::run(dir, {"diff", "--name-only", "--diff-filter=U", "-z"});
    if (r.exit_code != 0) {
        throw git::Error(git::error_message(r.err));
    }
    std::vector<std::string> files;
    std::istringstream in(r.out);
    for (std::string file; std::getline(in, file, '\0');) {
        files.push_back(file);
    }
    return files;
}

// Checks that `dest` still has `base` checked out at `commit`. Returns what is
// wrong, if anything.
// WARNING: compare the branch too, not only HEAD's commit: a hook can point
// HEAD at another branch, or detach it, at the same commit.
std::optional<std::string> position_problem(const git::Worktree& dest,
                                            const std::string& base,
                                            const std::string& commit) {
    if (git::head_branch(dest.path) != "refs/heads/" + base) {
        return base + "'s worktree no longer has " + base + " checked out";
    }
    if (git::resolve_commit(dest.path, "HEAD") != commit) {
        return base + " moved from " + commit.substr(0, 7);
    }
    return std::nullopt;
}

// Aborts the merge in `dest` and checks that it is back on `base` at `before`,
// clean, with no merge in progress. Returns what is wrong, if anything.
// WARNING: a check that cannot run counts as failed; wt reports the merge
// undone only once every check here has passed.
std::optional<std::string> abort_merge(const git::Worktree& dest,
                                       const std::string& base,
                                       const std::string& before) {
    try {
        const git::Result r = git::run(dest.path, {"merge", "--abort"});
        if (r.exit_code != 0) {
            return "git merge --abort failed: " + git::error_message(r.err);
        }
    } catch (const std::exception& e) {
        return std::string("git merge --abort failed: ") + e.what();
    }
    try {
        if (git::resolve_commit(dest.path, "MERGE_HEAD")) {
            return "a merge is still in progress";
        }
        if (auto problem = position_problem(dest, base, before)) {
            return problem;
        }
        const auto status = git::worktree_status(dest);
        if (status.status != git::Status::clean) {
            return "the worktree is " +
                   std::string(git::status_label(status.status));
        }
    } catch (const std::exception& e) {
        return std::string("cannot verify the abort: ") + e.what();
    }
    return std::nullopt;
}

// Checks, after Git reports a successful merge, that `base` now contains both
// `task_commit` and `before`, its own commit before the merge, and that its
// worktree is clean and on it. Git's exit code alone doesn't prove that: a
// post-merge hook, for one, runs after the merge and can undo or add to it,
// or reset `base` to the task and drop its own commits. Returns what is wrong,
// if anything.
// WARNING: wt reports a merge only once this passes.
std::optional<std::string> merge_problem(const git::Worktree& dest,
                                         const std::string& base,
                                         const std::string& task,
                                         const std::string& task_commit,
                                         const std::string& before) {
    const auto tip = git::resolve_commit(dest.path, "refs/heads/" + base);
    if (!tip || !git::is_ancestor(dest.path, task_commit, *tip)) {
        return base + " doesn't contain " + task;
    }
    if (!git::is_ancestor(dest.path, before, *tip)) {
        return base + " no longer contains " + before.substr(0, 7) +
               ", where it was before the merge";
    }
    if (auto problem = position_problem(dest, base, *tip)) {
        return problem;
    }
    if (auto operation = git::operation_in_progress(dest.path)) {
        return base + "'s worktree is in the middle of " + *operation;
    }
    const auto status = git::worktree_status(dest);
    if (status.status != git::Status::clean) {
        return base + "'s worktree is " +
               std::string(git::status_label(status.status));
    }
    return std::nullopt;
}

// Explains, on stderr, that a failed merge may still be in progress in
// `dest`, after `headline` says why. Lists `conflicts`, which may be empty.
void report_unrestored(const Context& ctx, const std::string& headline,
                       const git::Result& result,
                       const std::vector<std::string>& conflicts,
                       const git::Worktree& dest, const std::string& base) {
    ctx.err << error_prefix(ErrorKind::git) << ui::printable(headline) << '\n';
    print_git_output(ctx, result);
    if (!conflicts.empty()) {
        ctx.err << "\nConflicts:\n";
        for (const auto& file : conflicts) {
            ctx.err << "- " << ui::printable(file) << '\n';
        }
    }
    ctx.err << '\n'
            << ui::printable(base)
            << "'s worktree may still be in the middle of the merge.\n"
            << "Don't run other wt commands until you have inspected it with "
            << "`git -C " << ui::printable(ui::shell_quote(dest.path.string()))
            << " status`.\n";
}

// Merges `task` into `base` in `dest`, or finds it has already landed.
// Returns 0 once `base` is verified to contain the task.
int land(const Context& ctx, const std::string& task, const std::string& base,
         const git::Worktree& dest, const git::Worktree* source) {
    // WARNING: checked here, after the slower checks on the task, so as
    // little as possible can change before the merge; aborting a failed
    // merge relies on what this checks.
    check_destination(dest, base);
    const std::string shown_task = ui::printable(task);
    const std::string shown_base = ui::printable(base);
    const auto divergence =
        git::divergence(ctx.repo, "refs/heads/" + task, "refs/heads/" + base);
    if (divergence.ahead == 0) {
        ctx.out << shown_base << " already contains " << shown_task
                << "; nothing to merge\n";
        return 0;
    }
    check_merge_config(dest, base);
    // Git would refuse too, but without saying how to get the task landed.
    if (divergence.behind != 0 && fast_forward_only(dest.path)) {
        report_behind(ctx, task, base, divergence.behind, source);
        return exit_code(ErrorKind::refused);
    }

    const auto before = git::resolve_commit(dest.path, "HEAD");
    if (!before) {
        throw Error(ErrorKind::internal,
                    "cannot read HEAD in " + dest.path.string());
    }
    // What gets merged is checked against this, so a commit added to the task
    // meanwhile can't make the check fail; it would land too.
    const auto task_commit =
        git::resolve_commit(ctx.repo, "refs/heads/" + task);
    if (!task_commit) {
        throw Error(ErrorKind::internal, "cannot read branch '" + task + "'");
    }
    if (const auto in_the_way = ignored_in_the_way(dest, *before, task);
        !in_the_way.empty()) {
        ctx.err << error_prefix(ErrorKind::refused) << "cannot merge "
                << shown_task << " into " << shown_base
                << "\n\nMerging would overwrite these ignored "
                << "files in " << shown_base << "'s worktree:\n";
        for (const auto& path : in_the_way) {
            ctx.err << "- " << ui::printable(path) << '\n';
        }
        ctx.err << "\nNothing was merged.\nMove them out of the way first.\n";
        return exit_code(ErrorKind::refused);
    }

    // The full ref can't be mistaken for a tag of the same name; the message
    // keeps Git from naming the commit after it ("Merge branch
    // 'refs/heads/x'"). --no-edit because no one is there to edit it.
    // WARNING: from here on Git may have changed `base`, so every failure is
    // reported here with what is known, never thrown; an error that escapes
    // wt merge means nothing was merged.
    git::Result result{0, {}, {}};
    try {
        result =
            git::run(dest.path, {"merge", "--no-edit", "-m",
                                 "Merge branch '" + task + "' into " + base,
                                 "refs/heads/" + task});
    } catch (const std::exception& e) {
        report_unrestored(
            ctx,
            std::string("wt could not tell whether git merge finished: ") +
                e.what(),
            result, {}, dest, base);
        return exit_code(ErrorKind::git);
    }
    if (result.exit_code == 0) {
        // Nothing is undone here: the merge did happen, and guessing what
        // else changed could lose more than it fixes.
        std::optional<std::string> problem;
        try {
            problem = merge_problem(dest, base, task, *task_commit, *before);
        } catch (const std::exception& e) {
            problem = std::string("wt could not check the result: ") + e.what();
        }
        if (problem) {
            ctx.err << error_prefix(ErrorKind::git)
                    << "git merge reported success, but "
                    << ui::printable(*problem) << '\n';
            print_git_output(ctx, result);
            ctx.err << "\nThe result could not be verified; inspect "
                    << shown_base << "'s worktree with `git -C "
                    << ui::printable(ui::shell_quote(dest.path.string()))
                    << " status` before retrying.\n";
            return exit_code(ErrorKind::git);
        }
        ctx.out << "✓ Merged " << shown_task << " into " << shown_base << '\n';
        return 0;
    }

    // Without MERGE_HEAD, Git refused before merging anything (e.g. an
    // untracked file in the way), so there is nothing to undo.
    // WARNING: if that can't be read, neither aborting nor leaving it is
    // known to be safe, so wt stops and says a merge may remain.
    bool merging = false;
    try {
        merging = git::resolve_commit(dest.path, "MERGE_HEAD").has_value();
    } catch (const std::exception& e) {
        report_unrestored(ctx,
                          "the merge failed, and wt could not tell whether it "
                          "is still in progress: " +
                              std::string(e.what()),
                          result, {}, dest, base);
        return exit_code(ErrorKind::git);
    }
    if (!merging) {
        ctx.err << error_prefix(ErrorKind::git)
                << "git merge failed with exit code " << result.exit_code
                << '\n';
        print_git_output(ctx, result);
        std::optional<std::string> problem;
        try {
            problem = position_problem(dest, base, *before);
        } catch (const std::exception& e) {
            problem =
                "wt could not confirm " + base + " is unchanged: " + e.what();
        }
        if (problem) {
            ctx.err << '\n'
                    << ui::printable(*problem) << "; inspect " << shown_base
                    << "'s worktree with `git -C "
                    << ui::printable(ui::shell_quote(dest.path.string()))
                    << " status`.\n";
        } else {
            ctx.err << '\n' << shown_base << " is unchanged.\n";
        }
        return exit_code(ErrorKind::git);
    }

    // Listed before the abort, which clears them. Failing to list them
    // mustn't stop the abort.
    std::vector<std::string> conflicts;
    std::optional<std::string> unlisted;
    try {
        conflicts = conflicted_files(dest.path);
    } catch (const std::exception& e) {
        unlisted = e.what();
    }
    if (auto problem = abort_merge(dest, base, *before)) {
        report_unrestored(
            ctx,
            (conflicts.empty() ? "the merge failed"
                               : task + " conflicts with " + base) +
                ", and wt could not undo the merge: " + *problem,
            result, conflicts, dest, base);
        return exit_code(ErrorKind::git);
    }

    ErrorKind kind = ErrorKind::conflict;
    if (conflicts.empty()) {
        // Git stopped mid-merge without conflicts, e.g. a hook rejected the
        // merge commit, or the conflicts couldn't be listed.
        kind = ErrorKind::git;
        ctx.err << error_prefix(ErrorKind::git)
                << "git merge failed with exit code " << result.exit_code
                << '\n';
        print_git_output(ctx, result);
        if (unlisted) {
            ctx.err << "\nwt could not list the conflicting files: "
                    << ui::printable(*unlisted) << '\n';
        }
    } else {
        ctx.err << error_prefix(ErrorKind::conflict) << shown_task
                << " conflicts with " << shown_base << "\n\nConflicts:\n";
        for (const auto& file : conflicts) {
            ctx.err << "- " << ui::printable(file) << '\n';
        }
    }
    ctx.err << '\n'
            << shown_base << " was restored; no merge remains in progress.\n";
    return exit_code(kind);
}

}  // namespace

int merge_command(const Context& ctx, const std::string& task, bool cleanup,
                  bool delete_ignored) {
    // Also confirms we're inside a repository, with Git's own error if not.
    const auto worktrees = git::list_worktrees(ctx.repo);
    if (!git::is_valid_branch_name(ctx.repo, task) ||
        !git::ref_exists(ctx.repo, "refs/heads/" + task)) {
        throw Error(ErrorKind::user, "no branch named '" + task + "'");
    }
    const std::string base = git::integration_branch(ctx.repo);
    if (task == base) {
        throw Error(ErrorKind::user, "'" + task +
                                         "' is the integration branch; there "
                                         "is nothing to merge it into");
    }

    // Mid-rebase, the branch still points at the commits being rewritten.
    if (const auto where = git::rebasing_worktree(
            worktrees, git::common_dir(ctx.repo), task)) {
        throw Error(ErrorKind::refused,
                    "'" + task + "' is being rebased in " + where->string() +
                        "; finish the rebase (`git rebase --continue`) or "
                        "abort it (`git rebase --abort`), then merge again");
    }

    // Merging needs a checkout of the integration branch. wt uses the one
    // that exists rather than switching some worktree over to it.
    const git::Worktree* dest = find_branch_worktree(worktrees, base);
    if (dest == nullptr) {
        // Mid-rebase, its worktree is detached, so it looks checked out
        // nowhere.
        if (const auto where = git::rebasing_worktree(
                worktrees, git::common_dir(ctx.repo), base)) {
            throw Error(ErrorKind::refused,
                        "the integration branch '" + base +
                            "' is being rebased in " + where->string() +
                            "; finish or abort the rebase, then merge again");
        }
        throw Error(ErrorKind::user, "the integration branch '" + base +
                                         "' isn't checked out in any "
                                         "worktree; check it out to merge "
                                         "into it");
    }
    const git::Worktree* source = find_branch_worktree(worktrees, task);
    if (source != nullptr) {
        check_source(*source, task);
    }
    if (cleanup &&
        !check_cleanup(ctx, worktrees, source, task, base, delete_ignored)) {
        return exit_code(ErrorKind::refused);
    }

    if (const int code = land(ctx, task, base, *dest, source);
        code != 0 || !cleanup) {
        return code;
    }
    return clean_up(ctx, task, base, delete_ignored);
}

}  // namespace wt
