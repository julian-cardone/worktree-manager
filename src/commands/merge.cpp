#include "commands/merge.hpp"

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
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace wt {

namespace fs = std::filesystem;

namespace {

// git config exits 1 for a key that isn't set.
constexpr int kNotFound = 1;

// Reports whether Git is configured to merge only by fast-forwarding, as
// `git config merge.ff only` does in repositories that keep a linear history.
bool fast_forward_only(const fs::path& dir) {
    const git::Result r = git::run(dir, {"config", "--get", "merge.ff"});
    if (r.exit_code == kNotFound) {
        return false;
    }
    if (r.exit_code != 0) {
        throw git::Error(git::error_message(r.err));
    }
    return r.out.substr(0, r.out.find('\n')) == "only";
}

// Explains, on stderr, that `task` must be rebased before it can land.
void report_behind(const Context& ctx, const std::string& task,
                   const std::string& base, std::size_t behind,
                   const git::Worktree* source) {
    const std::string shown_task = ui::printable(task);
    const std::string shown_base = ui::printable(base);
    ctx.err << error_prefix(ErrorKind::refused) << "cannot fast-forward "
            << shown_base << " to " << shown_task << "\n\n"
            << shown_task << " is " << behind
            << (behind == 1 ? " commit" : " commits") << " behind "
            << shown_base << ", and merge.ff is set to \"only\".\n";
    if (source != nullptr) {
        ctx.err << "Rebase it onto " << shown_base
                << " in its worktree, resolving any conflicts there:\n\n"
                << "  git -C "
                << ui::printable(ui::shell_quote(source->path.string()))
                << " rebase " << ui::printable(ui::shell_quote(base)) << "\n\n";
    } else {
        ctx.err << shown_task << " has no worktree; rebase it onto "
                << shown_base << " wherever you check it out.\n";
    }
    ctx.err << "Then run `wt merge " << ui::printable(ui::shell_quote(task))
            << "` again.\n";
}

// Fails unless the integration branch's worktree can take a merge and, if it
// fails, be put back exactly as it was.
// WARNING: `git merge --abort` only restores a worktree that was clean before
// the merge; it can throw away changes that were already there. Don't relax
// this check without also changing how a failed merge is undone.
void check_destination(const git::Worktree& dest, const std::string& base) {
    const std::string where = base + "'s worktree at " + dest.path.string();
    const auto status = git::worktree_status(dest);
    switch (status.status) {
        case git::Status::clean:
            break;
        case git::Status::dirty:
            throw Error(
                ErrorKind::refused,
                where +
                    " has uncommitted changes; commit or stash them before "
                    "merging into " +
                    base);
        case git::Status::missing:
            throw Error(ErrorKind::user, where + " no longer exists");
        case git::Status::bare:
            throw Error(ErrorKind::user, "cannot merge into a bare repository");
        case git::Status::unknown:
            throw Error(ErrorKind::refused, "cannot read the status of " +
                                                where + ": " + status.detail);
    }
    if (auto operation = git::operation_in_progress(dest.path)) {
        throw Error(ErrorKind::refused, where + " is in the middle of " +
                                            *operation +
                                            "; finish or abort it first");
    }
}

// Fails if the task's worktree has changes the merge would leave out, which
// is almost always work someone forgot to commit, or is in the middle of an
// operation, so its branch may not hold the finished work yet.
// WARNING: wt merges only from a clean, idle worktree; anything else makes
// what gets landed ambiguous.
void check_source(const git::Worktree& source, const std::string& task) {
    const auto status = git::worktree_status(source);
    switch (status.status) {
        case git::Status::clean:
            break;
        // Its commits are all on the branch; only the directory is gone.
        case git::Status::missing:
            return;
        case git::Status::dirty:
            throw Error(
                ErrorKind::refused,
                "the worktree for '" + task +
                    "' has uncommitted changes, which the merge wouldn't "
                    "include; commit or stash them first (`wt status " +
                    ui::shell_quote(task) + "` shows them)");
        case git::Status::bare:
        case git::Status::unknown:
            throw Error(ErrorKind::refused, "cannot read the status of " +
                                                source.path.string() + ": " +
                                                status.detail);
    }
    if (auto operation = git::operation_in_progress(source.path)) {
        throw Error(ErrorKind::refused,
                    "the worktree for '" + task + "' is in the middle of " +
                        *operation + "; finish or abort it, then merge again");
    }
}

// Paths in `dest` that merging `task` would silently overwrite. Git refuses
// to overwrite untracked files, but replaces ignored ones (e.g. a .env file)
// without a word. The destination is clean, so anything on disk where the
// task adds a file is ignored. Can over-report (e.g. a file the merge would
// end up deleting), which only errs toward refusing.
std::set<std::string> ignored_in_the_way(const git::Worktree& dest,
                                         const std::string& before,
                                         const std::string& task) {
    const git::Result r = git::run(
        dest.path, {"diff", "--name-only", "--no-renames", "--diff-filter=A",
                    "-z", before, "refs/heads/" + task, "--"});
    if (r.exit_code != 0) {
        throw git::Error(git::error_message(r.err));
    }
    std::set<std::string> found;
    std::istringstream in(r.out);
    for (std::string file; std::getline(in, file, '\0');) {
        // A file at any parent (e.g. ignored `build` for `build/x`) is in the
        // way too; a directory there is not.
        fs::path relative;
        for (const auto& part : fs::path(file)) {
            relative /= part;
            std::error_code ec;
            const auto type =
                fs::symlink_status(dest.path / relative, ec).type();
            if (type == fs::file_type::not_found) {
                break;
            }
            if (ec) {
                throw Error(ErrorKind::refused,
                            "cannot inspect " +
                                (dest.path / relative).string() + ": " +
                                ec.message());
            }
            if (type != fs::file_type::directory ||
                relative == fs::path(file)) {
                found.insert(relative.string());
                break;
            }
        }
    }
    return found;
}

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

// Aborts the merge in `dest` and checks that it is back at `before`, clean,
// with no merge in progress. Returns what is wrong, if anything.
std::optional<std::string> abort_merge(const git::Worktree& dest,
                                       const std::string& before) {
    const git::Result r = git::run(dest.path, {"merge", "--abort"});
    if (r.exit_code != 0) {
        return "git merge --abort failed: " + git::error_message(r.err);
    }
    if (git::resolve_commit(dest.path, "MERGE_HEAD")) {
        return "a merge is still in progress";
    }
    if (git::resolve_commit(dest.path, "HEAD") != before) {
        return "HEAD moved from " + before.substr(0, 7);
    }
    const auto status = git::worktree_status(dest);
    if (status.status != git::Status::clean) {
        return "the worktree is " +
               std::string(git::status_label(status.status));
    }
    return std::nullopt;
}

// Checks, after Git reports a successful merge, that `base` now contains
// `task_commit` and its worktree is clean and on it. Git's exit code alone
// doesn't prove that: a post-merge hook, for one, runs after the merge and
// can undo or add to it. Returns what is wrong, if anything.
// WARNING: wt reports a merge only once this passes.
std::optional<std::string> merge_problem(const git::Worktree& dest,
                                         const std::string& base,
                                         const std::string& task,
                                         const std::string& task_commit) {
    const auto tip = git::resolve_commit(dest.path, "refs/heads/" + base);
    if (!tip || !git::is_ancestor(dest.path, task_commit, *tip)) {
        return base + " doesn't contain " + task;
    }
    if (git::resolve_commit(dest.path, "HEAD") != tip) {
        return base + "'s worktree is no longer at " + base;
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

// Explains, on stderr, why the task's worktree and branch are still there
// after it landed, and how to finish. Rerunning is safe: the task has landed,
// so `wt merge` finds nothing to merge and goes straight to the cleanup.
void report_unfinished(const Context& ctx, const std::string& task,
                       const std::string& base, bool worktree_removed,
                       bool delete_ignored) {
    const std::string shown_task = ui::printable(task);
    ctx.err << '\n'
            << shown_task << " has landed in " << ui::printable(base)
            << ", but the cleanup didn't finish:\n";
    if (worktree_removed) {
        ctx.err << "- its worktree was removed\n";
    } else {
        ctx.err << "- its worktree is still there (`wt list` shows it)\n";
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
    const std::string upstream = r.out.substr(0, r.out.find('\n'));
    if (upstream.empty() || !git::resolve_commit(dir, upstream)) {
        return std::nullopt;
    }
    return upstream;
}

// Fails unless `task` could be removed with its branch once it has landed in
// `base`, so a predictable refusal comes before anything is merged. These
// are rechecked after landing, right before each step. Throws for most
// refusals; for files that would be lost, prints them and returns false.
bool check_cleanup(const Context& ctx,
                   const std::vector<git::Worktree>& worktrees,
                   const git::Worktree* source, const std::string& task,
                   const std::string& base, bool delete_ignored) {
    if (source != nullptr && !check_removable(ctx, worktrees, *source, task,
                                              delete_ignored, "wt merge")
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
                        ui::shell_quote(task) +
                        "`), then rerun; nothing was merged");
    }
    return true;
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

// Removes the worktree and branch of `task`, which has just landed in `base`.
// Each step rechecks what it depends on first. Never touches remote branches.
int clean_up(const Context& ctx, const std::string& task,
             const std::string& base, bool delete_ignored) {
    bool worktree_removed = false;
    try {
        const auto worktrees = git::list_worktrees(ctx.repo);
        if (const auto* source = find_branch_worktree(worktrees, task)) {
            // WARNING: checked again, since the merge (and its hooks) ran
            // since the check before it.
            const auto check = check_removable(ctx, worktrees, *source, task,
                                               delete_ignored, "wt merge");
            if (!check.allowed) {
                report_unfinished(ctx, task, base, false, delete_ignored);
                return exit_code(ErrorKind::refused);
            }
            if (const int code = remove_worktree(ctx, *source, check);
                code != 0) {
                report_unfinished(ctx, task, base, false, delete_ignored);
                return code;
            }
            worktree_removed = true;
        }
        if (const int code = delete_branch(ctx, task, base); code != 0) {
            report_unfinished(ctx, task, base, worktree_removed,
                              delete_ignored);
            return code;
        }
        return 0;
    } catch (const Error& e) {
        ctx.err << error_prefix(e.kind()) << ui::printable(e.what()) << '\n';
        report_unfinished(ctx, task, base, worktree_removed, delete_ignored);
        return exit_code(e.kind());
    } catch (const std::exception& e) {
        // Reported as app.cpp would, but with what is left behind.
        ctx.err << error_prefix(ErrorKind::internal) << ui::printable(e.what())
                << '\n';
        report_unfinished(ctx, task, base, worktree_removed, delete_ignored);
        return exit_code(ErrorKind::internal);
    }
}

// Merges `task` into `base` in `dest`, whose worktree check_destination has
// accepted, or finds it has already landed. Returns 0 once `base` is verified
// to contain the task.
int land(const Context& ctx, const std::string& task, const std::string& base,
         const git::Worktree& dest, const git::Worktree* source) {
    const std::string shown_task = ui::printable(task);
    const std::string shown_base = ui::printable(base);
    const auto divergence =
        git::divergence(ctx.repo, "refs/heads/" + task, "refs/heads/" + base);
    if (divergence.ahead == 0) {
        ctx.out << shown_base << " already contains " << shown_task
                << "; nothing to merge\n";
        return 0;
    }
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
        ctx.err << "\nMove them out of the way first.\n";
        return exit_code(ErrorKind::refused);
    }

    // The full ref can't be mistaken for a tag of the same name; the message
    // keeps Git from naming the commit after it ("Merge branch
    // 'refs/heads/x'"). --no-edit because no one is there to edit it.
    const git::Result result =
        git::run(dest.path, {"merge", "--no-edit", "-m",
                             "Merge branch '" + task + "' into " + base,
                             "refs/heads/" + task});
    if (result.exit_code == 0) {
        // Nothing is undone here: the merge did happen, and guessing what
        // else changed could lose more than it fixes.
        if (auto problem = merge_problem(dest, base, task, *task_commit)) {
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
    if (!git::resolve_commit(dest.path, "MERGE_HEAD")) {
        ctx.err << error_prefix(ErrorKind::git)
                << "git merge failed with exit code " << result.exit_code
                << '\n';
        print_git_output(ctx, result);
        if (git::resolve_commit(dest.path, "HEAD") == before) {
            ctx.err << '\n' << shown_base << " is unchanged.\n";
        }
        return exit_code(ErrorKind::git);
    }

    const auto conflicts = conflicted_files(dest.path);
    if (auto problem = abort_merge(dest, *before)) {
        ctx.err << error_prefix(ErrorKind::git)
                << "the merge failed, and wt could not restore " << shown_base
                << ": " << ui::printable(*problem) << '\n';
        print_git_output(ctx, result);
        ctx.err << "\nInspect " << shown_base << "'s worktree with `git -C "
                << ui::printable(ui::shell_quote(dest.path.string()))
                << " status`.\n";
        return exit_code(ErrorKind::git);
    }

    ErrorKind kind = ErrorKind::conflict;
    if (conflicts.empty()) {
        // Git stopped mid-merge without conflicts, e.g. a hook rejected the
        // merge commit.
        kind = ErrorKind::git;
        ctx.err << error_prefix(ErrorKind::git)
                << "git merge failed with exit code " << result.exit_code
                << '\n';
        print_git_output(ctx, result);
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
    check_destination(*dest, base);
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
