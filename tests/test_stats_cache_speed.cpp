#include <catch2/catch_test_macros.hpp>

#include "session/stats.hpp"
#include "session/stats_cache.hpp"
#include "ui/ansi.hpp"
#include "ui/stats_panel.hpp"
#include "ui/theme.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cc;

namespace fs = std::filesystem;

namespace {

/// The folder of the user, for as long as a test runs.
///
/// The statistics are read from the folder of the user, and the cache of
/// them is written there, so a test that must not touch the real one points
/// the folder of the user at a folder of its own.
class TempHome {
public:
    TempHome() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("cc-stats-speed-test-" + std::to_string(++counter));
        fs::create_directories(path_);
        setenv("CLAUDE_CONFIG_DIR", path_.string().c_str(), 1);
    }

    ~TempHome() {
        unsetenv("CLAUDE_CONFIG_DIR");
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempHome(const TempHome&) = delete;
    TempHome& operator=(const TempHome&) = delete;

    /// The folder the user is at while the test runs.
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

/// Writes a transcript of a session that the model answered.
void write_transcript(const fs::path& file, int days_ago, int64_t input_tokens,
                      int64_t output_tokens) {
    fs::create_directories(file.parent_path());

    const int64_t stamp = now_ms() - static_cast<int64_t>(days_ago) * 24 * 60 * 60 * 1000;
    const std::string day = session::to_date_string(stamp);

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "{\"type\":\"assistant\",\"timestamp\":\"" << day
        << "T11:00:00.000Z\",\"message\":{\"role\":\"assistant\",\"model\":\"claude-sonnet-4-5\","
        << "\"content\":[{\"type\":\"text\",\"text\":\"hello\"}],\"usage\":{\"input_tokens\":"
        << input_tokens << ",\"output_tokens\":" << output_tokens << "}}}\n";
}

/// Puts the day a file was written on in the past.
///
/// The file of a session that is over is not read again, and the way the
/// port knows it is the day it was written on.
void write_in_the_past(const fs::path& file, int days_ago) {
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(file, now - std::chrono::hours(24 * days_ago));
}

/// The text of the panel, without the colours.
std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) out += ui::strip(line) + "\n";
    return out;
}

} // namespace

TEST_CASE("The days that are over are not read again when the panel opens") {
    const TempHome home;

    // A session of three days ago. The day is over, so it goes into the
    // cache.
    const fs::path old_transcript = home.path() / "projects" / "-one" / "old.jsonl";
    write_transcript(old_transcript, 3, 100, 50);
    write_in_the_past(old_transcript, 3);

    // The cache is written: it holds every day but the one that is going on.
    const auto first = session::collect_stats("/tmp", 0);
    REQUIRE(first.sessions == 1);

    const auto cache = session::load_stats_cache();
    REQUIRE(cache.last_computed_date == session::yesterday_date_string());
    REQUIRE(cache.daily_activity.size() == 1);
    REQUIRE(cache.daily_activity.front().date == session::to_date_string(
                                             now_ms() - 3 * 24 * 60 * 60 * 1000));

    // A session of today: the day that is going on is not over, so it is
    // read again every time.
    write_transcript(home.path() / "projects" / "-one" / "new.jsonl", 0, 20, 10);

    session::reset_transcripts_read(home.path().string());

    const ui::StatsPanel panel(ui::Theme::dark(), "/tmp");
    panel.wait();

    // Only the transcript of the day that is going on was read: the day that
    // is over comes from the cache.
    REQUIRE(session::transcripts_read(home.path().string()) == 1);

    // The numbers of the panel are the numbers of the cache and of the day
    // that is going on: the session of the day that is over and the session
    // of today.
    const std::string text = joined(panel.render(80));
    REQUIRE(text.find("Sessions: 2") != std::string::npos);
    REQUIRE(text.find("Active days: 2") != std::string::npos);
}

TEST_CASE("The panel of the statistics does not change under the eyes of the user") {
    const TempHome home;

    // Enough tokens for the remark at the end of the panel to be one of
    // several, so that a remark that is picked again every time would show.
    write_transcript(home.path() / "projects" / "-one" / "a.jsonl", 1, 100000, 0);

    const ui::StatsPanel panel(ui::Theme::dark(), "/tmp");
    panel.wait();

    const auto first = panel.render(80);
    REQUIRE(joined(first).find("tokens") != std::string::npos);

    // The panel is drawn again and again; it is the same every time.
    for (int i = 0; i < 20; ++i) {
        REQUIRE(panel.render(80) == first);
    }
}

TEST_CASE("The cache of the reference of today is read") {
    const TempHome home;

    // The cache is written the way the reference of today writes it: the
    // version of the shape and the version of the tokens of the days.
    const std::string cache = R"({
  "version": 5,
  "lastComputedDate": "2026-10-05",
  "dailyActivity": [
    {
      "date": "2026-10-05",
      "messageCount": 273,
      "sessionCount": 2,
      "toolCallCount": 56
    }
  ],
  "dailyModelTokens": [
    {
      "date": "2026-10-05",
      "tokensByModel": {
        "deepseek-flash": 536571
      }
    }
  ],
  "dailyModelTokensVersion": 5,
  "modelUsage": {
    "deepseek-flash": {
      "inputTokens": 22381061,
      "outputTokens": 8933387,
      "cacheReadInputTokens": 2898786353,
      "cacheCreationInputTokens": 0,
      "webSearchRequests": 0,
      "costUSD": 0.0,
      "contextWindow": 0,
      "maxOutputTokens": 0
    }
  },
  "totalSessions": 70,
  "totalMessages": 57314,
  "longestSession": {
    "sessionId": "7f61baeb-d6c7-45ad-a507-75761766e279",
    "duration": 400063432,
    "messageCount": 861,
    "timestamp": "2026-10-23T17:41:30.139Z"
  },
  "firstSessionDate": "2026-10-05T13:57:07.130Z",
  "hourCounts": {
    "0": 6,
    "9": 3,
    "10": 6
  },
  "totalSpeculationTimeSavedMs": 0,
  "shotDistribution": {}
})";

    REQUIRE(write_file((home.path() / "stats-cache.json").string(), cache));

    // A session of today: the day that is going on is read every time.
    write_transcript(home.path() / "projects" / "-one" / "a.jsonl", 0, 10, 5);

    const auto stats = session::collect_stats("/tmp", 0);

    // The numbers of the cache are the numbers of the statistics, and the
    // session of today is added to them.
    REQUIRE(stats.sessions == 71);
    REQUIRE(stats.messages == 57315);
    REQUIRE(stats.longest_session_ms == 400063432);
    REQUIRE(stats.daily_activity.at("2026-10-05") == 273);

    // The cache is not written again: the day that is going on is not in it,
    // and the day that is over is not counted a second time.
    const auto root = session::stats_cache_to_json(session::load_stats_cache()).as_object();
    REQUIRE(jget_int(root, "version", 0) == session::kStatsCacheVersion);
    REQUIRE(jget_int(root, "dailyModelTokensVersion", 0) == session::kDailyModelTokensVersion);
    REQUIRE(jget_int(root, "totalSessions", 0) == 70);
}

TEST_CASE("The tokens of the days are counted again when the cache is of an older version") {
    const TempHome home;

    // A session of three days ago, so that the day is over.
    write_transcript(home.path() / "projects" / "-one" / "old.jsonl", 3, 100, 50);

    // A cache of the version before the tokens of the days were counted in
    // the way of the reference: the version of the tokens is missing. The
    // cache is of yesterday, so that the days that are over are not counted
    // again.
    const std::string yesterday = session::yesterday_date_string();
    const std::string cache =
        "{\n"
        "  \"version\": 3,\n"
        "  \"lastComputedDate\": \"" +
        yesterday +
        "\",\n"
        "  \"dailyActivity\": [\n"
        "    {\n"
        "      \"date\": \"" +
        yesterday +
        "\",\n"
        "      \"messageCount\": 273,\n"
        "      \"sessionCount\": 2,\n"
        "      \"toolCallCount\": 56\n"
        "    }\n"
        "  ],\n"
        "  \"dailyModelTokens\": [\n"
        "    {\n"
        "      \"date\": \"" +
        yesterday +
        "\",\n"
        "      \"tokensByModel\": {\n"
        "        \"deepseek-flash\": 536571\n"
        "      }\n"
        "    }\n"
        "  ],\n"
        "  \"modelUsage\": {},\n"
        "  \"totalSessions\": 70,\n"
        "  \"totalMessages\": 57314,\n"
        "  \"longestSession\": null,\n"
        "  \"firstSessionDate\": \"2026-10-05T13:57:07.130Z\",\n"
        "  \"hourCounts\": {},\n"
        "  \"totalSpeculationTimeSavedMs\": 0\n"
        "}";

    REQUIRE(write_file((home.path() / "stats-cache.json").string(), cache));

    session::collect_stats("/tmp", 0);

    // The tokens of the days are counted again, and the version of them is
    // the one of the reference.
    const auto root = session::stats_cache_to_json(session::load_stats_cache()).as_object();
    REQUIRE(jget_int(root, "dailyModelTokensVersion", 0) == session::kDailyModelTokensVersion);

    // The day of the session is in the cache, with the tokens it used. The
    // tokens that were counted in the old way are gone.
    const auto tokens = jget_array(root, "dailyModelTokens");
    REQUIRE(tokens.size() == 1);
    REQUIRE(jget_string(tokens[0].as_object(), "date", "") ==
            session::to_date_string(now_ms() - 3 * 24 * 60 * 60 * 1000));
    REQUIRE(jget_int(jget_object(tokens[0].as_object(), "tokensByModel"), "claude-sonnet-4-5", 0) ==
            150);
}

TEST_CASE("The tokens of the days are counted the way the reference counts them") {
    const TempHome home;

    // A turn that read the cache of the prompt and wrote into it: the
    // tokens of the day are the four of them, the way the reference counts
    // them.
    const fs::path file = home.path() / "projects" / "-one" / "a.jsonl";
    fs::create_directories(file.parent_path());
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "{\"type\":\"assistant\",\"timestamp\":\""
            << session::today_date_string() << "T11:00:00.000Z\",\"message\":{\"role\":\"assistant\","
            << "\"model\":\"claude-sonnet-4-5\",\"content\":[{\"type\":\"text\",\"text\":\"hello\"}],"
            << "\"usage\":{\"input_tokens\":100,\"output_tokens\":50,\"cache_read_input_tokens\":"
            << "2000,\"cache_creation_input_tokens\":300}}}\n";
    }

    const auto stats = session::collect_stats("/tmp", 0);

    // The chart of the panel of the models is drawn from them: the tokens
    // that were sent, the ones that were answered with, and the ones of the
    // cache of the prompt.
    REQUIRE(stats.daily_model_tokens.at(session::today_date_string()).at("claude-sonnet-4-5") ==
            100 + 50 + 2000 + 300);
}
