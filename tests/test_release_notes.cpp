#include "release_notes.hpp"

#include "utils/path.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace cc {
namespace {

/// Writes a file and returns its path.
std::string write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path);
    stream << content;
    return path.string();
}

TEST_CASE("The notes of a changelog are read from its sections", "[release_notes]") {
    const std::string content = "# Changelog\n"
                                "\n"
                                "## 0.2.0 - 2026-11-05\n"
                                "\n"
                                "- The second release.\n"
                                "- Another note of it.\n"
                                "\n"
                                "## 0.1.0\n"
                                "\n"
                                "- The first release.\n";

    const auto releases = parse_changelog(content);
    REQUIRE(releases.size() == 2);
    CHECK(releases[0].first == "0.2.0");
    REQUIRE(releases[0].second.size() == 2);
    CHECK(releases[0].second[0] == "The second release.");
    CHECK(releases[1].first == "0.1.0");
    REQUIRE(releases[1].second.size() == 1);
}

TEST_CASE("A version is newer than another when its numbers are", "[release_notes]") {
    CHECK(version_is_newer("0.2.0", "0.1.0"));
    CHECK(version_is_newer("1.0.0", "0.9.9"));
    CHECK(version_is_newer("0.10.0", "0.9.0"));
    CHECK_FALSE(version_is_newer("0.1.0", "0.2.0"));
    CHECK_FALSE(version_is_newer("0.1.0", "0.1.0"));
    CHECK_FALSE(version_is_newer("not a version", "0.1.0"));
}

TEST_CASE("The notes of the versions that are newer are taken", "[release_notes]") {
    const std::string content = "## 0.3.0\n\n- The third.\n\n## 0.2.0\n\n- The second.\n\n"
                                "## 0.1.0\n\n- The first.\n";

    const auto notes = recent_release_notes("0.1.0", content);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0] == "The third.");
    CHECK(notes[1] == "The second.");
}

TEST_CASE("The notes of the newest releases are taken when no version was seen",
          "[release_notes]") {
    const std::string content = "## 0.2.0\n\n- The second.\n\n## 0.1.0\n\n- The first.\n";

    const auto notes = recent_release_notes("", content);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0] == "The second.");
}

TEST_CASE("The path of the program is remembered", "[release_notes]") {
    const std::string before = program_path();
    set_program_path("/usr/local/bin/cc++");
    CHECK(program_path() == "/usr/local/bin/cc++");
    CHECK(program_directory() == "/usr/local/bin");

    // The path of the program is put back, so that the other tests are not
    // troubled by it.
    set_program_path(before);
}

TEST_CASE("The changelog that came with the program is found beside it", "[release_notes]") {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "cc-changelog-test";
    std::filesystem::create_directories(directory);
    write_file(directory / "CHANGELOG.md", "## 9.9.9\n\n- The note of the test.\n");

    const std::string before = program_path();
    set_program_path((directory / "cc++").string());
    CHECK(bundled_changelog().find("The note of the test.") != std::string::npos);

    set_program_path(before);
    std::filesystem::remove_all(directory);
}

TEST_CASE("The changelog that came with the program is found in the layout of the install",
          "[release_notes]") {
    // The install of the reference puts the program in the versions folder
    // and the data of the program in the folder above it.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cc-install-test";
    const std::filesystem::path versions = root / "share" / "claude" / "versions" / "0.1.0";
    std::filesystem::create_directories(versions);
    std::filesystem::create_directories(root / "share" / "claude");
    write_file(root / "share" / "claude" / "CHANGELOG.md",
               "## 9.9.9\n\n- The note of the install.\n");

    const std::string before = program_path();
    set_program_path((versions / "cc++").string());
    CHECK(bundled_changelog().find("The note of the install.") != std::string::npos);

    set_program_path(before);
    std::filesystem::remove_all(root);
}

TEST_CASE("There is no changelog when none came with the program", "[release_notes]") {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "cc-changelog-empty";
    std::filesystem::create_directories(directory);

    const std::string before = program_path();
    set_program_path((directory / "cc++").string());
    CHECK(bundled_changelog().empty());

    set_program_path(before);
    std::filesystem::remove_all(directory);
}

} // namespace
} // namespace cc
