#include <catch2/catch_test_macros.hpp>

#include "ui/code_highlight.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace cc;

namespace {

/// The style of the piece of the code that holds `word`.
///
/// The code is split into pieces that hold one word each, so that the style
/// of a word can be found.
bool style_of(std::string_view code, std::string_view language, std::string_view word,
              ui::Style& out) {
    for (const auto& token : ui::code::highlight(code, language)) {
        if (token.text == word) {
            out = token.style;
            return true;
        }
    }
    return false;
}

/// The colour of the piece of the code that holds `word`.
std::optional<ui::BasicColor> colour_of(std::string_view code, std::string_view language,
                                         std::string_view word) {
    ui::Style style;
    if (!style_of(code, language, word, style)) return std::nullopt;
    return style.basic_foreground;
}

/// The whole code as one string, without the colours.
std::string plain(std::string_view code, std::string_view language) {
    std::string out;
    for (const auto& token : ui::code::highlight(code, language)) out += token.text;
    return out;
}

} // namespace

TEST_CASE("A block of C++ has a colour of its own for every kind of word") {
    const std::string code = R"(int main() {
    // a comment
    const char* s = "hello";
    auto x = 42;
    return 0;
}
)";

    const auto type = colour_of(code, "cpp", "int");
    const auto keyword = colour_of(code, "cpp", "auto");
    const auto comment = colour_of(code, "cpp", "// a comment");
    const auto string = colour_of(code, "cpp", "\"hello\"");
    const auto number = colour_of(code, "cpp", "42");

    // Every kind of word has a colour of its own: none of them is the plain
    // text of the answer.
    REQUIRE(type.has_value());
    REQUIRE(keyword.has_value());
    REQUIRE(comment.has_value());
    REQUIRE(string.has_value());
    REQUIRE(number.has_value());

    // The colours are the ones of the theme of `cli-highlight`: the type is
    // a dim cyan, the keyword a blue, the comment a green, the string a red
    // and the number a green. The comments and the numbers share the green
    // of the theme, the way the reference draws them.
    REQUIRE(*type == ui::BasicColor::Cyan);
    REQUIRE(*keyword == ui::BasicColor::Blue);
    REQUIRE(*comment == ui::BasicColor::Green);
    REQUIRE(*string == ui::BasicColor::Red);
    REQUIRE(*number == ui::BasicColor::Green);

    // The type is dim, the way the theme of `cli-highlight` draws it.
    ui::Style style;
    REQUIRE(style_of(code, "cpp", "int", style));
    REQUIRE(style.dim);

    // The kinds of word do not all share the same colour.
    REQUIRE(*type != *keyword);
    REQUIRE(*type != *comment);
    REQUIRE(*type != *string);
    REQUIRE(*type != *number);
    REQUIRE(*keyword != *comment);
    REQUIRE(*keyword != *string);
    REQUIRE(*keyword != *number);
    REQUIRE(*comment != *string);
    REQUIRE(*string != *number);
}

TEST_CASE("The name of a function and the name of a class have a colour of their own") {
    const std::string code = R"(class Widget {
    void draw() {}
};
)";

    // The name of the class is a blue, the name of the function a yellow.
    REQUIRE(colour_of(code, "cpp", "Widget") == ui::BasicColor::Blue);
    REQUIRE(colour_of(code, "cpp", "draw") == ui::BasicColor::Yellow);
}

TEST_CASE("A language that is not known is drawn as plain text") {
    const std::string code = R"(int main() { return 0; }
)";

    const auto tokens = ui::code::highlight(code, "not-a-language");

    // The code is there, and it has no colour at all.
    REQUIRE(tokens.size() == 1);
    REQUIRE(tokens[0].text == code);
    REQUIRE(tokens[0].style.basic_foreground == std::nullopt);
    REQUIRE(tokens[0].style.foreground == std::nullopt);
}

TEST_CASE("The plain text has no colour either") {
    const std::string code = "int main() { return 0; }\n";
    const auto tokens = ui::code::highlight(code, "plaintext");
    REQUIRE(tokens.size() == 1);
    REQUIRE(tokens[0].text == code);
    REQUIRE(tokens[0].style.basic_foreground == std::nullopt);
}

TEST_CASE("The tag of a fence may carry attributes after the language") {
    const std::string code = R"(int main() {
    // a comment
    auto x = 42;
}
)";

    // The language is the part of the tag in front of the attributes, the
    // way the reference reads it.
    REQUIRE(ui::code::language_of("rust,ignore") == "rust");
    REQUIRE(ui::code::language_of("cpp,ignore") == "cpp");
    REQUIRE(ui::code::language_of("c++") == "c++");
    REQUIRE(ui::code::language_of("js") == "js");
    REQUIRE(ui::code::language_of("not-a-language").empty());

    // A tag with attributes does not name a language, the way the reference
    // reads it; the code is still highlighted with the language in front of
    // the attributes.
    REQUIRE_FALSE(ui::code::names_a_language("cpp,ignore"));
    REQUIRE(ui::code::names_a_language("cpp"));

    const auto keyword = colour_of(code, "cpp,ignore", "int");
    const auto comment = colour_of(code, "cpp,ignore", "// a comment");
    const auto number = colour_of(code, "cpp,ignore", "42");
    REQUIRE(keyword.has_value());
    REQUIRE(comment.has_value());
    REQUIRE(number.has_value());
    REQUIRE(*keyword != *comment);
    REQUIRE(*keyword != *number);
}

TEST_CASE("The code that is highlighted is the code that was given") {
    const std::string code = "int x = 1;\n/* a comment */\nconst char* s = \"a\";\n";
    REQUIRE(plain(code, "cpp") == code);
    REQUIRE(plain(code, "python") == code);
    REQUIRE(plain(code, "not-a-language") == code);
}

TEST_CASE("A block of code is highlighted as it is, comment and string included") {
    // The comment of the C family is closed by another text.
    const std::string cpp = "/* a comment\n   that runs over a line */\nint x = 1;\n";
    const auto comment = colour_of(cpp, "cpp", "/* a comment\n   that runs over a line */");
    REQUIRE(comment.has_value());
    REQUIRE(*comment == ui::BasicColor::Green);

    // The comment of Python begins with a hash.
    const std::string python = "# a comment\nx = 1\n";
    const auto hash = colour_of(python, "python", "# a comment");
    REQUIRE(hash.has_value());
    REQUIRE(*hash == ui::BasicColor::Green);
}
