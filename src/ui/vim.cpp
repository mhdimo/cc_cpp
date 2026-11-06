#include "ui/vim.hpp"

#include "utils/string.hpp"

#include <algorithm>
#include <cctype>

namespace cc::ui::vim {
namespace {

/// The byte offset of the grapheme boundary before `offset`.
size_t previous_boundary(std::string_view text, size_t offset) {
    if (offset == 0) return 0;
    const auto clusters = cc::graphemes(text.substr(0, offset));
    if (clusters.size() <= 1) return 0;
    return offset - clusters.back().size();
}

/// The byte offset of the grapheme boundary after `offset`.
size_t next_boundary(std::string_view text, size_t offset) {
    if (offset >= text.size()) return text.size();
    const auto clusters = cc::graphemes(text.substr(offset));
    if (clusters.empty()) return text.size();
    return offset + clusters.front().size();
}

/// True when a character is a letter, a digit or an underscore.
bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// The start of the word the cursor is in.
size_t word_start(std::string_view text, size_t cursor) {
    size_t at = cursor;
    while (at > 0 && is_word_char(text[at - 1])) --at;
    return at;
}

/// The end of the word the cursor is in.
size_t word_end(std::string_view text, size_t cursor) {
    size_t at = cursor;
    while (at < text.size() && is_word_char(text[at])) ++at;
    return at;
}

/// The start of the line the cursor is in.
size_t line_start(std::string_view text, size_t cursor) {
    const size_t newline = text.rfind('\n', cursor == 0 ? 0 : cursor - 1);
    if (newline == std::string_view::npos) return 0;
    if (cursor > 0 && text[cursor - 1] == '\n') return cursor;
    return newline + 1;
}

/// The end of the line the cursor is in, without the newline.
size_t line_end(std::string_view text, size_t cursor) {
    const size_t newline = text.find('\n', cursor);
    if (newline == std::string_view::npos) return text.size();
    return newline;
}

/// The start of the next line.
size_t next_line_start(std::string_view text, size_t cursor) {
    const size_t newline = text.find('\n', cursor);
    if (newline == std::string_view::npos) return text.size();
    return newline + 1;
}

/// The end of the previous line, without the newline.
size_t previous_line_end(std::string_view text, size_t cursor) {
    const size_t start = line_start(text, cursor);
    if (start == 0) return 0;
    const size_t newline = text.rfind('\n', start - 2);
    if (newline == std::string_view::npos) return 0;
    return newline;
}

/// True when the character is one of the keys of the vim mode.
bool is_word_motion(char c) {
    return c == 'w' || c == 'W' || c == 'b' || c == 'B' || c == 'e' || c == 'E';
}

} // namespace

void State::set_mode(Mode mode) {
    mode_ = mode;
    command_ = Command{};
}

void State::remember(const std::string& text, size_t cursor) {
    undo_.push_back(Change{text, cursor});
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
}

Outcome State::handle(const Key& key, std::string& text, size_t& cursor) {
    if (!enabled_) return Outcome::Unhandled;

    if (mode_ == Mode::Insert) return handle_insert(key, text, cursor);
    return handle_normal(key, text, cursor);
}

Outcome State::handle_insert(const Key& key, std::string& text, size_t& cursor) {
    // The escape leaves the insert mode and the normal mode takes over. The
    // cursor moves one to the left, the way vim does it.
    if (key.type == Key::Type::Escape) {
        if (cursor > 0) cursor = previous_boundary(text, cursor);
        set_mode(Mode::Normal);
        return Outcome::Handled;
    }

    // The keys of the insert mode are the keys of the prompt box.
    return Outcome::Unhandled;
}

Outcome State::handle_normal(const Key& key, std::string& text, size_t& cursor) {
    // A key that is not a character has no meaning in the normal mode, and
    // the caller handles it.
    if (key.type != Key::Type::Character) {
        if (key.type == Key::Type::CtrlR && !undo_.empty()) {
            // The change is made again.
            redo_.push_back(Change{text, cursor});
            const Change change = undo_.back();
            undo_.pop_back();
            text = change.text;
            cursor = change.cursor;
            return Outcome::Handled;
        }
        if (key.type == Key::Type::Escape) {
            // The escape clears the command that is being typed.
            command_ = Command{};
            return Outcome::Handled;
        }
        return Outcome::Unhandled;
    }

    const std::string& input = key.text;

    // A count is typed before a motion.
    if (!input.empty() && std::isdigit(static_cast<unsigned char>(input[0])) &&
        !(input[0] == '0' && command_.type == Command::Type::Idle)) {
        if (command_.type == Command::Type::Operator) {
            command_.type = Command::Type::OperatorCount;
            command_.digits = input;
        } else if (command_.type == Command::Type::OperatorCount) {
            command_.digits += input;
        } else {
            command_.type = Command::Type::Count;
            command_.digits = input;
        }
        return Outcome::Handled;
    }

    switch (command_.type) {
    case Command::Type::Count: {
        const int count = std::atoi(command_.digits.c_str());
        command_ = Command{};
        if (input == "g") {
            move("gg", count, text, cursor);
        } else {
            move(input, count, text, cursor);
        }
        return Outcome::Handled;
    }
    case Command::Type::Operator: {
        // The operator waits for a motion.
        if (input.size() == 1 && input[0] == command_.op) {
            // The operator twice works on the whole line.
            const int count = command_.count;
            const char op = command_.op;
            command_ = Command{};
            operate_line(op, count, text, cursor);
            if (op == 'c') set_mode(Mode::Insert);
            return Outcome::Handled;
        }
        if (input == "g") {
            command_.type = Command::Type::OperatorG;
            return Outcome::Handled;
        }
        if (input == "i" || input == "a") {
            command_.type = Command::Type::OperatorTextObject;
            command_.scope = input[0];
            return Outcome::Handled;
        }
        if (input == "f" || input == "F" || input == "t" || input == "T") {
            command_.type = Command::Type::OperatorFind;
            command_.find = input[0];
            return Outcome::Handled;
        }
        const int count = command_.count;
        const char op = command_.op;
        command_ = Command{};
        operate(op, input, count, text, cursor);
        if (op == 'c') set_mode(Mode::Insert);
        return Outcome::Handled;
    }
    case Command::Type::OperatorCount: {
        command_.digits += input;
        return Outcome::Handled;
    }
    case Command::Type::OperatorFind: {
        const int count = command_.count;
        const char op = command_.op;
        const char find = command_.find;
        command_ = Command{};
        operate_find(find, input, count, text, cursor);
        if (op == 'c') set_mode(Mode::Insert);
        return Outcome::Handled;
    }
    case Command::Type::OperatorTextObject: {
        const int count = command_.count;
        const char op = command_.op;
        const char scope = command_.scope;
        command_ = Command{};
        operate_text_object(scope, input, count, text, cursor);
        if (op == 'c') set_mode(Mode::Insert);
        return Outcome::Handled;
    }
    case Command::Type::Find: {
        const int count = command_.count;
        const char find = command_.find;
        command_ = Command{};
        move_find(find, input, count, text, cursor);
        return Outcome::Handled;
    }
    case Command::Type::G: {
        command_ = Command{};
        move("gg", 1, text, cursor);
        return Outcome::Handled;
    }
    case Command::Type::OperatorG: {
        const int count = command_.count;
        const char op = command_.op;
        command_ = Command{};
        operate_line(op, count, text, cursor);
        if (op == 'c') set_mode(Mode::Insert);
        return Outcome::Handled;
    }
    case Command::Type::Replace: {
        command_ = Command{};
        remember(text, cursor);
        if (!input.empty() && cursor < text.size()) {
            const size_t next = next_boundary(text, cursor);
            text.replace(cursor, next - cursor, input);
        }
        return Outcome::Handled;
    }
    case Command::Type::Indent: {
        const int count = command_.count;
        const char direction = command_.direction;
        command_ = Command{};
        indent(direction, count, text, cursor);
        return Outcome::Handled;
    }
    case Command::Type::Idle:
        break;
    }

    // The keys of the normal mode.
    if (input == "d" || input == "c" || input == "y") {
        command_.type = Command::Type::Operator;
        command_.op = input[0];
        command_.count = 1;
        return Outcome::Handled;
    }

    if (input == "g") {
        command_.type = Command::Type::G;
        command_.count = 1;
        return Outcome::Handled;
    }

    if (input == "f" || input == "F" || input == "t" || input == "T") {
        command_.type = Command::Type::Find;
        command_.find = input[0];
        command_.count = 1;
        return Outcome::Handled;
    }

    if (input == "r") {
        command_.type = Command::Type::Replace;
        command_.count = 1;
        return Outcome::Handled;
    }

    if (input == ">" || input == "<") {
        command_.type = Command::Type::Indent;
        command_.direction = input[0];
        command_.count = 1;
        return Outcome::Handled;
    }

    if (input == "u") {
        // The change is taken back.
        if (!undo_.empty()) {
            redo_.push_back(Change{text, cursor});
            const Change change = undo_.back();
            undo_.pop_back();
            text = change.text;
            cursor = change.cursor;
        }
        return Outcome::Handled;
    }

    if (input == "i") {
        set_mode(Mode::Insert);
        return Outcome::Handled;
    }

    if (input == "a") {
        if (cursor < text.size()) cursor = next_boundary(text, cursor);
        set_mode(Mode::Insert);
        return Outcome::Handled;
    }

    if (input == "I") {
        cursor = line_start(text, cursor);
        set_mode(Mode::Insert);
        return Outcome::Handled;
    }

    if (input == "A") {
        cursor = line_end(text, cursor);
        set_mode(Mode::Insert);
        return Outcome::Handled;
    }

    if (input == "o" || input == "O") {
        remember(text, cursor);
        const size_t start = line_start(text, cursor);
        if (input == "O") {
            text.insert(start, "\n");
            cursor = start;
        } else {
            const size_t end = next_line_start(text, cursor);
            text.insert(end, "\n");
            cursor = end;
        }
        set_mode(Mode::Insert);
        return Outcome::Handled;
    }

    if (input == "x") {
        remember(text, cursor);
        if (cursor < text.size()) {
            const size_t next = next_boundary(text, cursor);
            text.erase(cursor, next - cursor);
        }
        return Outcome::Handled;
    }

    if (input == "p" || input == "P") {
        remember(text, cursor);
        if (!register_.empty()) {
            if (register_is_linewise_) {
                const size_t at = input == "p" ? next_line_start(text, cursor) : line_start(text, cursor);
                text.insert(at, register_);
                cursor = at;
            } else {
                const size_t at = input == "p" ? next_boundary(text, cursor) : cursor;
                text.insert(at, register_);
                cursor = at;
            }
        }
        return Outcome::Handled;
    }

    if (input == "J") {
        remember(text, cursor);
        const size_t end = line_end(text, cursor);
        if (end < text.size()) {
            text.erase(end, 1);
            text.insert(end, " ");
            cursor = end;
        }
        return Outcome::Handled;
    }

    // The motions.
    move(input, 1, text, cursor);
    return Outcome::Handled;
}

void State::move(const std::string& motion, int count, std::string& text, size_t& cursor) const {
    const int times = std::max(1, count);

    if (motion == "h") {
        for (int i = 0; i < times; ++i) cursor = previous_boundary(text, cursor);
        return;
    }
    if (motion == "l") {
        for (int i = 0; i < times; ++i) cursor = next_boundary(text, cursor);
        return;
    }
    if (motion == "j") {
        for (int i = 0; i < times; ++i) {
            const size_t previous = previous_line_end(text, cursor);
            if (previous == 0 && cursor == 0) break;
            const size_t column = cursor - line_start(text, cursor);
            const size_t start = line_start(text, previous);
            const size_t end = line_end(text, previous);
            cursor = std::min(start + column, end);
        }
        return;
    }
    if (motion == "k") {
        for (int i = 0; i < times; ++i) {
            const size_t start = line_start(text, cursor);
            if (start == 0) break;
            const size_t column = cursor - start;
            const size_t previous = previous_line_end(text, cursor);
            const size_t previous_start = line_start(text, previous);
            const size_t previous_end = line_end(text, previous);
            cursor = std::min(previous_start + column, previous_end);
        }
        return;
    }
    if (is_word_motion(motion[0])) {
        // The capital motions work on the words of the whole text: a run of
        // characters that are not spaces is a word.
        const bool big = std::isupper(static_cast<unsigned char>(motion[0])) != 0;
        const char kind = static_cast<char>(std::tolower(static_cast<unsigned char>(motion[0])));

        const auto is_word = [&](size_t at) {
            return big ? std::isspace(static_cast<unsigned char>(text[at])) == 0
                       : is_word_char(text[at]);
        };

        if (kind == 'w') {
            for (int i = 0; i < times; ++i) {
                size_t at = cursor;
                // The word under the cursor is left behind first.
                while (at < text.size() && is_word(at)) ++at;
                while (at < text.size() && !is_word(at)) ++at;
                if (at >= text.size()) break;
                cursor = at;
            }
            return;
        }
        if (kind == 'b') {
            for (int i = 0; i < times; ++i) {
                size_t at = cursor;
                if (at > 0) --at;
                while (at > 0 && !is_word(at)) --at;
                while (at > 0 && is_word(at - 1)) --at;
                cursor = at;
            }
            return;
        }
        if (kind == 'e') {
            for (int i = 0; i < times; ++i) {
                size_t at = cursor;
                while (at < text.size() && !is_word(at)) ++at;
                while (at + 1 < text.size() && is_word(at + 1)) ++at;
                cursor = at;
            }
            return;
        }
        return;
    }
    if (motion == "0") {
        cursor = line_start(text, cursor);
        return;
    }
    if (motion == "^") {
        size_t at = line_start(text, cursor);
        while (at < text.size() && text[at] == ' ') ++at;
        cursor = at;
        return;
    }
    if (motion == "$") {
        const size_t end = line_end(text, cursor);
        cursor = end == 0 ? 0 : previous_boundary(text, end);
        return;
    }
    if (motion == "G") {
        if (count > 1) {
            // The count names the line.
            size_t at = 0;
            for (int i = 1; i < count; ++i) {
                const size_t next = next_line_start(text, at);
                if (next >= text.size()) break;
                at = next;
            }
            cursor = at;
            return;
        }
        cursor = text.empty() ? 0 : previous_boundary(text, text.size());
        return;
    }
    if (motion == "gg") {
        cursor = 0;
        return;
    }
}

void State::move_find(char find, const std::string& character, int count, std::string& text,
                      size_t& cursor) const {
    if (character.empty()) return;

    const char wanted = character[0];
    const bool backwards = find == 'F' || find == 'T';

    for (int i = 0; i < std::max(1, count); ++i) {
        size_t at = cursor;
        while (true) {
            if (backwards) {
                if (at == 0) break;
                --at;
            } else {
                ++at;
                if (at >= text.size()) break;
            }
            if (text[at] != wanted) continue;

            if (find == 't' || find == 'T') {
                // The cursor stops before the character.
                if (!backwards) {
                    if (at == cursor) continue;
                    cursor = previous_boundary(text, at);
                } else {
                    cursor = at;
                }
            } else {
                cursor = at;
            }
            break;
        }
    }
}

void State::operate_find(char find, const std::string& character, int count, std::string& text,
                         size_t& cursor) {
    const size_t before = cursor;
    move_find(find, character, count, text, cursor);
    if (cursor == before) return;

    remember(text, cursor);
    const size_t from = std::min(before, cursor);
    const size_t to = std::max(before, cursor);
    register_ = take(from, to, text, cursor);
}

void State::operate_text_object(char scope, const std::string& type, int count, std::string& text,
                                size_t& cursor) {
    if (type != "w" && type != "W") return;

    size_t from = word_start(text, cursor);
    size_t to = word_end(text, cursor);
    if (scope == 'a') {
        // The space after the word belongs to it as well.
        while (to < text.size() && text[to] == ' ') ++to;
    }
    if (to <= from) return;

    remember(text, cursor);
    register_ = take(from, to, text, cursor);
}

void State::operate(char op, const std::string& motion, int count, std::string& text,
                    size_t& cursor) {
    const size_t before = cursor;
    move(motion, count, text, cursor);
    if (cursor == before) return;

    remember(text, cursor);
    const size_t from = std::min(before, cursor);
    const size_t to = std::max(before, cursor);
    register_ = take(from, to, text, cursor);
}

void State::operate_line(char op, int count, std::string& text, size_t& cursor) {
    const size_t start = line_start(text, cursor);
    size_t end = next_line_start(text, cursor);

    for (int i = 1; i < count; ++i) {
        if (end >= text.size()) break;
        const size_t next = next_line_start(text, end);
        end = next;
    }

    remember(text, cursor);

    const std::string lines = text.substr(start, end - start);
    register_ = lines;
    register_is_linewise_ = true;

    // The yank keeps the line; the other operators take it away.
    if (op == 'y') return;

    text.erase(start, end - start);
    cursor = std::min(start, text.size());
}

void State::indent(char direction, int count, std::string& text, size_t& cursor) {
    remember(text, cursor);

    const size_t start = line_start(text, cursor);
    size_t end = line_end(text, cursor);

    for (int i = 0; i < std::max(1, count); ++i) {
        if (direction == '>') {
            text.insert(start, "    ");
            end += 4;
        } else {
            size_t from = start;
            while (from < end && (text[from] == ' ' || text[from] == '\t')) ++from;
            if (from > start) {
                text.erase(start, from - start);
                end -= from - start;
            }
        }
    }
    cursor = std::min(cursor, text.size());
}

std::string State::take(size_t from, size_t to, std::string& text, size_t& cursor) {
    if (from > to) std::swap(from, to);
    if (from >= text.size()) return {};

    const std::string taken = text.substr(from, to - from);
    text.erase(from, to - from);
    cursor = std::min(from, text.size());
    register_is_linewise_ = false;
    return taken;
}

} // namespace cc::ui::vim
