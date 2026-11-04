#include "session/stats_cache.hpp"

#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/time.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>

namespace cc::session {
namespace {

namespace fs = std::filesystem;

/// The number of milliseconds in a day.
constexpr int64_t kDay = 24 * 60 * 60 * 1000;

/// True when the object holds an array under `key`.
bool has_array(const boost::json::object& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->value().is_array();
}

/// True when the object holds a number under `key`.
bool has_number(const boost::json::object& object, const char* key) {
    const auto it = object.find(key);
    if (it == object.end()) return false;
    const auto& value = it->value();
    return value.is_int64() || value.is_uint64() || value.is_double();
}

/// A string, or nothing when the text is empty.
boost::json::value optional_string(const std::string& text) {
    if (text.empty()) return boost::json::value{nullptr};
    return boost::json::value{text};
}

/// The numbers of an object, by the name of the member they are under.
std::map<std::string, int64_t> int_map(const boost::json::object& object) {
    std::map<std::string, int64_t> out;
    for (const auto& [key, value] : object) {
        if (value.is_int64()) {
            out[key] = value.as_int64();
        } else if (value.is_uint64()) {
            out[key] = static_cast<int64_t>(value.as_uint64());
        } else if (value.is_double()) {
            out[key] = static_cast<int64_t>(value.as_double());
        }
    }
    return out;
}

/// Reads the cache out of JSON.
///
/// The result is empty when the cache cannot be read: when it is not an
/// object, when its version is one the port cannot migrate, or when its
/// shape is not the one the reference writes.
std::optional<StatsCache> read_cache(const boost::json::value& value) {
    if (!value.is_object()) return std::nullopt;

    const auto& root = value.as_object();

    // The reference migrates the caches of the versions before its own, and
    // reads nothing when the version is one it cannot migrate.
    const int64_t version = jget_int(root, "version", 0);
    if (version < 1 || version > kStatsCacheVersion) return std::nullopt;

    // The shape of the cache is checked before it is read, the way the
    // reference checks it.
    if (!has_array(root, "dailyActivity") || !has_array(root, "dailyModelTokens") ||
        !has_number(root, "totalSessions") || !has_number(root, "totalMessages")) {
        return std::nullopt;
    }

    StatsCache cache;
    cache.version = kStatsCacheVersion;
    cache.last_computed_date = jget_string(root, "lastComputedDate", "");
    // The version of the tokens of the days is missing in the caches that
    // were written before the tokens were counted in the way of the
    // reference; they are counted again.
    cache.daily_model_tokens_version = jget_int(root, "dailyModelTokensVersion", 0);
    cache.total_sessions = jget_int(root, "totalSessions", 0);
    cache.total_messages = jget_int(root, "totalMessages", 0);
    cache.first_session_date = jget_string(root, "firstSessionDate", "");
    cache.total_speculation_time_saved_ms =
        jget_int(root, "totalSpeculationTimeSavedMs", 0);

    for (const auto& element : jget_array(root, "dailyActivity")) {
        if (!element.is_object()) continue;
        const auto& day = element.as_object();

        DailyActivity activity;
        activity.date = jget_string(day, "date", "");
        activity.message_count = jget_int(day, "messageCount", 0);
        activity.session_count = jget_int(day, "sessionCount", 0);
        activity.tool_call_count = jget_int(day, "toolCallCount", 0);
        cache.daily_activity.push_back(std::move(activity));
    }

    for (const auto& element : jget_array(root, "dailyModelTokens")) {
        if (!element.is_object()) continue;
        const auto& day = element.as_object();

        DailyModelTokens tokens;
        tokens.date = jget_string(day, "date", "");
        tokens.tokens_by_model = int_map(jget_object(day, "tokensByModel"));
        cache.daily_model_tokens.push_back(std::move(tokens));
    }

    for (const auto& [model, element] : jget_object(root, "modelUsage")) {
        if (!element.is_object()) continue;
        const auto& usage = element.as_object();

        ModelUsage model_usage;
        model_usage.input_tokens = jget_int(usage, "inputTokens", 0);
        model_usage.output_tokens = jget_int(usage, "outputTokens", 0);
        model_usage.cache_read_input_tokens = jget_int(usage, "cacheReadInputTokens", 0);
        model_usage.cache_creation_input_tokens = jget_int(usage, "cacheCreationInputTokens", 0);
        model_usage.web_search_requests = jget_int(usage, "webSearchRequests", 0);
        model_usage.cost_usd = jget_double(usage, "costUSD", 0.0);
        model_usage.context_window = jget_int(usage, "contextWindow", 0);
        model_usage.max_output_tokens = jget_int(usage, "maxOutputTokens", 0);
        cache.model_usage[model] = model_usage;
    }

    const auto longest = jget_object(root, "longestSession");
    if (!longest.empty()) {
        SessionStats session;
        session.session_id = jget_string(longest, "sessionId", "");
        session.duration = jget_int(longest, "duration", 0);
        session.message_count = jget_int(longest, "messageCount", 0);
        session.timestamp = jget_string(longest, "timestamp", "");
        cache.longest_session = std::move(session);
    }

    for (const auto& [hour, count] : int_map(jget_object(root, "hourCounts"))) {
        cache.hour_counts[std::atoi(hour.c_str())] = count;
    }

    // The shots are only counted in the builds of the reference that have
    // the feature; the field is kept so that the cache of the two has the
    // same shape.
    const auto shots = root.find("shotDistribution");
    if (shots != root.end() && shots->value().is_object()) {
        std::map<int, int64_t> distribution;
        for (const auto& [count, sessions] : int_map(shots->value().as_object())) {
            distribution[std::atoi(count.c_str())] = sessions;
        }
        cache.shot_distribution = std::move(distribution);
    }

    return cache;
}

/// A name for the file the cache is written to before it is moved over the
/// cache itself.
std::string temporary_name(const std::string& path) {
    std::random_device device;
    const unsigned long suffix = device();
    char text[32] = {};
    std::snprintf(text, sizeof(text), ".%08lx.tmp", suffix);
    return path + text;
}

} // namespace

std::string stats_cache_path() {
    return path_join(claude_config_dir(), kStatsCacheFilename);
}

std::string to_date_string(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) return {};

    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif

    char day[16] = {};
    std::strftime(day, sizeof(day), "%Y-%m-%d", &utc);
    return std::string(day);
}

std::string today_date_string() {
    return to_date_string(now_ms());
}

std::string yesterday_date_string() {
    return to_date_string(now_ms() - kDay);
}

std::string next_day_string(const std::string& day) {
    return next_day(day);
}

boost::json::value stats_cache_to_json(const StatsCache& cache) {
    boost::json::object root;

    root["version"] = cache.version;
    root["lastComputedDate"] = optional_string(cache.last_computed_date);
    root["dailyModelTokensVersion"] = cache.daily_model_tokens_version;

    boost::json::array activity;
    for (const auto& day : cache.daily_activity) {
        boost::json::object entry;
        entry["date"] = day.date;
        entry["messageCount"] = day.message_count;
        entry["sessionCount"] = day.session_count;
        entry["toolCallCount"] = day.tool_call_count;
        activity.emplace_back(std::move(entry));
    }
    root["dailyActivity"] = std::move(activity);

    boost::json::array tokens;
    for (const auto& day : cache.daily_model_tokens) {
        boost::json::object entry;
        entry["date"] = day.date;

        boost::json::object by_model;
        for (const auto& [model, count] : day.tokens_by_model) by_model[model] = count;
        entry["tokensByModel"] = std::move(by_model);

        tokens.emplace_back(std::move(entry));
    }
    root["dailyModelTokens"] = std::move(tokens);

    boost::json::object usage;
    for (const auto& [model, model_usage] : cache.model_usage) {
        boost::json::object entry;
        entry["inputTokens"] = model_usage.input_tokens;
        entry["outputTokens"] = model_usage.output_tokens;
        entry["cacheReadInputTokens"] = model_usage.cache_read_input_tokens;
        entry["cacheCreationInputTokens"] = model_usage.cache_creation_input_tokens;
        entry["webSearchRequests"] = model_usage.web_search_requests;
        entry["costUSD"] = model_usage.cost_usd;
        entry["contextWindow"] = model_usage.context_window;
        entry["maxOutputTokens"] = model_usage.max_output_tokens;
        usage[model] = std::move(entry);
    }
    root["modelUsage"] = std::move(usage);

    root["totalSessions"] = cache.total_sessions;
    root["totalMessages"] = cache.total_messages;

    if (cache.longest_session) {
        boost::json::object entry;
        entry["sessionId"] = cache.longest_session->session_id;
        entry["duration"] = cache.longest_session->duration;
        entry["messageCount"] = cache.longest_session->message_count;
        entry["timestamp"] = cache.longest_session->timestamp;
        root["longestSession"] = std::move(entry);
    } else {
        root["longestSession"] = boost::json::value{nullptr};
    }

    root["firstSessionDate"] = optional_string(cache.first_session_date);

    boost::json::object hours;
    for (const auto& [hour, count] : cache.hour_counts) hours[std::to_string(hour)] = count;
    root["hourCounts"] = std::move(hours);

    root["totalSpeculationTimeSavedMs"] = cache.total_speculation_time_saved_ms;

    if (cache.shot_distribution) {
        boost::json::object shots;
        for (const auto& [count, sessions] : *cache.shot_distribution) {
            shots[std::to_string(count)] = sessions;
        }
        root["shotDistribution"] = std::move(shots);
    }

    return boost::json::value{std::move(root)};
}

StatsCache stats_cache_from_json(const boost::json::value& value) {
    return read_cache(value).value_or(StatsCache{});
}

StatsCache load_stats_cache(const std::string& path) {
    const std::string file = path.empty() ? stats_cache_path() : path;

    const auto contents = read_file(file);
    if (!contents) return {};

    const boost::json::value value = json_parse(*contents);
    if (!value.is_object()) return {};

    const int64_t version = jget_int(value.as_object(), "version", 0);
    if (version == kStatsCacheVersion) return stats_cache_from_json(value);

    // The cache is of an older version: it is migrated and written back, so
    // that it is not migrated again on every run. The reference does the
    // same, because it does not save the cache when it is already current.
    auto migrated = read_cache(value);
    if (!migrated) return {};

    migrated->version = kStatsCacheVersion;
    save_stats_cache(*migrated, file);
    return *migrated;
}

bool save_stats_cache(const StatsCache& cache, const std::string& path) {
    const std::string file = path.empty() ? stats_cache_path() : path;

    const std::string contents = json_dump_pretty(stats_cache_to_json(cache));

    // The cache is written next to itself and moved over it, so that a
    // crash in the middle of the write does not leave a half-written cache
    // behind.
    const std::string temporary = temporary_name(file);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out) {
            out.close();
            remove_file(temporary);
            return false;
        }
    }

    // The cache holds what the user did, and is read by nobody else.
    std::error_code ec;
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write, ec);

    fs::rename(temporary, file, ec);
    if (ec) {
        remove_file(temporary);
        return false;
    }
    return true;
}

StatsCache merge_cache_with_new_stats(const StatsCache& existing, const ProcessedStats& fresh,
                                      const std::string& new_last_computed_date) {
    // The activity of a day is added to the activity the cache already
    // holds of that day.
    std::map<std::string, DailyActivity> activity;
    for (const auto& day : existing.daily_activity) activity[day.date] = day;
    for (const auto& day : fresh.daily_activity) {
        const auto it = activity.find(day.date);
        if (it == activity.end()) {
            activity.emplace(day.date, day);
            continue;
        }
        it->second.message_count += day.message_count;
        it->second.session_count += day.session_count;
        it->second.tool_call_count += day.tool_call_count;
    }

    // The tokens of a day are added to the tokens the cache already holds
    // of that day.
    std::map<std::string, std::map<std::string, int64_t>> tokens;
    for (const auto& day : existing.daily_model_tokens) tokens[day.date] = day.tokens_by_model;
    for (const auto& day : fresh.daily_model_tokens) {
        const auto it = tokens.find(day.date);
        if (it == tokens.end()) {
            tokens.emplace(day.date, day.tokens_by_model);
            continue;
        }
        for (const auto& [model, count] : day.tokens_by_model) it->second[model] += count;
    }

    // The usage of a model is added to the usage the cache already holds of
    // that model. The size of the window and the number of the tokens of
    // the answer are the largest of the two, and not their sum.
    std::map<std::string, ModelUsage> usage = existing.model_usage;
    for (const auto& [model, model_usage] : fresh.model_usage) {
        const auto it = usage.find(model);
        if (it == usage.end()) {
            usage.emplace(model, model_usage);
            continue;
        }
        it->second.input_tokens += model_usage.input_tokens;
        it->second.output_tokens += model_usage.output_tokens;
        it->second.cache_read_input_tokens += model_usage.cache_read_input_tokens;
        it->second.cache_creation_input_tokens += model_usage.cache_creation_input_tokens;
        it->second.web_search_requests += model_usage.web_search_requests;
        it->second.cost_usd += model_usage.cost_usd;
        it->second.context_window = std::max(it->second.context_window, model_usage.context_window);
        it->second.max_output_tokens =
            std::max(it->second.max_output_tokens, model_usage.max_output_tokens);
    }

    // The hours are added together.
    std::map<int, int64_t> hours = existing.hour_counts;
    for (const auto& [hour, count] : fresh.hour_counts) hours[hour] += count;

    // The number of the sessions and of the messages.
    const int64_t total_sessions =
        existing.total_sessions + static_cast<int64_t>(fresh.session_stats.size());
    const int64_t total_messages = existing.total_messages + fresh.total_messages;

    // The longest session.
    std::optional<SessionStats> longest = existing.longest_session;
    for (const auto& session : fresh.session_stats) {
        if (!longest || session.duration > longest->duration) longest = session;
    }

    // The first session.
    std::string first = existing.first_session_date;
    for (const auto& session : fresh.session_stats) {
        if (first.empty() || session.timestamp < first) first = session.timestamp;
    }

    StatsCache result;
    result.version = kStatsCacheVersion;
    result.last_computed_date = new_last_computed_date;
    // The version of the tokens of the days is the one of the cache that is
    // merged into: the days that are added are counted in the way of the
    // reference, and the days that were counted before are kept as they are.
    result.daily_model_tokens_version = existing.daily_model_tokens_version;

    for (const auto& [date, day] : activity) result.daily_activity.push_back(day);
    for (const auto& [date, by_model] : tokens) {
        DailyModelTokens day;
        day.date = date;
        day.tokens_by_model = by_model;
        result.daily_model_tokens.push_back(std::move(day));
    }

    result.model_usage = usage;
    result.total_sessions = total_sessions;
    result.total_messages = total_messages;
    result.longest_session = longest;
    result.first_session_date = first;
    result.hour_counts = hours;
    result.total_speculation_time_saved_ms =
        existing.total_speculation_time_saved_ms + fresh.total_speculation_time_saved_ms;

    // The shots are only counted in the builds of the reference that have
    // the feature.
    if (existing.shot_distribution || !fresh.shot_distribution.empty()) {
        std::map<int, int64_t> shots = existing.shot_distribution.value_or(std::map<int, int64_t>{});
        for (const auto& [count, sessions] : fresh.shot_distribution) shots[count] += sessions;
        result.shot_distribution = std::move(shots);
    }

    return result;
}

} // namespace cc::session
