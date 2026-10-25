#include "layout/container.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/error_kind.hpp"
#include "support/sandbox.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using Catch::Matchers::ContainsSubstring;
using wt::layout::container_path;
using wt::layout::marker_contents;
using wt::layout::ownership_problem;
using wt::layout::prepare_container;
using wt::test::kind_is;
using wt::test::Sandbox;

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out << content;
}

}  // namespace

TEST_CASE("container_path is a hidden sibling named after the repository") {
    const fs::path path = container_path("/code/repo", "/code/repo/.git");
    CHECK(path.parent_path() == "/code");
    const std::string name = path.filename().string();
    CHECK(name.starts_with(".repo.wt-"));
    CHECK(name.size() == std::string(".repo.wt-").size() + 8);
}

TEST_CASE("container_path is stable and differs between repositories") {
    CHECK(container_path("/code/repo", "/code/repo/.git") ==
          container_path("/code/repo", "/code/repo/.git"));
    CHECK(container_path("/code/repo", "/code/repo/.git") !=
          container_path("/code/repo", "/elsewhere/repo/.git"));
}

TEST_CASE("container_path drops .git from a bare repository's name") {
    const fs::path path = container_path("/code/repo.git", "/code/repo.git");
    CHECK(path.filename().string().starts_with(".repo.wt-"));
}

TEST_CASE("container_path refuses a main worktree at the root") {
    CHECK_THROWS_MATCHES(container_path("/", "/.git"), wt::Error,
                         kind_is(wt::ErrorKind::user));
}

TEST_CASE("marker_contents names the repository") {
    CHECK_THAT(marker_contents("/code/repo/.git"),
               ContainsSubstring("repository /code/repo/.git\n"));
}

TEST_CASE("prepare_container creates the container and its marker") {
    Sandbox box;
    const fs::path common = box.work() / "repo" / ".git";
    const fs::path container = box.work() / ".repo.wt-test";

    prepare_container(container, common);
    CHECK(fs::is_directory(container));
    CHECK(read_file(container / ".wt-managed") == marker_contents(common));
    CHECK_FALSE(ownership_problem(container, common).has_value());

    // Reusing it is fine and leaves its contents alone.
    write_file(container / "keep", "x");
    prepare_container(container, common);
    CHECK(read_file(container / "keep") == "x");
}

TEST_CASE("ownership_problem rejects containers wt can't prove it owns") {
    Sandbox box;
    const fs::path common = box.work() / "repo" / ".git";
    const fs::path container = box.work() / ".repo.wt-test";
    const fs::path marker = container / ".wt-managed";
    std::string expected_problem;

    SECTION("missing") {
        expected_problem = "doesn't exist";
    }
    SECTION("a regular file") {
        write_file(container, "x");
        expected_problem = "not a directory";
    }
    SECTION("a symlink to an owned container") {
        const fs::path real = box.work() / "real";
        prepare_container(real, common);
        fs::create_directory_symlink(real, container);
        expected_problem = "symbolic link";
    }
    SECTION("a directory with no marker") {
        fs::create_directory(container);
        expected_problem = "no .wt-managed marker";
    }
    SECTION("a marker that is a directory") {
        fs::create_directories(marker);
        expected_problem = "not a file";
    }
    SECTION("a symlinked marker") {
        fs::create_directory(container);
        write_file(box.work() / "elsewhere", marker_contents(common));
        fs::create_symlink(box.work() / "elsewhere", marker);
        expected_problem = "symbolic link";
    }
    SECTION("a marker for another repository") {
        fs::create_directory(container);
        write_file(marker, marker_contents(box.work() / "other" / ".git"));
        expected_problem = "different repository";
    }
    SECTION("a marker with extra contents") {
        fs::create_directory(container);
        write_file(marker, marker_contents(common) + "extra\n");
        expected_problem = "different repository";
    }
    SECTION("an empty marker") {
        fs::create_directory(container);
        write_file(marker, "");
        expected_problem = "different repository";
    }

    const auto problem = ownership_problem(container, common);
    REQUIRE(problem.has_value());
    CHECK_THAT(*problem, ContainsSubstring(expected_problem));
}

TEST_CASE("prepare_container leaves an unowned directory untouched") {
    Sandbox box;
    const fs::path common = box.work() / "repo" / ".git";
    const fs::path container = box.work() / ".repo.wt-test";
    fs::create_directory(container);
    write_file(container / "theirs", "data");

    CHECK_THROWS_MATCHES(prepare_container(container, common), wt::Error,
                         kind_is(wt::ErrorKind::refused));
    CHECK(read_file(container / "theirs") == "data");
    CHECK_FALSE(fs::exists(container / ".wt-managed"));
}

TEST_CASE("prepare_container fails cleanly when the parent is missing") {
    Sandbox box;
    const fs::path container = box.work() / "missing" / ".repo.wt-test";
    CHECK_THROWS_MATCHES(prepare_container(container, box.work() / ".git"),
                         wt::Error, kind_is(wt::ErrorKind::user));
    CHECK_FALSE(fs::exists(box.work() / "missing"));
}
