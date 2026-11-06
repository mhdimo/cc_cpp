#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/figures.hpp"
#include "ui/settings_panel.hpp"
#include "ui/theme.hpp"

using namespace cc;

namespace {

/// The row of the panel that carries the given label.
std::string row_with(const std::vector<std::string>& lines, const std::string& label) {
    for (const std::string& line : lines) {
        if (ui::strip(line).find(label) != std::string::npos) return line;
    }
    return {};
}

} // namespace

TEST_CASE("The chosen row of the settings panel is painted in the colour of the suggestion") {
    const ui::Theme theme = ui::Theme::dark();
    const ui::SettingsPanel panel(theme, {{"Model", "Sonnet", "model", ""},
                                           {"Theme", "Dark", "theme", ""}});

    const auto lines = panel.render(80);

    const std::string chosen = row_with(lines, "Model");
    REQUIRE_FALSE(chosen.empty());

    const std::string sequence = ui::style_sequence(ui::Style::fg(theme.suggestion));
    REQUIRE_FALSE(sequence.empty());

    // The whole field of the label — the pointer and the label itself — is
    // in the colour of the suggestion, the way the reference paints it.
    REQUIRE(chosen.find(sequence + std::string(ui::figures::kPointer) + " Model") != std::string::npos);

    // The value of the chosen row is in the same colour.
    REQUIRE(chosen.find(sequence + "Sonnet") != std::string::npos);
}

TEST_CASE("The rows of the settings panel that are not chosen keep the colour of the terminal") {
    const ui::Theme theme = ui::Theme::dark();
    const ui::SettingsPanel panel(theme, {{"Model", "Sonnet", "model", ""},
                                           {"Theme", "Dark", "theme", ""}});

    const auto lines = panel.render(80);

    const std::string other = row_with(lines, "Theme");
    REQUIRE_FALSE(other.empty());

    // The field of the label holds the pointer's room, and the label is in
    // the colour of the terminal.
    REQUIRE(other.find("  Theme") != std::string::npos);
    REQUIRE(other.find(ui::style_sequence(ui::Style::fg(theme.suggestion))) == std::string::npos);
}
