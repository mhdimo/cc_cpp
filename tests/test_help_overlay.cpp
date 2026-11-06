#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/help_overlay.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

using namespace cc;

namespace {

/// The text of the overlay, with the colours taken away.
std::string plain(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += ui::strip(line);
        out += "\n";
    }
    return out;
}

/// The line of the pane that holds the title and the tabs.
std::string header_line(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (std::string(ui::strip(line)).find("Claude Code v") != std::string::npos) return line;
    }
    return {};
}

/// The style sequence in front of `text` in `line`.
std::string style_of(const std::string& line, const std::string& text) {
    const size_t at = line.find(text);
    if (at == std::string::npos) return {};
    const size_t start = line.rfind("\x1b[", at);
    if (start == std::string::npos) return {};
    return line.substr(start, at - start);
}

} // namespace

TEST_CASE("The help overlay is at most half the screen tall") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::HelpCommand> commands;
    for (int i = 0; i < 30; ++i) {
        commands.push_back({"/command" + std::to_string(i), "The description"});
    }
    const ui::HelpOverlay overlay(theme, commands, {});

    // The reference wraps the pane in a box of half the screen, so the rows
    // that do not fit are cut off.
    REQUIRE(overlay.render(80, 40).size() <= 20);
    REQUIRE(overlay.render(80, 24).size() <= 12);
    REQUIRE(overlay.render(80, 80).size() <= 40);

    // The general tab is cut off as well.
    REQUIRE(overlay.render(80, 24).size() <= 12);
}

TEST_CASE("The help overlay wraps the introduction with a margin on the right") {
    const auto theme = ui::Theme::dark();
    const ui::HelpOverlay overlay(theme, {}, {});

    const auto lines = overlay.render(60, 80);

    // The pane of the reference has a padding of two columns on both sides,
    // so the text of the introduction stops two columns before the edge.
    size_t widest = 0;
    bool seen_intro = false;
    for (const auto& line : lines) {
        const std::string text = ui::strip(line);
        if (text.find("Claude understands") != std::string::npos) seen_intro = true;
        if (!seen_intro) continue;
        if (text.find("Shortcuts") != std::string::npos) break;
        widest = std::max(widest, cc::display_width(text));
    }

    REQUIRE(seen_intro);
    REQUIRE(widest > 0);
    REQUIRE(widest <= 60 - 2);
}

TEST_CASE("The help overlay writes the message of the empty list on the tab of the user") {
    const auto theme = ui::Theme::dark();

    // The list of the built-in commands is never empty in the reference, but
    // the tab of the user can be.
    ui::HelpOverlay overlay(theme, {}, {});

    // The tab of the built-in commands keeps its title above the empty list.
    overlay.handle_key(ui::Key{ui::Key::Type::Right, {}});
    const std::string builtin = plain(overlay.render(80, 80));
    REQUIRE(builtin.find("Browse default commands:") != std::string::npos);
    REQUIRE(builtin.find("No custom commands found") == std::string::npos);

    // The tab of the commands of the user says that there is nothing there.
    overlay.handle_key(ui::Key{ui::Key::Type::Right, {}});
    const std::string custom = plain(overlay.render(80, 80));
    REQUIRE(custom.find("No custom commands found") != std::string::npos);
    REQUIRE(custom.find("Browse custom commands:") == std::string::npos);
}

TEST_CASE("The description of a command is dimmed, and the one of the focused command is not") {
    const auto theme = ui::Theme::dark();

    const std::vector<ui::HelpCommand> commands = {
        {"/clear", "Clear the conversation"},
        {"/compact", "Compact the conversation"},
    };
    ui::HelpOverlay overlay(theme, commands, {});

    // The tab of the commands of the CLI.
    overlay.handle_key(ui::Key{ui::Key::Type::Right, {}});
    const auto lines = overlay.render(80, 80);

    std::string focused;
    std::string other;
    for (const auto& line : lines) {
        const std::string text = ui::strip(line);
        if (text.find("Clear the conversation") != std::string::npos) {
            focused = style_of(line, "Clear the conversation");
        }
        if (text.find("Compact the conversation") != std::string::npos) {
            other = style_of(line, "Compact the conversation");
        }
    }

    // Both of them are dimmed with the attribute of the terminal.
    REQUIRE(focused.rfind("\x1b[2", 0) == 0);
    REQUIRE(other.rfind("\x1b[2", 0) == 0);

    // The one of the focused command takes the colour of the highlight as
    // well, the way the reference paints it.
    REQUIRE(focused.find("38;2;") != std::string::npos);
    REQUIRE(other.find("38;2;") == std::string::npos);
}

TEST_CASE("The tab that is showing is bold, and it is inverted once the list has the focus") {
    const auto theme = ui::Theme::dark();
    ui::HelpOverlay overlay(theme, {}, {});

    // While the header holds the focus the tab that is showing is painted
    // with the colour of the pane.
    const std::string header = header_line(overlay.render(80, 80));
    const std::string tab = style_of(header, " general ");
    REQUIRE(tab.find("1;") != std::string::npos);
    REQUIRE(tab.find("48;2;") != std::string::npos);
    REQUIRE(tab.find("7m") == std::string::npos);

    // The first ↓ leaves the header, and the tab that is showing becomes
    // inverted text.
    overlay.handle_key(ui::Key{ui::Key::Type::Down, {}});
    REQUIRE_FALSE(overlay.header_focused());

    const std::string inverted = style_of(header_line(overlay.render(80, 80)), " general ");
    REQUIRE(inverted.find("1;") != std::string::npos);
    REQUIRE(inverted.find("7m") != std::string::npos);
    REQUIRE(inverted.find("48;2;") == std::string::npos);
}

TEST_CASE("The up arrow on the first entry of the list goes back to the header") {
    const auto theme = ui::Theme::dark();
    const std::vector<ui::HelpCommand> commands = {{"/clear", "Clear the conversation"}};
    ui::HelpOverlay overlay(theme, commands, {});

    // The tab of the commands of the CLI.
    overlay.handle_key(ui::Key{ui::Key::Type::Right, {}});

    // The first ↓ leaves the header, the second one moves the pointer.
    overlay.handle_key(ui::Key{ui::Key::Type::Down, {}});
    REQUIRE_FALSE(overlay.header_focused());
    overlay.handle_key(ui::Key{ui::Key::Type::Down, {}});

    // The up arrow on the first entry gives the header its focus back.
    overlay.handle_key(ui::Key{ui::Key::Type::Up, {}});
    overlay.handle_key(ui::Key{ui::Key::Type::Up, {}});
    REQUIRE(overlay.header_focused());
}

TEST_CASE("The return key does not close the help pane") {
    ui::HelpOverlay overlay(ui::Theme::dark(), {}, {});

    // The reference does nothing when the return key is pressed in the
    // pane.
    REQUIRE(overlay.handle_key(ui::Key{ui::Key::Type::Enter, {}}));
}

TEST_CASE("The Ctrl-C of the help pane asks for a second one") {
    ui::HelpOverlay overlay(ui::Theme::dark(), {}, {});

    // The first Ctrl-C does not close the pane.
    REQUIRE(overlay.handle_key(ui::Key{ui::Key::Type::CtrlC, {}}));
    REQUIRE(overlay.exit_pending());
    REQUIRE(plain(overlay.render(80, 80)).find("Press Ctrl-C again to exit") != std::string::npos);

    // The second one closes it.
    REQUIRE_FALSE(overlay.handle_key(ui::Key{ui::Key::Type::CtrlC, {}}));
}

TEST_CASE("The shift and the tab key move back through the tabs of the help pane") {
    ui::HelpOverlay overlay(ui::Theme::dark(), {}, {});

    REQUIRE(overlay.tab() == ui::HelpTab::General);

    // The tab key moves forward.
    overlay.handle_key(ui::Key{ui::Key::Type::Tab, {}});
    REQUIRE(overlay.tab() == ui::HelpTab::Commands);

    // The shift and the tab key moves back, and it goes round.
    overlay.handle_key(ui::Key{ui::Key::Type::ShiftTab, {}});
    REQUIRE(overlay.tab() == ui::HelpTab::General);
    overlay.handle_key(ui::Key{ui::Key::Type::ShiftTab, {}});
    REQUIRE(overlay.tab() == ui::HelpTab::CustomCommands);
}

TEST_CASE("The list of the commands does not jump back to the top") {
    const auto theme = ui::Theme::dark();

    std::vector<ui::HelpCommand> commands;
    for (int i = 0; i < 30; ++i) {
        commands.push_back({"/command" + std::to_string(i), "The description"});
    }
    ui::HelpOverlay overlay(theme, commands, {});

    // The tab of the commands of the CLI.
    overlay.handle_key(ui::Key{ui::Key::Type::Right, {}});
    overlay.render(80, 40);

    // The list is scrolled down through the commands.
    for (int i = 0; i < 10; ++i) {
        overlay.handle_key(ui::Key{ui::Key::Type::Down, {}});
    }
    REQUIRE(plain(overlay.render(80, 40)).find("/command0 ") == std::string::npos);

    // A single up arrow does not put the list back at its first row: the
    // window of the list is sticky.
    overlay.handle_key(ui::Key{ui::Key::Type::Up, {}});
    REQUIRE(plain(overlay.render(80, 40)).find("/command0 ") == std::string::npos);
}

TEST_CASE("The address of the help is a link") {
    // The terminal is told that it can follow a hyperlink.
    setenv("FORCE_HYPERLINK", "1", 1);

    ui::HelpOverlay overlay(ui::Theme::dark(), {}, {});
    const auto lines = overlay.render(80, 80);

    bool found = false;
    for (const auto& line : lines) {
        if (line.find("code.claude.com/docs/en/overview") == std::string::npos) continue;
        found = line.find("\x1b]8;;") != std::string::npos;
    }
    REQUIRE(found);

    unsetenv("FORCE_HYPERLINK");
}
