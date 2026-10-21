#include "layout/container.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace wt::layout {

namespace fs = std::filesystem;

namespace {

// FNV-1a rather than std::hash, whose output may change between builds and
// would orphan existing containers. The ID only needs to be stable and make
// the name distinctive; ownership is proven by the marker, not the hash.
std::string short_id(std::string_view key) {
    std::uint32_t hash = 2166136261u;
    for (const char c : key) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", hash);
    return buf;
}

std::string errno_message(int err) {
    return std::generic_category().message(err);
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

}  // namespace

fs::path container_path(const fs::path& main_worktree,
                        const fs::path& common_dir) {
    std::string name = main_worktree.filename().string();
    // A bare repository's main "worktree" is the repository itself.
    constexpr std::string_view kGitSuffix = ".git";
    if (name.size() > kGitSuffix.size() && name.ends_with(kGitSuffix)) {
        name.resize(name.size() - kGitSuffix.size());
    }
    if (name.empty() || name == "." || name == ".." ||
        !main_worktree.has_parent_path() ||
        main_worktree.parent_path() == main_worktree) {
        throw ContainerError("cannot place worktrees next to " +
                             main_worktree.string());
    }
    return main_worktree.parent_path() /
           ("." + name + ".wt-" + short_id(common_dir.string()));
}

std::string marker_contents(const fs::path& common_dir) {
    return "# This directory holds worktrees created by wt, which checks this "
           "file before touching anything here.\n"
           "wt-managed 1\n"
           "repository " +
           common_dir.string() + "\n";
}

std::optional<std::string> ownership_problem(const fs::path& container,
                                             const fs::path& common_dir) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(container, ec);
    if (st.type() == fs::file_type::not_found) {
        return "it doesn't exist";
    }
    if (ec) {
        return "cannot inspect it: " + ec.message();
    }
    if (st.type() == fs::file_type::symlink) {
        return "it is a symbolic link";
    }
    if (st.type() != fs::file_type::directory) {
        return "it is not a directory";
    }

    const fs::path marker = container / kMarkerName;
    // O_NOFOLLOW: a symlinked marker could point at any file that happens to
    // have the right contents.
    const int fd = ::open(marker.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        if (err == ENOENT) {
            return "it has no " + std::string(kMarkerName) + " marker";
        }
        if (err == ELOOP) {
            return "its " + std::string(kMarkerName) +
                   " marker is a symbolic link";
        }
        return "cannot read its " + std::string(kMarkerName) +
               " marker: " + errno_message(err);
    }

    struct stat info{};
    const bool regular = ::fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
    const std::string expected = marker_contents(common_dir);
    std::string actual;
    if (regular) {
        // Read one byte past the expected size so a longer file can't match.
        char buf[4096];
        while (actual.size() <= expected.size()) {
            const ssize_t n = ::read(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                break;
            }
            actual.append(buf, static_cast<std::size_t>(n));
        }
    }
    ::close(fd);

    if (!regular) {
        return "its " + std::string(kMarkerName) + " marker is not a file";
    }
    if (actual != expected) {
        return "its " + std::string(kMarkerName) +
               " marker belongs to a different repository";
    }
    return std::nullopt;
}

void prepare_container(const fs::path& container, const fs::path& common_dir) {
    std::error_code ec;
    if (!fs::create_directory(container, ec)) {
        // A non-directory in the way reports file_exists; that still gets
        // the ownership explanation below.
        if (ec && ec != std::errc::file_exists) {
            throw ContainerError("cannot create " + container.string() + ": " +
                                 ec.message());
        }
        // Something is already there; use it only if it's provably ours.
        if (auto problem = ownership_problem(container, common_dir)) {
            throw ContainerError(
                container.string() +
                " exists but wt can't confirm it manages it (" + *problem +
                "); move it aside and try again");
        }
        return;
    }

    const fs::path marker = container / kMarkerName;
    const int fd =
        ::open(marker.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    int err = fd < 0 ? errno : 0;
    if (fd >= 0) {
        if (!write_all(fd, marker_contents(common_dir))) {
            err = errno;
        }
        if (::close(fd) != 0 && err == 0) {
            err = errno;
        }
    }
    if (err != 0) {
        // Both were created moments ago by this call, so removing them can't
        // lose anything. fs::remove only deletes the directory if it's still
        // empty, so anything that appeared in it meanwhile is left alone.
        if (fd >= 0) {
            ::unlink(marker.c_str());
        }
        fs::remove(container, ec);
        throw ContainerError("cannot create " + marker.string() + ": " +
                             errno_message(err));
    }
}

}  // namespace wt::layout
