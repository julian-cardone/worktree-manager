#include "commands/merge.hpp"

#include "commands/task_worktree.hpp"
#include "git/git.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "ui/format.hpp"

#include <filesystem>
#include <optional>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace wt {

namespace fs = std::filesystem;

namespace {

// rev-parse exits 1 for a name that doesn't resolve.
constexpr int kNotFound = 1;

// The commit `name` (e.g. "HEAD", "MERGE_HEAD") resolves to in the worktree at
// `dir`, or nothing if it doesn't exist there.
std::optional<std::string> resolve(const fs::path& dir,
                                   const std::string& name) {
    const git::Result r =
        git::run(dir, {"rev-parse", "--verify", "--quiet", name});
    if (r.exit_code == kNotFound) {
        return std::nullopt;
    }
    if (r.exit_code != 0) {
        throw git::Error(git::error_message(r.err));
    }
    return r.out.substr(0, r.out.find('\n'));
}

// The operation Git has stopped in the middle of in the worktree at `dir`,
// e.g. "a merge", or nothing. These can leave the files clean, so a clean
// status alone doesn't mean it's safe to start a merge.
std::optional<std::string> operation_in_progress(const fs::path& dir) {
    const std::pair<const char*, const char*> kOperations[] = {
        {"MERGE_HEAD", "a merge"},
        {"CHERRY_PICK_HEAD", "a cherry-pick"},
        {"REVERT_HEAD", "a revert"},
    };
    for (const auto& [ref, what] : kOperations) {
        if (resolve(dir, ref)) {
            return what;
        }
    }
    return std::nullopt;
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
            throw std::runtime_error(
                where +
                " has uncommitted changes; commit or stash them before "
                "merging into " +
                base);
        case git::Status::missing:
            throw std::runtime_error(where + " no longer exists");
        case git::Status::bare:
            throw std::runtime_error("cannot merge into a bare repository");
        case git::Status::unknown:
            throw std::runtime_error("cannot read the status of " + where +
                                     ": " + status.detail);
    }
    if (auto operation = operation_in_progress(dest.path)) {
        throw std::runtime_error(where + " is in the middle of " + *operation +
                                 "; finish or abort it first");
    }
}

// Fails if the task's worktree has changes the merge would leave out, which
// is almost always work someone forgot to commit.
void check_source(const git::Worktree& source, const std::string& task) {
    const auto status = git::worktree_status(source);
    switch (status.status) {
        case git::Status::clean:
        // Its commits are all on the branch; only the directory is gone.
        case git::Status::missing:
            return;
        case git::Status::dirty:
            throw std::runtime_error(
                "the worktree for '" + task +
                "' has uncommitted changes, which the merge wouldn't "
                "include; commit or stash them first (`wt status " +
                task + "` shows them)");
        case git::Status::bare:
        case git::Status::unknown:
            throw std::runtime_error("cannot read the status of " +
                                     source.path.string() + ": " +
                                     status.detail);
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
                throw std::runtime_error("cannot inspect " +
                                         (dest.path / relative).string() +
                                         ": " + ec.message());
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
    if (resolve(dest.path, "MERGE_HEAD")) {
        return "a merge is still in progress";
    }
    if (resolve(dest.path, "HEAD") != before) {
        return "HEAD moved from " + before.substr(0, 7);
    }
    const auto status = git::worktree_status(dest);
    if (status.status != git::Status::clean) {
        return "the worktree is " +
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

}  // namespace

int merge_command(const Context& ctx, const std::string& task) {
    // Also confirms we're inside a repository, with Git's own error if not.
    const auto worktrees = git::list_worktrees(ctx.cwd);
    if (!git::is_valid_branch_name(ctx.cwd, task) ||
        !git::ref_exists(ctx.cwd, "refs/heads/" + task)) {
        throw std::runtime_error("no branch named '" + task + "'");
    }
    const std::string base = git::integration_branch(ctx.cwd);
    if (task == base) {
        throw std::runtime_error("'" + task +
                                 "' is the integration branch; there is "
                                 "nothing to merge it into");
    }

    // Merging needs a checkout of the integration branch. wt uses the one
    // that exists rather than switching some worktree over to it.
    const git::Worktree* dest = find_branch_worktree(worktrees, base);
    if (dest == nullptr) {
        throw std::runtime_error("the integration branch '" + base +
                                 "' isn't checked out in any worktree; "
                                 "check it out to merge into it");
    }
    check_destination(*dest, base);
    if (const auto* source = find_branch_worktree(worktrees, task)) {
        check_source(*source, task);
    }

    const std::string shown_task = ui::printable(task);
    const std::string shown_base = ui::printable(base);
    if (git::divergence(ctx.cwd, "refs/heads/" + task, "refs/heads/" + base)
            .ahead == 0) {
        ctx.out << shown_base << " already contains " << shown_task
                << "; nothing to merge\n";
        return 0;
    }

    const auto before = resolve(dest->path, "HEAD");
    if (!before) {
        throw std::runtime_error("cannot read HEAD in " + dest->path.string());
    }
    if (const auto in_the_way = ignored_in_the_way(*dest, *before, task);
        !in_the_way.empty()) {
        ctx.err << "wt: error: cannot merge " << shown_task << " into "
                << shown_base << "\n\nMerging would overwrite these ignored "
                << "files in " << shown_base << "'s worktree:\n";
        for (const auto& path : in_the_way) {
            ctx.err << "- " << ui::printable(path) << '\n';
        }
        ctx.err << "\nMove them out of the way first.\n";
        return 1;
    }

    // The full ref can't be mistaken for a tag of the same name; the message
    // keeps Git from naming the commit after it ("Merge branch
    // 'refs/heads/x'"). --no-edit because no one is there to edit it.
    const git::Result result =
        git::run(dest->path, {"merge", "--no-edit", "-m",
                              "Merge branch '" + task + "' into " + base,
                              "refs/heads/" + task});
    if (result.exit_code == 0) {
        ctx.out << "✓ Merged " << shown_task << " into " << shown_base << '\n';
        return 0;
    }

    // Without MERGE_HEAD, Git refused before merging anything (e.g. an
    // untracked file in the way), so there is nothing to undo.
    if (!resolve(dest->path, "MERGE_HEAD")) {
        ctx.err << "wt: error: git merge failed with exit code "
                << result.exit_code << '\n';
        print_git_output(ctx, result);
        if (resolve(dest->path, "HEAD") == before) {
            ctx.err << '\n' << shown_base << " is unchanged.\n";
        }
        return 1;
    }

    const auto conflicts = conflicted_files(dest->path);
    if (auto problem = abort_merge(*dest, *before)) {
        ctx.err << "wt: error: the merge failed, and wt could not restore "
                << shown_base << ": " << ui::printable(*problem) << '\n';
        print_git_output(ctx, result);
        ctx.err << "\nInspect " << shown_base << "'s worktree with `git -C "
                << ui::printable(dest->path.string()) << " status`.\n";
        return 1;
    }

    if (conflicts.empty()) {
        // Git stopped mid-merge without conflicts, e.g. a hook rejected the
        // merge commit.
        ctx.err << "wt: error: git merge failed with exit code "
                << result.exit_code << '\n';
        print_git_output(ctx, result);
    } else {
        ctx.err << "wt: error: merge blocked: " << shown_task
                << " conflicts with " << shown_base << "\n\nConflicts:\n";
        for (const auto& file : conflicts) {
            ctx.err << "- " << ui::printable(file) << '\n';
        }
    }
    ctx.err << '\n'
            << shown_base << " was restored; no merge remains in progress.\n";
    return 1;
}

}  // namespace wt
