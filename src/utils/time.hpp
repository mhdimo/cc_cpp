#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cc {

/// The current time, in milliseconds since the epoch.
int64_t now_ms();

/// Parses an ISO-8601 timestamp into milliseconds since the epoch.
///
/// The timestamps Claude Code writes look like
/// `2026-01-02T03:04:05.678Z`. Zero is returned when the text is not a
/// timestamp.
int64_t parse_timestamp(std::string_view text);

/// The time between `then_ms` and `now_ms`, the way Claude Code says it.
///
/// The wording is `just now`, `2 minutes ago` or `3 hours ago`.
std::string relative_time_ago(int64_t then_ms, int64_t now_ms);

/// The time a limit starts again, the way Claude Code writes it.
///
/// The wording is `3:45pm (PST)`: the hour has no leading zero, the
/// meridiem is in small letters, and the zone of the machine follows in
/// brackets. A limit that starts again more than a day away carries the
/// date as well — `Mar 5, 3:45pm (PST)` — and the time may be left out.
std::string format_reset_time(int64_t resets_at_ms, bool show_time = true);

/// The day a timestamp falls on, as `YYYY-MM-DD`.
std::string format_day(int64_t timestamp_ms);

/// The hour of the day a timestamp falls on, from 0 to 23.
int hour_of_day(int64_t timestamp_ms);

/// The day a day follows, as `YYYY-MM-DD`.
///
/// The day before the first of a month is the last of the one before it.
std::string next_day(std::string_view day);

/// The day a day comes from, as `YYYY-MM-DD`.
std::string previous_day(std::string_view day);

/// The time a day starts at, in milliseconds since the epoch.
///
/// Zero is returned when the text is not a day.
int64_t parse_day(std::string_view day);

/// The current time, the way an ISO-8601 stamp is written.
///
/// The stamp looks like `2026-01-02T03:04:05Z`; the reference writes the
/// same shape.
std::string format_iso_now();

/// The date of a timestamp, the way the reference writes it.
///
/// The reference shows the date of a marketplace with
/// `toLocaleDateString()`, which writes it the American way: `11/5/2026`.
std::string format_locale_date(int64_t timestamp_ms);

} // namespace cc
