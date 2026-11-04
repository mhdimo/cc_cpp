#include <catch2/catch_test_macros.hpp>

#include "ui/feed.hpp"
#include "ui/logo.hpp"

#include "utils/string.hpp"

using namespace cc;

namespace {

/// The information the logo shows, as the tests need it.
ui::Logo::Info logo_info() {
    ui::Logo::Info info;
    info.version = "2.0.0";
    info.model = "Sonnet 4.5";
    info.billing = "Claude Max";
    info.cwd = "/Users/ada/my-project";
    return info;
}

/// The widest line of the banner.
size_t widest(const std::vector<std::string>& lines) {
    size_t width = 0;
    for (const auto& line : lines) width = std::max(width, display_width(line));
    return width;
}

/// The column a text starts at in a line.
///
/// The columns are counted the way the terminal counts them, and not the
/// way the bytes of the line are counted.
size_t column_of(const std::string& line, const std::string& text) {
    const std::string plain = ui::strip(line);
    const size_t at = plain.find(text);
    if (at == std::string::npos) return std::string::npos;
    return display_width(plain.substr(0, at));
}

} // namespace

TEST_CASE("The greeting names the account when there is one") {
    REQUIRE(ui::format_welcome_message("Ada") == "Welcome back Ada!");
}

TEST_CASE("The greeting is plain when the account has no name") {
    REQUIRE(ui::format_welcome_message("") == "Welcome back!");
}

TEST_CASE("The greeting leaves out a name that is too long") {
    const std::string too_long(21, 'a');
    REQUIRE(ui::format_welcome_message(too_long) == "Welcome back!");

    const std::string just_right(20, 'a');
    REQUIRE(ui::format_welcome_message(just_right) == "Welcome back " + just_right + "!");
}

TEST_CASE("The layout follows the width of the terminal") {
    REQUIRE(ui::logo_layout_for_width(120) == ui::LogoLayout::Panel);
    REQUIRE(ui::logo_layout_for_width(70) == ui::LogoLayout::Panel);
    REQUIRE(ui::logo_layout_for_width(69) == ui::LogoLayout::Compact);
    REQUIRE(ui::logo_layout_for_width(40) == ui::LogoLayout::Compact);
}

TEST_CASE("The layout of the session follows the notes of the release") {
    // The notes have been seen already: the banner is the condensed one.
    REQUIRE(ui::logo_layout_for_session(false, false, 120) == ui::LogoLayout::Condensed);

    // There are notes to show: the banner is the panel.
    REQUIRE(ui::logo_layout_for_session(true, false, 120) == ui::LogoLayout::Panel);
    REQUIRE(ui::logo_layout_for_session(true, false, 40) == ui::LogoLayout::Compact);

    // The full logo is asked for: the banner is the panel as well.
    REQUIRE(ui::logo_layout_for_session(false, true, 120) == ui::LogoLayout::Panel);
}

TEST_CASE("The effort is shown after the name of the model") {
    REQUIRE(ui::effort_suffix("high") == " with high effort");
    REQUIRE(ui::effort_suffix("low") == " with low effort");
    REQUIRE(ui::effort_suffix("medium") == " with medium effort");
}

TEST_CASE("The effort is left out when the user chose none") {
    REQUIRE(ui::effort_suffix("") == "");
}

TEST_CASE("The effort max is shown as the level the user chose") {
    REQUIRE(ui::effort_suffix("max") == " with max effort");
}

TEST_CASE("The effort of the session follows the environment") {
    const char* previous = std::getenv("CLAUDE_CODE_EFFORT_LEVEL");
    const std::string kept = previous != nullptr ? previous : "";

    // The level the user chose is used when the environment says nothing.
    ::setenv("CLAUDE_CODE_EFFORT_LEVEL", "", 1);
    REQUIRE(ui::effective_effort_level("", "low") == "low");

    ::setenv("CLAUDE_CODE_EFFORT_LEVEL", "medium", 1);
    REQUIRE(ui::effective_effort_level("", "low") == "medium");

    // The environment asks for no effort at all.
    ::setenv("CLAUDE_CODE_EFFORT_LEVEL", "unset", 1);
    REQUIRE(ui::effective_effort_level("", "low").empty());

    // A variable that holds no level is ignored: the level the user chose
    // is used, the way the reference uses it.
    ::setenv("CLAUDE_CODE_EFFORT_LEVEL", "bogus", 1);
    REQUIRE(ui::effective_effort_level("", "low") == "low");

    // The flag of the command line comes before the setting of the user.
    ::setenv("CLAUDE_CODE_EFFORT_LEVEL", "", 1);
    REQUIRE(ui::effective_effort_level("high", "low") == "high");

    // A flag that holds no level is ignored.
    REQUIRE(ui::effective_effort_level("bogus", "low") == "low");

    // A number is an effort of its own, which the reference shows as
    // "high" for everybody but the users of the inside build.
    REQUIRE(ui::effective_effort_level("50", "") == "high");
    ::setenv("CLAUDE_CODE_EFFORT_LEVEL", "50", 1);
    REQUIRE(ui::effective_effort_level("", "low") == "high");
    ::setenv("CLAUDE_CODE_EFFORT_LEVEL", "", 1);

    // The user chose no effort at all: the banner says nothing about it,
    // the variable or not.
    REQUIRE(ui::effective_effort_level("", "").empty());
    ::setenv("CLAUDE_CODE_EFFORT_LEVEL", "high", 1);
    REQUIRE(ui::effective_effort_level("", "").empty());

    if (previous != nullptr) {
        ::setenv("CLAUDE_CODE_EFFORT_LEVEL", kept.c_str(), 1);
    } else {
        ::unsetenv("CLAUDE_CODE_EFFORT_LEVEL");
    }
}

TEST_CASE("The effort of the banner is the level the user chose") {
    ui::Logo::Info info = logo_info();
    info.model = "Opus 4.6";
    info.effort = "max";

    const ui::Logo logo(ui::Theme::dark(), info);
    std::string joined;
    for (const auto& line : logo.render(80)) joined += ui::strip(line) + "\n";

    REQUIRE(joined.find("Opus 4.6 with max effort") != std::string::npos);
}

TEST_CASE("The model of the banner carries the effort") {
    ui::Logo::Info info = logo_info();
    info.effort = "high";

    const ui::Logo logo(ui::Theme::dark(), info);
    const auto lines = logo.render(80);

    std::string joined;
    for (const auto& line : lines) joined += ui::strip(line) + "\n";

    REQUIRE(joined.find("Sonnet 4.5 with high effort") != std::string::npos);
}

TEST_CASE("The panel holds the frame, the greeting and the feeds") {
    ui::Logo::Info info = logo_info();
    info.display_name = "Ada";

    ui::Feed activity;
    activity.title = "Recent activity";
    activity.lines = {ui::FeedLine{"Fix the banner", "3h ago"}};
    activity.footer = "/resume for more";

    ui::Feed whats_new;
    whats_new.title = "What's new";
    whats_new.lines = {ui::FeedLine{"A new thing", ""}};
    whats_new.footer = "/release-notes for more";

    const ui::Logo logo(ui::Theme::dark(), info);
    const auto lines = logo.render_panel(100, {activity, whats_new});

    std::string joined;
    for (const auto& line : lines) joined += ui::strip(line) + "\n";

    // The frame of the panel, with the name of the product in it.
    REQUIRE(joined.find("╭") != std::string::npos);
    REQUIRE(joined.find("Claude Code") != std::string::npos);
    REQUIRE(joined.find("v2.0.0") != std::string::npos);
    REQUIRE(joined.find("╰") != std::string::npos);

    // The greeting, the mascot and the two feeds.
    REQUIRE(joined.find("Welcome back Ada!") != std::string::npos);
    REQUIRE(joined.find("▐") != std::string::npos);
    REQUIRE(joined.find("Recent activity") != std::string::npos);
    REQUIRE(joined.find("Fix the banner") != std::string::npos);
    REQUIRE(joined.find("/resume for more") != std::string::npos);
    REQUIRE(joined.find("What's new") != std::string::npos);
    REQUIRE(joined.find("A new thing") != std::string::npos);
    REQUIRE(joined.find("/release-notes for more") != std::string::npos);
}

TEST_CASE("The panel never reaches past the edge of the terminal") {
    ui::Logo::Info info = logo_info();
    info.display_name = "Ada";

    ui::Feed activity;
    activity.title = "Recent activity";
    activity.lines = {ui::FeedLine{"A line of the feed that is far too long to fit in the panel",
                                   "3h ago"}};
    activity.footer = "/resume for more";

    ui::Feed whats_new;
    whats_new.title = "What's new";
    whats_new.lines = {ui::FeedLine{"Another line that is far too long to fit in the panel", ""}};
    whats_new.footer = "/release-notes for more";

    const ui::Logo logo(ui::Theme::dark(), info);
    for (const int width : {70, 80, 100, 120}) {
        const auto lines = logo.render_panel(width, {activity, whats_new});
        REQUIRE(widest(lines) <= static_cast<size_t>(width));
    }
}

TEST_CASE("The lines of a feed are cut to the width of the panel") {
    ui::Feed activity;
    activity.title = "Recent activity";
    activity.lines = {ui::FeedLine{"A line of the feed that is far too long to fit in the panel",
                                   "3h ago"}};

    const ui::Logo logo(ui::Theme::dark(), logo_info());
    const auto lines = logo.render_panel(80, {activity});

    std::string joined;
    for (const auto& line : lines) joined += ui::strip(line) + "\n";

    // The line of the feed is cut, and the cut is marked.
    REQUIRE(joined.find("A line of the feed") != std::string::npos);
    REQUIRE(joined.find("far too long to fit in the panel") == std::string::npos);
    REQUIRE(joined.find("…") != std::string::npos);
}

TEST_CASE("The empty feeds say what they have nothing to show") {
    ui::Feed activity;
    activity.title = "Recent activity";
    activity.empty_message = "No recent activity";

    ui::Feed whats_new;
    whats_new.title = "What's new";
    whats_new.empty_message = "Check the Claude Code changelog for updates";

    const ui::Logo logo(ui::Theme::dark(), logo_info());
    const auto lines = logo.render_panel(100, {activity, whats_new});

    std::string joined;
    for (const auto& line : lines) joined += ui::strip(line) + "\n";

    REQUIRE(joined.find("No recent activity") != std::string::npos);
    REQUIRE(joined.find("Check the Claude Code changelog for updates") != std::string::npos);
    // The footers are left out when there is nothing under them.
    REQUIRE(joined.find("/resume for more") == std::string::npos);
}

TEST_CASE("The banner picks the layout the width asks for") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());

    const auto narrow = logo.render_banner(60, {});
    const auto wide = logo.render_banner(100, {});

    REQUIRE(widest(narrow) <= 60u);
    // The frame of the panel is drawn in the wide layout.
    std::string joined;
    for (const auto& line : wide) joined += ui::strip(line) + "\n";
    REQUIRE(joined.find("╭") != std::string::npos);
}

TEST_CASE("The time of a session is short in the feed") {
    const int64_t now = 1'700'000'000'000;
    REQUIRE(ui::relative_time_short(now - 3 * 3600 * 1000, now) == "3h ago");
    REQUIRE(ui::relative_time_short(now - 2 * 86400 * 1000, now) == "2d ago");
    REQUIRE(ui::relative_time_short(now - 90 * 1000, now) == "1m ago");
    REQUIRE(ui::relative_time_short(now, now) == "0s ago");
}

TEST_CASE("The place of the mascot is known") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());

    // In the compact box the mascot stands under the frame, the padding,
    // the greeting and the margin above it.
    const auto narrow = logo.mascot_place(60, {});
    REQUIRE(narrow.row == 4);
    REQUIRE(narrow.column > 0);

    // In the panel the mascot stands in the left column, below the
    // greeting.
    const auto wide = logo.mascot_place(100, {});
    REQUIRE(wide.row > 0);
    REQUIRE(wide.column > 0);
    REQUIRE(wide.column + ui::Mascot::kWidth < 100);
}

TEST_CASE("The mascot of the compact box stands where it is said to stand") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());

    for (const int width : {20, 40, 60, 69}) {
        const auto place = logo.mascot_place(width, {});
        const auto lines = logo.render_compact(width);

        // The second row of the mascot is nine columns wide, so it shows
        // where the mascot stands.
        const std::string row = ui::strip(lines[static_cast<size_t>(place.row) + 1]);
        const size_t start = row.find("▝");
        REQUIRE(start != std::string::npos);

        // The frame of the box and the padding around its contents take two
        // columns, so the mascot starts at the third one of the contents.
        REQUIRE(display_width(row.substr(0, start)) == static_cast<size_t>(place.column));
    }
}

TEST_CASE("The compact box holds the greeting, the mascot and the texts") {
    ui::Logo::Info info = logo_info();
    info.display_name = "Ada";

    const ui::Logo logo(ui::Theme::dark(), info);
    const auto lines = logo.render_compact(60);

    std::string joined;
    for (const auto& line : lines) joined += ui::strip(line) + "\n";

    // The frame of the box, with the name of the product in it.
    REQUIRE(joined.find("╭") != std::string::npos);
    REQUIRE(joined.find("Claude Code") != std::string::npos);
    REQUIRE(joined.find("╰") != std::string::npos);

    // The greeting, the mascot and the three texts.
    REQUIRE(joined.find("Welcome back Ada!") != std::string::npos);
    REQUIRE(joined.find("▐") != std::string::npos);
    REQUIRE(joined.find("Sonnet 4.5") != std::string::npos);
    REQUIRE(joined.find("Claude Max") != std::string::npos);
    REQUIRE(joined.find("/Users/ada/my-project") != std::string::npos);
}

TEST_CASE("The compact box is as wide as the terminal") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());
    for (const int width : {20, 40, 60, 69}) {
        const auto lines = logo.render_compact(width);
        REQUIRE(widest(lines) == static_cast<size_t>(width));
    }
}

TEST_CASE("The compact box leaves the name of the account out when it does not fit") {
    ui::Logo::Info info = logo_info();
    info.display_name = "A very long name of an account";

    const ui::Logo logo(ui::Theme::dark(), info);
    const auto lines = logo.render_compact(30);

    std::string joined;
    for (const auto& line : lines) joined += ui::strip(line) + "\n";

    REQUIRE(joined.find("Welcome back!") != std::string::npos);
    REQUIRE(joined.find("A very long name") == std::string::npos);
}

TEST_CASE("The narrow banner is the compact box") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());

    const auto narrow = logo.render_banner(60, {});
    std::string joined;
    for (const auto& line : narrow) joined += ui::strip(line) + "\n";

    REQUIRE(joined.find("╭") != std::string::npos);
    REQUIRE(widest(narrow) == 60u);
}

TEST_CASE("The condensed banner has no frame and no feeds") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());

    const auto lines = logo.render_session(100, {}, /*has_release_notes=*/false);

    std::string joined;
    for (const auto& line : lines) joined += ui::strip(line) + "\n";

    REQUIRE(joined.find("╭") == std::string::npos);
    REQUIRE(joined.find("Claude Code") != std::string::npos);
    REQUIRE(joined.find("Sonnet 4.5") != std::string::npos);
}

TEST_CASE("The title of the panel stands at the fifth column") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());

    const auto lines = logo.render_panel(80, {});
    const std::string top = ui::strip(lines[0]);

    // The frame keeps three columns for itself before the title, and the
    // title begins with a space of its own.
    REQUIRE(top.rfind("╭─── ", 0) == 0);
    REQUIRE(column_of(lines[0], "Claude Code") == 5);
}

TEST_CASE("The rows of the mascot stand at the same column") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());

    // The compact box: the frame, the padding, the greeting and the margin
    // above the mascot.
    const auto compact = logo.render_compact(60);
    const std::string first = ui::strip(compact[4]);
    const std::string second = ui::strip(compact[5]);
    const std::string third = ui::strip(compact[6]);

    // The art of the mascot is what it is: the first row starts with a
    // space of its own, and the third one with two.
    REQUIRE(first.find("▐") == second.find("▝") + 1);
    REQUIRE(second.find("▝") + 2 == third.find("▘"));
}

TEST_CASE("The rows of the mascot of the panel stand at the same column") {
    ui::Logo::Info info = logo_info();
    info.display_name = "Ada";

    const ui::Logo logo(ui::Theme::dark(), info);
    const auto lines = logo.render_panel(100, {});

    // The rows of the mascot are the ones that hold the art of it.
    size_t row = 0;
    while (row < lines.size() && ui::strip(lines[row]).find("▐") == std::string::npos) ++row;
    REQUIRE(row + 2 < lines.size());

    const std::string first = ui::strip(lines[row]);
    const std::string second = ui::strip(lines[row + 1]);
    const std::string third = ui::strip(lines[row + 2]);

    REQUIRE(first.find("▐") == second.find("▝") + 1);
    REQUIRE(second.find("▝") + 2 == third.find("▘"));
}

TEST_CASE("The text of the condensed banner stands in the column of the mascot") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());

    const auto lines = logo.render(80);
    REQUIRE(lines.size() == 3);

    // The mascot is nine columns wide and the gap beside it is two, so
    // the text stands at the eleventh column.
    REQUIRE(column_of(lines[0], "Claude Code") == 11);
    REQUIRE(column_of(lines[1], "Sonnet 4.5") == 11);
    REQUIRE(column_of(lines[2], "/Users/ada/my-project") == 11);
}

TEST_CASE("The condensed banner keeps four columns free") {
    ui::Logo::Info info = logo_info();
    info.cwd = "/Users/ada/a-very-long-path-of-a-project-that-does-not-fit";

    const ui::Logo logo(ui::Theme::dark(), info);

    // The mascot and the gap beside it take eleven columns, and the text
    // takes the rest of the width less four columns.
    for (const int width : {40, 60, 80, 120}) {
        const auto lines = logo.render(width);
        REQUIRE(widest(lines) <= static_cast<size_t>(width - 4));
    }
}

TEST_CASE("The condensed banner splits the model and the billing when they do not fit") {
    const ui::Logo logo(ui::Theme::dark(), logo_info());

    // The text of the banner is the width of the terminal less fifteen
    // columns. The model and the billing take twenty-three columns of it,
    // so they stand on one line only when the terminal is wide enough.
    REQUIRE(logo.render(80).size() == 3);
    REQUIRE(logo.render(38).size() == 3);
    REQUIRE(logo.render(37).size() == 4);
}

TEST_CASE("The condensed banner keeps the name of the agent whole") {
    ui::Logo::Info info = logo_info();
    info.agent_name = "my-agent";
    info.cwd = "/Users/ada/a-very-long-path-of-a-project";

    const ui::Logo logo(ui::Theme::dark(), info);
    const auto lines = logo.render(60);

    const std::string last = ui::strip(lines.back());
    REQUIRE(last.find("@my-agent · ") != std::string::npos);

    // The path is cut on its own, so that the name of the agent is not cut
    // with it, and it gets the room that is left of the text.
    REQUIRE(display_width(last.substr(11)) >= 30u);
    REQUIRE(display_width(last) <= 56u);
}

TEST_CASE("The mascot of the Terminal.app of Apple is drawn the other way round") {
    const char* previous = std::getenv("TERM_PROGRAM");
    const std::string kept = previous != nullptr ? previous : "";

    ::setenv("TERM_PROGRAM", "Apple_Terminal", 1);

    const ui::Mascot mascot(ui::Theme::dark());
    const auto rows = mascot.render();

    const ui::Mascot looking(ui::Theme::dark(), ui::MascotPose::LookLeft);
    const auto looking_rows = looking.render();

    const ui::Mascot arms(ui::Theme::dark(), ui::MascotPose::ArmsUp);
    const auto arms_rows = arms.render();

    // Another terminal draws the ordinary mascot.
    ::setenv("TERM_PROGRAM", "iTerm.app", 1);
    const ui::Mascot other(ui::Theme::dark());
    const auto other_rows = other.render();

    if (previous != nullptr) {
        ::setenv("TERM_PROGRAM", kept.c_str(), 1);
    } else {
        ::unsetenv("TERM_PROGRAM");
    }

    REQUIRE(rows.size() == 3);
    REQUIRE(ui::strip(rows[0]) == "▗ ▗   ▖ ▖");
    REQUIRE(ui::strip(rows[1]) == "         ");
    REQUIRE(ui::strip(rows[2]) == "  ▘▘ ▝▝  ");

    // The eyes of the pose are drawn in the body of the mascot.
    REQUIRE(ui::strip(looking_rows[0]) == "▗ ▘   ▘ ▖");

    // The arms are left out, so the pose with the arms raised looks like
    // the mascot at rest.
    REQUIRE(ui::strip(arms_rows[0]) == "▗ ▗   ▖ ▖");

    // Another terminal draws the ordinary mascot.
    REQUIRE(ui::strip(other_rows[0]) == " ▐▛███▜▌");
    REQUIRE(ui::strip(other_rows[2]) == "  ▘▘ ▝▝  ");
}
