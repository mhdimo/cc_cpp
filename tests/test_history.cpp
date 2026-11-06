#include <catch2/catch_test_macros.hpp>

#include "session/history.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace cc;

namespace {

/// A folder that stands in for the configuration of the user while a test
/// runs.
///
/// The reference keeps the history of the prompts in the folder of the
/// configuration of the user, so the test points the folder of the
/// configuration at a temporary one — the history of the user must not be
/// touched.
class TempConfigDir {
public:
    TempConfigDir() {
        const char* previous = std::getenv("CLAUDE_CONFIG_DIR");
        if (previous) {
            had_previous_ = true;
            previous_ = previous;
        }

        directory_ = std::filesystem::temp_directory_path() / "cc_history_test";
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
        ::setenv("CLAUDE_CONFIG_DIR", directory_.c_str(), 1);
    }

    ~TempConfigDir() {
        if (had_previous_) {
            ::setenv("CLAUDE_CONFIG_DIR", previous_.c_str(), 1);
        } else {
            ::unsetenv("CLAUDE_CONFIG_DIR");
        }
        std::filesystem::remove_all(directory_);
    }

    std::string path() const { return directory_.string(); }

private:
    std::filesystem::path directory_;
    bool had_previous_ = false;
    std::string previous_;
};

} // namespace

TEST_CASE("The prompts of the sessions are kept in the file of the user") {
    TempConfigDir config;

    const std::string path = session::history_path();
    REQUIRE(path == config.path() + "/history.jsonl");

    session::HistoryEntry entry;
    entry.display = "the first prompt";
    entry.timestamp = 1700000000000;
    entry.project = "/tmp/project";
    entry.session_id = "session-1";
    session::append_history(entry);

    // The line is written in the shape the reference writes it in.
    const auto text = cc::read_file(path);
    REQUIRE(text.has_value());
    REQUIRE(text->find("\"display\":\"the first prompt\"") != std::string::npos);
    REQUIRE(text->find("\"timestamp\":1700000000000") != std::string::npos);
    REQUIRE(text->find("\"project\":\"/tmp/project\"") != std::string::npos);
    REQUIRE(text->find("\"sessionId\":\"session-1\"") != std::string::npos);
    REQUIRE(text->find("pastedContents") != std::string::npos);
}

TEST_CASE("The history is read newest first") {
    TempConfigDir config;

    for (int i = 0; i < 3; ++i) {
        session::HistoryEntry entry;
        entry.display = "prompt " + std::to_string(i);
        entry.timestamp = 1700000000000 + i;
        entry.project = "/tmp/project";
        session::append_history(entry);
    }

    const auto entries = session::read_history();
    REQUIRE(entries.size() == 3);
    REQUIRE(entries[0].display == "prompt 2");
    REQUIRE(entries[1].display == "prompt 1");
    REQUIRE(entries[2].display == "prompt 0");
    REQUIRE(entries[0].timestamp == 1700000000002);
}

TEST_CASE("A line of the history that is broken is skipped") {
    TempConfigDir config;

    session::HistoryEntry entry;
    entry.display = "the good prompt";
    entry.timestamp = 1700000000000;
    session::append_history(entry);

    // A line that is not JSON, the way a half-written line of another
    // program looks.
    REQUIRE(cc::append_file(session::history_path(), "{not json\n"));

    const auto entries = session::read_history();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].display == "the good prompt");
}

TEST_CASE("A history that does not exist is an empty history") {
    TempConfigDir config;

    const auto entries = session::read_history();
    REQUIRE(entries.empty());
}
