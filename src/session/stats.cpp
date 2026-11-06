#include "session/stats.hpp"

#include "session/stats_cache.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace cc::session {
namespace {

namespace fs = std::filesystem;

/// The number of milliseconds in a day.
constexpr int64_t kDay = 24 * 60 * 60 * 1000;

/// The number of the files that are read at the same time.
///
/// The reference reads the transcripts in batches of twenty, so that the
/// disk is not asked for too much at once.
constexpr size_t kBatchSize = 20;

/// The size a file must be over before the day it began on is peeked at.
constexpr int64_t kPeekSize = 65536;

/// The number of the bytes that are read when the head of a file is peeked
/// at.
constexpr size_t kPeekWindow = 4096;

/// The model the reference marks as synthetic.
///
/// The messages of the synthetic model are internal, and are left out of the
/// statistics.
constexpr const char* kSyntheticModel = "<synthetic>";

/// The lock of the cache, so that two panels do not write it at the same
/// time.
std::mutex g_cache_mutex;

/// The number of transcripts that were read, by the folder they are in.
///
/// The counter is what the tests use to show that the days that are over are
/// not read a second time.
std::mutex g_reads_mutex;
std::map<std::string, size_t> g_transcripts_read;

/// True when the entry of a transcript is a message of the conversation.
///
/// The list is the one of the reference.
bool is_transcript_message(std::string_view type) {
    return type == "user" || type == "assistant" || type == "attachment" || type == "system";
}

/// True when the entry of a transcript is one the peek at the head of a file
/// accepts.
///
/// The list is the one of the reference, which lets a progress entry be the
/// first one of a session.
bool is_peek_message(std::string_view type) {
    return is_transcript_message(type) || type == "progress";
}

/// The time a timestamp of a transcript names, in milliseconds.
///
/// The timestamps look like `2026-01-02T03:04:05.678Z`; the ones without
/// the fraction of a second are read as well.
int64_t parse_stamp(const std::string& text) {
    const int64_t stamp = parse_timestamp(text);
    if (stamp > 0) return stamp;

    // The fraction of a second is missing: `2026-01-02T03:04:05Z`.
    if (text.size() == 20 && text.back() == 'Z') {
        return parse_timestamp(text.substr(0, 19) + ".000Z");
    }
    return 0;
}

/// The beginning of the day a time falls on, in milliseconds.
///
/// The reference counts the streak that is going on from the beginning of
/// the day of the machine, and writes the day of UTC, so the two have to
/// agree on the beginning of the day.
int64_t local_midnight(int64_t timestamp_ms) {
    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
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

    const std::time_t midnight = std::mktime(&local);
    if (midnight == static_cast<std::time_t>(-1)) return timestamp_ms;
    return static_cast<int64_t>(midnight) * 1000;
}

/// The transcripts of every project of the machine.
///
/// The statistics of the reference are of every project, not only of the
/// folder the session runs in, so the sessions of every project are read.
/// The files of the sub-agents are read as well: they are counted, but they
/// are not sessions of their own.
std::vector<std::string> all_session_files() {
    std::vector<std::string> files;

    const std::string projects = path_join(claude_config_dir(), "projects");

    std::error_code ec;
    if (!fs::is_directory(projects, ec)) return files;

    for (const auto& project : fs::directory_iterator(projects, ec)) {
        if (!project.is_directory(ec)) continue;
        const std::string project_dir = project.path().string();

        for (const std::string& name : list_dir(project_dir)) {
            // The sessions of the project.
            if (ends_with(name, ".jsonl") && is_file(path_join(project_dir, name))) {
                files.push_back(path_join(project_dir, name));
                continue;
            }

            // The sub-agents of the sessions of the project.
            if (!is_directory(path_join(project_dir, name))) continue;
            const std::string subagents = path_join(path_join(project_dir, name), "subagents");
            for (const std::string& agent : list_dir(subagents)) {
                if (!starts_with(agent, "agent-")) continue;
                if (!ends_with(agent, ".jsonl")) continue;
                const std::string path = path_join(subagents, agent);
                if (!is_file(path)) continue;
                files.push_back(path);
            }
        }
    }

    return files;
}

/// The day the session of a file began on, as `YYYY-MM-DD`.
///
/// The head of the file is read and the lines are looked at until a message
/// is found. The whole file is not read, the way the reference does it.
std::string read_session_start_date(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    std::string head(kPeekWindow, '\0');
    file.read(head.data(), static_cast<std::streamsize>(kPeekWindow));
    const std::streamsize read = file.gcount();
    if (read <= 0) return {};
    head.resize(static_cast<size_t>(read));

    // Only the whole lines are read: the end of the window may cut a line
    // in two.
    const size_t last = head.rfind('\n');
    if (last == std::string::npos) return {};

    size_t start = 0;
    while (start <= last) {
        const size_t end = head.find('\n', start);
        if (end == std::string::npos || end > last) break;

        const std::string line = head.substr(start, end - start);
        start = end + 1;
        if (line.empty()) continue;

        const boost::json::value value = json_parse(line);
        if (!value.is_object()) continue;
        const auto& entry = value.as_object();

        const std::string type = jget_string(entry, "type", "");
        if (!is_peek_message(type)) continue;
        if (jget_bool(entry, "isSidechain", false)) continue;

        const auto stamp = jget_optional_string(entry, "timestamp");
        if (!stamp) return {};
        const int64_t milliseconds = parse_stamp(*stamp);
        if (milliseconds <= 0) return {};
        return to_date_string(milliseconds);
    }

    return {};
}

/// True when the file may hold a message of `from_date`.
///
/// A file that was not written to since the day began holds no message of
/// it. A file that is large is peeked at, so that a session that began long
/// ago and was resumed today is not read again.
bool may_hold_day(const std::string& path, const std::string& from_date) {
    if (from_date.empty()) return true;

    const FileState state = file_state(path);
    if (state.exists && to_date_string(state.mtime_ms) < from_date) return false;

    if (state.size > kPeekSize) {
        const std::string start = read_session_start_date(path);
        if (!start.empty() && start < from_date) return false;
    }

    return true;
}

/// A message of a transcript, as the statistics need it.
struct StatEntry {
    /// The message was written by the model.
    bool is_assistant = false;

    /// The message was written by a sub-agent.
    bool is_sidechain = false;

    /// The time the message was written at.
    std::string timestamp;

    /// The model that answered.
    std::string model;

    /// The tokens the turn used.
    Usage usage;

    /// The turn carried a usage.
    bool has_usage = false;

    /// The number of the tools the turn asked for.
    int tool_calls = 0;
};

/// What a transcript holds that the statistics need.
struct TranscriptStats {
    /// The messages of the transcript.
    std::vector<StatEntry> messages;

    /// The time the speculation of the transcript saved, in milliseconds.
    int64_t speculation_time_saved_ms = 0;
};

/// Reads the messages of a transcript.
///
/// Only what the statistics need is read: the messages themselves are not
/// built, so that a transcript of many megabytes is read quickly.
TranscriptStats read_transcript(const std::string& path) {
    TranscriptStats stats;

    std::ifstream file(path, std::ios::binary);
    if (!file) return stats;

    // The transcript is read: the counter says how many of them were.
    {
        const std::lock_guard<std::mutex> lock(g_reads_mutex);
        g_transcripts_read[claude_config_dir()] += 1;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        const boost::json::value value = json_parse(line);
        if (!value.is_object()) continue;
        const auto& entry = value.as_object();

        const std::string type = jget_string(entry, "type", "");

        // The time a speculation saved is counted even when the transcript
        // holds no message at all.
        if (type == "speculation-accept") {
            stats.speculation_time_saved_ms += jget_int(entry, "timeSavedMs", 0);
            continue;
        }

        if (!is_transcript_message(type)) continue;

        StatEntry message;
        message.is_assistant = type == "assistant";
        message.timestamp = jget_string(entry, "timestamp", "");
        message.is_sidechain = jget_bool(entry, "isSidechain", false);

        // The turn of the model: the model and the tokens are only there.
        if (message.is_assistant) {
            const auto payload = entry.find("message");
            if (payload != entry.end() && payload->value().is_object()) {
                const auto& turn = payload->value().as_object();

                message.model = jget_string(turn, "model", "");
                const auto usage = turn.find("usage");
                if (usage != turn.end() && usage->value().is_object()) {
                    message.has_usage = true;
                    message.usage = Usage::from_json(usage->value());
                }

                const auto content = turn.find("content");
                if (content != turn.end() && content->value().is_array()) {
                    for (const auto& block : content->value().as_array()) {
                        if (!block.is_object()) continue;
                        if (jget_string(block.as_object(), "type", "") == "tool_use") {
                            ++message.tool_calls;
                        }
                    }
                }
            }
        }

        stats.messages.push_back(std::move(message));
    }

    return stats;
}

/// The statistics of the sessions of a set of files.
///
/// The files that were not written to since `from_date` are skipped before
/// they are read, and the sessions that began before it are skipped as well.
ProcessedStats process_session_files(const std::vector<std::string>& files,
                                     const std::string& from_date,
                                     const std::string& to_date) {
    ProcessedStats stats;

    std::map<std::string, DailyActivity> activity;
    std::map<std::string, std::map<std::string, int64_t>> tokens;
    std::map<std::string, ModelUsage> usage;
    std::map<int, int64_t> hours;

    for (size_t start = 0; start < files.size(); start += kBatchSize) {
        const size_t end = std::min(start + kBatchSize, files.size());

        // The files of the batch are read at the same time.
        std::vector<std::future<std::optional<TranscriptStats>>> reads;
        reads.reserve(end - start);
        for (size_t index = start; index < end; ++index) {
            const std::string path = files[index];
            reads.push_back(std::async(std::launch::async, [path, from_date]() {
                if (!may_hold_day(path, from_date)) return std::optional<TranscriptStats>{};
                return std::optional<TranscriptStats>{read_transcript(path)};
            }));
        }

        for (size_t index = start; index < end; ++index) {
            const std::string& path = files[index];

            // The transcripts of the sub-agents are marked as such: they are
            // counted, but they are not sessions of their own.
            const bool is_subagent = path.find("/subagents/") != std::string::npos;

            std::optional<TranscriptStats> read = reads[index - start].get();
            if (!read) continue;

            stats.total_speculation_time_saved_ms += read->speculation_time_saved_ms;
            if (read->messages.empty()) continue;

            // The messages of the session: every message of a sub-agent, and
            // the messages that are not of one otherwise.
            std::vector<const StatEntry*> messages;
            messages.reserve(read->messages.size());
            for (const StatEntry& message : read->messages) {
                if (is_subagent || !message.is_sidechain) messages.push_back(&message);
            }
            if (messages.empty()) continue;

            const StatEntry& first = *messages.front();
            const StatEntry& last = *messages.back();

            const int64_t first_ms = parse_stamp(first.timestamp);
            const int64_t last_ms = parse_stamp(last.timestamp);
            if (first_ms <= 0 || last_ms <= 0) continue;

            // The day of the session is the day of its first message.
            const std::string date = to_date_string(first_ms);
            if (!from_date.empty() && date < from_date) continue;
            if (!to_date.empty() && to_date < date) continue;

            const auto known = activity.find(date);
            DailyActivity day;
            day.date = date;
            if (known != activity.end()) day = known->second;

            if (!is_subagent) {
                SessionStats session;
                session.session_id = stem(path);
                session.duration = last_ms - first_ms;
                session.message_count = static_cast<int64_t>(messages.size());
                session.timestamp = first.timestamp;
                stats.session_stats.push_back(std::move(session));

                stats.total_messages += static_cast<int64_t>(messages.size());

                day.session_count += 1;
                day.message_count += static_cast<int64_t>(messages.size());

                const int hour = hour_of_day(first_ms);
                hours[hour] += 1;

                activity[date] = day;
            } else if (known != activity.end()) {
                // A sub-agent does not make a day of its own: the day is
                // only kept when a session already made it.
                activity[date] = day;
            }

            for (const StatEntry* message : messages) {
                if (!message->is_assistant) continue;

                // The tools the turn asked for.
                const auto entry = activity.find(date);
                if (entry != activity.end()) {
                    entry->second.tool_call_count += message->tool_calls;
                }

                if (!message->has_usage) continue;

                // The model that answered. The synthetic model is internal,
                // and is left out of the statistics.
                const std::string model = message->model.empty() ? "unknown" : message->model;
                if (model == kSyntheticModel) continue;

                ModelUsage& model_usage = usage[model];
                model_usage.input_tokens += message->usage.input_tokens;
                model_usage.output_tokens += message->usage.output_tokens;
                model_usage.cache_read_input_tokens += message->usage.cache_read_input_tokens;
                model_usage.cache_creation_input_tokens += message->usage.cache_creation_input_tokens;

                const int64_t total = message->usage.input_tokens + message->usage.output_tokens +
                                      message->usage.cache_read_input_tokens +
                                      message->usage.cache_creation_input_tokens;
                if (total > 0) tokens[date][model] += total;
            }
        }
    }

    for (const auto& [date, day] : activity) stats.daily_activity.push_back(day);
    for (const auto& [date, by_model] : tokens) {
        DailyModelTokens day;
        day.date = date;
        day.tokens_by_model = by_model;
        stats.daily_model_tokens.push_back(std::move(day));
    }
    stats.model_usage = usage;
    stats.hour_counts = hours;

    return stats;
}

/// The streaks of the days that follow one another.
struct Streaks {
    /// The number of the days of the streak that is going on.
    size_t current = 0;

    /// The number of the days of the longest streak.
    size_t longest = 0;
};

/// Counts the streaks of the days that hold a session.
///
/// The streak that is going on is counted back from the beginning of today,
/// the way the reference counts it.
Streaks calculate_streaks(const std::vector<DailyActivity>& activity) {
    Streaks streaks;
    if (activity.empty()) return streaks;

    std::set<std::string> active;
    for (const auto& day : activity) active.insert(day.date);

    // The streak that is going on is counted back from the beginning of
    // today. The reference starts at the midnight of the machine and writes
    // the day of UTC, so the two agree on the day the streak starts at.
    int64_t moment = local_midnight(now_ms());
    while (true) {
        const std::string day = to_date_string(moment);
        if (active.count(day) == 0) break;
        ++streaks.current;
        moment -= kDay;
    }

    // The longest streak: the days that follow one another.
    size_t run = 0;
    std::string previous;
    for (const std::string& day : active) {
        if (!previous.empty() && next_day_string(previous) == day) {
            ++run;
        } else {
            run = 1;
        }
        streaks.longest = std::max(streaks.longest, run);
        previous = day;
    }

    return streaks;
}

/// The usage of a model, as the port counts it.
Usage to_usage(const ModelUsage& usage) {
    Usage out;
    out.input_tokens = usage.input_tokens;
    out.output_tokens = usage.output_tokens;
    out.cache_read_input_tokens = usage.cache_read_input_tokens;
    out.cache_creation_input_tokens = usage.cache_creation_input_tokens;
    return out;
}

/// The hour the sessions were begun in most often.
int peak_hour(const std::map<int, int64_t>& hours) {
    int peak = -1;
    int64_t most = 0;
    for (const auto& [hour, count] : hours) {
        if (count > most) {
            most = count;
            peak = hour;
        }
    }
    return peak;
}

/// The number of the days the sessions span.
size_t days_between(const std::string& first, const std::string& last) {
    if (first.empty() || last.empty()) return 0;

    const int64_t from = parse_stamp(first);
    const int64_t to = parse_stamp(last);
    if (from <= 0 || to <= 0) return 0;

    return static_cast<size_t>(std::ceil(static_cast<double>(to - from) / kDay)) + 1;
}

/// The statistics of a range of days, which are counted from the files
/// themselves.
Stats processed_stats_to_stats(const ProcessedStats& stats) {
    Stats out;

    out.sessions = stats.session_stats.size();
    out.messages = static_cast<size_t>(stats.total_messages);
    out.active_days = stats.daily_activity.size();

    for (const auto& [model, usage] : stats.model_usage) out.model_usage[model] = to_usage(usage);
    for (const auto& day : stats.daily_activity) {
        out.daily_activity[day.date] = static_cast<size_t>(day.message_count);
    }
    for (const auto& day : stats.daily_model_tokens) {
        out.daily_model_tokens[day.date] = day.tokens_by_model;
    }

    // The longest session.
    for (const auto& session : stats.session_stats) {
        out.longest_session_ms = std::max(out.longest_session_ms, session.duration);
    }

    // The first and the last session.
    std::string first;
    std::string last;
    for (const auto& session : stats.session_stats) {
        if (first.empty() || session.timestamp < first) first = session.timestamp;
        if (last.empty() || session.timestamp > last) last = session.timestamp;
    }
    if (!first.empty()) out.first_session_date = to_date_string(parse_stamp(first));
    if (!last.empty()) out.last_session_date = to_date_string(parse_stamp(last));
    out.days = days_between(first, last);

    // The hour the sessions were begun in most often.
    out.peak_hour = peak_hour(stats.hour_counts);

    // The streaks.
    const Streaks streaks = calculate_streaks(stats.daily_activity);
    out.longest_streak = streaks.longest;
    out.current_streak = streaks.current;

    return out;
}

/// The statistics of the whole of the time.
///
/// The days that are over come from the cache, and the day that is going on
/// is counted again, because it is not over yet.
Stats cache_to_stats(const StatsCache& cache, const ProcessedStats& today) {
    // The activity of the days.
    std::map<std::string, DailyActivity> activity;
    for (const auto& day : cache.daily_activity) activity[day.date] = day;
    for (const auto& day : today.daily_activity) {
        const auto it = activity.find(day.date);
        if (it == activity.end()) {
            activity.emplace(day.date, day);
            continue;
        }
        it->second.message_count += day.message_count;
        it->second.session_count += day.session_count;
        it->second.tool_call_count += day.tool_call_count;
    }

    // The tokens of the days.
    std::map<std::string, std::map<std::string, int64_t>> tokens;
    for (const auto& day : cache.daily_model_tokens) tokens[day.date] = day.tokens_by_model;
    for (const auto& day : today.daily_model_tokens) {
        const auto it = tokens.find(day.date);
        if (it == tokens.end()) {
            tokens.emplace(day.date, day.tokens_by_model);
            continue;
        }
        for (const auto& [model, count] : day.tokens_by_model) it->second[model] += count;
    }

    // The usage of the models.
    std::map<std::string, ModelUsage> usage = cache.model_usage;
    for (const auto& [model, model_usage] : today.model_usage) {
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

    // The hours.
    std::map<int, int64_t> hours = cache.hour_counts;
    for (const auto& [hour, count] : today.hour_counts) hours[hour] += count;

    Stats out;

    out.sessions = static_cast<size_t>(cache.total_sessions + today.session_stats.size());
    out.messages = static_cast<size_t>(cache.total_messages + today.total_messages);
    out.active_days = activity.size();

    for (const auto& [model, model_usage] : usage) out.model_usage[model] = to_usage(model_usage);
    for (const auto& [date, day] : activity) {
        out.daily_activity[date] = static_cast<size_t>(day.message_count);
    }
    for (const auto& [date, by_model] : tokens) out.daily_model_tokens[date] = by_model;

    // The longest session.
    std::optional<SessionStats> longest = cache.longest_session;
    for (const auto& session : today.session_stats) {
        if (!longest || session.duration > longest->duration) longest = session;
    }
    if (longest) out.longest_session_ms = longest->duration;

    // The first session: the one that began first, of all of them.
    std::string first = cache.first_session_date;
    for (const auto& session : today.session_stats) {
        if (first.empty() || session.timestamp < first) first = session.timestamp;
    }
    if (!first.empty()) out.first_session_date = to_date_string(parse_stamp(first));

    // The last session: the newest of today's, or the last day that holds a
    // session.
    std::string last;
    for (const auto& session : today.session_stats) {
        if (last.empty() || session.timestamp > last) last = session.timestamp;
    }
    if (last.empty() && !activity.empty()) last = activity.rbegin()->first;
    if (!last.empty()) out.last_session_date = to_date_string(parse_stamp(last));

    // The number of the days the sessions span. The last session is a day
    // when there is none of today, and a day is read as the midnight of UTC.
    int64_t last_ms = 0;
    if (!last.empty()) {
        last_ms = parse_stamp(last);
        if (last_ms <= 0) last_ms = parse_day(last);
    }
    const int64_t first_ms = first.empty() ? 0 : parse_stamp(first);
    if (first_ms > 0 && last_ms > 0) {
        out.days = static_cast<size_t>(std::ceil(static_cast<double>(last_ms - first_ms) / kDay)) + 1;
    }

    // The hour the sessions were begun in most often.
    out.peak_hour = peak_hour(hours);

    // The streaks.
    std::vector<DailyActivity> days;
    days.reserve(activity.size());
    for (const auto& [date, day] : activity) days.push_back(day);
    const Streaks streaks = calculate_streaks(days);
    out.longest_streak = streaks.longest;
    out.current_streak = streaks.current;

    return out;
}

} // namespace

int64_t Stats::total_tokens() const {
    int64_t total = 0;
    for (const auto& [model, usage] : model_usage) {
        total += usage.input_tokens + usage.output_tokens;
    }
    return total;
}

Stats collect_stats(const std::string& cwd, int days) {
    // The statistics are of every project of the machine, the way the
    // statistics of the reference are: the folder the session runs in does
    // not narrow them down.
    (void)cwd;

    const std::vector<std::string> files = all_session_files();
    if (files.empty()) return {};

    if (days > 0) {
        // A range is counted from the files themselves. The days that are
        // older are skipped before the files are read.
        const std::string from_date = to_date_string(now_ms() - (days - 1) * kDay);
        return processed_stats_to_stats(process_session_files(files, from_date, {}));
    }

    // The whole of the time comes from the cache: the days that are over
    // are in it, and the day that is going on is counted again.
    const std::string yesterday = yesterday_date_string();

    StatsCache cache;
    {
        // The cache is written by one thread at a time.
        const std::lock_guard<std::mutex> lock(g_cache_mutex);

        cache = load_stats_cache();

        // The tokens of the days are counted in a way of their own, and the
        // version of the cache says in which way they were counted. A cache
        // of an older version has them counted again, the way the reference
        // counts them again.
        if (cache.daily_model_tokens_version < kDailyModelTokensVersion &&
            !cache.last_computed_date.empty()) {
            // The watermark of the cache is ahead of yesterday when the clock
            // of the machine went back; the days are counted up to
            // yesterday then.
            std::string through = cache.last_computed_date;
            if (through > yesterday) through = yesterday;

            const ProcessedStats rebuilt = process_session_files(files, {}, through);

            // The tokens are only replaced when the days were found: a cache
            // whose transcripts are gone keeps the tokens it has.
            if (!rebuilt.daily_model_tokens.empty()) {
                cache.daily_model_tokens = rebuilt.daily_model_tokens;
            }
            cache.daily_model_tokens_version = kDailyModelTokensVersion;
            save_stats_cache(cache);
        }

        if (cache.last_computed_date.empty()) {
            // There is no cache: everything that is over is counted.
            const ProcessedStats historical = process_session_files(files, {}, yesterday);
            if (!historical.session_stats.empty() || !historical.daily_activity.empty() ||
                !historical.daily_model_tokens.empty()) {
                cache = merge_cache_with_new_stats(cache, historical, yesterday);
                save_stats_cache(cache);
            }
        } else if (cache.last_computed_date < yesterday) {
            // The cache is stale: the days that came after it are counted.
            const std::string next = next_day_string(cache.last_computed_date);
            const ProcessedStats fresh = process_session_files(files, next, yesterday);
            if (!fresh.session_stats.empty() || !fresh.daily_activity.empty() ||
                !fresh.daily_model_tokens.empty()) {
                cache = merge_cache_with_new_stats(cache, fresh, yesterday);
            } else {
                cache.last_computed_date = yesterday;
            }
            save_stats_cache(cache);
        }
    }

    // The day that is going on is counted again every time: it is not over
    // yet, so it is not in the cache.
    const std::string today = today_date_string();
    const ProcessedStats today_stats = process_session_files(files, today, today);
    return cache_to_stats(cache, today_stats);
}

Stats cached_stats() {
    // The numbers of the cache alone: the day that is going on is not in it,
    // because it is not over yet.
    return cache_to_stats(load_stats_cache(), ProcessedStats{});
}

size_t transcripts_read(const std::string& folder) {
    const std::lock_guard<std::mutex> lock(g_reads_mutex);
    const auto it = g_transcripts_read.find(folder);
    return it == g_transcripts_read.end() ? 0 : it->second;
}

void reset_transcripts_read(const std::string& folder) {
    const std::lock_guard<std::mutex> lock(g_reads_mutex);
    g_transcripts_read[folder] = 0;
}

/// The state of a reading that is at hand.
struct BackgroundStats::Impl {
    /// The lock of the numbers.
    mutable std::mutex mutex;

    /// The sign that the reading is over.
    mutable std::condition_variable done;

    /// The numbers that are known.
    Stats stats;

    /// True when the reading is over.
    bool ready = false;

    /// True when the reading has been started.
    bool started = false;
};

BackgroundStats::BackgroundStats() : impl_(std::make_shared<Impl>()) {}

BackgroundStats::~BackgroundStats() = default;

void BackgroundStats::start(const std::string& cwd) {
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->started) return;
        impl_->started = true;

        // The numbers of the cache are there at once: the days that are over
        // are not read again.
        impl_->stats = cached_stats();
    }

    // The day that is going on is read in a thread of its own, so that the
    // panel opens at once. The thread only fills the numbers in; the panel
    // is drawn by the thread that owns the terminal.
    const std::shared_ptr<Impl> impl = impl_;
    std::thread([impl, cwd]() {
        const Stats stats = collect_stats(cwd, 0);
        {
            const std::lock_guard<std::mutex> lock(impl->mutex);
            impl->stats = stats;
            impl->ready = true;
        }
        impl->done.notify_all();
    }).detach();
}

void BackgroundStats::start_range(const std::string& cwd, int days) {
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->started) return;
        impl_->started = true;
    }

    const std::shared_ptr<Impl> impl = impl_;
    std::thread([impl, cwd, days]() {
        const Stats stats = collect_stats(cwd, days);
        {
            const std::lock_guard<std::mutex> lock(impl->mutex);
            impl->stats = stats;
            impl->ready = true;
        }
        impl->done.notify_all();
    }).detach();
}

bool BackgroundStats::ready() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->ready;
}

void BackgroundStats::wait() const {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->done.wait(lock, [this]() { return impl_->ready; });
}

const Stats& BackgroundStats::stats() const {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stats;
}

} // namespace cc::session
