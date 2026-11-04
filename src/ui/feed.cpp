#include "ui/feed.hpp"

#include "config/global_config.hpp"
#include "release_notes.hpp"
#include "session/transcript.hpp"
#include "utils/time.hpp"

#include <algorithm>
#include <string>

namespace cc::ui {
namespace {

/// The number of sessions the feed of the recent activity shows.
constexpr size_t kRecentActivityLines = 3;

/// The number of notes the feed of the release shows.
///
/// The reference asks for the notes of the newest three releases, and shows
/// three notes of them at most.
constexpr size_t kWhatsNewLines = 3;

/// A unit of time, and the letter the reference writes it with.
struct TimeUnit {
    /// The number of seconds the unit lasts.
    int64_t seconds;
    /// The letter of the unit, e.g. "h" for the hours.
    const char* suffix;
};

/// The units the reference measures the time in, the longest first.
constexpr TimeUnit kTimeUnits[] = {
    {31536000, "y"}, {2592000, "mo"}, {604800, "w"}, {86400, "d"}, {3600, "h"}, {60, "m"}, {1, "s"},
};

} // namespace

std::string relative_time_short(int64_t then_ms, int64_t now_ms) {
    if (then_ms <= 0) return {};

    // The reference cuts the difference towards zero, so that a moment that
    // has not passed yet is not shown as one that has.
    const int64_t seconds = (then_ms - now_ms) / 1000;
    const int64_t elapsed = seconds < 0 ? -seconds : seconds;

    for (const auto& unit : kTimeUnits) {
        if (elapsed < unit.seconds) continue;
        const int64_t value = seconds / unit.seconds;
        if (seconds < 0) return std::to_string(-value) + unit.suffix + " ago";
        return "in " + std::to_string(value) + unit.suffix;
    }

    // Less than a second has passed.
    return seconds <= 0 ? "0s ago" : "in 0s";
}

Feed recent_activity_feed(const std::string& cwd) {
    Feed feed;
    feed.title = "Recent activity";
    feed.empty_message = "No recent activity";

    const int64_t now = cc::now_ms();
    for (const auto& session : session::Transcript::list_sessions(cwd)) {
        if (feed.lines.size() >= kRecentActivityLines) break;
        // A session without a title has nothing to say.
        if (session.title.empty()) continue;
        feed.lines.push_back(
            FeedLine{session.title, relative_time_short(session.modified_ms, now)});
    }

    // The reference says where the rest of the sessions are when there are
    // any.
    if (!feed.lines.empty()) feed.footer = "/resume for more";
    return feed;
}

Feed whats_new_feed(std::string* seen_version) {
    Feed feed;
    feed.title = "What's new";
    feed.empty_message = "Check the Claude Code changelog for updates";

    const std::string previous = config::global_last_release_notes_seen();

    // The changelog that came with the program is the one of the port, and
    // it is the one the notes are taken from when it is there. The changelog
    // that was fetched is the one of the reference, and it is the one the
    // notes are taken from when the port brings none.
    std::string content = cc::bundled_changelog();
    if (content.empty()) content = cc::stored_changelog();

    const auto releases = cc::parse_changelog(content);

    // The version that was seen is only of use when it is one of the
    // versions of the changelog that is read: the versions of the reference
    // and the versions of the port are numbered differently.
    std::string seen = previous;
    const bool known = std::any_of(releases.begin(), releases.end(),
                                   [&previous](const auto& entry) {
                                       return entry.first == previous;
                                   });
    if (!known) seen.clear();

    for (const auto& note : cc::recent_release_notes(seen, content)) {
        feed.lines.push_back(FeedLine{note, {}});
        // The feed of the banner holds three notes at most: the reference
        // shows five of them in the notes of the release, but the column of
        // the banner is shorter.
        if (feed.lines.size() >= kWhatsNewLines) break;
    }

    if (!feed.lines.empty()) feed.footer = "/release-notes for more";

    // The version of the newest release is remembered, so that the notes
    // are shown once.
    if (seen_version != nullptr) {
        if (!releases.empty()) *seen_version = releases.front().first;
    }
    return feed;
}

} // namespace cc::ui
