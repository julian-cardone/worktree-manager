#pragma once

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace wt::test {

// A fresh, private temp directory that is deleted when the test finishes.
// Commands under test run inside it so a test can never act on the repository
// it was built in.
class Sandbox {
public:
    Sandbox() {
        namespace fs = std::filesystem;
        std::string tmpl =
            (fs::temp_directory_path() / "wt_test_XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        // Canonical so path comparisons and GIT_CEILING_DIRECTORIES match;
        // on macOS the temp dir sits behind a /var -> /private/var symlink.
        root_ = fs::canonical(tmpl);
        fs::create_directory(root_ / "home");
        fs::create_directory(root_ / "work");
    }
    ~Sandbox() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path home() const { return root_ / "home"; }
    std::filesystem::path work() const { return root_ / "work"; }

private:
    std::filesystem::path root_;
};

}  // namespace wt::test
