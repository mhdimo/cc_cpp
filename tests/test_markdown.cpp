#include <catch2/catch_test_macros.hpp>

#include "ui/markdown.hpp"
#include "ui/theme.hpp"
#include "utils/string.hpp"

#include <string>
#include <vector>

using namespace cc;

namespace {

/// The lines of a markdown text, without the colours.
std::vector<std::string> plain(const std::string& text, int width = 40) {
    std::vector<std::string> out;
    for (const auto& line : ui::render_markdown(text, ui::Theme::dark(), width)) {
        out.push_back(cc::strip_ansi(line));
    }
    return out;
}

} // namespace

TEST_CASE("The text of the model keeps every character") {
    // A character of more than one byte must not be broken in two, and no
    // character may be dropped: the second plus of "C++" used to vanish.
    REQUIRE(plain("C++ port")[0] == "C++ port");
    REQUIRE(plain("Hello世界")[0] == "Hello世界");
    REQUIRE(plain("完成C++移植")[0] == "完成C++移植");

    // The space between two words is kept as well.
    REQUIRE(plain("in ~/cc_cpp your port")[0] == "in ~/cc_cpp your port");
}

TEST_CASE("A backslash escapes only the punctuation of markdown") {
    REQUIRE(plain("a \\*b\\* c")[0] == "a *b* c");
    REQUIRE(plain("a \\b c")[0] == "a \\b c");
}

TEST_CASE("The emphasis of the model is read the way markdown reads it") {
    REQUIRE(plain("***both***")[0] == "both");
    REQUIRE(plain("**strong**")[0] == "strong");
    REQUIRE(plain("*italic*")[0] == "italic");
}

TEST_CASE("A code span is written with a run of backticks") {
    REQUIRE(plain("``a ` b``")[0] == "a ` b");
    REQUIRE(plain("`code`")[0] == "code");
}

TEST_CASE("An address is a link") {
    // The address is written with an escape sequence around it, so only the
    // text of it is compared here.
    const auto lines = plain("see https://example.com/x, ok");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "see https://example.com/x, ok");

    const auto styled = ui::render_markdown("https://example.com", ui::Theme::dark(), 40);
    REQUIRE(styled.size() == 1);
    REQUIRE(styled[0].find("https://example.com") != std::string::npos);
}

TEST_CASE("The blocks of the text are separated the way the reference separates them") {
    // A heading leaves a blank line under it.
    const auto heading = plain("# Title\n\ntext");
    REQUIRE(heading.size() == 3);
    REQUIRE(heading[0] == "Title");
    REQUIRE(heading[1].empty());
    REQUIRE(heading[2] == "text");

    // A paragraph that is followed by another one keeps the blank line the
    // text had.
    const auto paragraphs = plain("one\n\ntwo");
    REQUIRE(paragraphs.size() == 3);
    REQUIRE(paragraphs[1].empty());

    // The text is trimmed, so no blank line is left at the end.
    REQUIRE(plain("text\n\n").size() == 1);
}

TEST_CASE("The cells of a table are read for the inline markup") {
    const auto lines = plain("| a | b |\n| --- | --- |\n| `x` | **y** |");
    REQUIRE(lines.size() >= 4);
    for (const auto& line : lines) {
        REQUIRE(line.find("`x`") == std::string::npos);
        REQUIRE(line.find("**y**") == std::string::npos);
    }
}

TEST_CASE("The items of an ordered list are numbered from the first one") {
    // The reference renumbers the items of a list from the number of the
    // first one.
    const auto numbered = plain("1. one\n2. two\n3. three");
    REQUIRE(numbered.size() == 3);
    REQUIRE(numbered[0] == "1. one");
    REQUIRE(numbered[1] == "2. two");
    REQUIRE(numbered[2] == "3. three");

    // The number of the first item is kept, the ones that follow it are
    // counted from it.
    const auto started = plain("3. a\n1. b");
    REQUIRE(started.size() == 2);
    REQUIRE(started[0] == "3. a");
    REQUIRE(started[1] == "4. b");
}

TEST_CASE("The tick of a task list is not drawn") {
    const auto tasks = plain("- [ ] todo\n- [x] done");
    REQUIRE(tasks.size() == 2);
    REQUIRE(tasks[0] == "- todo");
    REQUIRE(tasks[1] == "- done");
}

TEST_CASE("The tags of HTML are thrown away") {
    // A block of HTML is drawn as nothing at all.
    REQUIRE(plain("<div>hello</div>").empty());

    // A tag inside a paragraph is thrown away, the text around it stays.
    const auto tagged = plain("Hello <b>world</b> end");
    REQUIRE(tagged.size() == 1);
    REQUIRE(tagged[0] == "Hello world end");
}

TEST_CASE("A reference to an issue keeps its name when the terminal cannot follow a link") {
    // The tests write to a pipe, so the terminal cannot follow a hyperlink.
    const auto lines = plain("See owner/repo#123 here");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "See owner/repo#123 here");
}

TEST_CASE("The width of a column is measured on the text as it is drawn") {
    const auto lines = plain("| **bold** | b |\n| --- | --- |\n| c | d |");
    REQUIRE(lines.size() >= 5);
    // The stars of the markdown are not counted: the column is four columns
    // wide and not eight.
    REQUIRE(lines[0] == "┌──────┬─────┐");
}

TEST_CASE("A table that follows a paragraph is drawn as a table") {
    const auto lines = plain("Here is a table:\n| a | b |\n| --- | --- |\n| c | d |");
    REQUIRE(lines.size() >= 5);
    REQUIRE(lines[0] == "Here is a table:");
    REQUIRE(lines[1].empty());
    REQUIRE(lines[2] == "┌─────┬─────┐");
}

TEST_CASE("A table is drawn with its frame") {
    const auto lines = plain("| a | b |\n| --- | --- |\n| c | d |");
    REQUIRE(lines.size() == 5);
    REQUIRE(lines[0] == "┌─────┬─────┐");
    REQUIRE(lines[1] == "│  a  │  b  │");
    REQUIRE(lines[4] == "└─────┴─────┘");
}

TEST_CASE("The paragraphs that follow the first one are items of the list") {
    const auto items = plain("- first paragraph\n\n  second paragraph");
    REQUIRE(items.size() == 3);
    REQUIRE(items[0] == "- first paragraph");
    REQUIRE(items[1].empty());
    REQUIRE(items[2] == "- second paragraph");
}

TEST_CASE("The empty line that stands before a block of a list is drawn") {
    const auto nested = plain("- one\n\n  - nested");
    REQUIRE(nested.size() == 3);
    REQUIRE(nested[0] == "- one");
    REQUIRE(nested[1].empty());
    REQUIRE(nested[2] == "  - nested");
}

TEST_CASE("The indent of a fence is taken off the code") {
    // The text is trimmed, so the fence keeps its indent only when it does
    // not stand at the beginning of the answer.
    const auto code = plain("text\n\n  ```\n  const x = 1;\n  ```");
    REQUIRE(code.size() == 3);
    REQUIRE(code[2] == "const x = 1;");
}

TEST_CASE("An address that begins with www. is a link") {
    // The terminal of the tests cannot follow a hyperlink, so the address of
    // the link is shown alone. The scheme is written in front of it, the way
    // the reference writes it.
    const auto lines = plain("see www.example.com now");
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0] == "see http://www.example.com now");
}

TEST_CASE("A rule keeps its own line when the text had an empty line") {
    const auto ruled = plain("para\n\n---\n\npara2");
    REQUIRE(ruled.size() == 4);
    REQUIRE(ruled[0] == "para");
    REQUIRE(ruled[1].empty());
    REQUIRE(ruled[2] == "---");
    REQUIRE(ruled[3] == "para2");
}

TEST_CASE("The definition of a link is thrown away and the link is resolved") {
    // The terminal of the tests cannot follow a hyperlink, so the address of
    // the link is shown alone.
    const auto defined = plain("[foo]: https://example.com\n\nSee [foo].");
    REQUIRE(defined.size() == 1);
    REQUIRE(defined[0] == "See https://example.com.");
}
