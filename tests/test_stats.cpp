#include <catch2/catch_test_macros.hpp>

#include "session/stats.hpp"
#include "session/stats_cache.hpp"
#include "session/transcript.hpp"
#include "ui/ansi.hpp"
#include "ui/stats_panel.hpp"
#include "ui/theme.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/time.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace cc;

namespace {

namespace fs = std::filesystem;

/// The folder of the user, for as long as a test runs.
///
/// The statistics are read from the folder of the user, and the cache of
/// them is written there, so a test that must not touch the real one points
/// the folder of the user at a folder of its own.
class TempHome {
public:
    TempHome() {
        if (const char* old = std::getenv("CLAUDE_CONFIG_DIR"); old && *old) {
            had_old_ = true;
            old_ = old;
        }

        static int counter = 0;
        path_ = fs::temp_directory_path() / ("cc-stats-test-" + std::to_string(++counter));
        fs::create_directories(path_);
        setenv("CLAUDE_CONFIG_DIR", path_.string().c_str(), 1);
    }

    ~TempHome() {
        if (had_old_) {
            setenv("CLAUDE_CONFIG_DIR", old_.c_str(), 1);
        } else {
            unsetenv("CLAUDE_CONFIG_DIR");
        }
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempHome(const TempHome&) = delete;
    TempHome& operator=(const TempHome&) = delete;

    /// The folder the user is at while the test runs.
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    std::string old_;
    bool had_old_ = false;
};

/// A timestamp of a day that is `days` ago, as an ISO-8601 timestamp.
std::string stamp(int days_ago, int hour) {
    const int64_t ms = now_ms() - static_cast<int64_t>(days_ago) * 24 * 60 * 60 * 1000;
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%sT%02d:00:00.000Z",
                  session::to_date_string(ms).c_str(), hour);
    return std::string(buffer);
}

/// The day the streak that is going on is counted from.
///
/// The reference counts the streak from the midnight of the machine and
/// writes the day of UTC, so the day is the one before today when the
/// machine is ahead of UTC.
std::string streak_start_day() {
    const std::time_t seconds = static_cast<std::time_t>(now_ms() / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    local.tm_hour = 0;
    local.tm_min = 0;
    local.tm_sec = 0;
    local.tm_isdst = -1;
    return session::to_date_string(static_cast<int64_t>(std::mktime(&local)) * 1000);
}

/// Writes a transcript of a session that began `days` ago.
///
/// Every message of the transcript is a message of the user, so that the
/// number of the messages of the session is known.
void write_transcript(const fs::path& file, int days_ago, int messages) {
    fs::create_directories(file.parent_path());

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (int i = 0; i < messages; ++i) {
        boost::json::object payload;
        payload["role"] = "user";
        payload["content"] = "hello";

        boost::json::object entry;
        entry["type"] = "user";
        entry["timestamp"] = stamp(days_ago, 10);
        entry["message"] = payload;

        out << json_dump(boost::json::value(entry)) << "\n";
    }
}

/// Writes a transcript of a session that began `days` ago, and that the
/// model answered.
void write_answered_transcript(const fs::path& file, int days_ago, const std::string& model,
                                int64_t input_tokens, int64_t output_tokens) {
    fs::create_directories(file.parent_path());

    boost::json::object usage;
    usage["input_tokens"] = input_tokens;
    usage["output_tokens"] = output_tokens;

    boost::json::array content;
    content.emplace_back(boost::json::object{{"type", "text"}, {"text", "hello"}});
    content.emplace_back(boost::json::object{{"type", "tool_use"}, {"id", "t1"}, {"name", "Bash"}});

    boost::json::object payload;
    payload["role"] = "assistant";
    payload["model"] = model;
    payload["content"] = content;
    payload["usage"] = usage;

    boost::json::object entry;
    entry["type"] = "assistant";
    entry["timestamp"] = stamp(days_ago, 11);
    entry["message"] = payload;

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << json_dump(boost::json::value(entry)) << "\n";
}

/// The cache of the statistics of the folder of the user.
boost::json::object read_cache(const fs::path& home) {
    const auto contents = read_file((home / "stats-cache.json").string());
    REQUIRE(contents.has_value());
    const boost::json::value value = json_parse(*contents);
    REQUIRE(value.is_object());
    return value.as_object();
}

/// A cache as the reference writes it.
///
/// The numbers are the ones of a cache the reference wrote, so that the
/// shape of the file is the one of the reference and not one that was made
/// up.
const char* kReferenceCache = R"({
  "version": 3,
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
  "modelUsage": {
    "MiniMax-M3.1-Flash": {
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
  "totalSpeculationTimeSavedMs": 0
})";

} // namespace

TEST_CASE("The statistics of the sessions are collected") {
    TempHome home;

    // A folder with no sessions has no statistics.
    const auto empty = session::collect_stats("/tmp", 0);
    REQUIRE(empty.sessions == 0);
    REQUIRE(empty.messages == 0);
}

TEST_CASE("The statistics of a range skip the sessions that are older") {
    TempHome home;

    write_transcript(home.path() / "projects" / "-one" / "old.jsonl", 20, 2);
    write_transcript(home.path() / "projects" / "-one" / "new.jsonl", 1, 3);

    const auto all = session::collect_stats("/tmp", 0);
    const auto week = session::collect_stats("/tmp", 7);
    const auto month = session::collect_stats("/tmp", 30);

    REQUIRE(all.sessions == 2);
    REQUIRE(week.sessions == 1);
    REQUIRE(month.sessions == 2);

    // A range never counts more sessions than the whole of the time.
    REQUIRE(week.sessions <= all.sessions);
}

TEST_CASE("The statistics are of every project") {
    TempHome home;

    write_transcript(home.path() / "projects" / "-one" / "a.jsonl", 3, 2);
    write_transcript(home.path() / "projects" / "-two" / "b.jsonl", 4, 3);
    write_transcript(home.path() / "projects" / "-three" / "c.jsonl", 5, 1);

    const auto stats = session::collect_stats("/tmp", 0);
    REQUIRE(stats.sessions == 3);
    REQUIRE(stats.messages == 6);
}

TEST_CASE("The transcripts of the sub-agents are counted, but are not sessions") {
    TempHome home;

    write_transcript(home.path() / "projects" / "-one" / "a.jsonl", 3, 2);
    write_transcript(home.path() / "projects" / "-one" / "a" / "subagents" / "agent-1.jsonl", 3, 4);

    const auto stats = session::collect_stats("/tmp", 0);
    REQUIRE(stats.sessions == 1);
    REQUIRE(stats.messages == 2);
}

TEST_CASE("The tokens of every day are counted by the model that used them") {
    TempHome home;

    write_answered_transcript(home.path() / "projects" / "-one" / "a.jsonl", 3, "claude-sonnet-4-5",
                              100, 50);
    write_answered_transcript(home.path() / "projects" / "-two" / "b.jsonl", 2, "claude-haiku-4-5",
                              200, 25);

    const auto stats = session::collect_stats("/tmp", 0);

    // The chart of the panel of the models is drawn from them.
    REQUIRE(stats.daily_model_tokens.size() == 2);
    REQUIRE(stats.daily_model_tokens.at(stamp(3, 11).substr(0, 10)).at("claude-sonnet-4-5") == 150);
    REQUIRE(stats.daily_model_tokens.at(stamp(2, 11).substr(0, 10)).at("claude-haiku-4-5") == 225);
}

TEST_CASE("The panel of the statistics writes the tokens the way the reference writes them") {
    // The reference writes the numbers in the short form: 1321 tokens are
    // `1.3k`, 12000 are `12.0k` and 999999 are `1.0m`. The zero of a whole
    // number is kept.
    const std::pair<int64_t, std::string> cases[] = {
        {900, "900"}, {1321, "1.3k"}, {12000, "12.0k"}, {999999, "1.0m"}};

    for (const auto& [tokens, expected] : cases) {
        TempHome home;

        write_answered_transcript(home.path() / "projects" / "-one" / "a.jsonl", 1,
                                  "claude-sonnet-4-5", tokens, 0);

        const ui::StatsPanel panel(ui::Theme::dark(), "/tmp");
        // The panel reads the transcripts in the background, so that it opens
        // at once; the test waits for the numbers.
        panel.wait();
        const auto lines = panel.render(80);

        std::string joined;
        for (const std::string& line : lines) joined += ui::strip(line) + "\n";

        REQUIRE(joined.find("Total tokens: " + expected) != std::string::npos);
    }
}

TEST_CASE("The streaks are counted from the days that hold a session") {
    TempHome home;

    // Four days in a row, and a day that is far away from them.
    write_transcript(home.path() / "projects" / "-one" / "a.jsonl", 0, 1);
    write_transcript(home.path() / "projects" / "-one" / "b.jsonl", 1, 1);
    write_transcript(home.path() / "projects" / "-one" / "c.jsonl", 2, 1);
    write_transcript(home.path() / "projects" / "-one" / "d.jsonl", 3, 1);
    write_transcript(home.path() / "projects" / "-one" / "e.jsonl", 20, 1);

    const auto stats = session::collect_stats("/tmp", 0);

    REQUIRE(stats.active_days == 5);
    REQUIRE(stats.longest_streak == 4);

    // The streak that is going on is counted from the beginning of today,
    // the way the reference counts it.
    const bool starts_today = streak_start_day() == session::to_date_string(now_ms());
    REQUIRE(stats.current_streak == (starts_today ? 4 : 3));
}

TEST_CASE("The cache of the statistics is written where the reference writes it") {
    TempHome home;

    write_transcript(home.path() / "projects" / "-one" / "a.jsonl", 3, 4);

    const auto stats = session::collect_stats("/tmp", 0);
    REQUIRE(stats.sessions == 1);
    REQUIRE(stats.messages == 4);

    // The file is `stats-cache.json` in the folder of the user.
    const fs::path cache = home.path() / "stats-cache.json";
    REQUIRE(fs::exists(cache));

    const auto root = read_cache(home.path());

    // The version is the one of the reference.
    REQUIRE(jget_int(root, "version", 0) == session::kStatsCacheVersion);

    // The names of the members are the ones of the reference.
    for (const char* key :
         {"lastComputedDate", "dailyModelTokensVersion", "dailyActivity", "dailyModelTokens",
          "modelUsage", "totalSessions", "totalMessages", "longestSession", "firstSessionDate",
          "hourCounts", "totalSpeculationTimeSavedMs"}) {
        INFO("the member " << key << " is missing");
        REQUIRE(root.contains(key));
    }

    // The days that are over are in the cache, and the day that is going on
    // is not.
    REQUIRE(jget_string(root, "lastComputedDate", "") == session::yesterday_date_string());
    REQUIRE(jget_int(root, "totalSessions", 0) == 1);
    REQUIRE(jget_int(root, "totalMessages", 0) == 4);

    // The activity of the day is of the shape of the reference.
    const auto activity = jget_array(root, "dailyActivity");
    REQUIRE(activity.size() == 1);
    REQUIRE(activity[0].is_object());
    const auto& day = activity[0].as_object();
    REQUIRE(jget_string(day, "date", "") == stamp(3, 10).substr(0, 10));
    REQUIRE(jget_int(day, "messageCount", 0) == 4);
    REQUIRE(jget_int(day, "sessionCount", 0) == 1);
    REQUIRE(jget_int(day, "toolCallCount", 0) == 0);
}

TEST_CASE("The cache of the port is of the shape the reference reads") {
    TempHome home;

    write_answered_transcript(home.path() / "projects" / "-one" / "a.jsonl", 3, "claude-sonnet-4-5",
                               100, 50);

    session::collect_stats("/tmp", 0);
    const auto root = read_cache(home.path());

    // The usage of a model holds the members of the reference.
    const auto usage = jget_object(root, "modelUsage");
    REQUIRE(usage.size() == 1);
    const auto model = usage.begin()->value();
    REQUIRE(model.is_object());
    const auto& entry = model.as_object();
    for (const char* key : {"inputTokens", "outputTokens", "cacheReadInputTokens",
                            "cacheCreationInputTokens", "webSearchRequests", "costUSD",
                            "contextWindow", "maxOutputTokens"}) {
        INFO("the member " << key << " is missing");
        REQUIRE(entry.contains(key));
    }
    REQUIRE(jget_int(entry, "inputTokens", 0) == 100);
    REQUIRE(jget_int(entry, "outputTokens", 0) == 50);

    // The tokens of the day are of the shape of the reference.
    const auto tokens = jget_array(root, "dailyModelTokens");
    REQUIRE(tokens.size() == 1);
    REQUIRE(tokens[0].is_object());
    REQUIRE(jget_string(tokens[0].as_object(), "date", "") == stamp(3, 11).substr(0, 10));
    REQUIRE(jget_object(tokens[0].as_object(), "tokensByModel").size() == 1);

    // The hours are of the shape of the reference.
    const auto hours = jget_object(root, "hourCounts");
    REQUIRE(hours.size() == 1);

    // The longest session is of the shape of the reference.
    const auto longest = jget_object(root, "longestSession");
    REQUIRE(longest.size() == 4);
    REQUIRE(jget_string(longest, "sessionId", "") == "a");
    REQUIRE(jget_int(longest, "messageCount", 0) == 1);
}

TEST_CASE("The days that are over are not read again") {
    TempHome home;

    const fs::path transcript = home.path() / "projects" / "-one" / "a.jsonl";
    write_transcript(transcript, 3, 4);

    const auto first = session::collect_stats("/tmp", 0);
    REQUIRE(first.sessions == 1);
    REQUIRE(first.messages == 4);

    const auto root = read_cache(home.path());
    const std::string computed = jget_string(root, "lastComputedDate", "");
    REQUIRE(computed == session::yesterday_date_string());

    // The transcript is changed, and the statistics do not move: the days
    // that are over come from the cache, and the files are not read again.
    write_transcript(transcript, 3, 9);

    const auto second = session::collect_stats("/tmp", 0);
    REQUIRE(second.sessions == 1);
    REQUIRE(second.messages == 4);

    // The cache was not written again either.
    const auto again = read_cache(home.path());
    REQUIRE(jget_string(again, "lastComputedDate", "") == computed);
}

TEST_CASE("The cache is written again when the day that is over comes") {
    TempHome home;

    write_transcript(home.path() / "projects" / "-one" / "a.jsonl", 3, 4);

    session::collect_stats("/tmp", 0);
    const auto root = read_cache(home.path());
    REQUIRE(jget_string(root, "lastComputedDate", "") == session::yesterday_date_string());

    // A cache that is two days old is brought up to date: the day that came
    // after it is counted, and the cache is written again.
    session::StatsCache cache = session::stats_cache_from_json(boost::json::value(root));
    cache.last_computed_date = previous_day(session::yesterday_date_string());
    REQUIRE(session::save_stats_cache(cache, (home.path() / "stats-cache.json").string()));

    const auto stats = session::collect_stats("/tmp", 0);
    REQUIRE(stats.sessions == 1);
    REQUIRE(stats.messages == 4);

    const auto again = read_cache(home.path());
    REQUIRE(jget_string(again, "lastComputedDate", "") == session::yesterday_date_string());
}

TEST_CASE("The cache the reference writes is read") {
    TempHome home;

    // The cache is written the way the reference writes it.
    REQUIRE(write_file((home.path() / "stats-cache.json").string(), kReferenceCache));

    // A session of today, so that the day that is going on is counted.
    write_transcript(home.path() / "projects" / "-one" / "a.jsonl", 0, 1);

    const auto stats = session::collect_stats("/tmp", 0);

    // The numbers of the cache are the numbers of the statistics.
    REQUIRE(stats.sessions == 71);
    REQUIRE(stats.messages == 57315);
    REQUIRE(stats.longest_session_ms == 400063432);
    REQUIRE(stats.first_session_date == "2026-10-05");
    REQUIRE(stats.daily_activity.at("2026-10-05") == 273);

    const auto usage = stats.model_usage.at("MiniMax-M3.1-Flash");
    REQUIRE(usage.input_tokens == 22381061);
    REQUIRE(usage.output_tokens == 8933387);
    REQUIRE(usage.cache_read_input_tokens == 2898786353);

    // The cache is brought up to date, and keeps the numbers of the
    // reference.
    const auto root = read_cache(home.path());
    REQUIRE(jget_string(root, "lastComputedDate", "") == session::yesterday_date_string());
    REQUIRE(jget_int(root, "totalSessions", 0) == 70);
    REQUIRE(jget_int(root, "totalMessages", 0) == 57314);
}

TEST_CASE("The statistics are collected quickly when the cache is there") {
    // The timings are printed, so that a session that opens the panel
    // slowly is seen as one. The real folder of the user is only read when
    // the benchmark is asked for, so that the test does not depend on what
    // the user has.
    const bool bench = std::getenv("CC_STATS_BENCH") != nullptr;
    std::unique_ptr<TempHome> home;
    if (!bench) home = std::make_unique<TempHome>();
    if (home) {
        for (int i = 0; i < 20; ++i) {
            write_transcript(home->path() / "projects" / "-one" / ("s" + std::to_string(i) + ".jsonl"),
                             3, 20);
        }
    }

    const auto start = std::chrono::steady_clock::now();
    const auto all = session::collect_stats(cc::cwd(), 0);
    const auto middle = std::chrono::steady_clock::now();
    const auto again = session::collect_stats(cc::cwd(), 0);
    const auto end = std::chrono::steady_clock::now();
    const auto week = session::collect_stats(cc::cwd(), 7);
    const auto done = std::chrono::steady_clock::now();

    WARN("all time: "
         << std::chrono::duration_cast<std::chrono::milliseconds>(middle - start).count() << " ms, "
         << all.sessions << " sessions");
    WARN("all time, with the cache: "
         << std::chrono::duration_cast<std::chrono::milliseconds>(end - middle).count() << " ms, "
         << again.sessions << " sessions");
    WARN("last week: "
         << std::chrono::duration_cast<std::chrono::milliseconds>(done - end).count() << " ms, "
         << week.sessions << " sessions");

    // The totals are the same, the second time round.
    REQUIRE(again.sessions == all.sessions);
    REQUIRE(again.messages == all.messages);
}
