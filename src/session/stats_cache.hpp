#pragma once

#include <boost/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cc::session {

/// The version of the shape of the cache.
///
/// The reference writes the same version, so that the two read each other's
/// cache.
inline constexpr int kStatsCacheVersion = 5;

/// The version of the tokens of the days.
///
/// The tokens of the days are counted in a way of their own, and the version
/// says in which way they were counted. A cache of an older version has them
/// counted again, the way the reference counts them again.
inline constexpr int kDailyModelTokensVersion = 5;

/// The name of the file the cache is kept in.
inline constexpr const char* kStatsCacheFilename = "stats-cache.json";

/// The activity of a day.
///
/// The days are the days of UTC, the way the reference counts them, so that
/// the cache of the two agrees on the day a session belongs to.
struct DailyActivity {
    /// The day, as `YYYY-MM-DD`.
    std::string date;

    /// The number of the messages of the day.
    int64_t message_count = 0;

    /// The number of the sessions of the day.
    int64_t session_count = 0;

    /// The number of the tool calls of the day.
    int64_t tool_call_count = 0;
};

/// The tokens of a day, by the model that used them.
struct DailyModelTokens {
    /// The day, as `YYYY-MM-DD`.
    std::string date;

    /// The tokens of every model: the input and the output together.
    std::map<std::string, int64_t> tokens_by_model;
};

/// The usage of a model.
///
/// The shape is the one of the reference, so that the two caches hold the
/// same numbers under the same names.
struct ModelUsage {
    /// The tokens that were sent to the model.
    int64_t input_tokens = 0;

    /// The tokens the model answered with.
    int64_t output_tokens = 0;

    /// The tokens that were read from the cache of the prompt.
    int64_t cache_read_input_tokens = 0;

    /// The tokens that were written into the cache of the prompt.
    int64_t cache_creation_input_tokens = 0;

    /// The number of the searches the model made on the web.
    int64_t web_search_requests = 0;

    /// What the turn cost, in dollars.
    double cost_usd = 0.0;

    /// The size of the window of the model.
    int64_t context_window = 0;

    /// The number of the tokens the model may answer with.
    int64_t max_output_tokens = 0;
};

/// The statistics of one session.
struct SessionStats {
    /// The id of the session.
    std::string session_id;

    /// The length of the session, in milliseconds.
    int64_t duration = 0;

    /// The number of the messages of the session.
    int64_t message_count = 0;

    /// The time the session began at, as an ISO-8601 timestamp.
    std::string timestamp;
};

/// The statistics of a set of sessions, before they are merged into the
/// cache.
struct ProcessedStats {
    /// The activity of every day.
    std::vector<DailyActivity> daily_activity;

    /// The tokens of every day, by the model.
    std::vector<DailyModelTokens> daily_model_tokens;

    /// The usage of every model.
    std::map<std::string, ModelUsage> model_usage;

    /// The statistics of every session.
    std::vector<SessionStats> session_stats;

    /// The number of the sessions of every hour of the day.
    std::map<int, int64_t> hour_counts;

    /// The number of the messages of the sessions.
    int64_t total_messages = 0;

    /// The time the speculation saved, in milliseconds.
    int64_t total_speculation_time_saved_ms = 0;

    /// The number of the sessions of every shot count.
    ///
    /// The reference only counts the shots in its own builds; the field is
    /// kept so that the cache of the two has the same shape.
    std::map<int, int64_t> shot_distribution;
};

/// The cache of the statistics, as it is kept on disk.
///
/// The cache holds the statistics of the days that are over. The day that is
/// going on is counted again every time, because it is not over yet.
struct StatsCache {
    /// The version of the shape of the cache.
    int version = kStatsCacheVersion;

    /// The day up to which the statistics were counted, as `YYYY-MM-DD`.
    ///
    /// The field is empty when the cache holds nothing yet.
    std::string last_computed_date;

    /// The version of the tokens of the days.
    ///
    /// The tokens of the days are counted in a way of their own, and the
    /// version says in which way they were counted. A cache of an older
    /// version has them counted again.
    int daily_model_tokens_version = kDailyModelTokensVersion;

    /// The activity of every day.
    std::vector<DailyActivity> daily_activity;

    /// The tokens of every day, by the model.
    std::vector<DailyModelTokens> daily_model_tokens;

    /// The usage of every model.
    std::map<std::string, ModelUsage> model_usage;

    /// The number of the sessions.
    int64_t total_sessions = 0;

    /// The number of the messages.
    int64_t total_messages = 0;

    /// The longest session.
    std::optional<SessionStats> longest_session;

    /// The time the first session began at, as an ISO-8601 timestamp.
    std::string first_session_date;

    /// The number of the sessions of every hour of the day.
    std::map<int, int64_t> hour_counts;

    /// The time the speculation saved, in milliseconds.
    int64_t total_speculation_time_saved_ms = 0;

    /// The number of the sessions of every shot count.
    std::optional<std::map<int, int64_t>> shot_distribution;
};

/// The path of the cache: `~/.claude/stats-cache.json`.
std::string stats_cache_path();

/// Loads the cache from `path`.
///
/// An empty cache is returned when the file is missing, unreadable, or of a
/// version the port cannot read. The path defaults to
/// `stats_cache_path()`.
StatsCache load_stats_cache(const std::string& path = {});

/// Saves the cache to `path`.
///
/// The file is written next to the cache and moved over it, so that a crash
/// in the middle of the write does not leave a half-written cache behind.
/// The path defaults to `stats_cache_path()`.
bool save_stats_cache(const StatsCache& cache, const std::string& path = {});

/// Merges the statistics of the sessions into the cache.
///
/// The days that the new statistics hold are added to the days the cache
/// already holds, the way the reference merges them.
StatsCache merge_cache_with_new_stats(const StatsCache& existing,
                                      const ProcessedStats& fresh,
                                      const std::string& new_last_computed_date);

/// The cache as JSON, in the shape the reference reads.
boost::json::value stats_cache_to_json(const StatsCache& cache);

/// The cache as it is read from JSON.
///
/// A cache of a version the port cannot read comes back empty.
StatsCache stats_cache_from_json(const boost::json::value& value);

/// The day a time falls on, as `YYYY-MM-DD`.
///
/// The day is the day of UTC, the way the reference writes it.
std::string to_date_string(int64_t timestamp_ms);

/// The day of today, as `YYYY-MM-DD`.
std::string today_date_string();

/// The day of yesterday, as `YYYY-MM-DD`.
std::string yesterday_date_string();

/// The day a day follows, as `YYYY-MM-DD`.
std::string next_day_string(const std::string& day);

} // namespace cc::session
