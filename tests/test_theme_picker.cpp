#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/theme_picker.hpp"

#include <string>
#include <vector>

using namespace cc;

namespace {

/// The text of the picker, with the colours taken away.
std::string plain(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += ui::strip(line);
        out += "\n";
    }
    return out;
}

} // namespace

TEST_CASE("The picker of the theme lists the themes of the reference") {
    ui::ThemePicker picker(ui::Theme::dark(), "dark");

    const std::string text = plain(picker.render(80));

    REQUIRE(text.find("Dark mode") != std::string::npos);
    REQUIRE(text.find("Light mode") != std::string::npos);
    REQUIRE(text.find("Dark mode (colorblind-friendly)") != std::string::npos);
    REQUIRE(text.find("Dark mode (ANSI colors only)") != std::string::npos);
}

TEST_CASE("The picker of the theme asks which style looks best") {
    ui::ThemePicker picker(ui::Theme::dark(), "dark");

    const std::string text = plain(picker.render(80));

    // The line of the reference.
    REQUIRE(text.find("Choose the text style that looks best with your terminal") !=
            std::string::npos);
    // The name of the picker.
    REQUIRE(text.find("Theme") != std::string::npos);
    // The hint under the picker.
    REQUIRE(text.find("Enter to select · Esc to cancel") != std::string::npos);
}

TEST_CASE("The picker of the theme shows the preview of the theme") {
    ui::ThemePicker picker(ui::Theme::dark(), "dark");

    const std::string text = plain(picker.render(80));

    // The frame of the preview is made of dashes, the way the reference
    // draws it.
    REQUIRE(text.find("╌") != std::string::npos);
    // The patch of the preview.
    REQUIRE(text.find("function greet()") != std::string::npos);
    REQUIRE(text.find("Hello, World!") != std::string::npos);
    REQUIRE(text.find("Hello, Claude!") != std::string::npos);
    // The line that says whether the colours of the code are on.
    REQUIRE(text.find("Syntax highlighting enabled (ctrl+t to disable)") != std::string::npos);
}

TEST_CASE("The preview of the picker is drawn in the colours of the theme") {
    ui::ThemePicker dark(ui::Theme::dark(), "dark");
    ui::ThemePicker light(ui::Theme::light(), "light");

    const std::string one = dark.render(80)[6];
    const std::string other = light.render(80)[6];

    // The colours of the preview are the ones of the theme that is
    // previewed, so the two previews do not look the same.
    REQUIRE(one != other);
}
