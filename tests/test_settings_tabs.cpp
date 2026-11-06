#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/figures.hpp"
#include "ui/settings_panel.hpp"
#include "ui/settings_tabs.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// The row of the panel that carries the given label.
std::string row_with(const std::vector<std::string>& lines, const std::string& label) {
    for (const std::string& line : lines) {
        if (ui::strip(line).find(label) != std::string::npos) return line;
    }
    return {};
}

/// A key of the given kind.
ui::Key key_of(ui::Key::Type type, std::string text = {}) {
    ui::Key key;
    key.type = type;
    key.text = std::move(text);
    return key;
}

/// The panel of the settings, with its tab of the configuration shown.
ui::SettingsTabs settings_panel(const ui::Theme& theme) {
    std::vector<ui::Setting> settings = {{"Model", "Sonnet", "model", ""},
                                          {"Theme", "Dark mode", "theme", ""}};
    std::vector<std::unique_ptr<ui::SettingsTab>> tabs;
    tabs.push_back(std::make_unique<ui::StatusTab>(theme, std::vector<ui::StatusTab::Property>{}));
    tabs.push_back(std::make_unique<ui::ConfigTab>(theme, settings));
    return ui::SettingsTabs(theme, std::move(tabs), 1);
}

} // namespace

TEST_CASE("The field of the search of the settings panel is as wide as its box") {
    const ui::Theme theme = ui::Theme::dark();
    const ui::SettingsPanel panel(theme, {{"Model", "Sonnet", "model", ""}});

    const auto lines = panel.render(80);

    // The three lines of the box have the same width.
    std::string top;
    std::string middle;
    std::string bottom;
    for (const std::string& line : lines) {
        const std::string plain = ui::strip(line);
        if (plain.empty()) continue;
        if (plain.rfind("╭", 0) == 0) top = plain;
        if (plain.rfind("│", 0) == 0) middle = plain;
        if (plain.rfind("╰", 0) == 0) bottom = plain;
    }

    REQUIRE_FALSE(top.empty());
    REQUIRE_FALSE(middle.empty());
    REQUIRE_FALSE(bottom.empty());
    REQUIRE(cc::display_width(middle) == cc::display_width(top));
    REQUIRE(cc::display_width(bottom) == cc::display_width(top));
}

TEST_CASE("The list of the settings is cut to the height of the pane") {
    const ui::Theme theme = ui::Theme::dark();
    std::vector<ui::Setting> settings;
    for (int i = 0; i < 20; ++i) {
        settings.push_back({"Setting " + std::to_string(i), "value", "id-" + std::to_string(i), ""});
    }
    const ui::SettingsPanel panel(theme, settings);

    // The pane of the reference is at most thirty rows tall, and ten of
    // them are kept for the search, the gaps and the footer.
    const auto lines = panel.render(80, 24);

    std::string text;
    for (const std::string& line : lines) text += ui::strip(line) + "\n";

    // Nine of the settings are shown, and the rows that are out of the way
    // are counted.
    REQUIRE(text.find("↓ 11 more below") != std::string::npos);
    REQUIRE(text.find("Setting 8") != std::string::npos);
    REQUIRE(text.find("Setting 9") == std::string::npos);
}

TEST_CASE("The keys of the list of the settings are the ones of the reference") {
    const ui::Theme theme = ui::Theme::dark();
    ui::SettingsPanel panel(theme, {{"Model", "Sonnet", "model", ""},
                                     {"Theme", "Dark mode", "theme", ""}});
    // The reference opens the tab of the configuration in its search mode.
    panel.set_searching(true);

    std::string picked;
    bool close = false;

    // The down arrow gives the focus to the list of the settings.
    REQUIRE(panel.handle(key_of(ui::Key::Type::Down), picked, close));
    REQUIRE_FALSE(panel.searching());
    REQUIRE_FALSE(close);

    // The space picks the setting that is chosen.
    REQUIRE(panel.handle(key_of(ui::Key::Type::Character, " "), picked, close));
    REQUIRE(picked == "model");
    REQUIRE_FALSE(close);

    // The slash takes the panel back to its search.
    REQUIRE(panel.handle(key_of(ui::Key::Type::Character, "/"), picked, close));
    REQUIRE(panel.searching());

    // The enter key leaves the search first, the way the reference does.
    REQUIRE(panel.handle(key_of(ui::Key::Type::Enter), picked, close));
    REQUIRE_FALSE(panel.searching());
    REQUIRE_FALSE(close);

    // The enter key saves the settings and closes the panel.
    REQUIRE(panel.handle(key_of(ui::Key::Type::Enter), picked, close));
    REQUIRE(close);
}

TEST_CASE("The tab of the configuration opens in its search mode") {
    const ui::Theme theme = ui::Theme::dark();
    ui::ConfigTab tab(theme, {{"Model", "Sonnet", "model", ""},
                               {"Theme", "Dark mode", "theme", ""}});

    // The hint of the tab is the one of the search mode of the reference.
    REQUIRE(tab.hint() == "Type to filter · Enter/↓ to select · ↑ to tabs · Esc to clear");

    // No row of the list is marked while the search has the focus.
    const auto lines = tab.render(80, 24);
    const std::string chosen = row_with(lines, "Model");
    REQUIRE_FALSE(chosen.empty());
    REQUIRE(chosen.find(ui::figures::kPointer) == std::string::npos);

    // The up arrow gives the focus to the strip of the tabs.
    const ui::Key up = key_of(ui::Key::Type::Up);
    const ui::TabResult result = tab.handle(up);
    REQUIRE(result.used);
    REQUIRE(result.focus == ui::TabResult::Focus::Header);

    // The strip of the tabs has the focus: the hint is the one of the
    // header, and the down arrow gives the focus back to the content.
    tab.set_header_focused(true);
    REQUIRE(tab.hint() == "←/→ tab · ↓ return · Esc to close");

    const ui::TabResult back = tab.handle(key_of(ui::Key::Type::Down));
    REQUIRE(back.used);
    REQUIRE(back.focus == ui::TabResult::Focus::Content);
}

TEST_CASE("The tab of the configuration is drawn in the inverted video") {
    const ui::Theme theme = ui::Theme::dark();
    ui::SettingsTabs panel = settings_panel(theme);

    const auto lines = panel.render(80, 24);

    // The strip of the tabs is the third line of the panel.
    const std::string& strip = lines[2];

    // The tab of the configuration is the one that is shown, and the header
    // does not have the focus: the tab is drawn in the inverted video.
    ui::Style inverse;
    inverse.bold = true;
    inverse.inverse = true;
    const std::string sequence = ui::style_sequence(inverse);
    REQUIRE_FALSE(sequence.empty());
    REQUIRE(strip.find(sequence + " Config ") != std::string::npos);
}

TEST_CASE("The body of the tab of the configuration is wrapped in a margin") {
    const ui::Theme theme = ui::Theme::dark();
    ui::SettingsTabs panel = settings_panel(theme);

    const auto lines = panel.render(80, 24);

    // The empty line under the strip of the tabs, and the margin of the tab
    // of the configuration: two empty rows before the body.
    REQUIRE(ui::strip(lines[3]).empty());
    REQUIRE(ui::strip(lines[4]).empty());
    REQUIRE_FALSE(ui::strip(lines[5]).empty());

    // The margin is below the footer of the tab as well.
    REQUIRE(ui::strip(lines.back()).empty());
}

TEST_CASE("The tab of the usage is measured against the width of the terminal") {
    const ui::Theme theme = ui::Theme::dark();
    ui::UsageTab::Limit limit;
    limit.title = "Current session";
    limit.utilization = 0.5;
    const ui::UsageTab tab(theme, {limit});

    // A terminal of sixty-four columns already has the wide shape: the
    // title is on a line of its own and the bar is fifty columns wide.
    const auto wide = tab.render(64, 24);
    REQUIRE(wide.size() >= 2);
    REQUIRE(ui::strip(wide[0]) == "Current session");
    REQUIRE(ui::strip(wide[1]).find("50% used") != std::string::npos);

    // A narrow terminal has the narrow shape: the bar is as wide as the
    // terminal less two columns.
    const auto narrow = tab.render(40, 24);
    REQUIRE(narrow.size() >= 2);
    REQUIRE(ui::strip(narrow[0]) == "Current session");
    REQUIRE(cc::display_width(ui::strip(narrow[1])) == 38);
}

TEST_CASE("The tab of the usage shows the error of the API and asks again") {
    const ui::Theme theme = ui::Theme::dark();
    ui::UsageTab tab(theme);
    tab.set_data({}, {}, "Failed to load usage data");

    const auto lines = tab.render(80, 24);
    REQUIRE(lines.size() == 1);
    REQUIRE(ui::strip(lines[0]) == "Error: Failed to load usage data");

    // The error is in the colour of the error.
    REQUIRE(lines[0].find(ui::style_sequence(ui::Style::fg(theme.error))) != std::string::npos);

    // The hint offers to ask again.
    REQUIRE(tab.hint() == "r to retry · Esc to cancel");
}
