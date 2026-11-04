#pragma once

#include "core/message.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cc::session {

/// The statistics of the sessions of the machine.
///
/// The reference collects them from the transcripts of the sessions, and
/// shows them in the panel of `/stats`.
struct Stats {
    /// The number of the sessions.
    size_t sessions = 0;

    /// The number of the messages of the sessions.
    ///
    /// The messages of the sub-agents are not counted: they are not
    /// sessions of their own.
    size_t messages = 0;

    /// The number of the days the sessions span.
    size_t days = 0;

    /// The number of the days a session was run on.
    size_t active_days = 0;

    /// The tokens of every model, by the name the model was known by.
    std::map<std::string, Usage> model_usage;

    /// The number of the messages of every day, by the day they were written
    /// on.
    std::map<std::string, size_t> daily_activity;

    /// The tokens of every day, by the model that used them.
    ///
    /// The chart of the panel of the models is drawn from them, the way the
    /// reference draws it.
    std::map<std::string, std::map<std::string, int64_t>> daily_model_tokens;

    /// The length of the longest session, in milliseconds.
    int64_t longest_session_ms = 0;

    /// The day the first session was run on.
    std::string first_session_date;

    /// The day the last session was run on.
    std::string last_session_date;

    /// The hour of the day the most sessions were run in.
    int peak_hour = -1;

    /// The number of the days of the longest streak of days with a session.
    size_t longest_streak = 0;

    /// The number of the days of the streak that is going on.
    size_t current_streak = 0;

    /// The number of the tokens of every model, added together.
    int64_t total_tokens() const;
};

/// Collects the statistics of the sessions of `cwd`.
///
/// The statistics are of every project of the machine, the way the
/// statistics of the reference are: the folder the session runs in does not
/// narrow them down.
///
/// When `days` is not zero, only the sessions of the last `days` days are
/// counted; the reference offers the three ranges the same way.
Stats collect_stats(const std::string& cwd, int days = 0);

/// The statistics that the cache holds, without reading a transcript.
///
/// The cache holds every day but the one that is going on: the day that is
/// going on is not over yet, so it is not in the cache. The numbers are
/// therefore those of yesterday, and the panel shows them while the day that
/// is going on is read in the background.
Stats cached_stats();

/// The number of transcripts that were read under `folder`.
///
/// The counter is what the tests use to show that the days that are over are
/// not read a second time: only the transcripts of the day that is going on
/// are read. The folder is the one of the user, so that the readings of one
/// test are not counted in the readings of another.
size_t transcripts_read(const std::string& folder);

/// Sets the counter of the transcripts that were read under `folder` back to
/// zero.
void reset_transcripts_read(const std::string& folder);

/// The statistics of the sessions, read in the background.
///
/// The transcripts are read in a thread of their own, so that the panel opens
/// at once: the numbers of the cache are shown while the day that is going
/// on is read. The thread only fills the numbers in — the panel is drawn by
/// the thread that owns the terminal.
class BackgroundStats {
public:
    BackgroundStats();
    ~BackgroundStats();

    BackgroundStats(const BackgroundStats&) = delete;
    BackgroundStats& operator=(const BackgroundStats&) = delete;

    /// Starts the reading of the statistics of every day.
    ///
    /// The numbers of the cache are there at once; the numbers of the day
    /// that is going on are added to them when the reading is over.
    void start(const std::string& cwd);

    /// Starts the reading of the statistics of the last `days` days.
    ///
    /// The reading is done only once: asking again while it is at hand does
    /// nothing.
    void start_range(const std::string& cwd, int days);

    /// True when the numbers that were read are there.
    bool ready() const;

    /// Waits until the numbers that were read are there.
    void wait() const;

    /// The numbers that were read.
    ///
    /// The numbers of the cache are returned while the reading is at hand.
    const Stats& stats() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cc::session
