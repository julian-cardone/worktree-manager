#include "git/repo.hpp"

#include "error.hpp"
#include "git/git.hpp"
#include "ui/format.hpp"

#include <charconv>
#include <sstream>
#include <system_error>

namespace wt::git {

namespace fs = std::filesystem;

namespace {

// Git exits 1 for "not found" from the queries below; anything else non-zero
// is a real failure that shouldn't be mistaken for an answer.
constexpr int kNotFound = 1;

std::string first_line(const std::string& out) {
    return out.substr(0, out.find('\n'));
}

bool local_branch_exists(const fs::path& dir, const std::string& name) {
    return ref_exists(dir, "refs/heads/" + name);
}

// Absolute, symlink-free, so paths from different queries compare equal.
fs::path canonical_path(const std::string& path) {
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(path, ec);
    return ec ? fs::path(path).lexically_normal() : resolved;
}

}  // namespace

fs::path invoked_repository(const fs::path& cwd,
                            const std::optional<std::string>& git_dir) {
    if (!git_dir) {
        return cwd;
    }
    // Every Git command wt runs drops GIT_DIR (see git::run), since it would
    // override the -C that aims checks at a particular worktree. This is the
    // one place it is read: --git-dir, which Git resolves against -C just as
    // it resolves GIT_DIR against the current directory.
    const Result named =
        run(cwd, {"--git-dir=" + *git_dir, "rev-parse",
                  "--path-format=absolute", "--git-dir", "--git-common-dir"});
    const auto newline = named.out.find('\n');
    if (named.exit_code != 0 || newline == std::string::npos) {
        throw wt::Error(
            ErrorKind::user,
            "GIT_DIR is set to '" + *git_dir +
                "', which isn't a Git directory: " + error_message(named.err) +
                "; unset GIT_DIR or point it at one");
    }
    const fs::path named_dir = canonical_path(named.out.substr(0, newline));
    const fs::path named_common =
        canonical_path(first_line(named.out.substr(newline + 1)));

    const Result here =
        run(cwd, {"rev-parse", "--path-format=absolute", "--git-dir"});
    if (here.exit_code != 0) {
        // Not in a repository, so GIT_DIR alone names one, as it would for Git
        // itself (e.g. a bare repository of dotfiles).
        return named_common;
    }
    // WARNING: Unknown state means stop. GIT_DIR naming another repository,
    // or another worktree of this one, leaves it unclear what the user means;
    // picking either could aim a mutation at the wrong worktree.
    const fs::path here_dir = canonical_path(first_line(here.out));
    if (here_dir != named_dir) {
        throw wt::Error(ErrorKind::refused,
                        "GIT_DIR points at " + named_dir.string() +
                            ", but the current directory belongs to " +
                            here_dir.string() +
                            "; unset GIT_DIR, or run wt from the worktree it "
                            "points at");
    }
    return cwd;
}

fs::path common_dir(const fs::path& dir) {
    const Result r =
        run(dir, {"rev-parse", "--path-format=absolute", "--git-common-dir"});
    if (r.exit_code != 0) {
        throw Error(error_message(r.err));
    }
    return canonical_path(first_line(r.out));
}

bool ref_exists(const fs::path& dir, std::string_view ref) {
    const Result r =
        run(dir, {"show-ref", "--verify", "--quiet", std::string(ref)});
    if (r.exit_code == 0) {
        return true;
    }
    if (r.exit_code == kNotFound) {
        return false;
    }
    throw Error(error_message(r.err));
}

std::optional<std::string> conflicting_branch(const fs::path& dir,
                                              std::string_view branch) {
    const std::string name(branch);
    for (auto slash = name.find('/'); slash != std::string::npos;
         slash = name.find('/', slash + 1)) {
        if (local_branch_exists(dir, name.substr(0, slash))) {
            return name.substr(0, slash);
        }
    }
    const Result r =
        run(dir, {"for-each-ref", "--count=1", "--format=%(refname:strip=2)",
                  "refs/heads/" + name + "/"});
    if (r.exit_code != 0) {
        throw Error(error_message(r.err));
    }
    const std::string nested = first_line(r.out);
    if (nested.empty()) {
        return std::nullopt;
    }
    return nested;
}

std::vector<std::string> local_branches(const fs::path& dir) {
    // The full refname, because %(refname:short) can add a "heads/" prefix
    // when a tag has the same name.
    const Result r =
        run(dir, {"for-each-ref", "--format=%(refname)", "refs/heads/"});
    if (r.exit_code != 0) {
        throw Error(error_message(r.err));
    }
    constexpr std::string_view kPrefix = "refs/heads/";
    std::vector<std::string> branches;
    std::istringstream lines(r.out);
    for (std::string line; std::getline(lines, line);) {
        if (line.starts_with(kPrefix)) {
            branches.push_back(line.substr(kPrefix.size()));
        }
    }
    return branches;
}

bool ignores_case(const fs::path& dir) {
    const Result r =
        run(dir, {"config", "--type=bool", "--get", "core.ignorecase"});
    if (r.exit_code == kNotFound) {
        return false;
    }
    if (r.exit_code != 0) {
        throw Error(error_message(r.err));
    }
    return first_line(r.out) == "true";
}

std::optional<std::string> resolve_commit(const fs::path& dir,
                                          std::string_view name) {
    const Result r =
        run(dir, {"rev-parse", "--verify", "--quiet", std::string(name)});
    if (r.exit_code == kNotFound) {
        return std::nullopt;
    }
    if (r.exit_code != 0) {
        throw Error(error_message(r.err));
    }
    return first_line(r.out);
}

bool is_ancestor(const fs::path& dir, std::string_view ancestor,
                 std::string_view commit) {
    const Result r = run(dir, {"merge-base", "--is-ancestor",
                               std::string(ancestor), std::string(commit)});
    if (r.exit_code == 0) {
        return true;
    }
    if (r.exit_code == kNotFound) {
        return false;
    }
    throw Error(error_message(r.err));
}

bool is_valid_branch_name(const fs::path& dir, std::string_view name) {
    // check-ref-format has no "--" separator, so a leading '-' would be read
    // as an option.
    if (name.empty() || name.front() == '-') {
        return false;
    }
    const Result r =
        run(dir, {"check-ref-format", "--branch", std::string(name)});
    // --branch expands shorthands like @{-1}; only accept names that are
    // already literal.
    return r.exit_code == 0 && first_line(r.out) == name;
}

std::string commit_summary(const fs::path& dir, std::string_view commit) {
    // --no-show-signature keeps log.showSignature from adding GPG output.
    const Result r = run(dir, {"log", "--no-show-signature", "-1",
                               "--format=%h %s", std::string(commit), "--"});
    if (r.exit_code != 0) {
        throw Error(error_message(r.err));
    }
    return first_line(r.out);
}

Divergence divergence(const fs::path& dir, std::string_view ref,
                      std::string_view base) {
    // Prints "<only in base>\t<only in ref>".
    const Result r =
        run(dir, {"rev-list", "--left-right", "--count",
                  std::string(base) + "..." + std::string(ref), "--"});
    if (r.exit_code != 0) {
        throw Error(error_message(r.err));
    }
    const char* pos = r.out.data();
    const char* const end = pos + r.out.size();
    // Reads a count followed by `separator`, advancing past both.
    auto read_count = [&](std::size_t& count, char separator) {
        const auto [next, ec] = std::from_chars(pos, end, count);
        if (ec != std::errc() || next == end || *next != separator) {
            throw wt::Error(ErrorKind::internal,
                            "unexpected output from git rev-list");
        }
        pos = next + 1;
    };
    Divergence d{};
    read_count(d.behind, '\t');
    read_count(d.ahead, '\n');
    return d;
}

std::string integration_branch(const fs::path& dir) {
    const Result config = run(dir, {"config", "--get", "wt.integrationBranch"});
    if (config.exit_code == 0) {
        const std::string name = first_line(config.out);
        if (!is_valid_branch_name(dir, name) ||
            !local_branch_exists(dir, name)) {
            throw wt::Error(
                ErrorKind::user,
                "wt.integrationBranch is set to '" + name +
                    "', but there is no local branch with that name");
        }
        return name;
    }
    if (config.exit_code != kNotFound) {
        throw Error(error_message(config.err));
    }

    const Result origin =
        run(dir, {"symbolic-ref", "--quiet", "refs/remotes/origin/HEAD"});
    if (origin.exit_code == 0) {
        constexpr std::string_view kPrefix = "refs/remotes/origin/";
        const std::string target = first_line(origin.out);
        if (!target.starts_with(kPrefix)) {
            throw wt::Error(ErrorKind::user, "origin/HEAD points to " + target +
                                                 ", not a branch of origin");
        }
        const std::string name = target.substr(kPrefix.size());
        if (!is_valid_branch_name(dir, name) ||
            !local_branch_exists(dir, name)) {
            throw wt::Error(
                ErrorKind::user,
                "origin's default branch is '" + name +
                    "', but there is no local branch with that name; "
                    "create it with `git branch " +
                    ui::shell_quote(name) + " " +
                    ui::shell_quote("origin/" + name) +
                    "`, or choose another with `git config "
                    "wt.integrationBranch <branch>`");
        }
        return name;
    }
    if (origin.exit_code != kNotFound) {
        throw Error(error_message(origin.err));
    }

    for (const std::string name : {"main", "master"}) {
        if (local_branch_exists(dir, name)) {
            return name;
        }
    }
    if (run(dir, {"rev-parse", "--verify", "--quiet", "HEAD"}).exit_code != 0) {
        throw wt::Error(ErrorKind::user,
                        "the repository has no commits yet; make one first");
    }
    throw wt::Error(
        ErrorKind::user,
        "cannot tell which branch to start from: there is no local main or "
        "master branch; choose one with `git config wt.integrationBranch "
        "<branch>`");
}

}  // namespace wt::git
