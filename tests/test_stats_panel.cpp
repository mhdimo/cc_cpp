#include <catch2/catch_test_macros.hpp>

#include "session/stats.hpp"
#include "session/stats_cache.hpp"
#include "ui/ansi.hpp"
#include "ui/stats_panel.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cc;

namespace fs = std::filesystem;

namespace {

/// The usage of a model.
Usage usage(int64_t input, int64_t output) {
    Usage out;
    out.input_tokens = input;
    out.output_tokens = output;
    return out;
}

/// The folder of the user, for as long as a test runs.
///
/// The statistics are read from the folder of the user, and the cache of
/// them is written there, so a test that must not touch the real one points
/// the folder of the user at a folder of its own.
class TempHome {
public:
    TempHome() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("cc-stats-panel-test-" + std::to_string(++counter));
        fs::create_directories(path_);
        setenv("CLAUDE_CONFIG_DIR", path_.string().c_str(), 1);
    }

    ~TempHome() {
        unsetenv("CLAUDE_CONFIG_DIR");
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempHome(const TempHome&) = delete;
    TempHome& operator=(const TempHome&) = delete;

    /// The folder the user is at while the test runs.
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

/// Writes a transcript of a session that the model answered.
void write_transcript(const fs::path& file, int days_ago, const std::string& model,
                      int64_t input_tokens, int64_t output_tokens) {
    fs::create_directories(file.parent_path());

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "{\"type\":\"assistant\",\"timestamp\":\""
        << session::to_date_string(now_ms() - static_cast<int64_t>(days_ago) * 24 * 60 * 60 * 1000)
        << "T11:00:00.000Z\",\"message\":{\"role\":\"assistant\",\"model\":\"" << model
        << "\",\"content\":[{\"type\":\"text\",\"text\":\"hello\"}],\"usage\":{\"input_tokens\":"
        << input_tokens << ",\"output_tokens\":" << output_tokens << "}}}\n";
}

/// The text of a line, without the colours.
std::string plain(const std::string& line) {
    return ui::strip(line);
}

/// Every line of the tab, joined with a newline.
std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) out += plain(line) + "\n";
    return out;
}

/// The row of the tab that holds the given text.
std::string row_with(const std::vector<std::string>& lines, const std::string& text) {
    for (const std::string& line : lines) {
        if (plain(line).find(text) != std::string::npos) return plain(line);
    }
    return {};
}

} // namespace

TEST_CASE("The tab of the models of the statistics panel shows the share of every model") {
    const ui::Theme theme = ui::Theme::dark();

    session::Stats stats;
    stats.model_usage["claude-sonnet-4-5"] = usage(1000, 500);
    stats.model_usage["claude-haiku-4-5"] = usage(250, 250);

    const auto lines = ui::render_models_tab(theme, stats, 80, 0, 0);
    const std::string text = joined(lines);

    // The name of the model is shown with the share of the tokens it used.
    REQUIRE(text.find("• Sonnet 4.5 (75.0%)") != std::string::npos);
    REQUIRE(text.find("• Haiku 4.5 (25.0%)") != std::string::npos);

    // The tokens that were given to the model and the ones it answered
    // with are under it.
    REQUIRE(text.find("  In: 1.0k · Out: 500") != std::string::npos);
    REQUIRE(text.find("  In: 250 · Out: 250") != std::string::npos);
}

TEST_CASE("The models of the statistics panel are in two columns of thirty-six characters") {
    const ui::Theme theme = ui::Theme::dark();

    session::Stats stats;
    stats.model_usage["claude-sonnet-4-5"] = usage(1000, 500);
    stats.model_usage["claude-haiku-4-5"] = usage(250, 250);

    const auto lines = ui::render_models_tab(theme, stats, 80, 0, 0);

    // The two models share the row: the first one is in the left column and
    // the second one is in the right one.
    const std::string row = row_with(lines, "Sonnet 4.5");
    REQUIRE_FALSE(row.empty());

    const size_t at = row.find("Haiku 4.5");
    REQUIRE(at != std::string::npos);

    // The left column is thirty-six columns wide and four columns separate
    // the two of them, so the right one starts at the fortieth column. The
    // bullet and the space before the name take two of them.
    REQUIRE(cc::display_width(row.substr(0, at)) == 42);
}

TEST_CASE("The tab of the models of the statistics panel says when there is no model") {
    const ui::Theme theme = ui::Theme::dark();

    const auto lines = ui::render_models_tab(theme, session::Stats{}, 80, 0, 0);

    REQUIRE(lines.size() == 1);
    REQUIRE(plain(lines[0]) == "No model usage data available");

    // The empty state is painted with the faint colour of the reference.
    REQUIRE(lines[0].find(ui::style_sequence(ui::Style::fg(theme.subtle))) != std::string::npos);
}

TEST_CASE("The tab of the models of the statistics panel draws the tokens of every day") {
    const ui::Theme theme = ui::Theme::dark();

    session::Stats stats;
    stats.model_usage["claude-sonnet-4-5"] = usage(1000, 500);
    stats.daily_model_tokens["2026-03-01"] = {{"claude-sonnet-4-5", 100}};
    stats.daily_model_tokens["2026-03-02"] = {{"claude-sonnet-4-5", 300}};
    stats.daily_model_tokens["2026-03-03"] = {{"claude-sonnet-4-5", 200}};

    const auto lines = ui::render_models_tab(theme, stats, 80, 0, 0);
    const std::string text = joined(lines);

    // The chart has its title, its axis and its legend.
    REQUIRE(text.find("Tokens per Day") != std::string::npos);
    REQUIRE(text.find("┤") != std::string::npos);
    REQUIRE(text.find("• Sonnet 4.5") != std::string::npos);

    // The axis of the chart shows the greatest number of tokens of the day.
    const std::string top = row_with(lines, "300");
    REQUIRE_FALSE(top.empty());
    REQUIRE(top.find("┤") != std::string::npos);

    // The days under the chart are named.
    REQUIRE(text.find("Mar 1") != std::string::npos);
    REQUIRE(text.find("Mar 3") != std::string::npos);
}

TEST_CASE("The tab of the models of the statistics panel says that the list goes on") {
    const ui::Theme theme = ui::Theme::dark();

    session::Stats stats;
    for (int i = 0; i < 6; ++i) {
        stats.model_usage["model-" + std::to_string(i)] = usage(100 - i, 0);
    }

    const auto lines = ui::render_models_tab(theme, stats, 80, 0, 0);
    const std::string text = joined(lines);

    // Four models are shown at a time, and the hint says how many there are.
    REQUIRE(text.find("model-3") != std::string::npos);
    REQUIRE(text.find("model-4") == std::string::npos);
    REQUIRE(text.find("1-4 of 6 models (↑↓ to scroll)") != std::string::npos);
}

TEST_CASE("The tab of the models of the statistics panel has no chart without the days") {
    const ui::Theme theme = ui::Theme::dark();

    session::Stats stats;
    stats.model_usage["claude-sonnet-4-5"] = usage(1000, 500);
    stats.daily_model_tokens["2026-03-01"] = {{"claude-sonnet-4-5", 100}};

    const auto lines = ui::render_models_tab(theme, stats, 80, 0, 0);
    const std::string text = joined(lines);

    REQUIRE(text.find("Tokens per Day") == std::string::npos);
    REQUIRE(text.find("• Sonnet 4.5 (100.0%)") != std::string::npos);
}

TEST_CASE("The length of a session carries every part of it") {
    // The reference writes every part of the length, and rounds the
    // seconds with the carry-over.
    REQUIRE(ui::format_session_length(24 * 60 * 60 * 1000) == "1d 0h 0m");
    REQUIRE(ui::format_session_length(60 * 60 * 1000) == "1h 0m 0s");
    REQUIRE(ui::format_session_length(2 * 60 * 1000) == "2m 0s");
    REQUIRE(ui::format_session_length(119500) == "2m 0s");
    REQUIRE(ui::format_session_length(0) == "0s");
}

TEST_CASE("The panel of the statistics draws the message alone when there are no statistics") {
    const TempHome home;
    const ui::Theme theme = ui::Theme::dark();
    ui::StatsPanel panel(theme, "/tmp");

    // The panel reads the transcripts in the background, so that it opens at
    // once; the test waits for the numbers.
    panel.wait();

    const auto lines = panel.render(80);

    // The pane, the rule and the strip of the tabs are not drawn at all.
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].empty());
    REQUIRE(plain(lines[1]) == "No stats available yet. Start using Claude Code!");
}

TEST_CASE("The panel of the statistics names the model the sessions used the most") {
    const TempHome home;
    write_transcript(home.path() / "projects" / "-one" / "a.jsonl", 1, "claude-sonnet-4-5", 1000, 500);

    const ui::Theme theme = ui::Theme::dark();
    ui::StatsPanel panel(theme, "/tmp");

    // The panel reads the transcripts in the background, so that it opens at
    // once; the test waits for the numbers.
    panel.wait();

    const std::string text = joined(panel.render(80));

    // The name of the model is the one of the reference, not its id.
    REQUIRE(text.find("Favorite model: Sonnet 4.5") != std::string::npos);
    REQUIRE(text.find("claude-sonnet-4-5") == std::string::npos);

    // The strip of the tabs keeps the gap of the empty title of the
    // reference, so there are four columns before the name of the tab.
    const std::string strip = plain({panel.render(80)[2]});
    REQUIRE(strip.rfind("    Overview", 0) == 0);

    // The block of the usage and the block of the activity are a row apart.
    const std::vector<std::string> lines = panel.render(80);
    bool empty_between = false;
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (plain(lines[i]).find("Favorite model:") != std::string::npos) {
            empty_between = plain(lines[i + 1]).find_first_not_of(' ') == std::string::npos;
            break;
        }
    }
    REQUIRE(empty_between);
}
