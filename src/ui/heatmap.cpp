#include "ui/heatmap.hpp"

#include "utils/time.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace cc::ui {
namespace {

/// The quarters of the numbers of the messages.
///
/// The intensity of a day is decided by them: a day with more messages than
/// the third quarter is full, a day with fewer than the first is faint.
struct Percentiles {
    size_t p25 = 0;
    size_t p50 = 0;
    size_t p75 = 0;
};

/// The quarters of the days that have messages.
Percentiles calculate_percentiles(const std::map<std::string, size_t>& activity) {
    std::vector<size_t> counts;
    for (const auto& [day, count] : activity) {
        if (count > 0) counts.push_back(count);
    }
    if (counts.empty()) return {};

    std::sort(counts.begin(), counts.end());
    return {counts[counts.size() * 25 / 100], counts[counts.size() * 50 / 100],
            counts[counts.size() * 75 / 100]};
}

/// The intensity of a day, from 0 (none) to 4 (full).
int intensity_of(size_t count, const Percentiles& percentiles) {
    if (count == 0 || percentiles.p25 == 0) return 0;
    if (count >= percentiles.p75) return 4;
    if (count >= percentiles.p50) return 3;
    if (count >= percentiles.p25) return 2;
    return 1;
}

/// The block of an intensity.
const char* block_of(int intensity) {
    switch (intensity) {
    case 1:
        return "░";
    case 2:
        return "▒";
    case 3:
        return "▓";
    case 4:
    default:
        return "█";
    }
}

/// The names of the months, the way the reference writes them.
const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/// The names of the days of the week.
const char* const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

/// The number of the month a day falls in, from 1 to 12.
int month_of(std::string_view day) {
    if (day.size() < 7) return 1;
    return std::atoi(std::string(day.substr(5, 2)).c_str());
}

/// The day of the week a day falls on, from 0 (Sunday) to 6.
int weekday_of(std::string_view day) {
    const int64_t stamp = cc::parse_day(day);
    if (stamp <= 0) return 0;
    // The epoch fell on a Thursday.
    return static_cast<int>(((stamp / (24 * 60 * 60 * 1000)) + 4) % 7);
}

} // namespace

std::vector<std::string> generate_heatmap(const std::map<std::string, size_t>& daily_activity,
                                          const Theme& theme, int width) {
    // The days of the week take four columns, and the grid holds a year at
    // most.
    const int columns = std::min(52, std::max(10, width - 4));

    const Percentiles percentiles = calculate_percentiles(daily_activity);

    // The grid ends at today and reaches back a year.
    const std::string today = cc::format_day(cc::now_ms());
    std::string start = today;
    for (int i = 0; i < weekday_of(today); ++i) start = cc::previous_day(start);
    for (int week = 1; week < columns; ++week) {
        for (int day = 0; day < 7; ++day) start = cc::previous_day(start);
    }

    // The grid: a row per day of the week, a column per week.
    std::vector<std::vector<std::string>> grid(7, std::vector<std::string>(columns));
    std::vector<int> month_starts;

    std::string current = start;
    for (int week = 0; week < columns; ++week) {
        for (int day = 0; day < 7; ++day) {
            const bool future = current > today;
            const auto found = daily_activity.find(current);
            const size_t count = future || found == daily_activity.end() ? 0 : found->second;
            const int intensity = intensity_of(count, percentiles);

            Style style;
            if (intensity == 0) {
                style.foreground = theme.inactive;
                grid[day][week] = styled("·", style);
            } else {
                // The blocks of the heatmap are the orange of Claude.
                style.foreground = Rgb{218, 119, 86};
                grid[day][week] = styled(block_of(intensity), style);
            }

            if (day == 0) {
                const int month = month_of(current);
                if (month_starts.empty() || month_starts.back() != month) {
                    month_starts.push_back(month);
                }
            }

            current = cc::next_day(current);
        }
    }

    std::vector<std::string> lines;

    // The names of the months, spread over the grid.
    {
        const size_t label_width = static_cast<size_t>(columns) / std::max<size_t>(month_starts.size(), 1);
        std::string labels = "    ";
        for (int month : month_starts) {
            std::string label = kMonths[month - 1];
            while (label.size() < label_width) label += ' ';
            labels += label;
        }
        lines.push_back(labels);
    }

    // The grid, with the names of the days on the left.
    for (int day = 0; day < 7; ++day) {
        std::string line;
        if (day == 1 || day == 3 || day == 5) {
            // The name of the day is three columns wide, so that the grid
            // of every row starts at the same column.
            std::string label(kDays[day]);
            while (label.size() < 3) label += ' ';
            line = label + ' ';
        } else {
            line = "    ";
        }
        for (int week = 0; week < columns; ++week) {
            line += grid[day][week];
        }
        lines.push_back(line);
    }

    // The legend.
    Style orange;
    orange.foreground = Rgb{218, 119, 86};
    lines.push_back("");
    lines.push_back("    Less " + styled("░", orange) + " " + styled("▒", orange) + " " +
                    styled("▓", orange) + " " + styled("█", orange) + " More");

    return lines;
}

} // namespace cc::ui
