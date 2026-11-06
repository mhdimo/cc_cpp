#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/theme.hpp"
#include "ui/transcript_view.hpp"

#include <string>
#include <vector>

using namespace cc;

namespace {

/// The text of the view, with the colours taken away.
std::string plain(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += ui::strip(line);
        out += "\n";
    }
    return out;
}

/// A transcript of `count` messages, each one a line of its own.
std::vector<ui::TranscriptEntry> entries(size_t count) {
    std::vector<ui::TranscriptEntry> out;
    for (size_t i = 0; i < count; ++i) {
        ui::TranscriptEntry entry;
        entry.lines.push_back("message " + std::to_string(i));
        out.push_back(std::move(entry));
    }
    return out;
}

} // namespace

TEST_CASE("The transcript shows the last thirty messages") {
    const auto theme = ui::Theme::dark();
    ui::TranscriptView view(theme, entries(40));

    const std::string text = plain(view.render(80, 40));

    // The reference shows the last thirty messages and hides the ten older
    // ones behind a rule that names their number.
    REQUIRE(text.find("ctrl+e to show 10 previous messages") != std::string::npos);
    REQUIRE(text.find("message 10") != std::string::npos);
    REQUIRE(text.find("message 9\n") == std::string::npos);
}

TEST_CASE("The transcript shows everything when it is asked to") {
    const auto theme = ui::Theme::dark();
    ui::TranscriptView view(theme, entries(40));

    view.set_show_all(true);
    const std::string text = plain(view.render(80, 40));

    // The rule turns into the one that hides the messages again.
    REQUIRE(text.find("ctrl+e to hide 10 previous messages") != std::string::npos);
    REQUIRE(text.find("message 0") != std::string::npos);
}

TEST_CASE("The transcript of a short conversation has no rule") {
    const auto theme = ui::Theme::dark();
    ui::TranscriptView view(theme, entries(3));

    const std::string text = plain(view.render(80, 40));

    REQUIRE(text.find("previous messages") == std::string::npos);
    REQUIRE(text.find("message 0") != std::string::npos);
}

TEST_CASE("The footer of the transcript names the keys that scroll it") {
    const auto theme = ui::Theme::dark();
    ui::TranscriptView view(theme, entries(3));

    const std::string text = plain(view.render(80, 40));

    REQUIRE(text.find("Showing detailed transcript") != std::string::npos);
    REQUIRE(text.find("ctrl+o to toggle") != std::string::npos);
    REQUIRE(text.find("scroll") != std::string::npos);
    REQUIRE(text.find("home/end top/bottom") != std::string::npos);

    // The reference names either the keys that scroll or the shortcut that
    // shows everything, and never both; the rule in the body names the
    // shortcut.
    REQUIRE(text.find("ctrl+e to show all") == std::string::npos);
}

TEST_CASE("The number of the rule of the transcript is grey and in bold") {
    const auto theme = ui::Theme::dark();
    ui::TranscriptView view(theme, entries(40));

    // The rule is the first line of the body of the view.
    const std::string text = view.render(80, 40)[0];

    // The number stands out in the grey of the rule, and in bold.
    REQUIRE(text.find("\x1b[1;38;2;153;153;153m10") != std::string::npos);
}
