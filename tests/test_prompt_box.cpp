#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/prompt_input.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// The text of the lines, without the colours.
std::string plain(const std::vector<std::string>& lines) {
    std::string joined;
    for (const auto& line : lines) {
        joined += ui::strip(line);
        joined += "\n";
    }
    return joined;
}

/// The number of lines of the input that the box may draw.
///
/// The box may not be taller than the screen, so the reference keeps half
/// of the screen for the input and the rest for the footer.
size_t visible_lines() {
    const size_t rows = static_cast<size_t>(ui::Terminal::instance().height());
    const size_t half = rows / 2;
    return std::max<size_t>(3, half > 5 ? half - 5 : 0);
}

} // namespace

TEST_CASE("The shortcuts menu stays inside the terminal") {
    // The menu under the prompt box is drawn two columns to the right of
    // the edge of the terminal.
    const int width = 80;
    const auto menu = ui::PromptInput::shortcuts_menu(width - 2);

    for (const auto& line : menu) {
        REQUIRE(cc::display_width(ui::strip(line)) <= static_cast<size_t>(width - 2));
    }
}

TEST_CASE("The shortcuts menu wraps its third column") {
    const auto menu = ui::PromptInput::shortcuts_menu(78);
    const std::string text = plain(menu);

    // The third column takes the room that is left — seventeen columns at
    // eighty columns, the two columns of the padding of the reference's box
    // taken off — so the entries that do not fit are wrapped inside it
    // instead of being cut off by the terminal.
    REQUIRE(text.find("ctrl + z to\n") != std::string::npos);
    REQUIRE(text.find("suspend") != std::string::npos);
    REQUIRE(text.find("ctrl + v to paste\n") != std::string::npos);
    REQUIRE(text.find("images") != std::string::npos);
}

TEST_CASE("The shortcuts menu names the keys the actions are bound to") {
    // The reference writes the key the action is bound to, so the undo is
    // named by the key of the binding and not by the key of the fallback.
    // The test assumes that the user has written no keybindings of their
    // own.
    const auto menu = ui::PromptInput::shortcuts_menu(140);
    const std::string text = plain(menu);

    REQUIRE(text.find("ctrl + shift + - to undo") != std::string::npos);
    REQUIRE(text.find("meta + p to switch model") != std::string::npos);
    REQUIRE(text.find("ctrl + o for verbose output") != std::string::npos);
    REQUIRE(text.find("ctrl + t to toggle tasks") != std::string::npos);
    REQUIRE(text.find("shift + tab to auto-accept edits") != std::string::npos);
    REQUIRE(text.find("ctrl + s to stash prompt") != std::string::npos);
    REQUIRE(text.find("ctrl + g to edit in $EDITOR") != std::string::npos);
    REQUIRE(text.find("ctrl + v to paste images") != std::string::npos);
}

TEST_CASE("The prompt box shows only the lines around the caret") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    // A text of many lines.
    std::string text;
    for (int i = 0; i < 40; ++i) text += "line " + std::to_string(i) + "\n";
    input.set_text(text);

    const auto lines = input.render_box(40, false);

    // The box is not taller than the screen: the rule above, the lines of
    // the text and the rule below.
    REQUIRE(lines.size() <= visible_lines() + 2);

    // The lines around the caret are the ones that are shown.
    REQUIRE(plain(lines).find("line 39") != std::string::npos);
    REQUIRE(plain(lines).find("line 0\n") == std::string::npos);
}

TEST_CASE("The prompt box shows the hint of the arguments of the command") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");
    input.set_text("/model ");

    const auto lines = input.render_box(40, false, "[model]");
    const std::string text = plain(lines);

    // The hint stands right after the text.
    const size_t command = text.find("/model");
    const size_t hint = text.find("[model]");
    REQUIRE(command != std::string::npos);
    REQUIRE(hint != std::string::npos);
    REQUIRE(hint > command);

    // The hint is dim.
    REQUIRE(lines[1].find("\x1b[2m") != std::string::npos);
}

TEST_CASE("The prompt box leaves the hint of the arguments out when there is no text") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    const auto lines = input.render_box(40, false, "[model]");
    REQUIRE(plain(lines).find("[model]") == std::string::npos);
}

TEST_CASE("The status line is indented by its padding") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    const auto lines = input.render(40, false, {}, "hello", false, {}, {}, 3);

    // The status line sits below the box and is indented by two columns
    // and by the padding of the setting.
    REQUIRE(ui::strip(lines[3]) == "     hello");
}

TEST_CASE("Every line of the status line is indented") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    const auto lines = input.render(40, false, {}, "one\ntwo", false);

    REQUIRE(ui::strip(lines[3]) == "  one");
    REQUIRE(ui::strip(lines[4]) == "  two");
}

TEST_CASE("The bash mode leaves the status line out") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");
    input.set_text("!ls");

    const auto lines = input.render(40, false, {}, "hello", /*suppress_hint=*/true);

    // The status line is left out in the bash mode.
    REQUIRE(plain(lines).find("hello") == std::string::npos);

    // The bash mode is named all the same.
    REQUIRE(plain(lines).find("! for bash mode") != std::string::npos);
}

TEST_CASE("The prompt box names the paste") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    input.set_pasting(true);
    REQUIRE(input.hint_line(false) == "Pasting text…");

    // The status line is left out while the user is pasting.
    const auto lines = input.render(40, false, {}, "hello", /*suppress_hint=*/true);
    REQUIRE(plain(lines).find("hello") == std::string::npos);
    REQUIRE(plain(lines).find("Pasting text…") != std::string::npos);
}

TEST_CASE("The notification is raised at the right of the footer") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    const auto lines = input.render(80, false, {}, {}, false, {}, "Esc again to clear");
    const std::string footer = ui::strip(lines.back());

    REQUIRE(footer.find("Esc again to clear") != std::string::npos);

    // The notification ends two columns short of the edge of the terminal,
    // the way the reference leaves the margin of its footer.
    REQUIRE(footer.size() == 78);
}

TEST_CASE("The notification is raised on the first row of the footer") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    const auto lines = input.render(80, false, {}, "hello", false, {}, "Esc again to clear");

    // The status line and the notification stand on the same row, the way
    // the reference draws them.
    const std::string row = ui::strip(lines[3]);
    REQUIRE(row.find("hello") != std::string::npos);
    REQUIRE(row.find("Esc again to clear") != std::string::npos);
}

TEST_CASE("The notification is dim when it carries no colour of its own") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    const auto lines = input.render(80, false, {}, {}, false, {}, "Esc again to clear");

    // The reference dims a notification that has no colour of its own.
    REQUIRE(lines.back().find("\x1b[2m") != std::string::npos);
}

TEST_CASE("The notification stands on its own row in a narrow terminal") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    const auto lines = input.render(60, false, {}, {}, false, {}, "Esc again to clear");

    // Below eighty columns the footer is a column: the notification stands
    // on a row of its own, under the hint.
    REQUIRE(ui::strip(lines.back()) == "  Esc again to clear");
    REQUIRE(ui::strip(lines[lines.size() - 2]).find("Esc again to clear") == std::string::npos);
}

TEST_CASE("The ansi themes draw the rule in the white of the terminal") {
    ui::PromptInput input(ui::Theme::ansi_dark(), "/tmp");

    const auto lines = input.render_box(20, false);

    // The rule is drawn with the code of the palette of the terminal.
    REQUIRE(lines[0].find("\x1b[37m") != std::string::npos);
}

TEST_CASE("The themes of the colours draw the rule in their own colour") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    const auto lines = input.render_box(20, false);

    // The rule is drawn with the twenty-four bit colour of the theme.
    REQUIRE(lines[0].find("\x1b[38;2;136;136;136m") != std::string::npos);
}

TEST_CASE("The prompt box measures the text at four columns less than the width") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    // A word of seventy-seven columns is too wide for the box of eighty
    // columns: the reference measures the text at seventy-six columns.
    input.set_text(std::string(77, 'a'));

    const auto lines = input.render_box(80, false);
    const std::string text = plain(lines);

    // The word is split: the first row holds seventy-six columns of it and
    // the second one the last column.
    REQUIRE(text.find("❯ " + std::string(76, 'a')) != std::string::npos);
    REQUIRE(text.find(std::string(76, 'a') + "\n  a") != std::string::npos);
}

TEST_CASE("The prompt box fills the row before it splits a long word") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");

    // A word of eighty columns that follows a space: the reference fills
    // the row with the beginning of the word and splits it there.
    input.set_text("hello " + std::string(80, 'b'));

    const auto lines = input.render_box(80, false);
    const std::string text = plain(lines);

    REQUIRE(text.find("hello " + std::string(70, 'b')) != std::string::npos);
    REQUIRE(text.find(std::string(70, 'b') + "\n  " + std::string(10, 'b')) != std::string::npos);
}

TEST_CASE("The command of the input is painted in the colour of the suggestions") {
    const auto theme = ui::Theme::dark();
    ui::PromptInput input(theme, "/tmp");
    input.set_text("/model");

    const auto lines = input.render_box(80, false);

    // The reference paints a command it knows in the colour of the
    // suggestions.
    const std::string style = ui::style_sequence(ui::Style::fg(theme.suggestion));
    REQUIRE(lines[1].find(style) != std::string::npos);
}

TEST_CASE("The command of the middle of the input is completed in dim") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");
    input.set_text("hello /mod");

    const auto lines = input.render_box(80, false);

    // The rest of the name of the command stands right after the caret.
    REQUIRE(plain(lines).find("hello /model") != std::string::npos);
    REQUIRE(lines[1].find("\x1b[2m") != std::string::npos);
}

