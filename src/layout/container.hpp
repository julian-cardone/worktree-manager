#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wt::layout {

// A container that wt can't prove it owns, or can't create.
class ContainerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Name of the ownership marker file inside every container.
inline constexpr std::string_view kMarkerName = ".wt-managed";

// The directory holding every worktree wt creates for a repository: a hidden
// sibling of the main worktree named after the repository plus an ID derived
// from its Git directory, e.g. /code/repo -> /code/.repo.wt-a13f92c4.
// Throws ContainerError if no sensible name can be derived.
std::filesystem::path container_path(const std::filesystem::path& main_worktree,
                                     const std::filesystem::path& common_dir);

// The exact marker contents for the repository whose shared Git directory is
// `common_dir`. A container belongs to that repository only if its marker
// matches byte for byte.
std::string marker_contents(const std::filesystem::path& common_dir);

// Explains why wt can't treat `container` as its own for this repository, or
// returns nothing if it can: it must be a real directory (not a symlink)
// holding a regular-file marker that names this repository. Only reads.
//
// WARNING: every operation that writes into or removes anything from a
// container must pass this check first; it is what stops wt from touching a
// directory it didn't create.
std::optional<std::string> ownership_problem(
    const std::filesystem::path& container,
    const std::filesystem::path& common_dir);

// Creates `container` with its marker if it doesn't exist, or verifies that
// wt owns it if it does. Throws ContainerError otherwise.
void prepare_container(const std::filesystem::path& container,
                       const std::filesystem::path& common_dir);

}  // namespace wt::layout
