#include <catch2/catch_test_macros.hpp>

#include "ui/feed.hpp"

#include "utils/fs.hpp"
#include "utils/path.hpp"

#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace cc;

namespace {

/// A folder that holds the configuration of the user while the test runs.
///
/// The variable `CLAUDE_CONFIG_DIR` points the configuration of Claude Code
/// at the folder, so that the test never reads the settings of the machine
/// it runs on.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const char* base = std::getenv("TMPDIR");
        std::string root = base != nullptr ? base : "/tmp";
        while (!root.empty() && root.back() == '/') root.pop_back();

        path_ = path_join(path_join(root, "cc-feed-test-" + std::to_string(::getpid())),
                          "home-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        remove_dir_all(path_);
        mkdirp(path_);

        had_previous_ = std::getenv("CLAUDE_CONFIG_DIR") != nullptr;
        if (had_previous_) previous_ = std::getenv("CLAUDE_CONFIG_DIR");
        ::setenv("CLAUDE_CONFIG_DIR", path_.c_str(), 1);
    }

    ~TemporaryDirectory() {
        if (had_previous_) {
            ::setenv("CLAUDE_CONFIG_DIR", previous_.c_str(), 1);
        } else {
            ::unsetenv("CLAUDE_CONFIG_DIR");
        }
        remove_dir_all(path_);
    }

    /// The folder the configuration lives in.
    std::string path() const { return path_; }

private:
    std::string path_;
    bool had_previous_ = false;
    std::string previous_;
};

/// A changelog of three releases, with two notes in each of them.
const char* kChangelog =
    "## 2.0.0\n"
    "- The first note\n"
    "- The second note\n"
    "\n"
    "## 1.9.0\n"
    "- The third note\n"
    "- The fourth note\n"
    "\n"
    "## 1.8.0\n"
    "- The fifth note\n"
    "- The sixth note\n";

} // namespace

TEST_CASE("The feed of the release shows three notes at most") {
    TemporaryDirectory home;
    REQUIRE(write_file(path_join(home.path(), "cache/changelog.md"), kChangelog));

    const ui::Feed feed = ui::whats_new_feed(nullptr);

    // The reference asks for the notes of the newest three releases, and
    // shows three notes of them at most.
    REQUIRE(feed.lines.size() == 3);
    REQUIRE(feed.lines[0].text == "The first note");
    REQUIRE(feed.lines[1].text == "The second note");
    REQUIRE(feed.lines[2].text == "The third note");
    REQUIRE(feed.footer == "/release-notes for more");
}

TEST_CASE("The feed of the release says when there is nothing to show") {
    TemporaryDirectory home;

    const ui::Feed feed = ui::whats_new_feed(nullptr);

    REQUIRE(feed.lines.empty());
    REQUIRE(feed.empty_message == "Check the Claude Code changelog for updates");
    REQUIRE(feed.footer.empty());
}

TEST_CASE("The feed of the release names the version that was seen") {
    TemporaryDirectory home;
    REQUIRE(write_file(path_join(home.path(), "cache/changelog.md"), kChangelog));

    std::string seen;
    const ui::Feed feed = ui::whats_new_feed(&seen);

    REQUIRE(feed.lines.size() == 3);
    REQUIRE(seen == "2.0.0");
}
