#include "commands/new.hpp"

#include "error.hpp"
#include "git/git.hpp"
#include "git/repo.hpp"
#include "git/worktree.hpp"
#include "layout/container.hpp"
#include "layout/task.hpp"
#include "ui/format.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace wt {

namespace fs = std::filesystem;

namespace {

bool path_exists(const fs::path& path) {
    std::error_code ec;
    // symlink_status so a dangling symlink still counts as taken.
    const fs::file_status st = fs::symlink_status(path, ec);
    if (st.type() == fs::file_type::not_found) {
        return false;
    }
    if (ec) {
        throw Error(ErrorKind::user,
                    "cannot inspect " + path.string() + ": " + ec.message());
    }
    return true;
}

// Task names are ASCII, so ASCII case folding is enough.
bool same_ignoring_case(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

// Whether one name is a directory of the other, e.g. "fix" and "fix/login".
bool nested(std::string_view a, std::string_view b, bool ignore_case) {
    if (a.size() > b.size()) {
        std::swap(a, b);
    }
    if (a.size() == b.size() || b[a.size()] != '/') {
        return false;
    }
    const std::string_view prefix = b.substr(0, a.size());
    return ignore_case ? same_ignoring_case(a, prefix) : a == prefix;
}

struct CaseConflict {
    std::string branch;
    bool nested;  // otherwise the names differ only in case
};

// An existing local branch that clashes with `branch` only when case is
// ignored: "Fix" for "fix", or, if `check_nested`, "Fix" for "fix/login" and
// the reverse. Nothing if `branch` itself exists; clashes that hold with case
// are left to the checks that report them.
std::optional<CaseConflict> case_conflict(const Context& ctx,
                                          const std::string& branch,
                                          bool check_nested) {
    std::optional<CaseConflict> conflict;
    for (auto& existing : git::local_branches(ctx.repo)) {
        if (existing == branch) {
            return std::nullopt;
        }
        if (same_ignoring_case(existing, branch)) {
            if (!conflict || conflict->nested) {
                conflict = CaseConflict{std::move(existing), false};
            }
        } else if (!conflict && check_nested &&
                   nested(existing, branch, true) &&
                   !nested(existing, branch, false)) {
            conflict = CaseConflict{std::move(existing), true};
        }
    }
    return conflict;
}

// The existing entry the filesystem matched for `target`, with its own
// casing: on a case-insensitive filesystem, "Fix" finds a folder named "fix".
// Returns `target` itself if there's no differently cased entry to name.
fs::path existing_entry(const fs::path& target) {
    const std::string name = target.filename().string();
    std::optional<fs::path> variant;
    std::error_code ec;
    for (fs::directory_iterator it(target.parent_path(), ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string entry = it->path().filename().string();
        if (entry == name) {
            return target;
        }
        if (!variant && same_ignoring_case(entry, name)) {
            variant = it->path();
        }
    }
    return ec ? target : variant.value_or(target);
}

// Fails if `branch` exists, Git couldn't create it next to an existing one, or,
// where Git treats case as insignificant, it would clash with an existing
// branch when case is ignored.
// WARNING: wt new never attaches the task to a branch that already exists; its
// commits may belong to unrelated work.
void check_branch_free(const Context& ctx,
                       const std::vector<git::Worktree>& worktrees,
                       const std::string& branch) {
    const bool exists = git::ref_exists(ctx.repo, "refs/heads/" + branch);
    // On a case-insensitive filesystem, loose refs differing only in case are
    // the same file, so Git finds "Fix" when only "fix" exists. A packed "Fix"
    // isn't found that way, and without reflogs nothing else stops Git from
    // creating "fix" or "fix/login" next to it; either ref's next loose write
    // then shadows the other or fails.
    // WARNING: refuse these clashes however the existing ref is stored, so two
    // branches can never share a ref file or directory.
    const bool ignore_case = git::ignores_case(ctx.repo);
    if (exists || ignore_case) {
        if (auto conflict = case_conflict(ctx, branch, ignore_case)) {
            throw Error(
                ErrorKind::user,
                conflict->nested
                    ? "cannot create branch '" + branch + "' because branch '" +
                          conflict->branch +
                          "' exists, and Git can't have both when case is "
                          "ignored; choose another task name"
                    : "task '" + branch + "' conflicts with existing branch '" +
                          conflict->branch +
                          "' (names differ only in case); choose another task "
                          "name");
        }
    }
    if (exists) {
        std::string where;
        for (const auto& worktree : worktrees) {
            if (worktree.branch == branch) {
                where = " and is checked out at " + worktree.path.string();
                break;
            }
        }
        throw Error(ErrorKind::user,
                    "branch '" + branch + "' already exists" + where +
                        "; wt new only creates new branches, so choose "
                        "another task name");
    }
    if (auto other = git::conflicting_branch(ctx.repo, branch)) {
        throw Error(ErrorKind::user,
                    "cannot create branch '" + branch + "' because branch '" +
                        *other +
                        "' exists, and Git can't have both; choose another "
                        "task name");
    }
}

// Fails if the target path is already taken in any way Git or the filesystem
// would notice. Git checks too, but its errors are less clear. Only call this
// once the container is verified, so wt never looks inside one it doesn't own.
// WARNING: never reuse or overwrite what is there; it isn't wt's. A refusal,
// not a user error: the task itself may be fine.
void check_target_free(const std::vector<git::Worktree>& worktrees,
                       const fs::path& requested) {
    const bool exists = path_exists(requested);
    // Errors name what is actually there, which may differ in case.
    const fs::path target = exists ? existing_entry(requested) : requested;
    const std::string note =
        target == requested ? "" : " (folder names differ only in case)";
    for (const auto& worktree : worktrees) {
        if (worktree.path != target) {
            continue;
        }
        if (exists) {
            throw Error(ErrorKind::refused,
                        target.string() + " is already the worktree for " +
                            (worktree.branch ? "branch " + *worktree.branch
                                             : std::string("a detached HEAD")) +
                            note);
        }
        // `git worktree prune` would also forget every other stale worktree.
        const std::string forget = worktree.branch
                                       ? "`wt remove " +
                                             ui::shell_quote(*worktree.branch) +
                                             " --forget-missing`"
                                       : std::string("`git worktree prune`");
        throw Error(ErrorKind::refused,
                    "Git already has a worktree registered at " +
                        target.string() + " although its directory is gone; " +
                        forget + " forgets it");
    }
    if (exists) {
        throw Error(ErrorKind::refused,
                    "cannot create the worktree: " + target.string() +
                        " already exists" + note +
                        "; wt never reuses or overwrites it, "
                        "so move it away or choose another task name");
    }
}

// Says what exists now after wt new failed partway, once Git may have created
// the branch or the worktree, but removes none of it: it may already hold a
// hook's output, or not be the task's at all if something else created it
// meanwhile, and cleanup that guesses wrong would lose more than it saves.
// WARNING: never remove anything here, and suggest a cleanup only when every
// part is exactly what wt new would have made, so the advice can't touch
// anything wt didn't create.
void report_state(const Context& ctx, const std::string& branch,
                  const fs::path& target, const std::string& start) {
    std::optional<std::string> tip;
    std::optional<git::Worktree> registered;
    bool dir_exists = false;
    try {
        tip = git::resolve_commit(ctx.repo, "refs/heads/" + branch);
        for (auto& worktree : git::list_worktrees(ctx.repo)) {
            if (worktree.path == target) {
                registered = std::move(worktree);
                break;
            }
        }
        dir_exists = path_exists(target);
    } catch (const std::exception& e) {
        ctx.err << "wt: warning: cannot check what was left behind: "
                << ui::printable(e.what()) << '\n'
                << "wt: no cleanup was attempted; check the branch and "
                   "worktree with `wt list` and `git branch`\n";
        return;
    }

    const bool branch_as_made = tip == start;
    const bool worktree_as_made =
        registered && dir_exists && registered->branch == branch;

    ctx.err << "wt: wt new did not complete; current state:\n"
            << "  branch " << ui::printable(branch) << ": ";
    if (!tip) {
        ctx.err << "doesn't exist";
    } else if (branch_as_made) {
        ctx.err << "exists, at the commit wt started it from";
    } else {
        ctx.err << "exists, but is at " << *tip << ", not at " << start;
    }
    ctx.err << "\n  " << ui::printable(target.string()) << ": ";
    if (registered && dir_exists) {
        ctx.err << "a worktree with "
                << (registered->branch
                        ? "branch " + ui::printable(*registered->branch)
                        : std::string("a detached HEAD"))
                << " checked out";
    } else if (registered) {
        ctx.err << "Git lists a worktree here, but its directory is gone";
    } else if (dir_exists) {
        ctx.err << "exists, but Git doesn't list it as a worktree";
    } else {
        ctx.err << "doesn't exist";
    }
    ctx.err << '\n';

    const bool worktree_absent = !registered && !dir_exists;
    if (!tip && worktree_absent) {
        ctx.err << "wt: nothing was left behind\n";
        return;
    }
    if ((tip && !branch_as_made) || !(worktree_as_made || worktree_absent)) {
        ctx.err << "wt: no cleanup was attempted; wt can't tell how all of "
                   "this came about, so check it with `wt list` and "
                   "`git branch` before removing anything\n";
        return;
    }
    // The worktree goes first: Git won't delete a branch that is checked out.
    ctx.err << "wt: no cleanup was attempted; if you don't need "
            << (worktree_as_made ? "them" : "it") << ", ";
    if (worktree_as_made) {
        ctx.err << "remove the worktree with `wt remove "
                << ui::printable(ui::shell_quote(branch)) << "`, then ";
    }
    ctx.err << "delete the branch with `git branch -d "
            << ui::printable(ui::shell_quote(branch)) << "`\n";
}

// Checks, after Git reports success, that it lists a worktree at `target` with
// `branch` checked out, and that `branch` starts at `start`. A post-checkout
// hook, for one, runs in the new worktree once Git has made it, and can switch
// it to another branch, move the branch, or delete it. Returns what is wrong,
// if anything.
// WARNING: wt prints the path, which scripts `cd` into, only once this passes.
std::optional<std::string> creation_problem(const Context& ctx,
                                            const std::string& branch,
                                            const fs::path& target,
                                            const std::string& start) {
    for (const auto& worktree : git::list_worktrees(ctx.repo)) {
        if (worktree.path != target) {
            continue;
        }
        if (worktree.branch != branch) {
            return target.string() + " has " +
                   (worktree.branch ? "branch " + *worktree.branch
                                    : std::string("a detached HEAD")) +
                   " checked out, not " + branch;
        }
        if (!path_exists(target)) {
            return target.string() + " is gone";
        }
        const auto tip = git::resolve_commit(ctx.repo, "refs/heads/" + branch);
        if (tip != start) {
            return "branch " + branch + " is at " + tip.value_or("nothing") +
                   ", not at " + start;
        }
        return std::nullopt;
    }
    return "Git doesn't list a worktree at " + target.string();
}

}  // namespace

int new_command(const Context& ctx, const std::string& task) {
    if (auto problem = layout::task_name_problem(task)) {
        throw Error(ErrorKind::user,
                    "invalid task name '" + task + "': " + *problem);
    }
    // Also confirms we're inside a repository, with Git's own error if not.
    const auto worktrees = git::list_worktrees(ctx.repo);
    if (worktrees.empty()) {
        throw Error(ErrorKind::internal, "git reported no worktrees");
    }
    if (!git::is_valid_branch_name(ctx.repo, task)) {
        throw Error(ErrorKind::user,
                    "invalid task name '" + task +
                        "': Git doesn't accept it as a branch name");
    }
    const std::string& branch = task;
    const std::string base = git::integration_branch(ctx.repo);

    // Always the main worktree, even when run from a linked one, so every
    // worktree of the repository ends up in the same container.
    const fs::path common = git::common_dir(ctx.repo);
    const fs::path container =
        layout::container_path(worktrees.front().path, common);
    const fs::path target = container / layout::task_folder_name(task);

    check_branch_free(ctx, worktrees, branch);
    layout::prepare_container(container, common);
    check_target_free(worktrees, target);

    if (git::ref_exists(ctx.repo, "refs/remotes/origin/" + branch)) {
        ctx.err << "wt: warning: origin/" << ui::printable(branch)
                << " already exists; the new branch starts from "
                << ui::printable(base) << " and doesn't track it\n";
    }

    // WARNING: resolved immediately before creating, and given to Git as a
    // commit, so the task starts exactly where wt reports even if the base
    // moves meanwhile, and never from anything but the integration branch.
    const auto start =
        git::resolve_commit(ctx.repo, "refs/heads/" + base + "^{commit}");
    if (!start) {
        throw Error(ErrorKind::user, "the integration branch '" + base +
                                         "' disappeared; check it with "
                                         "`git branch` and rerun");
    }
    // Read now, so nothing can fail between creating and reporting.
    const std::string start_summary = git::commit_summary(ctx.repo, *start);
    // "--" keeps the path from being read as an option.
    const git::Result result = git::run(
        ctx.repo,
        {"worktree", "add", "-b", branch, "--", target.string(), *start});
    if (result.exit_code != 0) {
        ctx.err << error_prefix(ErrorKind::git)
                << "git worktree add failed with exit code " << result.exit_code
                << '\n';
        std::istringstream lines(result.err);
        for (std::string line; std::getline(lines, line);) {
            ctx.err << "  " << ui::printable(line) << '\n';
        }
        report_state(ctx, branch, target, *start);
        return exit_code(ErrorKind::git);
    }
    // Nothing goes on stdout, so `cd "$(wt new ...)"` can't land somewhere
    // unexpected.
    if (auto problem = creation_problem(ctx, branch, target, *start)) {
        ctx.err << error_prefix(ErrorKind::git)
                << "git worktree add reported success, but "
                << ui::printable(*problem) << '\n';
        report_state(ctx, branch, target, *start);
        return exit_code(ErrorKind::git);
    }

    ctx.err << "wt: created branch " << ui::printable(branch) << " from "
            << ui::printable(base) << " at " << ui::printable(start_summary)
            << '\n';
    // Unsanitized on purpose: `cd "$(wt new foo)"` needs the exact path.
    ctx.out << target.string() << '\n';
    return 0;
}

}  // namespace wt
