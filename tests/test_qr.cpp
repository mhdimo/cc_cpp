#include <catch2/catch_test_macros.hpp>

#include "ui/qr.hpp"

#include <cstdint>
#include <string>

using namespace cc;

namespace {

/// The format of a code, the way the code holds it around the finder
/// patterns.
uint32_t format_of(const ui::QrCode& code) {
    const int places[15][2] = {{8, 0}, {8, 1}, {8, 2}, {8, 3}, {8, 4}, {8, 5}, {8, 7},
                               {8, 8}, {7, 8}, {5, 8}, {4, 8}, {3, 8}, {2, 8}, {1, 8},
                               {0, 8}};

    uint32_t value = 0;
    for (int i = 0; i < 15; ++i) {
        // The places are given as a row and a column.
        if (code.dark(places[i][1], places[i][0])) value |= 1u << (14 - i);
    }
    return value ^ 0x5412u;
}

} // namespace

TEST_CASE("The code of a text has the size of its version") {
    // The short text fits in the first version.
    const auto small = ui::QrCode::encode("hello");
    REQUIRE(small.has_value());
    REQUIRE(small->size() == 21);

    // The two addresses of the applications fit in the fourth.
    const auto ios = ui::QrCode::encode("https://apps.apple.com/app/claude-by-anthropic/id6473753684");
    REQUIRE(ios.has_value());
    REQUIRE(ios->size() == 33);

    const auto android =
        ui::QrCode::encode("https://play.google.com/store/apps/details?id=com.anthropic.claude");
    REQUIRE(android.has_value());
    REQUIRE(android->size() == 33);
}

TEST_CASE("The code holds the patterns of the codes") {
    const auto code = ui::QrCode::encode("https://apps.apple.com/app/claude-by-anthropic/id6473753684");
    REQUIRE(code.has_value());

    // The finder patterns, in the three corners.
    for (const auto& corner : {std::pair<int, int>{0, 0}, {26, 0}, {0, 26}}) {
        for (int dy = 0; dy < 7; ++dy) {
            for (int dx = 0; dx < 7; ++dx) {
                const bool dark = dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                                  (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
                REQUIRE(code->dark(corner.first + dx, corner.second + dy) == dark);
            }
        }
    }

    // The timing patterns, which alternate.
    for (int i = 8; i <= 24; ++i) {
        REQUIRE(code->dark(i, 6) == (i % 2 == 0));
        REQUIRE(code->dark(6, i) == (i % 2 == 0));
    }

    // The pattern of the alignment, in the middle of the code.
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const bool dark = dx == -2 || dx == 2 || dy == -2 || dy == 2 || (dx == 0 && dy == 0);
            REQUIRE(code->dark(26 + dx, 26 + dy) == dark);
        }
    }

    // The dark module.
    REQUIRE(code->dark(8, 25));
}

TEST_CASE("The code holds the level of the correction of the errors") {
    const auto code = ui::QrCode::encode("hello");
    REQUIRE(code.has_value());

    const uint32_t format = format_of(*code);
    // The level of the correction is the one of the reference.
    REQUIRE(((format >> 13) & 3) == 1);
    // The mask is one of the eight of the code.
    REQUIRE(((format >> 10) & 7) < 8);
}

TEST_CASE("The code is drawn the way the reference draws it") {
    const auto code = ui::QrCode::encode("hello");
    REQUIRE(code.has_value());

    const std::vector<std::string> lines = code->lines();

    // The margin above the code, the code itself, and the margin below it.
    // The reference writes the quiet zone of four modules as two lines above
    // the code and two lines below it.
    REQUIRE(lines.size() == (21 + 1) / 2 + 4);
    REQUIRE(lines.front().size() == 21 + 8);
    REQUIRE(lines.back().size() == 21 + 8);

    // The margin is made of spaces.
    for (const char c : lines.front()) REQUIRE(c == ' ');

    // The code holds the half blocks of the terminal.
    bool any_block = false;
    for (const auto& line : lines) {
        for (size_t i = 0; i < line.size();) {
            const std::string character = line.substr(i, line[i] == ' ' ? 1 : 3);
            REQUIRE((character == " " || character == "█" || character == "▀" || character == "▄"));
            if (character != " ") any_block = true;
            i += character.size();
        }
    }
    REQUIRE(any_block);
}

TEST_CASE("The code of a text is always the same") {
    const auto one = ui::QrCode::encode("https://apps.apple.com/app/claude-by-anthropic/id6473753684");
    const auto other = ui::QrCode::encode("https://apps.apple.com/app/claude-by-anthropic/id6473753684");
    REQUIRE(one.has_value());
    REQUIRE(other.has_value());

    for (int y = 0; y < one->size(); ++y) {
        for (int x = 0; x < one->size(); ++x) {
            REQUIRE(one->dark(x, y) == other->dark(x, y));
        }
    }
}
