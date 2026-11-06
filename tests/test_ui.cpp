#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/diff.hpp"
#include "ui/markdown.hpp"
#include "ui/prompt_input.hpp"
#include "ui/spinner.hpp"
#include "ui/theme.hpp"
#include "ui/tool_display.hpp"

#include "utils/string.hpp"

using namespace cc;

TEST_CASE("Styled text carries the colour and resets afterwards") {
    const ui::Style style = ui::Style::fg(ui::Rgb{215, 119, 87});
    const std::string text = ui::styled("hi", style);

    REQUIRE(text.find("\x1b[38;2;215;119;87m") != std::string::npos);
    REQUIRE(text.ends_with("\x1b[0m"));
}

TEST_CASE("Strip removes the escape sequences") {
    const std::string text = ui::styled("hi", ui::Style::fg(ui::Rgb{1, 2, 3}));
    REQUIRE(ui::strip(text) == "hi");
}

TEST_CASE("Markdown renders headings, bold and inline code") {
    const auto theme = ui::Theme::dark();
    const auto lines = ui::render_markdown("# Title\n\nSome **bold** text and `code`.\n", theme, 80);

    std::string joined;
    for (const auto& line : lines) {
        joined += line;
        joined += "\n";
    }

    REQUIRE(joined.find("Title") != std::string::npos);
    REQUIRE(joined.find("bold") != std::string::npos);
    REQUIRE(joined.find("code") != std::string::npos);
}

TEST_CASE("Markdown separates blocks with a blank line") {
    const auto theme = ui::Theme::dark();
    const auto lines = ui::render_markdown("one\n\ntwo\n", theme, 80);
    REQUIRE(lines.size() >= 3);
    REQUIRE(lines[1].empty());
}

TEST_CASE("Markdown keeps the indentation of a line of code") {
    const auto theme = ui::Theme::dark();
    const auto lines = ui::render_markdown("```\n    indented\n```\n", theme, 80);

    std::string joined;
    for (const auto& line : lines) {
        joined += ui::strip(line);
        joined += "\n";
    }

    REQUIRE(joined.find("    indented") != std::string::npos);
}

TEST_CASE("Diff finds the changed lines") {
    const auto lines = ui::diff_lines("a\nb\nc\n", "a\nB\nc\n");

    int added = 0;
    int removed = 0;
    for (const auto& line : lines) {
        if (line.kind == ui::DiffLine::Kind::Added) ++added;
        if (line.kind == ui::DiffLine::Kind::Removed) ++removed;
    }

    REQUIRE(added == 1);
    REQUIRE(removed == 1);
}

TEST_CASE("The diff summary reads like Claude Code's") {
    const auto lines = ui::diff_lines("a\nb\nc\n", "a\nB\nc\nd\n");
    const std::string summary = ui::diff_summary(lines);

    REQUIRE(summary.find("Added") != std::string::npos);
    REQUIRE(summary.find("line") != std::string::npos);
}

TEST_CASE("The prompt box draws a rule above and below the input") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");
    input.set_text("hello");

    const auto lines = input.render_box(40, false);
    REQUIRE(lines.size() >= 3);

    // The first and last lines are rules.
    REQUIRE(lines.front().find("─") != std::string::npos);
    REQUIRE(lines.back().find("─") != std::string::npos);

    // The input sits in between, behind the pointer.
    bool found = false;
    for (const auto& line : lines) {
        if (line.find("hello") != std::string::npos) found = true;
    }
    REQUIRE(found);
}

TEST_CASE("The prompt box switches to bash mode on a leading bang") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");
    input.set_text("!ls -la");

    REQUIRE(input.mode() == ui::InputMode::Bash);
    REQUIRE(input.value_for_mode() == "ls -la");
}

TEST_CASE("The prompt box edits the text") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");
    input.set_text("abc");

    input.backspace();
    REQUIRE(input.text() == "ab");

    input.insert("c");
    REQUIRE(input.text() == "abc");
}

TEST_CASE("The hint line shows the shortcuts hint while the input is empty") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");
    REQUIRE(input.hint_line(false).find("? for shortcuts") != std::string::npos);

    input.set_text("x");
    REQUIRE(input.hint_line(false).empty());
}

TEST_CASE("The hint line offers to interrupt while a request runs") {
    ui::PromptInput input(ui::Theme::dark(), "/tmp");
    REQUIRE(input.hint_line(true).find("esc to interrupt") != std::string::npos);
}

TEST_CASE("The spinner advances through its frames") {
    ui::Spinner spinner(ui::Theme::dark());
    const std::string first = spinner.glyph();
    spinner.tick();
    REQUIRE_FALSE(spinner.verb().empty());
    REQUIRE(spinner.line().find("…") != std::string::npos);
}
