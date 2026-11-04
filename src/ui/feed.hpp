#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cc::ui {

/// One line of a feed of the banner.
struct FeedLine {
    /// The text of the line.
    std::string text;
    /// The time the line is about, e.g. "3h ago".
    ///
    /// The time is empty when the feed has none.
    std::string timestamp;
};

/// A feed of the banner: a title, some lines and a footer.
///
/// The reference shows two of them in the right-hand column of the panel:
/// the recent activity of the folder and the notes of the release.
struct Feed {
    /// The title of the feed.
    std::string title;
    /// The lines of the feed.
    std::vector<FeedLine> lines;
    /// The line under the feed, e.g. "/resume for more".
    ///
    /// The footer is empty when the feed has none.
    std::string footer;
    /// The text shown when the feed has no lines.
    std::string empty_message;
};

/// The feed of the sessions of the folder that were touched last.
///
/// The three newest sessions are shown. A session without a title is left
/// out, the way the reference leaves out a session without a prompt.
Feed recent_activity_feed(const std::string& cwd);

/// The feed of the notes of the release.
///
/// The notes are the ones of the versions that came after the version the
/// user has seen. `seen_version` is set to the newest of them, so that the
/// caller can remember it and the notes are shown only once.
Feed whats_new_feed(std::string* seen_version = nullptr);

/// The time a session was touched, the way the reference writes it in the
/// feed of the recent activity.
///
/// The wording is "3h ago", "2d ago" or "5mo ago" — the short form the
/// reference uses there.
std::string relative_time_short(int64_t then_ms, int64_t now_ms);

} // namespace cc::ui
