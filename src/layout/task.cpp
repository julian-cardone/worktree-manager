#include "layout/task.hpp"

#include <algorithm>
#include <cctype>

namespace wt::layout {

namespace {

// Leaves room under the usual 255-byte file name limit.
constexpr std::size_t kMaxTaskNameLength = 100;

bool allowed_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
           c == '/';
}

}  // namespace

std::optional<std::string> task_name_problem(std::string_view task) {
    if (task.empty()) {
        return "the task name is empty";
    }
    if (task.size() > kMaxTaskNameLength) {
        return "the task name is longer than " +
               std::to_string(kMaxTaskNameLength) + " characters";
    }
    if (!std::all_of(task.begin(), task.end(), allowed_char)) {
        return "the task name may only contain letters, digits, '.', '_', "
               "'-', and '/'";
    }
    // A leading '-' would let the name be read as an option by git.
    if (task.front() == '-') {
        return "the task name can't start with '-'";
    }
    // Keeps worktree folders visible and distinct from the container's
    // .wt-managed marker.
    if (task.front() == '.') {
        return "the task name can't start with '.'";
    }
    if (task.front() == '/' || task.back() == '/') {
        return "the task name can't start or end with '/'";
    }
    if (task.find("//") != std::string_view::npos) {
        return "the task name can't contain '//'";
    }
    if (task.find("..") != std::string_view::npos) {
        return "the task name can't contain '..'";
    }
    if (task == "HEAD") {
        return "'HEAD' is reserved by Git";
    }
    return std::nullopt;
}

std::string task_folder_name(std::string_view task) {
    std::string folder(task);
    std::replace(folder.begin(), folder.end(), '/', '-');
    return folder;
}

bool same_ignoring_case(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) ==
               std::tolower(static_cast<unsigned char>(y));
    });
}

}  // namespace wt::layout
