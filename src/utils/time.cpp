#include "utils/time.hpp"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace cc {
namespace {

/// The number of milliseconds in a second.
constexpr int64_t kSecond = 1000;

/// The number of milliseconds in a minute.
constexpr int64_t kMinute = 60 * kSecond;

/// The number of milliseconds in an hour.
constexpr int64_t kHour = 60 * kMinute;

/// The number of milliseconds in a day.
constexpr int64_t kDay = 24 * kHour;

/// The number of milliseconds in a week.
constexpr int64_t kWeek = 7 * kDay;

/// The number of milliseconds in a month, which the reference takes as 30
/// days.
constexpr int64_t kMonth = 30 * kDay;

/// The number of milliseconds in a year, which the reference takes as 365
/// days.
constexpr int64_t kYear = 365 * kDay;

/// The number of milliseconds after which the time is said in seconds.
constexpr int64_t kSecondsThreshold = 10 * kSecond;

/// The number of milliseconds after which the time is said in minutes.
constexpr int64_t kMinutesThreshold = 60 * kSecond;

/// The number of milliseconds after which the time is said in hours.
constexpr int64_t kHoursThreshold = 60 * kMinute;

/// The number of milliseconds after which the time is said in days.
constexpr int64_t kDaysThreshold = 24 * kHour;

/// The number of milliseconds after which the time is said in weeks.
constexpr int64_t kWeeksThreshold = 7 * kDay;

/// The number of milliseconds after which the time is said in months.
constexpr int64_t kMonthsThreshold = 30 * kDay;

/// The number of milliseconds after which the time is said in years.
constexpr int64_t kYearsThreshold = 365 * kDay;

/// The number of `units` and the name of the unit.
///
/// The name is the singular for one unit and the plural for the rest.
std::string plural(int64_t units, const char* unit) {
    std::string name(unit);
    if (units != 1) name += "s";
    return std::to_string(units) + " " + name;
}

} // namespace

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t parse_timestamp(std::string_view text) {
    // The timestamps look like `2026-01-02T03:04:05.678Z`.
    if (text.size() < 20) return 0;

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;

    const int matched = std::sscanf(text.data(), "%4d-%2d-%2dT%2d:%2d:%2d.%3dZ", &year, &month,
                                    &day, &hour, &minute, &second, &millisecond);
    if (matched != 7) return 0;

    // The days since the epoch are counted from the civil calendar, so the
    // leap years are worked out here.
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        days += leap ? 366 : 365;
    }
    static constexpr int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int m = 1; m < month; ++m) {
        days += kDaysInMonth[m - 1];
        if (m == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) ++days;
    }
    days += day - 1;

    return days * kDay + hour * kHour + minute * kMinute + second * kSecond + millisecond;
}

std::string relative_time_ago(int64_t then_ms, int64_t now_ms) {
    if (then_ms <= 0) return {};

    const int64_t elapsed = now_ms - then_ms;
    if (elapsed < 0) return "just now";
    if (elapsed < kSecondsThreshold) return "just now";
    if (elapsed < kMinutesThreshold) return plural(elapsed / kSecond, "second") + " ago";
    if (elapsed < kHoursThreshold) return plural(elapsed / kMinute, "minute") + " ago";
    if (elapsed < kDaysThreshold) return plural(elapsed / kHour, "hour") + " ago";
    if (elapsed < kWeeksThreshold) return plural(elapsed / kDay, "day") + " ago";
    if (elapsed < kMonthsThreshold) return plural(elapsed / kWeek, "week") + " ago";
    if (elapsed < kYearsThreshold) return plural(elapsed / kMonth, "month") + " ago";
    return plural(elapsed / kYear, "year") + " ago";
}

namespace {

/// The clock of a moment, the way the reference writes it.
///
/// The hour has no leading zero, the minutes are left out when they are
/// zero, and the meridiem is in small letters.
std::string format_clock(const std::tm& local, bool with_minutes) {
    char clock[32] = {};
    if (with_minutes) {
        std::strftime(clock, sizeof(clock), "%I:%M%p", &local);
    } else {
        std::strftime(clock, sizeof(clock), "%I%p", &local);
    }

    std::string text(clock);
    if (text.size() > 2 && text[0] == '0') text.erase(0, 1);
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

/// The name of the zone of the terminal, the way the reference names it.
///
/// The reference writes the name of the zone of the standard of the IANA in
/// the brackets of the time a limit starts again: `America/Los_Angeles`.
/// The abbreviation of the zone (`PDT`) is not its name, so it is only used
/// when the name of the zone cannot be found.
std::string zone_name(const std::tm& local) {
    // The variable of the environment names the zone, when it names one.
    if (const char* zone = std::getenv("TZ")) {
        std::string name(zone);
        if (!name.empty() && name[0] == ':') name.erase(0, 1);
        if (name.find('/') != std::string::npos) return name;
    }

#if !defined(_WIN32)
    // The link of the local time of the system names the zone as well.
    char buffer[256] = {};
    const ssize_t length = ::readlink("/etc/localtime", buffer, sizeof(buffer) - 1);
    if (length > 0) {
        buffer[length] = '\0';
        const std::string link(buffer);
        const size_t at = link.find("zoneinfo/");
        if (at != std::string::npos) return link.substr(at + 9);
    }
#endif

    // The abbreviation of the zone is the last resort.
    char zone[32] = {};
    std::strftime(zone, sizeof(zone), "%Z", &local);
    if (zone[0] == '\0') return {};
    return zone;
}

/// The name of the zone of the terminal, in the brackets the reference uses.
std::string format_zone(const std::tm& local) {
    const std::string name = zone_name(local);
    if (name.empty()) return {};
    return " (" + name + ")";
}

} // namespace

std::string format_reset_time(int64_t resets_at_ms, bool show_time) {
    if (resets_at_ms <= 0) return {};

    const std::time_t seconds = static_cast<std::time_t>(resets_at_ms / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    // The time the terminal is at, so that the moments that are compared are
    // both local.
    const std::time_t now_seconds = static_cast<std::time_t>(now_ms() / 1000);
    std::tm now_local{};
#if defined(_WIN32)
    localtime_s(&now_local, &now_seconds);
#else
    localtime_r(&now_seconds, &now_local);
#endif

    const double hours_until_reset =
        static_cast<double>(resets_at_ms - now_ms()) / (1000.0 * 60.0 * 60.0);
    const std::string zone = format_zone(local);

    // A reset that is more than a day away carries the date as well.
    if (hours_until_reset > 24) {
        char month[16] = {};
        std::strftime(month, sizeof(month), "%b", &local);

        std::string text = std::string(month) + " " + std::to_string(local.tm_mday);
        if (local.tm_year != now_local.tm_year) {
            text += ", " + std::to_string(local.tm_year + 1900);
        }
        if (show_time) {
            text += ", " + format_clock(local, local.tm_min != 0);
        }
        return text + zone;
    }

    // A reset that is less than a day away is told in its time alone: the
    // flag of the reference only touches the branch of the resets that are
    // more than a day away.
    return format_clock(local, local.tm_min != 0) + zone;
}

std::string format_day(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) return {};

    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    char day[32] = {};
    std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
    return std::string(day);
}

int hour_of_day(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) return -1;

    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    return local.tm_hour;
}

/// The parts of a day, as the numbers they are written with.
bool day_parts(std::string_view day, int& year, int& month, int& date) {
    if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;

    year = std::atoi(std::string(day.substr(0, 4)).c_str());
    month = std::atoi(std::string(day.substr(5, 2)).c_str());
    date = std::atoi(std::string(day.substr(8, 2)).c_str());
    return month >= 1 && month <= 12 && date >= 1 && date <= 31;
}

/// The number of the days of the month.
int days_in_month(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return kDays[month - 1];
}

/// Writes a day as `YYYY-MM-DD`.
std::string write_day(int year, int month, int date) {
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%04d-%02d-%02d", year, month, date);
    return std::string(text);
}

std::string next_day(std::string_view day) {
    int year = 0;
    int month = 0;
    int date = 0;
    if (!day_parts(day, year, month, date)) return {};

    ++date;
    if (date > days_in_month(year, month)) {
        date = 1;
        ++month;
        if (month > 12) {
            month = 1;
            ++year;
        }
    }
    return write_day(year, month, date);
}

std::string previous_day(std::string_view day) {
    int year = 0;
    int month = 0;
    int date = 0;
    if (!day_parts(day, year, month, date)) return {};

    --date;
    if (date < 1) {
        --month;
        if (month < 1) {
            month = 12;
            --year;
        }
        date = days_in_month(year, month);
    }
    return write_day(year, month, date);
}

int64_t parse_day(std::string_view day) {
    int year = 0;
    int month = 0;
    int date = 0;
    if (!day_parts(day, year, month, date)) return 0;

    // The days of the years before the one of the day, and then the days of
    // the months of the year.
    int64_t days = 0;
    for (int y = 1970; y < year; ++y) {
        days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;
    }
    for (int m = 1; m < month; ++m) days += days_in_month(year, m);
    days += date - 1;

    return days * 24 * 60 * 60 * 1000;
}

std::string format_iso_now() {
    const std::time_t seconds = static_cast<std::time_t>(now_ms() / 1000);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif

    char stamp[32] = {};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(stamp);
}

std::string format_locale_date(int64_t timestamp_ms) {
    if (timestamp_ms <= 0) return {};

    const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    // The American way of writing a date: the month, the day of the month
    // and the year.
    return std::to_string(local.tm_mon + 1) + "/" + std::to_string(local.tm_mday) + "/" +
           std::to_string(local.tm_year + 1900);
}

} // namespace cc
