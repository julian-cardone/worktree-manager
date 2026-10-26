#include "commands/merge_steps.hpp"

#include "commands/remove.hpp"
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
#include <string>
#include <vector>

namespace wt {

namespace fs = std::filesystem;

namespace {

// Explains, on stderr, what is left of `task` after it landed in `base` but
// the cleanup failed, and how to finish. Reads the current state rather than
// trusting what the failed step reported, which may not know. Rerunning is
// safe: the task has landed, so `wt merge` finds nothing to merge and goes
// straight to the cleanup.
void report_unfinished(const Context& ctx, const std::string& task,
                       const std::string& base, bool delete_ignored) {
    const std::string shown_task = ui::printable(task);
    ctx.err << '\n' << shown_task << " has landed in " << ui::printable(base);
    bool has_worktree = false;
    bool has_branch = false;
    try {
        has_worktree = find_branch_worktree(git::list_worktrees(ctx.repo),
                                            task) != nullptr;
        has_branch = git::ref_exists(ctx.repo, "refs/heads/" + task);
    } catch (const std::exception& e) {
        ctx.err << ", but the cleanup didn't finish, and wt could not check "
                   "what is left: "
                << ui::printable(e.what()) << '\n'
                << "Check it with `wt list` and `git branch` before "
                   "retrying.\n";
        return;
    }
    if (!has_branch && !has_worktree) {
        ctx.err << "; its worktree and branch are gone, so nothing is left "
                   "to clean up.\n";
        return;
    }
    ctx.err << ", but the cleanup didn't finish:\n"
            << (has_worktree ? "- its worktree is still there (`wt list` "
                               "shows it)\n"
                             : "- it has no worktree\n");
    // Git still lists a worktree whose branch was deleted under it, e.g. by
    // a hook; rerunning can't find the task then.
    if (!has_branch) {
        ctx.err << "- its branch " << shown_task << " no longer exists\n"
                << "\nCheck it with `wt list` and `git branch` before "
                   "removing anything.\n";
        return;
    }
    ctx.err << "- its branch " << shown_task << " was kept\n"
            << "\nOnce that's dealt with, finish the cleanup with:\n\n"
            << "  wt merge " << ui::printable(ui::shell_quote(task))
            << " --cleanup" << (delete_ignored ? " --delete-ignored" : "")
            << "\n";
}

// The upstream `git branch -d` would check `task` against, if it has one: the
// full name of a ref that exists, e.g. "refs/remotes/origin/task". For an
// upstream that is configured but gone, Git checks against HEAD instead.
std::optional<std::string> existing_upstream(const fs::path& dir,
                                             const std::string& task) {
    const git::Result r = git::run(
        dir, {"for-each-ref", "--format=%(upstream)", "refs/heads/" + task});
    if (r.exit_code != 0) {
        throw git::Error(git::error_message(r.err));
    }
    const std::string upstream = git::single_value(r.out);
    if (upstream.empty() || !git::resolve_commit(dir, upstream)) {
        return std::nullopt;
    }
    return upstream;
}

// Deletes the branch of `task`, which has landed in `base`, once it is proven
// that no commit would be lost. Prints what was deleted and returns 0, or
// reports the failure to stderr and returns its exit code.
int delete_branch(const Context& ctx, const std::string& task,
                  const std::string& base) {
    const std::string ref = "refs/heads/" + task;
    // WARNING: everything is read again here, after the merge and the
    // worktree's removal; only the branch as it is now may be deleted.
    const auto worktrees = git::list_worktrees(ctx.repo);
    if (const auto* other = find_branch_worktree(worktrees, task)) {
        throw Error(ErrorKind::refused, "'" + task + "' is checked out in " +
                                            other->path.string() +
                                            ", so wt won't delete it");
    }
    if (const auto where = git::rebasing_worktree(
            worktrees, git::common_dir(ctx.repo), task)) {
        throw Error(ErrorKind::refused, "'" + task + "' is being rebased in " +
                                            where->string() +
                                            ", so wt won't delete it");
    }
    const git::Worktree* dest = find_branch_worktree(worktrees, base);
    if (dest == nullptr) {
        throw Error(ErrorKind::user, "the integration branch '" + base +
                                         "' isn't checked out in any "
                                         "worktree anymore");
    }
    const auto tip = git::resolve_commit(ctx.repo, ref);
    const auto base_tip = git::resolve_commit(ctx.repo, "refs/heads/" + base);
    if (!tip || !base_tip) {
        throw Error(ErrorKind::refused,
                    "cannot read branch '" + (tip ? base : task) +
                        "', so wt won't delete '" + task + "'");
    }
    // WARNING: this is the proof that deleting the branch loses no commits.
    if (!git::is_ancestor(ctx.repo, *tip, *base_tip)) {
        throw Error(ErrorKind::refused, "'" + task + "' now has commits that " +
                                            base + " doesn't (it is at " +
                                            tip->substr(0, 7) +
                                            "), so wt won't delete it");
    }
    // WARNING: -d, never -D. Git checks again that the branch is merged into
    // HEAD, the integration branch in this worktree (or into its upstream),
    // so a commit added since the check above makes it refuse.
    const git::Result result =
        git::run(dest->path, {"branch", "-d", "--", task});
    if (result.exit_code != 0) {
        ctx.err << error_prefix(ErrorKind::git)
                << "git branch -d failed with exit code " << result.exit_code
                << '\n';
        print_git_output(ctx, result);
        return exit_code(ErrorKind::git);
    }
    if (git::ref_exists(ctx.repo, ref)) {
        ctx.err << error_prefix(ErrorKind::git)
                << "git branch -d reported success, but branch "
                << ui::printable(task) << " still exists\n";
        return exit_code(ErrorKind::git);
    }
    ctx.out << "Deleted branch " << ui::printable(task) << " (was "
            << tip->substr(0, 7) << ")\n";
    return 0;
}

}  // namespace

bool check_cleanup(const Context& ctx,
                   const std::vector<git::Worktree>& worktrees,
                   const git::Worktree* source, const std::string& task,
                   const std::string& base, bool delete_ignored) {
    // A missing worktree is never forgotten here: that takes an explicit
    // `wt remove --forget-missing` first, which the refusal explains.
    if (source != nullptr &&
        !check_removable(ctx, worktrees, *source, task, delete_ignored,
                         /*forget_missing=*/false, "wt merge")
             .allowed) {
        ctx.err << "Nothing was merged.\n";
        return false;
    }
    // `git branch -d` also wants the branch in its upstream, which landing
    // only takes care of when the upstream is the integration branch.
    const auto upstream = existing_upstream(ctx.repo, task);
    if (upstream && *upstream != "refs/heads/" + base &&
        !git::is_ancestor(ctx.repo, "refs/heads/" + task, *upstream)) {
        throw Error(ErrorKind::refused,
                    "'" + task + "' has commits its upstream " + *upstream +
                        " doesn't, so Git wouldn't delete the branch once it "
                        "lands; update the upstream or unset it (`git branch "
                        "--unset-upstream " +
                        ui::shell_quote(task) + "`), then rerun");
    }
    return true;
}

int clean_up(const Context& ctx, const std::string& task,
             const std::string& base, bool delete_ignored) {
    try {
        const auto worktrees = git::list_worktrees(ctx.repo);
        if (const auto* source = find_branch_worktree(worktrees, task)) {
            // WARNING: checked again, since the merge (and its hooks) ran
            // since the check before it.
            const auto check =
                check_removable(ctx, worktrees, *source, task, delete_ignored,
                                /*forget_missing=*/false, "wt merge");
            if (!check.allowed) {
                report_unfinished(ctx, task, base, delete_ignored);
                return exit_code(ErrorKind::refused);
            }
            if (const int code = remove_worktree(ctx, *source, check);
                code != 0) {
                report_unfinished(ctx, task, base, delete_ignored);
                return code;
            }
        }
        if (const int code = delete_branch(ctx, task, base); code != 0) {
            report_unfinished(ctx, task, base, delete_ignored);
            return code;
        }
        return 0;
    } catch (const Error& e) {
        ctx.err << error_prefix(e.kind()) << ui::printable(e.what()) << '\n';
        report_unfinished(ctx, task, base, delete_ignored);
        return exit_code(e.kind());
    } catch (const std::exception& e) {
        // Reported as app.cpp would, but with what is left behind.
        ctx.err << error_prefix(ErrorKind::internal) << ui::printable(e.what())
                << '\n';
        report_unfinished(ctx, task, base, delete_ignored);
        return exit_code(ErrorKind::internal);
    }
}

}  // namespace wt
