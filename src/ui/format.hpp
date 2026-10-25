#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace wt::ui {

// Replaces control characters with '?'. Branch names and paths come from the
// repository, and printing them raw could inject terminal escape sequences.
std::string printable(std::string_view text);

// Quotes `word` for a POSIX shell, so a command printed for users to paste
// runs with it as one argument, whatever it contains. Words made only of
// characters no shell treats specially are left as they are.
std::string shell_quote(std::string_view word);

// Shortens `path` to start with "~" when it lies inside `home`.
std::string tilde_path(const std::filesystem::path& path,
                       const std::filesystem::path& home);

// Lays out rows as left-aligned columns; the first row is the header.
std::string format_table(const std::vector<std::vector<std::string>>& rows);

}  // namespace wt::ui
