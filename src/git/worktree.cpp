#include "git/worktree.hpp"

#include "git/git.hpp"

#include <algorithm>
#include <fstream>
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

Changes parse_status(std::string_view porcelain) {
    // Each entry is NUL-terminated. A rename or copy ("2") is followed by an
    // extra entry holding the original path, which must not be read as an
    // entry of its own: a path like "? x" would otherwise count as untracked.
    Changes changes;
    bool original_path_next = false;
    std::size_t pos = 0;
    while (pos < porcelain.size()) {
        std::size_t end = porcelain.find('\0', pos);
        if (end == std::string_view::npos) {
            end = porcelain.size();
        }
        const std::string_view entry = porcelain.substr(pos, end - pos);
        pos = end + 1;

        if (original_path_next) {
            original_path_next = false;
            continue;
        }
        if (entry.size() < 2 || entry[1] != ' ') {
            throw Error("unexpected output from git status");
        }
        switch (entry.front()) {
            case '1':
            case '2':
                // "<type> XY ...": X is the staged state, Y the unstaged
                // one, and '.' means unchanged.
                if (entry.size() < 4) {
                    throw Error("unexpected output from git status");
                }
                changes.staged += entry[2] != '.' ? 1 : 0;
                changes.modified += entry[3] != '.' ? 1 : 0;
                original_path_next = entry.front() == '2';
                break;
            case 'u':
                ++changes.conflicts;
                break;
            case '?':
                ++changes.untracked;
                break;
            case '!':  // only listed with --ignored
                changes.ignored.emplace_back(entry.substr(2));
                break;
            case '#':  // headers, e.g. from --branch
                break;
            default:
                throw Error("unexpected output from git status");
        }
    }
    return changes;
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

namespace {

// Git reports symlink-free paths, so resolve `dir` the same way.
fs::path resolve(const fs::path& dir) {
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(dir, ec);
    return ec ? dir.lexically_normal() : resolved;
}

// Reports whether `resolved` is `root` or lies inside it.
bool within(const fs::path& root, const fs::path& resolved) {
    return std::mismatch(root.begin(), root.end(), resolved.begin(),
                         resolved.end())
               .first == root.end();
}

}  // namespace

std::optional<std::size_t> find_worktree_containing(
    const std::vector<Worktree>& worktrees, const fs::path& dir) {
    const fs::path resolved = resolve(dir);
    std::optional<std::size_t> best;
    std::size_t best_depth = 0;
    for (std::size_t i = 0; i < worktrees.size(); ++i) {
        const fs::path& root = worktrees[i].path;
        if (!within(root, resolved)) {
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

bool worktree_contains(const Worktree& worktree, const fs::path& dir) {
    return within(worktree.path, resolve(dir));
}

namespace {

// The first line of the file at `path`, or nothing if there is no such file.
std::optional<std::string> read_first_line(const fs::path& path) {
    std::error_code ec;
    if (fs::symlink_status(path, ec).type() == fs::file_type::not_found) {
        return std::nullopt;
    }
    std::ifstream in(path);
    std::string line;
    if (ec || !in || (!std::getline(in, line) && !in.eof())) {
        throw Error("cannot read " + path.string());
    }
    return line;
}

// Reports whether a rebase of `ref` is in progress in the worktree whose
// administrative directory is `admin`.
bool rebasing(const fs::path& admin, const std::string& ref) {
    // The two directories rebase keeps its state in, depending on backend.
    for (const char* state : {"rebase-merge", "rebase-apply"}) {
        if (read_first_line(admin / state / "head-name") == ref) {
            return true;
        }
    }
    return false;
}

}  // namespace

// WARNING: Git has no command that reports this, so it reads the rebase state
// Git keeps in each worktree's administrative directory, as Git itself does
// before letting a branch be deleted or checked out elsewhere.
std::optional<fs::path> rebasing_worktree(
    const std::vector<Worktree>& worktrees, const fs::path& common,
    std::string_view branch) {
    const std::string ref = "refs/heads/" + std::string(branch);
    // The main worktree's state lives in the shared directory itself.
    if (!worktrees.empty() && !worktrees.front().bare &&
        rebasing(common, ref)) {
        return worktrees.front().path;
    }

    // Linked worktrees each have a directory under worktrees/, including ones
    // whose files are gone.
    const fs::path linked = common / "worktrees";
    std::error_code ec;
    fs::directory_iterator it(linked, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        return std::nullopt;
    }
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path admin = it->path();
        if (!rebasing(admin, ref)) {
            continue;
        }
        // gitdir holds the path of the worktree's .git file.
        if (const auto dot_git = read_first_line(admin / "gitdir")) {
            return fs::path(*dot_git).parent_path();
        }
        return admin;
    }
    if (ec) {
        throw Error("cannot read " + linked.string() + ": " + ec.message());
    }
    return std::nullopt;
}

StatusResult worktree_status(const Worktree& worktree, bool count_ignored) {
    if (worktree.bare) {
        return {Status::bare, {}, {}};
    }
    std::error_code ec;
    if (!fs::is_directory(worktree.path, ec)) {
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return {Status::unknown, ec.message(), {}};
        }
        return {Status::missing, {}, {}};
    }
    // Without its .git file, git would search upward and report the status of
    // whatever repository happens to enclose the directory.
    if (!fs::exists(worktree.path / ".git", ec)) {
        return {Status::unknown,
                ec ? ec.message() : "its .git file is missing",
                {}};
    }

    // --no-optional-locks stops status from rewriting the index, so it can't
    // collide with another git process (e.g. an agent) using the worktree.
    // --untracked-files=normal overrides status.showUntrackedFiles=no, which
    // would otherwise hide untracked files and report the worktree as clean.
    std::vector<std::string> args = {"--no-optional-locks", "status",
                                     "--porcelain=v2", "-z",
                                     "--untracked-files=normal"};
    if (count_ignored) {
        args.push_back("--ignored");
    }
    const Result r = run(worktree.path, args);
    if (r.exit_code != 0) {
        return {Status::unknown, error_message(r.err), {}};
    }
    Changes changes;
    try {
        changes = parse_status(r.out);
    } catch (const Error& e) {
        return {Status::unknown, e.what(), {}};
    }
    return {changes.empty() ? Status::clean : Status::dirty, {}, changes};
}

std::string_view status_label(Status status) {
    switch (status) {
        case Status::clean:
            return "clean";
        case Status::dirty:
            return "dirty";
        case Status::bare:
            return "-";
        case Status::missing:
            return "missing";
        case Status::unknown:
            return "unknown";
    }
    return "unknown";
}

}  // namespace wt::git
