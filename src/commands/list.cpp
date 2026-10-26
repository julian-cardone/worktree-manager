#include "commands/list.hpp"

#include "error.hpp"
#include "git/worktree.hpp"
#include "ui/format.hpp"

#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace wt {

namespace {

// `rebasing` is the branch a rebase there is rewriting, if any.
std::string branch_label(const git::Worktree& worktree,
                         const std::optional<std::string>& rebasing) {
    if (worktree.bare) {
        return "(bare)";
    }
    if (worktree.branch) {
        return *worktree.branch;
    }
    // A rebase detaches HEAD until it finishes, but the worktree is still
    // working on the branch.
    if (rebasing) {
        return "(rebasing " + *rebasing + ")";
    }
    if (worktree.detached) {
        constexpr std::size_t kShortHashLength = 7;
        return "(detached " + worktree.head.substr(0, kShortHashLength) + ")";
    }
    return "(unknown)";
}

// "clean", or e.g. "clean (bisect)" when an operation that can leave the
// files clean is in progress, since `wt merge` and `wt remove` refuse those.
std::string status_label(git::Status status,
                         const std::optional<std::string>& operation) {
    std::string label(git::status_label(status));
    if (operation) {
        // Drop the article: "a git am" reads "(git am)".
        constexpr std::string_view kArticle = "a ";
        label += " (" +
                 (operation->starts_with(kArticle)
                      ? operation->substr(kArticle.size())
                      : *operation) +
                 ")";
    }
    return label;
}

}  // namespace

int list_command(const Context& ctx) {
    const auto worktrees = git::list_worktrees(ctx.repo);
    const auto current = git::find_worktree_containing(worktrees, ctx.cwd);

    std::vector<std::vector<std::string>> rows = {
        {"  BRANCH", "STATUS", "PATH"}};
    for (std::size_t i = 0; i < worktrees.size(); ++i) {
        const auto& worktree = worktrees[i];
        const auto status = git::worktree_status(worktree);
        if (status.status == git::Status::unknown) {
            ctx.err << "wt: warning: cannot read the status of "
                    << ui::printable(worktree.path.string()) << ": "
                    << ui::printable(status.detail) << '\n';
        }
        std::optional<std::string> operation;
        std::optional<std::string> rebasing;
        if (status.status == git::Status::clean ||
            status.status == git::Status::dirty) {
            try {
                operation = git::operation_in_progress(worktree.path);
                if (!worktree.branch) {
                    rebasing = git::rebased_branch(worktree);
                }
            } catch (const Error& e) {
                // Not "clean" alone, which would claim the worktree is idle.
                operation = "operation unknown";
                ctx.err << "wt: warning: " << ui::printable(e.what()) << '\n';
            }
        }
        const std::string marker = i == current ? "* " : "  ";
        rows.push_back(
            {marker + ui::printable(branch_label(worktree, rebasing)),
             status_label(status.status, operation),
             ui::printable(ui::tilde_path(worktree.path, ctx.home))});
    }
    ctx.out << ui::format_table(rows);
    return 0;
}

}  // namespace wt
