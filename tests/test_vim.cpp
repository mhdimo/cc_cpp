#include <catch2/catch_test_macros.hpp>

#include "ui/vim.hpp"

#include <string>

using namespace cc;

namespace {

/// A key of the vim mode.
ui::Key character(const std::string& text) {
    ui::Key key;
    key.type = ui::Key::Type::Character;
    key.text = text;
    return key;
}

/// The escape key.
ui::Key escape() {
    ui::Key key;
    key.type = ui::Key::Type::Escape;
    return key;
}

/// Types a string in the insert mode.
void type(ui::vim::State& state, std::string& text, size_t& cursor, const std::string& keys) {
    for (const char c : keys) {
        const ui::Key key = character(std::string(1, c));
        if (state.handle(key, text, cursor) == ui::vim::Outcome::Unhandled) {
            text.insert(cursor, 1, c);
            ++cursor;
        }
    }
}

} // namespace

TEST_CASE("The vim mode is off until it is turned on") {
    ui::vim::State state;
    REQUIRE_FALSE(state.enabled());
    REQUIRE(state.mode() == ui::vim::Mode::Insert);
}

TEST_CASE("The escape leaves the insert mode") {
    ui::vim::State state;
    state.set_enabled(true);

    std::string text = "hello";
    size_t cursor = text.size();

    REQUIRE(state.handle(escape(), text, cursor) == ui::vim::Outcome::Handled);
    REQUIRE(state.mode() == ui::vim::Mode::Normal);
    REQUIRE(text == "hello");
}

TEST_CASE("The motions of the normal mode move the cursor") {
    ui::vim::State state;
    state.set_enabled(true);

    std::string text = "one two three";
    size_t cursor = 0;
    state.handle(escape(), text, cursor);

    // `l` moves one character to the right.
    state.handle(character("l"), text, cursor);
    REQUIRE(cursor == 1);

    // `w` moves to the start of the next word.
    state.handle(character("w"), text, cursor);
    REQUIRE(cursor == 4);

    // `b` moves back to the start of the word.
    state.handle(character("b"), text, cursor);
    REQUIRE(cursor == 0);

    // `$` moves to the end of the line.
    state.handle(character("$"), text, cursor);
    REQUIRE(cursor == text.size() - 1);

    // `0` moves to the start of the line.
    state.handle(character("0"), text, cursor);
    REQUIRE(cursor == 0);
}

TEST_CASE("The delete operator removes the text of a motion") {
    ui::vim::State state;
    state.set_enabled(true);

    std::string text = "one two three";
    size_t cursor = 0;
    state.handle(escape(), text, cursor);

    // `dw` removes the word under the cursor and the space after it, the
    // way vim does.
    state.handle(character("d"), text, cursor);
    state.handle(character("w"), text, cursor);
    REQUIRE(text == "two three");
    REQUIRE(cursor == 0);
}

TEST_CASE("The delete operator removes whole lines") {
    ui::vim::State state;
    state.set_enabled(true);

    std::string text = "one\ntwo\nthree";
    size_t cursor = 0;
    state.handle(escape(), text, cursor);

    state.handle(character("d"), text, cursor);
    state.handle(character("d"), text, cursor);
    REQUIRE(text == "two\nthree");
}

TEST_CASE("The undo takes a change back") {
    ui::vim::State state;
    state.set_enabled(true);

    std::string text = "one two";
    size_t cursor = 0;
    state.handle(escape(), text, cursor);

    state.handle(character("x"), text, cursor);
    REQUIRE(text == "ne two");

    state.handle(character("u"), text, cursor);
    REQUIRE(text == "one two");
}

TEST_CASE("The yank and the paste move a line") {
    ui::vim::State state;
    state.set_enabled(true);

    std::string text = "one\ntwo";
    size_t cursor = 0;
    state.handle(escape(), text, cursor);

    state.handle(character("y"), text, cursor);
    state.handle(character("y"), text, cursor);
    REQUIRE(text == "one\ntwo");

    // The line is pasted after the line the cursor is in.
    state.handle(character("p"), text, cursor);
    REQUIRE(text == "one\none\ntwo");
}

TEST_CASE("The insert mode writes the text again") {
    ui::vim::State state;
    state.set_enabled(true);

    std::string text = "hello";
    size_t cursor = text.size();
    state.handle(escape(), text, cursor);

    // `A` moves to the end of the line and writes there.
    state.handle(character("A"), text, cursor);
    REQUIRE(state.mode() == ui::vim::Mode::Insert);

    type(state, text, cursor, "!");
    REQUIRE(text == "hello!");
}

TEST_CASE("A count is typed before a motion") {
    ui::vim::State state;
    state.set_enabled(true);

    std::string text = "one two three";
    size_t cursor = 0;
    state.handle(escape(), text, cursor);

    state.handle(character("3"), text, cursor);
    state.handle(character("l"), text, cursor);
    REQUIRE(cursor == 3);
}

TEST_CASE("The keys of the insert mode are left to the prompt box") {
    ui::vim::State state;
    state.set_enabled(true);

    std::string text;
    size_t cursor = 0;

    const ui::Key key = character("a");
    REQUIRE(state.handle(key, text, cursor) == ui::vim::Outcome::Unhandled);
}
