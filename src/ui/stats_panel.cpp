#include "ui/stats_panel.hpp"

#include "commands/registry.hpp"
#include "ui/figures.hpp"
#include "ui/heatmap.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace cc::ui {
namespace {

/// The number of the tokens, the way the reference writes them.
///
/// The reference writes the numbers in the short form: 1321 becomes `1.3k`
/// and 900 stays `900`. The zero of a whole number is kept: 12000 is
/// `12.0k`.
std::string number(int64_t value) {
    if (value < 1000) return std::to_string(value);

    static const char* kUnits[] = {"k", "m", "b", "t"};
    const size_t kUnitCount = sizeof(kUnits) / sizeof(kUnits[0]);

    double scaled = static_cast<double>(value);
    size_t unit = 0;
    while (scaled >= 1000.0 && unit + 1 < kUnitCount) {
        scaled /= 1000.0;
        ++unit;
    }

    // The rounding may push the value into the next unit: 999999 is `1.0m`.
    if (unit + 1 < kUnitCount && std::round(scaled * 10.0) / 10.0 >= 1000.0) {
        scaled /= 1000.0;
        ++unit;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << scaled;
    return out.str() + kUnits[unit - 1];
}

/// The day of the month of a day, the way the reference writes it.
///
/// The day comes as `2026-03-05` and is written `Mar 5`.
std::string peak_day(const std::string& day) {
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (day.size() < 10) return day;
    const int month = std::atoi(day.substr(5, 2).c_str());
    const int number = std::atoi(day.substr(8, 2).c_str());
    if (month < 1 || month > 12) return day;
    return std::string(kMonths[month - 1]) + " " + std::to_string(number);
}

/// The number of the columns the axis of the chart takes.
///
/// The reference leaves six columns for the label of the values and one for
/// the axis itself.
constexpr int kChartAxisWidth = 7;

/// The number of the days the chart shows at the most.
///
/// The chart is as wide as a year of days, the way the heatmap is.
constexpr int kChartMaxDays = 52;

/// The number of the days the chart shows at the least.
constexpr int kChartMinDays = 20;

/// The number of the rows of the chart.
constexpr int kChartHeight = 8;

/// A model of the statistics of the session.
struct Model {
    /// The name the model was known by.
    std::string name;

    /// The tokens that were sent to the model.
    int64_t input = 0;

    /// The tokens the model answered with.
    int64_t output = 0;

    /// The number of the tokens of the model, the input and the output
    /// together.
    int64_t tokens() const { return input + output; }
};

/// A day of the chart of the tokens.
struct ChartDay {
    /// The day, as `YYYY-MM-DD`.
    std::string date;

    /// The tokens of every model.
    std::map<std::string, int64_t> tokens_by_model;
};

/// One line of the chart of the tokens.
struct ChartSeries {
    /// The tokens of every day of the chart.
    std::vector<double> values;

    /// The colour of the line.
    Rgb colour;
};

/// The chart of the tokens of the models.
struct Chart {
    /// The lines of the chart itself.
    std::vector<std::string> lines;

    /// The labels of the days under the chart.
    std::string x_axis;

    /// The legend of the colours of the chart.
    std::string legend;
};

/// Rounds a number the way JavaScript rounds it.
double js_round(double value) {
    return std::floor(value + 0.5);
}

/// A number with the given number of decimals, the way JavaScript writes it.
///
/// The digits are rounded away from zero, the way `toFixed` rounds them.
std::string to_fixed(double value, int digits) {
    const double factor = std::pow(10.0, digits);
    const double rounded = std::round(value * factor) / factor;

    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << rounded;
    return out.str();
}

/// The label of a value of the axis of the chart.
///
/// The reference writes the thousands with a `k` and the millions with an
/// `M`, and the label is six columns wide.
std::string chart_label(double value) {
    std::string label;
    if (value >= 1000000.0) {
        label = to_fixed(value / 1000000.0, 1) + "M";
    } else if (value >= 1000.0) {
        label = to_fixed(value / 1000.0, 0) + "k";
    } else {
        label = to_fixed(value, 0);
    }
    if (label.size() < 6) label.insert(label.begin(), 6 - label.size(), ' ');
    return label;
}

/// The chart of the tokens of the models.
///
/// The chart is drawn the way the `asciichart` package of the reference
/// draws it: the values are on the left, and the line of every model is
/// drawn with the characters of the box.
std::vector<std::string> draw_chart(const std::vector<ChartSeries>& series) {
    double lowest = series[0].values[0];
    double highest = series[0].values[0];
    for (const ChartSeries& line : series) {
        for (double value : line.values) {
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
        }
    }

    const double range = std::abs(highest - lowest);
    const int offset = 3;
    const double ratio = range != 0 ? static_cast<double>(kChartHeight) / range : 1.0;
    const long long bottom = static_cast<long long>(js_round(lowest * ratio));
    const long long top = static_cast<long long>(js_round(highest * ratio));
    const long long rows = std::abs(top - bottom);

    size_t width = 0;
    for (const ChartSeries& line : series) width = std::max(width, line.values.size());
    width += static_cast<size_t>(offset);

    // Every cell of the chart holds one character; the cell of the label of
    // the axis holds the whole label, the way the library of the reference
    // holds it.
    std::vector<std::vector<std::string>> grid(static_cast<size_t>(rows + 1),
                                                std::vector<std::string>(width, " "));

    for (long long y = bottom; y <= top; ++y) {
        const double value = rows > 0 ? highest - static_cast<double>(y - bottom) * range /
                                                         static_cast<double>(rows)
                                      : static_cast<double>(y);
        const std::string label = chart_label(value);
        const size_t column = static_cast<size_t>(
            std::max<long long>(offset - static_cast<long long>(label.size()), 0));
        grid[static_cast<size_t>(y - bottom)][column] = label;
        grid[static_cast<size_t>(y - bottom)][static_cast<size_t>(offset - 1)] = (y == 0) ? "┼" : "┤";
    }

    for (const ChartSeries& line : series) {
        const Style style = Style::fg(line.colour);

        const long long first =
            static_cast<long long>(js_round(line.values[0] * ratio)) - bottom;
        grid[static_cast<size_t>(rows - first)][static_cast<size_t>(offset - 1)] = styled("┼", style);

        for (size_t x = 0; x + 1 < line.values.size(); ++x) {
            const long long from = static_cast<long long>(js_round(line.values[x] * ratio)) - bottom;
            const long long to =
                static_cast<long long>(js_round(line.values[x + 1] * ratio)) - bottom;
            const size_t column = x + static_cast<size_t>(offset);

            if (from == to) {
                grid[static_cast<size_t>(rows - from)][column] = styled("─", style);
                continue;
            }

            grid[static_cast<size_t>(rows - to)][column] = styled(from > to ? "╰" : "╭", style);
            grid[static_cast<size_t>(rows - from)][column] = styled(from > to ? "╮" : "╯", style);
            for (long long step = std::min(from, to) + 1; step < std::max(from, to); ++step) {
                grid[static_cast<size_t>(rows - step)][column] = styled("│", style);
            }
        }
    }

    std::vector<std::string> out;
    out.reserve(grid.size());
    for (const std::vector<std::string>& row : grid) {
        std::string line;
        for (const std::string& cell : row) line += cell;
        out.push_back(line);
    }
    return out;
}

/// The labels of the days under the chart.
///
/// The reference writes three or four of them, spread over the width of the
/// chart.
std::string chart_x_axis(const std::vector<ChartDay>& days) {
    if (days.empty()) return {};

    const size_t labels = std::min<size_t>(4, std::max<size_t>(2, days.size() / 8));
    const long long usable = static_cast<long long>(days.size()) - 6;
    // The step is kept at one when the days are too few for the labels to
    // be spread over them.
    const long long step =
        std::max<long long>(1, usable / static_cast<long long>(labels - 1));

    std::string out(static_cast<size_t>(kChartAxisWidth), ' ');
    long long current = 0;
    for (size_t i = 0; i < labels; ++i) {
        const long long index =
            std::min(static_cast<long long>(i) * step, static_cast<long long>(days.size()) - 1);
        const std::string label = peak_day(days[static_cast<size_t>(index)].date);
        const long long spaces = std::max<long long>(1, index - current);
        out.append(static_cast<size_t>(spaces), ' ');
        out += label;
        current = index + static_cast<long long>(label.size());
    }
    return out;
}

/// The chart of the tokens of every day.
///
/// The chart shows the three models that used the most tokens, the way the
/// reference shows them.
Chart generate_chart(const std::map<std::string, std::map<std::string, int64_t>>& daily_tokens,
                     const std::vector<Model>& models,
                     int width,
                     const Theme& theme) {
    Chart chart;
    if (daily_tokens.size() < 2 || models.empty()) return chart;

    // The days of the chart.
    std::vector<ChartDay> days;
    days.reserve(daily_tokens.size());
    for (const auto& [date, by_model] : daily_tokens) days.push_back({date, by_model});

    // The chart is as wide as the terminal allows, but never wider than a
    // year of days.
    const int available = width - kChartAxisWidth;
    const size_t columns =
        static_cast<size_t>(std::min(kChartMaxDays, std::max(kChartMinDays, available)));

    // More days than the chart holds: the newest ones are kept. Fewer days
    // than the chart holds: every day is repeated, so that the chart is
    // filled.
    std::vector<ChartDay> recent;
    if (days.size() >= columns) {
        recent.assign(days.end() - static_cast<std::ptrdiff_t>(columns), days.end());
    } else {
        const size_t repeat = columns / days.size();
        for (const ChartDay& day : days) {
            for (size_t i = 0; i < repeat; ++i) recent.push_back(day);
        }
    }

    // The colours of the lines of the chart.
    const std::vector<Rgb> colours = {theme.suggestion, theme.success, theme.warning};

    std::vector<ChartSeries> series;
    for (size_t i = 0; i < models.size() && i < colours.size(); ++i) {
        std::vector<double> values;
        values.reserve(recent.size());
        bool any = false;
        for (const ChartDay& day : recent) {
            const auto it = day.tokens_by_model.find(models[i].name);
            const double value =
                it == day.tokens_by_model.end() ? 0.0 : static_cast<double>(it->second);
            if (value > 0) any = true;
            values.push_back(value);
        }
        // A model that used no token in the days of the chart is left out.
        if (!any) continue;

        series.push_back({values, colours[series.size()]});

        if (!chart.legend.empty()) chart.legend += " · ";
        chart.legend +=
            styled("•", Style::fg(colours[i])) + " " + commands::render_model_name(models[i].name);
    }
    if (series.empty()) return chart;

    chart.lines = draw_chart(series);
    chart.x_axis = chart_x_axis(recent);
    return chart;
}

/// The line of the ranges of the days.
///
/// The reference offers the three ranges, with the one that is shown in
/// bold. The ranges are in the order the reference cycles through them. The
/// spinner is shown next to them, one column away, while the numbers of the
/// range are read.
std::string range_line(const Theme& theme, int chosen, const std::string& spinner) {
    static const char* const kLabels[] = {"All time", "Last 7 days", "Last 30 days"};

    std::string line;
    for (int i = 0; i < 3; ++i) {
        if (!line.empty()) line += " · ";
        if (i == chosen) {
            Style style;
            style.foreground = theme.claude;
            style.bold = true;
            line += styled(kLabels[i], style);
        } else {
            Style dim;
            dim.foreground = theme.inactive;
            line += styled(kLabels[i], dim);
        }
    }

    if (!spinner.empty()) {
        Style style;
        style.foreground = theme.text;
        line += " " + styled(spinner, style);
    }

    return line;
}

/// A book and the number of the tokens it is worth.
struct Book {
    /// The name of the book.
    const char* name;
    /// The number of the tokens the book is worth.
    size_t tokens;
};

/// The books the statistics are compared with, the way the reference does.
const Book kBooks[] = {
    {"The Little Prince", 22000},
    {"The Old Man and the Sea", 35000},
    {"A Christmas Carol", 37000},
    {"Animal Farm", 39000},
    {"Fahrenheit 451", 60000},
    {"The Great Gatsby", 62000},
    {"Slaughterhouse-Five", 64000},
    {"Brave New World", 83000},
    {"The Catcher in the Rye", 95000},
    {"Harry Potter and the Philosopher's Stone", 103000},
    {"The Hobbit", 123000},
    {"1984", 123000},
    {"To Kill a Mockingbird", 130000},
    {"Pride and Prejudice", 156000},
    {"Dune", 244000},
    {"Moby-Dick", 268000},
    {"Crime and Punishment", 274000},
    {"A Game of Thrones", 381000},
    {"Anna Karenina", 468000},
    {"Don Quixote", 520000},
    {"The Lord of the Rings", 576000},
    {"The Count of Monte Cristo", 603000},
    {"Les Misérables", 689000},
    {"War and Peace", 730000},
};

/// Something the length of a session is compared with.
struct Duration {
    /// The name of it.
    const char* name;
    /// The number of the minutes it takes.
    double minutes;
};

/// The lengths the sessions are compared with, the way the reference does.
const Duration kDurations[] = {
    {"a TED talk", 18},
    {"an episode of The Office", 22},
    {"listening to Abbey Road", 47},
    {"a yoga class", 60},
    {"a World Cup soccer match", 90},
    {"a half marathon (average time)", 120},
    {"the movie Inception", 148},
    {"watching Titanic", 195},
    {"a transatlantic flight", 420},
    {"a full night of sleep", 480},
};

/// The remark the statistics end with.
///
/// The reference compares the tokens with the length of some books and the
/// length of the longest session with the time some things take, and picks
/// one of the remarks at random.
std::string fun_factoid(const session::Stats& stats, int64_t total_tokens) {
    std::vector<std::string> remarks;

    if (total_tokens > 0) {
        for (const Book& book : kBooks) {
            if (static_cast<size_t>(total_tokens) < book.tokens) continue;
            const double times = static_cast<double>(total_tokens) / static_cast<double>(book.tokens);
            if (times >= 2) {
                remarks.push_back("You've used ~" + std::to_string(static_cast<int>(times)) +
                                  "x more tokens than " + book.name);
            } else {
                remarks.push_back(std::string("You've used the same number of tokens as ") +
                                  book.name);
            }
        }
    }

    if (stats.longest_session_ms > 0) {
        const double minutes = static_cast<double>(stats.longest_session_ms) / (1000 * 60);
        for (const Duration& duration : kDurations) {
            const double ratio = minutes / duration.minutes;
            if (ratio >= 2) {
                remarks.push_back("Your longest session is ~" +
                                  std::to_string(static_cast<int>(ratio)) + "x longer than " +
                                  duration.name);
            }
        }
    }

    if (remarks.empty()) return {};

    // The remark is picked at random, the way the reference picks it.
    std::random_device device;
    std::mt19937 generator(device());
    std::uniform_int_distribution<size_t> pick(0, remarks.size() - 1);
    return remarks[pick(generator)];
}

} // namespace

/// The length of a session, the way the reference writes it.
///
/// The reference writes every part of the length — "1d 0h 0m", "1h 0m 0s" —
/// and rounds the seconds with the carry-over.
std::string format_session_length(int64_t ms) {
    if (ms < 60000) {
        if (ms == 0) return "0s";
        if (ms < 1000) {
            std::ostringstream out;
            out.precision(1);
            out << std::fixed << (static_cast<double>(ms) / 1000.0) << "s";
            return out.str();
        }
        return std::to_string(ms / 1000) + "s";
    }

    int64_t days = ms / 86400000;
    int64_t hours = (ms % 86400000) / 3600000;
    int64_t minutes = (ms % 3600000) / 60000;
    // The seconds are rounded, and the carry-over is carried over.
    int64_t seconds = (ms % 60000 + 500) / 1000;
    if (seconds == 60) {
        seconds = 0;
        ++minutes;
    }
    if (minutes == 60) {
        minutes = 0;
        ++hours;
    }
    if (hours == 24) {
        hours = 0;
        ++days;
    }

    if (days > 0) {
        return std::to_string(days) + "d " + std::to_string(hours) + "h " +
               std::to_string(minutes) + "m";
    }
    if (hours > 0) {
        return std::to_string(hours) + "h " + std::to_string(minutes) + "m " +
               std::to_string(seconds) + "s";
    }
    if (minutes > 0) {
        return std::to_string(minutes) + "m " + std::to_string(seconds) + "s";
    }
    return std::to_string(seconds) + "s";
}

/// The lines of the tab of the models of the panel of the statistics.
///
/// The tab is drawn by a function of its own, outside the class, so that it
/// can be drawn without a terminal. The range of the days is the number of
/// the range in the order the reference cycles through them.
std::vector<std::string> render_models_tab(const Theme& theme, const session::Stats& data, int width,
                                           int scroll, int range, const std::string& spinner) {
    std::vector<std::string> out;

    Style subtle;
    subtle.foreground = theme.subtle;

    // The models are listed by the tokens they used, the greatest first.
    std::vector<Model> models;
    int64_t total = 0;
    for (const auto& [name, usage] : data.model_usage) {
        models.push_back({name, usage.input_tokens, usage.output_tokens});
        total += usage.input_tokens + usage.output_tokens;
    }
    std::sort(models.begin(), models.end(),
              [](const Model& a, const Model& b) { return a.tokens() > b.tokens(); });

    if (models.empty()) {
        out.push_back(styled("No model usage data available", subtle));
        return out;
    }

    // The chart of the tokens of every day, with its legend and the labels
    // of the days under it.
    const Chart chart = generate_chart(data.daily_model_tokens, models, width, theme);
    if (!chart.lines.empty()) {
        Style bold;
        bold.bold = true;
        out.push_back(styled("Tokens per Day", bold));
        for (const std::string& line : chart.lines) out.push_back(line);
        out.push_back(styled(chart.x_axis, subtle));
        out.push_back(chart.legend);
        out.push_back("");
    }

    // The range of days: all three of them, with the one that is shown in
    // bold.
    out.push_back(range_line(theme, range, spinner));
    out.push_back("");

    // Four models are shown at a time, the way the reference shows them.
    const size_t offset = models.size() > 4
                              ? std::min(static_cast<size_t>(std::max(0, scroll)), models.size() - 4)
                              : 0;
    const size_t shown = std::min<size_t>(4, models.size() - offset);
    const std::vector<Model> visible(
        models.begin() + static_cast<std::ptrdiff_t>(offset),
        models.begin() + static_cast<std::ptrdiff_t>(offset + shown));

    // Every model takes two lines: its name with its share of the tokens,
    // and the tokens it was given and the ones it answered with.
    const auto entry = [&](const Model& model) {
        const double share = total > 0 ? static_cast<double>(model.tokens()) /
                                             static_cast<double>(total) * 100.0
                                       : 0.0;
        Style bold;
        bold.bold = true;
        return std::vector<std::string>{
            "• " + styled(commands::render_model_name(model.name), bold) + " " +
                styled("(" + to_fixed(share, 1) + "%)", subtle),
            styled("  In: " + number(model.input) + " · Out: " + number(model.output), subtle)};
    };

    // The models are in two columns of thirty-six characters, with four
    // characters between them.
    const size_t midpoint = (shown + 1) / 2;
    std::vector<std::string> left;
    std::vector<std::string> right;
    for (size_t i = 0; i < shown; ++i) {
        const std::vector<std::string> lines = entry(visible[i]);
        std::vector<std::string>& column = i < midpoint ? left : right;
        column.push_back(lines[0]);
        column.push_back(lines[1]);
    }

    for (size_t row = 0; row < std::max(left.size(), right.size()); ++row) {
        std::string line = row < left.size() ? left[row] : "";
        const size_t used = cc::display_width(strip(line));
        if (used < 36) line += std::string(36 - used, ' ');
        line += "    ";
        if (row < right.size()) line += right[row];
        out.push_back(line);
    }

    // The hint that says that the list of the models goes on.
    if (models.size() > 4) {
        out.push_back("");

        std::string hint;
        hint += offset > 0 ? std::string(figures::kArrowUp) : " ";
        hint += " ";
        hint += offset < models.size() - 4 ? std::string(figures::kArrowDown) : " ";
        hint += " " + std::to_string(offset + 1) + "-" +
                std::to_string(std::min(offset + 4, models.size())) + " of " +
                std::to_string(models.size()) + " models (↑↓ to scroll)";
        out.push_back(styled(hint, subtle));
    }

    return out;
}

StatsPanel::StatsPanel(Theme theme, std::string cwd)
    : theme_(theme), cwd_(std::move(cwd)), spinner_(theme) {}

std::string StatsPanel::range_label(Range range) {
    switch (range) {
    case Range::SevenDays:
        return "Last 7 days";
    case Range::ThirtyDays:
        return "Last 30 days";
    case Range::All:
    default:
        return "All time";
    }
}

int StatsPanel::range_days(Range range) {
    switch (range) {
    case Range::SevenDays:
        return 7;
    case Range::ThirtyDays:
        return 30;
    case Range::All:
    default:
        return 0;
    }
}

session::Stats StatsPanel::stats() const {
    // The numbers of the range that is shown. While they are read, the
    // numbers of every day are shown, the way the reference shows them.
    switch (range_) {
    case Range::SevenDays:
        if (seven_days_.ready()) return seven_days_.stats();
        break;
    case Range::ThirtyDays:
        if (thirty_days_.ready()) return thirty_days_.stats();
        break;
    case Range::All:
    default:
        break;
    }

    return all_time_.stats();
}

session::Stats StatsPanel::all_time_stats() const { return all_time_.stats(); }

bool StatsPanel::loading() const {
    switch (range_) {
    case Range::SevenDays:
        return !seven_days_.ready();
    case Range::ThirtyDays:
        return !thirty_days_.ready();
    case Range::All:
    default:
        return false;
    }
}

std::string StatsPanel::spinner_glyph() const {
    if (!loading()) return {};
    return spinner_.glyph();
}

std::string StatsPanel::factoid(const session::Stats& stats) const {
    const int64_t tokens = stats.total_tokens();

    // The reference keeps the remark, so that the panel does not change
    // under the eyes of the user while it is drawn again and again. A new
    // one is picked when the numbers change.
    if (factoid_sessions_ != stats.sessions || factoid_tokens_ != tokens ||
        factoid_longest_ != stats.longest_session_ms) {
        factoid_ = fun_factoid(stats, tokens);
        factoid_sessions_ = stats.sessions;
        factoid_tokens_ = tokens;
        factoid_longest_ = stats.longest_session_ms;
    }

    return factoid_;
}

std::vector<std::string> StatsPanel::render_overview(int width) const {
    const session::Stats data = stats();

    std::vector<std::string> out;

    Style claude;
    claude.foreground = theme_.claude;

    Style claude_bold;
    claude_bold.foreground = theme_.claude;
    claude_bold.bold = true;

    Style subtle;
    subtle.foreground = theme_.subtle;

    if (data.sessions == 0) {
        Style warning;
        warning.foreground = theme_.warning;
        out.push_back(styled("No stats available yet. Start using Claude Code!", warning));
        return out;
    }

    // The heatmap of the activity is at the top of the tab, the way the
    // reference draws it.
    {
        const auto all = all_time_stats();
        if (!all.daily_activity.empty()) {
            for (const std::string& line : generate_heatmap(all.daily_activity, theme_, width)) {
                out.push_back(line);
            }
            out.push_back("");
        }
    }

    // The range of days: all three of them, with the one that is shown in
    // bold.
    {
        out.push_back(range_line(theme_, static_cast<int>(range_), spinner_glyph()));
        out.push_back("");
    }

    // The rows are in two columns of 28 characters.
    const auto row = [&](const std::string& left, const std::string& right) {
        std::string line = left;
        const size_t used = cc::display_width(strip(left));
        if (used < 28) line += std::string(28 - used, ' ');
        line += "    ";
        line += right;
        out.push_back(line);
    };

    // The model the sessions used the most.
    std::string favourite;
    int64_t most = 0;
    for (const auto& [model, usage] : data.model_usage) {
        if (usage.input_tokens + usage.output_tokens > most) {
            most = usage.input_tokens + usage.output_tokens;
            favourite = model;
        }
    }

    if (!favourite.empty()) {
        // The reference writes the name of the model, the way the tab of the
        // models writes it.
        row("Favorite model: " + styled(commands::render_model_name(favourite), claude_bold),
            "Total tokens: " + styled(number(data.total_tokens()), claude));
    } else {
        row("", "Total tokens: " + styled(number(data.total_tokens()), claude));
    }

    // The block of the usage and the block of the activity are a row apart.
    out.push_back("");

    std::string longest;
    if (data.longest_session_ms > 0) {
        longest = "Longest session: " + styled(format_session_length(data.longest_session_ms), claude);
    }
    row("Sessions: " + styled(number(static_cast<int64_t>(data.sessions)), claude), longest);

    const size_t days = range_ == Range::All ? std::max<size_t>(1, data.days)
                                             : static_cast<size_t>(range_days(range_));
    row("Active days: " + styled(std::to_string(data.active_days), claude) +
            styled("/" + std::to_string(days), subtle),
        "Longest streak: " + styled(std::to_string(data.longest_streak), claude_bold) + " " +
            (data.longest_streak == 1 ? "day" : "days"));

    // The day the most messages were written on.
    std::string peak;
    size_t peak_count = 0;
    for (const auto& [day, count] : data.daily_activity) {
        if (count > peak_count) {
            peak_count = count;
            peak = day;
        }
    }
    std::string peak_text;
    if (!peak.empty()) {
        peak_text = "Most active day: " + styled(peak_day(peak), claude);
    }
    // The streak of the day is the one of all the time, the way the
    // reference takes it.
    const int64_t current_streak = all_time_stats().current_streak;
    row(peak_text,
        "Current streak: " + styled(std::to_string(current_streak), claude_bold) + " " +
            (current_streak == 1 ? "day" : "days"));

    // The remark the statistics end with.
    const std::string remark = factoid(data);
    if (!remark.empty()) {
        out.push_back("");
        Style suggestion;
        suggestion.foreground = theme_.suggestion;
        out.push_back(styled(remark, suggestion));
    }

    return out;
}

std::vector<std::string> StatsPanel::render_models(int width) const {
    return render_models_tab(theme_, stats(), width, models_scroll_, static_cast<int>(range_),
                              spinner_glyph());
}

std::vector<std::string> StatsPanel::render(int width, int height) const {
    // The transcripts are read in the background, so that the panel opens at
    // once: the numbers of the cache are shown while the day that is going
    // on is read.
    all_time_.start(cwd_);

    const int w = width > 0 ? width : 1;

    std::vector<std::string> out;

    // The reference draws the message alone when there are no statistics:
    // the pane, the rule and the strip of the tabs are not drawn at all.
    if (stats().sessions == 0) {
        out.push_back("");

        // While the statistics are read, the reference shows the spinner and
        // the words that say so.
        if (!all_time_.ready()) {
            Style style;
            style.foreground = theme_.text;
            out.push_back(styled(spinner_.glyph(), style) + " Loading your Claude Code stats…");
            return out;
        }

        for (const std::string& line : render_overview(w)) out.push_back(line);
        return out;
    }

    // The pane of the reference leaves an empty line above its rule, and
    // the rule is painted with the colour of Claude.
    out.push_back("");
    std::string rule;
    for (int i = 0; i < w; ++i) rule += "─";
    out.push_back(styled(rule, Style::fg(theme_.claude)));

    // The tabs. The one that is shown is painted with the colour of the
    // pane, and the ones that are not keep the colour of the text.
    std::string header;
    for (const auto& [name, chosen] : std::vector<std::pair<std::string, bool>>{
             {"Overview", !models_}, {"Models", models_}}) {
        if (!header.empty()) header += " ";

        Style style;
        style.bold = chosen;
        if (chosen) {
            style.background = theme_.claude;
            style.foreground = theme_.inverse_text;
        }
        header += styled(" " + name + " ", style);
    }
    // The reference passes an empty title to the tabs, and the gap of the
    // row is kept even though the title has no width.
    out.push_back("   " + header);
    out.push_back("");
    // The body of the tab starts a row below the strip of the tabs, the
    // way the reference lays it out.
    out.push_back("");

    // The heatmap of the activity is measured against the width of the
    // terminal, the way the reference measures it.
    std::vector<std::string> body = models_ ? render_models(w) : render_overview(w);

    // The panel may not be taller than the screen: the region is drawn
    // again on every frame, and a region that is taller than the screen
    // cannot be erased — the frames pile up in the scrollback. The lines
    // that do not fit are left out.
    if (height > 0) {
        // The rows that are not the body of the tab: the pane, the strip of
        // the tabs and the hint below the body.
        const int fixed = static_cast<int>(out.size()) + 2;
        const int room = height - fixed;
        if (room > 0 && static_cast<int>(body.size()) > room) {
            body.resize(static_cast<size_t>(room));
        }
    }

    for (const std::string& line : body) {
        out.push_back("  " + line);
    }

    out.push_back("");
    Style dim;
    dim.foreground = theme_.inactive;
    out.push_back("    " + styled("Esc to cancel · r to cycle dates · ctrl+s to copy" +
                                      (copy_status_.empty() ? std::string() : " · " + copy_status_),
                                  dim));

    return out;
}

void StatsPanel::wait() const {
    // The numbers of the sessions are read at once, so that the panel can be
    // drawn with them.
    all_time_.start(cwd_);
    all_time_.wait();
}

void StatsPanel::run(Terminal& terminal) {
    // The transcripts are read in the background, so that the panel opens at
    // once: the numbers of the cache are shown while the day that is going
    // on is read. The thread that reads them does not draw anything — the
    // panel is drawn by the thread that owns the terminal.
    all_time_.start(cwd_);

    while (true) {
        // The reference writes what happened after the hint of the panel
        // for two seconds.
        if (!copy_status_.empty() && cc::now_ms() - copy_at_ > 2000) copy_status_.clear();

        // The spinner moves on, so that the numbers that are read show that
        // something is happening.
        spinner_.tick();

        terminal.render_dynamic(render(terminal.width(), terminal.height()));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return;
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Tab:
            models_ = !models_;
            models_scroll_ = 0;
            break;
        case Key::Type::CtrlS: {
            // The screen of the statistics is put on the clipboard, the way
            // the reference puts it there.
            std::string text;
            for (const std::string& line : render(terminal.width(), terminal.height())) {
                text += strip(line) + "\n";
            }
            copy_to_clipboard(text);
            copy_status_ = "copied!";
            copy_at_ = cc::now_ms();
            break;
        }
        case Key::Type::Character:
            if (key->text == "r") {
                switch (range_) {
                case Range::All:
                    range_ = Range::SevenDays;
                    break;
                case Range::SevenDays:
                    range_ = Range::ThirtyDays;
                    break;
                case Range::ThirtyDays:
                default:
                    range_ = Range::All;
                    break;
                }
                models_scroll_ = 0;

                // The numbers of the range are read in the background; the
                // numbers of every day are shown until they are there, the
                // way the reference shows them.
                if (range_ == Range::SevenDays) seven_days_.start_range(cwd_, range_days(range_));
                if (range_ == Range::ThirtyDays) thirty_days_.start_range(cwd_, range_days(range_));
            }
            break;
        case Key::Type::Up:
            // The list of the models is scrolled, the way the reference
            // scrolls it.
            if (models_ && models_scroll_ >= 2) models_scroll_ -= 2;
            break;
        case Key::Type::Down: {
            if (!models_) break;
            const int count = static_cast<int>(stats().model_usage.size());
            const int last = count - 4;
            if (models_scroll_ < last) {
                models_scroll_ = std::min(models_scroll_ + 2, last);
            }
            break;
        }
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            terminal.clear_dynamic();
            return;
        default:
            break;
        }
    }
}

} // namespace cc::ui
