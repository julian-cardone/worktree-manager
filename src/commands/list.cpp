#include "commands/list.hpp"

#include "error.hpp"
#include "git/worktree.hpp"
#include "ui/format.hpp"
#include "ui/json.hpp"

#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace wt {

namespace {

// What `wt list` reports about one worktree, in either format.
struct Entry {
    const git::Worktree& worktree;
    git::Status status;
    bool current;
    std::optional<git::Operation> operation;
    // Whether an operation might be in progress but wt couldn't tell.
    bool operation_unknown = false;
    // The branch a rebase there is rewriting, if any.
    std::optional<std::string> rebasing;
};

std::string branch_label(const Entry& entry) {
    const auto& worktree = entry.worktree;
    if (worktree.bare) {
        return "(bare)";
    }
    if (worktree.branch) {
        return *worktree.branch;
    }
    // A rebase detaches HEAD until it finishes, but the worktree is still
    // working on the branch.
    if (entry.rebasing) {
        return "(rebasing " + *entry.rebasing + ")";
    }
    if (worktree.detached) {
        constexpr std::size_t kShortHashLength = 7;
        return "(detached " + worktree.head.substr(0, kShortHashLength) + ")";
    }
    return "(unknown)";
}

// "clean", or e.g. "clean (bisect)" when an operation that can leave the
// files clean is in progress, since `wt merge` and `wt remove` refuse those.
std::string status_label(const Entry& entry) {
    std::string label(git::status_label(entry.status));
    if (entry.operation_unknown) {
        // Not "clean" alone, which would claim the worktree is idle.
        label += " (operation unknown)";
    } else if (entry.operation) {
        // Drop the article: "a git am" reads "(git am)".
        const std::string_view operation = git::describe(*entry.operation);
        constexpr std::string_view kArticle = "a ";
        label += " (" +
                 std::string(operation.starts_with(kArticle)
                                 ? operation.substr(kArticle.size())
                                 : operation) +
                 ")";
    }
    return label;
}

void print_table(const Context& ctx, const std::vector<Entry>& entries) {
    std::vector<std::vector<std::string>> rows = {
        {"  BRANCH", "STATUS", "PATH"}};
    for (const auto& entry : entries) {
        const std::string marker = entry.current ? "* " : "  ";
        rows.push_back(
            {marker + ui::printable(branch_label(entry)), status_label(entry),
             ui::printable(ui::tilde_path(entry.worktree.path, ctx.home))});
    }
    ctx.out << ui::format_table(rows);
}

// WARNING: the field names and meanings are part of wt's interface,
// documented in README.md. Add fields rather than change existing ones.
void print_json(const Context& ctx, const std::vector<Entry>& entries) {
    using ui::Json;
    std::vector<Json> items;
    for (const auto& entry : entries) {
        const auto& worktree = entry.worktree;
        const Json operation =
            entry.operation_unknown
                ? Json::string("unknown")
                : (entry.operation
                       ? Json::string(git::operation_name(*entry.operation))
                       : Json::null());
        items.push_back(Json::object({
            {"path", Json::string(worktree.path.string())},
            {"branch", Json::string_or_null(worktree.branch ? worktree.branch
                                                            : entry.rebasing)},
            {"head", Json::string_or_null(git::head_commit(worktree))},
            {"status", Json::string(git::status_name(entry.status))},
            {"operation", operation},
            {"current", Json::boolean(entry.current)},
            {"detached", Json::boolean(worktree.detached)},
            {"locked", Json::boolean(worktree.locked)},
        }));
    }
    ctx.out << Json::object({{"worktrees", Json::array(items)}}).text() << '\n';
}

}  // namespace

int list_command(const Context& ctx, bool json) {
    const auto worktrees = git::list_worktrees(ctx.repo);
    const auto current = git::find_worktree_containing(worktrees, ctx.cwd);

    std::vector<Entry> entries;
    for (std::size_t i = 0; i < worktrees.size(); ++i) {
        const auto& worktree = worktrees[i];
        const auto status = git::worktree_status(worktree);
        if (status.status == git::Status::unknown) {
            ctx.err << "wt: warning: cannot read the status of "
                    << ui::printable(worktree.path.string()) << ": "
                    << ui::printable(status.detail) << '\n';
        }
        Entry entry{.worktree = worktree,
                    .status = status.status,
                    .current = i == current};
        if (status.status == git::Status::clean ||
            status.status == git::Status::dirty) {
            try {
                entry.operation = git::current_operation(worktree.path);
                if (!worktree.branch) {
                    entry.rebasing = git::rebased_branch(worktree);
                }
            } catch (const Error& e) {
                entry.operation_unknown = true;
                ctx.err << "wt: warning: " << ui::printable(e.what()) << '\n';
            }
        }
        entries.push_back(entry);
    }
    if (json) {
        print_json(ctx, entries);
    } else {
        print_table(ctx, entries);
    }
    return 0;
}

}  // namespace wt
