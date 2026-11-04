#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/spinner.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

using namespace cc;

namespace {

/// The colour the text is drawn in, if the text is drawn in a twenty-four
/// bit colour.
///
/// The escape sequence of the colour stands right before the text.
std::string colour_before(const std::string& line, const std::string& text) {
    const size_t at = line.find(text);
    if (at == std::string::npos) return {};
    const size_t start = line.rfind("\x1b[38;2;", at);
    if (start == std::string::npos) return {};
    const size_t end = line.find('m', start);
    if (end == std::string::npos) return {};
    return line.substr(start + 7, end - start - 7);
}

} // namespace

TEST_CASE("The thinking of the model is shown in parentheses") {
    ui::Spinner spinner(ui::Theme::dark());
    spinner.set_verb("Herding");
    spinner.note_thinking_start();

    const std::string line = spinner.line(80, 0);

    // The reference puts the word of the thinking in parentheses when it is
    // the only part of the status.
    REQUIRE(ui::strip(line).find("(thinking)") != std::string::npos);

    // The word is drawn in the grey the reference shimmers it in.
    REQUIRE(colour_before(line, "(thinking)") == "153;153;153");
}

TEST_CASE("The duration of the thinking keeps the brackets of the status") {
    ui::Spinner spinner(ui::Theme::dark());
    spinner.set_verb("Herding");
    spinner.note_thinking_start();
    spinner.note_thinking_end();

    // The duration is a part of the status like any other, so it is shown
    // in the brackets of the status.
    const std::string line = ui::strip(spinner.line(80, 0));
    REQUIRE(line.find("(thought for 1s)") != std::string::npos);
}

TEST_CASE("The duration of the thinking is rounded the way Claude Code rounds it") {
    ui::Spinner spinner(ui::Theme::dark());
    spinner.reset();
    spinner.set_verb("Herding");

    const int64_t before = spinner.elapsed_ms();
    spinner.note_thinking_start();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    spinner.note_thinking_end();
    const int64_t window = spinner.elapsed_ms() - before;

    const std::string line = ui::strip(spinner.line(80, 0));

    // A moment and a half is two seconds once it is rounded.
    const int64_t seconds = std::max<int64_t>(1, (window + 500) / 1000);
    REQUIRE(line.find("thought for " + std::to_string(seconds) + "s") != std::string::npos);
}

TEST_CASE("The thinking of the model shimmers") {
    ui::Spinner spinner(ui::Theme::dark());
    spinner.reset();
    spinner.set_verb("Herding");
    spinner.note_thinking_start();

    // The pulse of the reference begins after three seconds.
    std::this_thread::sleep_for(std::chrono::milliseconds(3050));

    // The grey the pulse starts at is 153; the pulse has moved on from it,
    // and it never leaves the greys it shimmers between. The pulse is at its
    // darkest once every two seconds, so a second sample is taken when the
    // first one catches it there.
    int red = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string colour = colour_before(spinner.line(80, 0), "(thinking)");
        REQUIRE_FALSE(colour.empty());
        red = std::stoi(colour);
        REQUIRE(red >= 153);
        REQUIRE(red <= 185);
        if (red > 153) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    REQUIRE(red > 153);
}
