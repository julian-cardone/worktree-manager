#include "commands/list.hpp"

#include "git/worktree.hpp"
#include "ui/format.hpp"

#include <ostream>
#include <string>
#include <vector>

namespace wt {

namespace {

std::string branch_label(const git::Worktree& worktree) {
    if (worktree.bare) {
        return "(bare)";
    }
    if (worktree.branch) {
        return *worktree.branch;
    }
    if (worktree.detached) {
        constexpr std::size_t kShortHashLength = 7;
        return "(detached " + worktree.head.substr(0, kShortHashLength) + ")";
    }
    return "(unknown)";
}

}  // namespace

int list_command(const Context& ctx) {
    const auto worktrees = git::list_worktrees(ctx.cwd);
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
        const std::string marker = i == current ? "* " : "  ";
        rows.push_back(
            {marker + ui::printable(branch_label(worktree)),
             std::string(git::status_label(status.status)),
             ui::printable(ui::tilde_path(worktree.path, ctx.home))});
    }
    ctx.out << ui::format_table(rows);
    return 0;
}

}  // namespace wt
