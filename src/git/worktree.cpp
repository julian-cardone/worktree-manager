#include "git/worktree.hpp"

#include "git/git.hpp"

#include <algorithm>
#include <iterator>
#include <system_error>

namespace wt::git {

namespace fs = std::filesystem;

std::vector<Worktree> parse_worktree_list(std::string_view porcelain) {
    // Each attribute is NUL-terminated; an empty attribute ends a record.
    std::vector<Worktree> worktrees;
    bool in_record = false;
    std::size_t pos = 0;
    while (pos < porcelain.size()) {
        std::size_t end = porcelain.find('\0', pos);
        if (end == std::string_view::npos) {
            end = porcelain.size();
        }
        const std::string_view field = porcelain.substr(pos, end - pos);
        pos = end + 1;

        if (field.empty()) {
            in_record = false;
            continue;
        }
        const std::size_t space = field.find(' ');
        const std::string_view key = field.substr(0, space);
        const std::string_view value = space == std::string_view::npos
                                           ? std::string_view{}
                                           : field.substr(space + 1);

        if (key == "worktree") {
            worktrees.push_back(Worktree{.path = fs::path(std::string(value))});
            in_record = true;
            continue;
        }
        if (!in_record) {
            throw Error("unexpected output from git worktree list");
        }
        Worktree& worktree = worktrees.back();
        if (key == "HEAD") {
            worktree.head = value;
        } else if (key == "branch") {
            constexpr std::string_view kHeads = "refs/heads/";
            worktree.branch = std::string(value.starts_with(kHeads)
                                              ? value.substr(kHeads.size())
                                              : value);
        } else if (key == "bare") {
            worktree.bare = true;
        } else if (key == "detached") {
            worktree.detached = true;
        } else if (key == "locked") {
            worktree.locked = true;
        } else if (key == "prunable") {
            worktree.prunable = true;
        }
        // Attributes added by newer Git versions are ignored.
    }
    return worktrees;
}

std::vector<Worktree> list_worktrees(const fs::path& dir) {
    // -z keeps paths containing newlines from corrupting the output.
    const Result r = run(dir, {"worktree", "list", "--porcelain", "-z"});
    if (r.exit_code != 0) {
        if (r.err.find("unknown switch") != std::string::npos) {
            throw Error("wt requires Git 2.36 or newer");
        }
        throw Error(error_message(r.err));
    }
    return parse_worktree_list(r.out);
}

std::optional<std::size_t> find_worktree_containing(
    const std::vector<Worktree>& worktrees, const fs::path& dir) {
    // Git reports symlink-free paths, so resolve `dir` the same way.
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(dir, ec);
    if (ec) {
        resolved = dir.lexically_normal();
    }

    std::optional<std::size_t> best;
    std::size_t best_depth = 0;
    for (std::size_t i = 0; i < worktrees.size(); ++i) {
        const fs::path& root = worktrees[i].path;
        const auto [root_it, dir_it] = std::mismatch(
            root.begin(), root.end(), resolved.begin(), resolved.end());
        if (root_it != root.end()) {
            continue;
        }
        const auto depth =
            static_cast<std::size_t>(std::distance(root.begin(), root.end()));
        if (!best || depth > best_depth) {
            best = i;
            best_depth = depth;
        }
    }
    return best;
}

StatusResult worktree_status(const Worktree& worktree) {
    if (worktree.bare) {
        return {Status::bare, {}};
    }
    std::error_code ec;
    if (!fs::is_directory(worktree.path, ec)) {
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return {Status::unknown, ec.message()};
        }
        return {Status::missing, {}};
    }
    // Without its .git file, git would search upward and report the status of
    // whatever repository happens to enclose the directory.
    if (!fs::exists(worktree.path / ".git", ec)) {
        return {Status::unknown,
                ec ? ec.message() : "its .git file is missing"};
    }

    // --no-optional-locks stops status from rewriting the index, so it can't
    // collide with another git process (e.g. an agent) using the worktree.
    const Result r = run(
        worktree.path, {"--no-optional-locks", "status", "--porcelain", "-z"});
    if (r.exit_code != 0) {
        return {Status::unknown, error_message(r.err)};
    }
    return {r.out.empty() ? Status::clean : Status::dirty, {}};
}

}  // namespace wt::git
