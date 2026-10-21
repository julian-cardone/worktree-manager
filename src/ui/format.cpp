#include "ui/format.hpp"

#include <algorithm>

namespace wt::ui {

namespace fs = std::filesystem;

std::string printable(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) {
            c = '?';
        }
    }
    return result;
}

std::string tilde_path(const fs::path& path, const fs::path& home) {
    fs::path h = home.lexically_normal();
    if (h.has_parent_path() && h.filename().empty()) {
        h = h.parent_path();  // drop a trailing slash
    }
    // A home of "/" would turn every path into "~/...", which helps no one.
    if (!h.is_absolute() || h == h.root_path()) {
        return path.string();
    }
    auto [home_it, path_it] =
        std::mismatch(h.begin(), h.end(), path.begin(), path.end());
    if (home_it != h.end()) {
        return path.string();
    }
    fs::path rest;
    for (; path_it != path.end(); ++path_it) {
        rest /= *path_it;
    }
    return rest.empty() ? "~" : "~/" + rest.string();
}

std::string format_table(const std::vector<std::vector<std::string>>& rows) {
    constexpr std::size_t kColumnGap = 2;

    std::vector<std::size_t> widths;
    for (const auto& row : rows) {
        widths.resize(std::max(widths.size(), row.size()));
        for (std::size_t i = 0; i < row.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }

    std::string out;
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            out += row[i];
            // Skip padding the last column to avoid trailing spaces.
            if (i + 1 < row.size()) {
                out.append(widths[i] - row[i].size() + kColumnGap, ' ');
            }
        }
        out += '\n';
    }
    return out;
}

}  // namespace wt::ui
