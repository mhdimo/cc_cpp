#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/diff.hpp"
#include "ui/suggestions.hpp"
#include "ui/theme.hpp"
#include "ui/tool_display.hpp"
#include "utils/string.hpp"

using namespace cc;

TEST_CASE("A file edit renders as a diff") {
    const auto theme = ui::Theme::dark();
    const auto lines =
        ui::render_diff_result("one\ntwo\nthree\n", "one\nTWO\nthree\nfour\n", theme, 60);

    REQUIRE(lines.size() >= 2);
    REQUIRE(lines[0].find("Added") != std::string::npos);
    REQUIRE(lines[0].find("⎿") != std::string::npos);

    // The diff body carries the line numbers and the signs.
    bool saw_added = false;
    bool saw_removed = false;
    for (const auto& line : lines) {
        if (line.find("+four") != std::string::npos) saw_added = true;
        if (line.find("-two") != std::string::npos) saw_removed = true;
    }
    REQUIRE(saw_added);
    REQUIRE(saw_removed);
}

TEST_CASE("The suggestions overlay lists the slash commands") {
    const auto items = ui::suggestions_for("/cl", "/tmp");
    REQUIRE_FALSE(items.empty());
    REQUIRE(items.front().display == "/clear");
    REQUIRE(items.front().kind == ui::Suggestion::Kind::Command);
}

TEST_CASE("The suggestions overlay lists the files") {
    const auto items = ui::suggestions_for("@", "/tmp");
    // The files are capped, so the list never grows without bound.
    REQUIRE(items.size() <= ui::kMaxFileSuggestions);
}

TEST_CASE("The suggestions overlay keeps every command") {
    // The commands are not capped: the window of the overlay follows the
    // chosen one, so the list may be longer than the window.
    const auto items = ui::suggestions_for("/", "/tmp");
    REQUIRE(items.size() > ui::kMaxOverlayItems);
}

TEST_CASE("The suggestions overlay is empty for ordinary text") {
    REQUIRE(ui::suggestions_for("hello", "/tmp").empty());
}

TEST_CASE("The diff summary counts the added and removed lines") {
    const auto lines = ui::diff_lines("a\nb\n", "a\nc\nd\n");
    // The counts are bold, so the escape codes are stripped first.
    const std::string summary = ui::strip(ui::diff_summary(lines));

    REQUIRE(summary.find("Added 2 lines") != std::string::npos);
    REQUIRE(summary.find("removed 1 line") != std::string::npos);
}

TEST_CASE("The diff dims the gutter of an unchanged line and not its text") {
    const auto theme = ui::Theme::dark();
    const auto lines = ui::diff_lines("one\ntwo\n", "one\nTWO\n");
    const auto rendered = ui::render_diff(lines, theme, 40);

    // The unchanged line, the removed one and the added one.
    REQUIRE(rendered.size() == 3);

    // An unchanged row is drawn in two pieces: the gutter, which carries the
    // number and the sign, is dimmed, and the text of the line is not.
    const size_t context_end = rendered[0].find(ui::kReset);
    REQUIRE(context_end != std::string::npos);
    const std::string context_gutter = rendered[0].substr(0, context_end);
    const std::string context_text = rendered[0].substr(context_end + ui::kReset.size());

    REQUIRE(context_gutter.find("\x1b[2m") != std::string::npos);
    REQUIRE(context_text.find("\x1b[") == std::string::npos);
    REQUIRE(context_text.find("one") != std::string::npos);

    // A changed line keeps the background of the theme on the whole row.
    REQUIRE(rendered[1].find("\x1b[48;2;") != std::string::npos);
    REQUIRE(rendered[1].find("-two") != std::string::npos);
    REQUIRE(rendered[2].find("\x1b[48;2;") != std::string::npos);
    REQUIRE(rendered[2].find("+TWO") != std::string::npos);
}

TEST_CASE("The text is wrapped the way the reference wraps it") {
    // The reference fills the row: a line of the width of the row is a
    // line that is allowed, and the word that does not fit goes to the
    // next line.
    const std::string text =
        "Quick safety check: Is this a project you created or one you trust? (Like your own code, a "
        "well-known open source project, or work from your team). If not, take a moment to review "
        "what's in this folder first.";

    const auto lines = cc::word_wrap(text, 78);
    REQUIRE(lines.size() == 3);
    REQUIRE(lines[0] ==
            "Quick safety check: Is this a project you created or one you trust? (Like your");
    REQUIRE(lines[1] ==
            "own code, a well-known open source project, or work from your team). If not,");
    REQUIRE(lines[2] == "take a moment to review what's in this folder first.");

    // No line is wider than the row.
    for (const auto& line : lines) REQUIRE(line.size() <= 78);

    // A word that is longer than the row is broken in the middle.
    const auto broken = cc::word_wrap("abcdefghijklmnopqrstuvwxyz", 10);
    REQUIRE(broken.size() == 3);
    REQUIRE(broken[0] == "abcdefghij");
    REQUIRE(broken[1] == "klmnopqrst");
    REQUIRE(broken[2] == "uvwxyz");
}

TEST_CASE("The unified diff names the line its hunks start at") {
    const std::string diff = ui::unified_diff("one\ntwo\nthree\n", "one\nTWO\nthree\n");

    // The header of the hunk names the line the hunk starts at in the old
    // text and in the new one, and how many lines it holds on either side.
    REQUIRE(diff.find("@@ -1,3 +1,3 @@") != std::string::npos);
    REQUIRE(diff.find("-two") != std::string::npos);
    REQUIRE(diff.find("+TWO") != std::string::npos);
}

TEST_CASE("The unified diff keeps three lines of context around a change") {
    std::string before;
    std::string after;
    for (int i = 1; i <= 10; ++i) {
        before += "line " + std::to_string(i) + "\n";
        after += (i == 5 ? "changed " : "line ") + std::to_string(i) + "\n";
    }

    const std::string diff = ui::unified_diff(before, after);

    // The hunk holds the three lines before the change, the change itself
    // and the three lines after it.
    REQUIRE(diff.find(" line 2\n") != std::string::npos);
    REQUIRE(diff.find(" line 1\n") == std::string::npos);
    REQUIRE(diff.find(" line 8\n") != std::string::npos);
    REQUIRE(diff.find(" line 9\n") == std::string::npos);
}

TEST_CASE("The unified diff of a file that was created holds every line") {
    const std::string diff = ui::unified_diff("", "one\ntwo\n");

    // A file that is new has no line in the old text.
    REQUIRE(diff.find("@@ -0,0 +1,2 @@") != std::string::npos);
    REQUIRE(diff.find("+one") != std::string::npos);
    REQUIRE(diff.find("+two") != std::string::npos);
}
