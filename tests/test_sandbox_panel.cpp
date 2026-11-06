#include <catch2/catch_test_macros.hpp>

#include "sandbox/manager.hpp"
#include "sandbox/settings.hpp"
#include "ui/ansi.hpp"
#include "ui/figures.hpp"
#include "ui/settings_tabs.hpp"
#include "ui/theme.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace cc;

namespace cc::ui {

/// The panel of the settings of the sandbox.
///
/// The panel is built by the library; its header is not part of the public
/// API of the port, so the test declares it here.
std::unique_ptr<SettingsTabs> sandbox_settings_panel(Theme theme, sandbox::Manager& manager,
                                                      std::function<void(const sandbox::Settings&)> apply);

} // namespace cc::ui

namespace {

/// True when the line holds nothing but the padding of the pane.
bool blank(const std::string& line) {
    for (const char c : line) {
        if (c != ' ') return false;
    }
    return true;
}

/// The panel of the sandbox, with the sandbox turned on.
std::unique_ptr<ui::SettingsTabs> panel_of(sandbox::Manager& manager) {
    return ui::sandbox_settings_panel(ui::Theme::dark(), manager,
                                      [](const sandbox::Settings&) {});
}

/// Every line of the panel, joined with a newline.
std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) out += line + "\n";
    return out;
}

/// The line of the panel that holds the given text.
std::string row_with(const std::vector<std::string>& lines, const std::string& text) {
    for (const std::string& line : lines) {
        if (ui::strip(line).find(text) != std::string::npos) return line;
    }
    return {};
}

} // namespace

TEST_CASE("The panel of the sandbox leaves two rows between the tabs and the body") {
    sandbox::Settings settings;
    settings.enabled = true;
    sandbox::Manager manager(settings);

    const auto panel = panel_of(manager);
    const std::vector<std::string> lines = panel->render(80);

    REQUIRE(lines.size() > 5);

    // The pane of the reference leaves an empty line above its rule.
    REQUIRE(lines[0].empty());
    REQUIRE(ui::strip(lines[1]).find("─") != std::string::npos);

    // The strip of the tabs names the panel.
    REQUIRE(lines[2].find("Sandbox:") != std::string::npos);

    // The reference leaves an empty line under the strip of the tabs, and
    // the body of the sandbox is wrapped in a box with a row of padding
    // above it — two empty rows before the body.
    REQUIRE(blank(lines[3]));
    REQUIRE(blank(lines[4]));

    // The body of the tab.
    REQUIRE_FALSE(blank(lines[5]));
}

TEST_CASE("The body of every tab of the sandbox is wrapped in the padding of the reference") {
    sandbox::Settings settings;
    settings.enabled = true;
    sandbox::Manager manager(settings);

    const auto panel = panel_of(manager);

    // The panel shows one tab at a time; the tabs of the reference are
    // walked in turn.
    for (size_t tab = 0; tab < 4; ++tab) {
        panel->set_selected(tab);
        const std::vector<std::string> lines = panel->render(80);

        REQUIRE(lines.size() > 5);

        // The empty line under the strip of the tabs and the padding above
        // the body.
        REQUIRE(blank(lines[3]));
        REQUIRE(blank(lines[4]));

        // The body of the tab.
        REQUIRE_FALSE(blank(lines[5]));

        // The padding below the body.
        REQUIRE(blank(lines.back()));
    }
}

TEST_CASE("The tab of the mode of the sandbox names the mode that is in use") {
    sandbox::Settings settings;
    settings.enabled = true;
    settings.auto_allow_bash_if_sandboxed = true;
    sandbox::Manager manager(settings);

    const auto panel = panel_of(manager);
    const std::vector<std::string> lines = panel->render(80);

    // The body of the tab of the mode is the one that is shown first.
    std::string body;
    for (const std::string& line : lines) {
        body += ui::strip(line);
        body += "\n";
    }

    REQUIRE(body.find("Configure Mode:") != std::string::npos);
    REQUIRE(body.find("Sandbox BashTool, with auto-allow (current)") != std::string::npos);
}

TEST_CASE("The options of the sandbox carry their number") {
    sandbox::Settings settings;
    settings.enabled = true;
    sandbox::Manager manager(settings);

    const auto panel = panel_of(manager);
    const std::vector<std::string> lines = panel->render(80);

    // The number of the option is written before its label, padded to the
    // width of the largest number plus two, and the pointer and the number
    // are one space apart.
    REQUIRE(ui::strip(row_with(lines, "Sandbox BashTool, with auto-allow")) ==
            "  " + std::string(ui::figures::kPointer) +
                " 1. Sandbox BashTool, with auto-allow (current)");
    REQUIRE(ui::strip(row_with(lines, "Sandbox BashTool, with regular permissions")) ==
            "    2. Sandbox BashTool, with regular permissions");
    REQUIRE(ui::strip(row_with(lines, "No Sandbox")) == "    3. No Sandbox");
}

TEST_CASE("The options of the sandbox that are not focused keep the colour of the text") {
    const ui::Theme theme = ui::Theme::dark();
    sandbox::Settings settings;
    settings.enabled = true;
    sandbox::Manager manager(settings);

    const auto panel = panel_of(manager);
    const std::vector<std::string> lines = panel->render(80);

    // The label of the row that is not focused is in the colour of the
    // text: the number before it is dim, and the label follows the reset.
    const std::string other = row_with(lines, "Sandbox BashTool, with regular permissions");
    REQUIRE_FALSE(other.empty());
    REQUIRE(other.find(std::string(ui::kReset) + "Sandbox BashTool, with regular permissions") !=
            std::string::npos);

    // The label of the row that is focused is in the colour of the
    // suggestion.
    const std::string chosen = row_with(lines, "Sandbox BashTool, with auto-allow");
    REQUIRE_FALSE(chosen.empty());
    REQUIRE(chosen.find(ui::style_sequence(ui::Style::fg(theme.suggestion)) +
                        "Sandbox BashTool, with auto-allow") != std::string::npos);
}

TEST_CASE("The labels of the explanations of the sandbox are dim") {
    const ui::Theme theme = ui::Theme::dark();
    sandbox::Settings settings;
    settings.enabled = true;
    sandbox::Manager manager(settings);

    const auto panel = panel_of(manager);
    const std::string body = joined(panel->render(80));

    // The label is bold and dim, the way the reference writes it.
    ui::Style label;
    label.bold = true;
    label.foreground = theme.inactive;
    REQUIRE(body.find(ui::style_sequence(label) + "Auto-allow mode:") != std::string::npos);
}

TEST_CASE("The warnings of the dependencies of the sandbox are a row apart from the line above") {
    sandbox::Settings settings;
    settings.enabled = false;
    sandbox::Manager manager(settings);

    const std::vector<std::string> warnings = manager.dependency_warnings();
    if (warnings.empty()) {
        WARN("the dependencies of the sandbox are all there");
        return;
    }

    const auto panel = panel_of(manager);
    const std::vector<std::string> lines = panel->render(80);

    // The line that says that the sandbox is not enabled, and the empty
    // line that follows it.
    size_t at = 0;
    while (at < lines.size() && ui::strip(lines[at]).find("Sandbox is not enabled") ==
                                   std::string::npos) {
        ++at;
    }
    REQUIRE(at + 1 < lines.size());
    REQUIRE(blank(ui::strip(lines[at + 1])));

    // The warnings are in the dim colour of the reference.
    const ui::Theme theme = ui::Theme::dark();
    REQUIRE(lines[at + 2].find(ui::style_sequence(ui::Style::fg(theme.inactive))) !=
            std::string::npos);
}
