#include "commands/merge_steps.hpp"

#include "error.hpp"
#include "git/git.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "ui/format.hpp"

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

namespace wt {

namespace fs = std::filesystem;

namespace {

// git config exits 1 for a key that isn't set.
constexpr int kNotFound = 1;

}  // namespace

bool fast_forward_only(const fs::path& dir) {
    const git::Result r = git::run(dir, {"config", "--get", "merge.ff"});
    if (r.exit_code == kNotFound) {
        return false;
    }
    if (r.exit_code != 0) {
        throw git::Error(git::error_message(r.err));
    }
    return git::single_value(r.out) == "only";
}

// WARNING: refuse rather than override: clearing the settings with `-c` or
// `-s` would silently ignore the repository's configuration. Any value
// refuses; telling safe ones from unsafe ones isn't worth getting one wrong.
void check_merge_config(const git::Worktree& dest, const std::string& base) {
    std::string found;
    std::string where;
    for (const std::string& key :
         {"branch." + base + ".mergeOptions", std::string("pull.twohead")}) {
        // Read where the merge runs, so it sees the same config Git will.
        const git::Result r =
            git::run(dest.path, {"config", "-z", "--get-all", key});
        if (r.exit_code == kNotFound) {
            continue;
        }
        if (r.exit_code != 0) {
            throw git::Error(git::error_message(r.err));
        }
        std::string values;
        std::istringstream in(r.out);
        for (std::string value; std::getline(in, value, '\0');) {
            values += (values.empty() ? "" : ", ") + ("'" + value + "'");
        }
        found += (found.empty() ? "" : "; ") + key + " is set to " + values;
        // It may come from any config file, so say how to find it rather than
        // offer a command that only edits one of them.
        where += std::string(where.empty() ? "" : " and ") +
                 "`git config --show-origin --get-all " + ui::shell_quote(key) +
                 "`";
    }
    if (found.empty()) {
        return;
    }
    throw Error(ErrorKind::refused,
                found +
                    ". This merge configuration can change how Git merges "
                    "into " +
                    base +
                    " and resolve conflicts without stopping, so wt can't "
                    "guarantee they are left to you, and it won't override "
                    "the repository's settings; to merge with wt, unset them "
                    "where they are configured (" +
                    where + " shows where)");
}

void report_behind(const Context& ctx, const std::string& task,
                   const std::string& base, std::size_t behind,
                   const git::Worktree* source) {
    const std::string shown_task = ui::printable(task);
    const std::string shown_base = ui::printable(base);
    ctx.err << error_prefix(ErrorKind::refused) << "cannot fast-forward "
            << shown_base << " to " << shown_task << "\n\n"
            << shown_task << " is " << behind
            << (behind == 1 ? " commit" : " commits") << " behind "
            << shown_base << ", and merge.ff is set to \"only\".\n"
            << "Nothing was merged.\n";
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

// WARNING: `git merge --abort` only restores a worktree that was clean before
// the merge; it can throw away changes that were already there. Don't relax
// this check without also changing how a failed merge is undone.
void check_destination(const git::Worktree& dest, const std::string& base) {
    const std::string where = base + "'s worktree at " + dest.path.string();
    const auto status = git::worktree_status(dest);
    switch (status.status) {
        case git::Status::clean:
        case git::Status::dirty:
            break;
        case git::Status::missing:
            throw Error(ErrorKind::user, where + " no longer exists");
        case git::Status::bare:
            throw Error(ErrorKind::user, "cannot merge into a bare repository");
        case git::Status::unknown:
            throw Error(ErrorKind::refused, "cannot read the status of " +
                                                where + ": " + status.detail);
    }
    // The worktree was found from a listing taken earlier; merging there if
    // someone has since switched it would land the task on another branch.
    if (git::head_branch(dest.path) != "refs/heads/" + base) {
        throw Error(ErrorKind::refused,
                    where + " no longer has " + base + " checked out");
    }
    // Checked before the changes because an interrupted merge leaves both,
    // and Git won't commit or stash while conflicts are unresolved.
    if (auto operation = git::operation_in_progress(dest.path)) {
        throw Error(ErrorKind::refused, where + " is in the middle of " +
                                            *operation +
                                            "; finish or abort it first");
    }
    if (status.status == git::Status::dirty) {
        throw Error(ErrorKind::refused,
                    where +
                        " has uncommitted changes; commit or stash them before "
                        "merging into " +
                        base);
    }
}

// WARNING: wt merges only from a clean, idle worktree; anything else makes
// what gets landed ambiguous.
void check_source(const git::Worktree& source, const std::string& task) {
    const auto status = git::worktree_status(source);
    switch (status.status) {
        case git::Status::clean:
        case git::Status::dirty:
            break;
        // Its commits are all on the branch; only the directory is gone.
        case git::Status::missing:
            return;
        case git::Status::bare:
        case git::Status::unknown:
            throw Error(ErrorKind::refused, "cannot read the status of " +
                                                source.path.string() + ": " +
                                                status.detail);
    }
    // Checked before the changes, as for the destination.
    if (auto operation = git::operation_in_progress(source.path)) {
        throw Error(ErrorKind::refused,
                    "the worktree for '" + task + "' is in the middle of " +
                        *operation + "; finish or abort it, then merge again");
    }
    if (status.status == git::Status::dirty) {
        throw Error(ErrorKind::refused,
                    "the worktree for '" + task +
                        "' has uncommitted changes, which the merge wouldn't "
                        "include; commit or stash them first (`wt status " +
                        ui::shell_quote(task) + "` shows them)");
    }
}

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

}  // namespace wt
