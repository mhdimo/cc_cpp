#pragma once

#include "session/stats.hpp"
#include "ui/ansi.hpp"
#include "ui/spinner.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cc::ui {

/// The lines of the tab of the models of the panel of the statistics.
///
/// The tab is drawn by a function of its own, outside the class, so that it
/// can be drawn without a terminal. The range of the days is the number of
/// the range in the order the reference cycles through them. The spinner is
/// shown next to the ranges while the numbers of the range are read.
std::vector<std::string> render_models_tab(const Theme& theme, const session::Stats& data, int width,
                                           int scroll, int range, const std::string& spinner = {});

/// The length of a session, the way the reference writes it.
///
/// The reference writes every part of the length — "1d 0h 0m", "1h 0m 0s" —
/// and rounds the seconds with the carry-over.
std::string format_session_length(int64_t ms);

/// The panel of the statistics.
///
/// The reference shows the statistics of the sessions in two tabs — the
/// overview and the models — and lets the user cycle through the ranges of
/// days with the `r` key.
class StatsPanel {
public:
    StatsPanel(Theme theme, std::string cwd);

    /// Renders the panel.
    /// The height is the height of the terminal: the panel is cut so that
    /// it fits on the screen, the way the reference cuts it.
    ///
    /// The transcripts are read in the background, so that the panel opens at
    /// once: the numbers of the cache are shown while the day that is going
    /// on is read. The panel is drawn again when the numbers are there.
    std::vector<std::string> render(int width, int height = 0) const;

    /// Waits for the numbers that are read in the background.
    ///
    /// The tests wait for them, so that the numbers they look at are the
    /// ones of the transcripts.
    void wait() const;

    /// Runs the panel until the user closes it.
    ///
    /// The transcripts are read in the background, so that the panel opens at
    /// once: the numbers of the cache are shown while the day that is going
    /// on is read.
    void run(Terminal& terminal);

private:
    /// The range of days the statistics cover.
    ///
    /// The reference offers the three ranges: all the time, the last seven
    /// days and the last thirty days.
    enum class Range {
        All,
        SevenDays,
        ThirtyDays,
    };

    /// The name of the range, the way the reference writes it.
    static std::string range_label(Range range);

    /// The number of days the range covers; zero means every day.
    static int range_days(Range range);

    /// The statistics of the range that is shown.
    session::Stats stats() const;

    /// The statistics of every session, the way the heatmap wants them.
    session::Stats all_time_stats() const;

    /// The lines of the tab of the overview.
    std::vector<std::string> render_overview(int width) const;

    /// The lines of the tab of the models.
    std::vector<std::string> render_models(int width) const;

    /// The remark the statistics end with.
    ///
    /// The reference picks the remark once and keeps it, so that the panel
    /// does not change under the eyes of the user.
    std::string factoid(const session::Stats& stats) const;

    /// True while the numbers of the range that is shown are read.
    bool loading() const;

    /// The spinner that is shown while the numbers of the range are read.
    std::string spinner_glyph() const;

    Theme theme_;
    std::string cwd_;
    Range range_ = Range::All;
    bool models_ = false;

    /// The number of the models the tab of the models has scrolled down.
    int models_scroll_ = 0;

    /// What happened the last time the screen was copied.
    ///
    /// The reference writes it after the hint of the panel.
    std::string copy_status_;

    /// The time the screen was copied.
    int64_t copy_at_ = 0;

    /// The spinner of the panel, the one the reference shows while the
    /// numbers are read.
    mutable Spinner spinner_;

    /// The statistics of every day, read in the background.
    ///
    /// The numbers of the cache are there at once; the day that is going on
    /// is read in the background, and the numbers are put in the panel when
    /// they are there.
    mutable session::BackgroundStats all_time_;

    /// The statistics of the last seven days, read in the background.
    mutable session::BackgroundStats seven_days_;

    /// The statistics of the last thirty days, read in the background.
    mutable session::BackgroundStats thirty_days_;

    /// The remark of the statistics, and the numbers it belongs to.
    ///
    /// A new remark is picked when the numbers change, the way the
    /// reference picks one.
    mutable std::string factoid_;
    mutable size_t factoid_sessions_ = 0;
    mutable int64_t factoid_tokens_ = 0;
    mutable int64_t factoid_longest_ = 0;
};

} // namespace cc::ui
