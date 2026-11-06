#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/mobile_view.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

using namespace cc;

namespace {

/// The row of the view that names the two applications.
std::string platform_row(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        const std::string plain = ui::strip(line);
        if (plain.find("tab to switch") != std::string::npos) return plain;
    }
    return {};
}

} // namespace

TEST_CASE("The mobile view puts the slash between the two applications") {
    const ui::MobileView view(ui::Theme::dark());
    const auto lines = view.render(80, 40);

    const std::string row = platform_row(lines);
    REQUIRE_FALSE(row.empty());

    // The slash sits right between the two names, with no room before it.
    REQUIRE(row.find("iOS / Android") != std::string::npos);
    REQUIRE(row.find("iOS  / Android") == std::string::npos);

    // The hint keeps the two columns of room the reference leaves between
    // the names and it.
    REQUIRE(row.find("iOS / Android  (tab to switch, esc to close)") != std::string::npos);
}

TEST_CASE("The mobile view marks the application that is showing") {
    const ui::MobileView view(ui::Theme::dark());
    const auto lines = view.render(80, 40);

    std::string raw;
    for (const auto& line : lines) {
        if (ui::strip(line).find("tab to switch") != std::string::npos) raw = line;
    }
    REQUIRE_FALSE(raw.empty());

    // The application of the iPhone is the one that shows, so it is bold
    // and underlined; the other one is plain.
    REQUIRE(raw.find("\x1b[1;4miOS") != std::string::npos);
    REQUIRE(raw.find("Android") != std::string::npos);
}

TEST_CASE("The mobile view writes the address of the application") {
    const ui::MobileView view(ui::Theme::dark());
    const auto lines = view.render(80, 40);

    REQUIRE(view.url() == "https://apps.apple.com/app/claude-by-anthropic/id6473753684");

    const std::string last = ui::strip(lines.back());
    REQUIRE(last == "  " + view.url());
}

TEST_CASE("The mobile view of the modal slot is topped by a rule of its own") {
    ui::MobileView view(ui::Theme::dark());
    view.set_modal(true);

    const auto lines = view.render(80, 40);

    // The rule of the modal slot is drawn in the colour of the permission.
    REQUIRE(ui::strip(lines[0]).find("▔") != std::string::npos);
    REQUIRE(lines[0].find("\x1b[38;2;177;185;249m") != std::string::npos);

    // The contents of the pane sit three columns in.
    const std::string last = ui::strip(lines.back());
    REQUIRE(last == "   " + view.url());
}
