#include "git/repo.hpp"

#include "git/git.hpp"

#include <charconv>
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

}  // namespace

fs::path common_dir(const fs::path& dir) {
    const Result r =
        run(dir, {"rev-parse", "--path-format=absolute", "--git-common-dir"});
    if (r.exit_code != 0) {
        throw Error(error_message(r.err));
    }
    std::string path = r.out;
    if (path.ends_with('\n')) {
        path.pop_back();
    }
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(path, ec);
    return ec ? fs::path(path).lexically_normal() : resolved;
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
            throw Error("unexpected output from git rev-list");
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
            throw Error("wt.integrationBranch is set to '" + name +
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
            throw Error("origin/HEAD points to " + target +
                        ", not a branch of origin");
        }
        const std::string name = target.substr(kPrefix.size());
        if (!is_valid_branch_name(dir, name) ||
            !local_branch_exists(dir, name)) {
            throw Error("origin's default branch is '" + name +
                        "', but there is no local branch with that name; "
                        "create it with `git branch " +
                        name + " origin/" + name +
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
        throw Error("the repository has no commits yet; make one first");
    }
    throw Error(
        "cannot tell which branch to start from: there is no local main or "
        "master branch; choose one with `git config wt.integrationBranch "
        "<branch>`");
}

}  // namespace wt::git
