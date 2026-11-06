#include "ui/prompt_input.hpp"

#include "commands/registry.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace cc::ui {
namespace {

/// The byte offset of the grapheme boundary before `offset`.
size_t previous_boundary(std::string_view text, size_t offset) {
    if (offset == 0) return 0;
    const auto clusters = cc::graphemes(text.substr(0, offset));
    if (clusters.size() <= 1) return 0;

    // The boundary is the start of the last cluster.
    return offset - clusters.back().size();
}

/// The byte offset of the grapheme boundary after `offset`.
size_t next_boundary(std::string_view text, size_t offset) {
    if (offset >= text.size()) return text.size();
    const auto clusters = cc::graphemes(text.substr(offset));
    if (clusters.empty()) return text.size();

    // The boundary is the end of the first cluster.
    return offset + clusters.front().size();
}

/// Repeats `unit` `count` times.
std::string repeat(std::string_view unit, size_t count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (size_t i = 0; i < count; ++i) out += unit;
    return out;
}

/// True when the character may be part of the name of a command.
///
/// The reference reads the name with the letters, the digits and the marks
/// a name may carry.
bool name_character(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) return true;
    return c == '_' || c == ':' || c == '-';
}

/// True when the session knows a command with that name.
///
/// The reference looks the name up in the list of the commands; the aliases
/// answer to it as well.
bool known_command(std::string_view name) {
    for (const auto& command : commands::all()) {
        if (command.name == name) return true;
        for (const auto& alias : command.aliases) {
            if (alias == name) return true;
        }
    }
    for (const auto& known : commands::known_command_names()) {
        if (known == name) return true;
    }
    return false;
}

/// A part of the input that carries a colour of its own.
struct Painted {
    size_t start;  ///< The first byte of the part.
    size_t end;    ///< The byte past the last one.
    Style style;   ///< The style of the part.
};

/// The commands the user is typing that the session knows.
///
/// The reference paints a "/command" it knows in the colour of the
/// suggestions, and the "/btw" that stands at the beginning of the input in
/// the colour of the warning.
std::vector<Painted> painted_commands(std::string_view text, const Theme& theme) {
    std::vector<Painted> out;

    // The "/btw" stands at the beginning of the input.
    if (text.size() >= 4 && cc::to_lower(text.substr(0, 4)) == "/btw" &&
        (text.size() == 4 || !name_character(text[4]))) {
        Style style;
        style.foreground = theme.warning;
        out.push_back({0, 4, style});
    }

    // A "/command" stands at the beginning of the input or after a space.
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '/' || (i != 0 && !std::isspace(static_cast<unsigned char>(text[i - 1])))) {
            ++i;
            continue;
        }

        // The name of the command begins with a letter.
        size_t end = i + 1;
        if (end >= text.size() || !std::isalpha(static_cast<unsigned char>(text[end]))) {
            ++i;
            continue;
        }
        while (end < text.size() && name_character(text[end])) ++end;

        // The command is painted only when the session knows it.
        const std::string name(text.substr(i + 1, end - i - 1));
        const bool covered =
            std::any_of(out.begin(), out.end(),
                        [&](const Painted& part) { return part.start < end && i < part.end; });
        if (known_command(name) && !covered) {
            Style style;
            style.foreground = theme.suggestion;
            out.push_back({i, end, style});
        }
        i = end;
    }

    return out;
}

/// Writes a part of the input to the line, painting the commands in it.
///
/// The parts are the byte offsets of the parts of the input that carry a
/// colour of their own; the text outside them is written as it is.
void append_painted(std::string& line, std::string_view text, size_t offset,
                    const std::vector<Painted>& painted) {
    size_t at = 0;
    for (const Painted& part : painted) {
        if (part.end <= offset || part.start >= offset + text.size()) continue;

        const size_t from = std::max(part.start, offset);
        const size_t to = std::min(part.end, offset + text.size());
        if (from > offset + at) line.append(text.substr(at, from - offset - at));
        line += styled(text.substr(from - offset, to - from), part.style);
        at = to - offset;
    }
    if (at < text.size()) line.append(text.substr(at));
}

/// The rest of the name of the command that is being typed in the middle of
/// the input.
///
/// The reference looks for a slash that stands after a space, and completes
/// the name that follows it with the name of the command it knows.
std::string ghost_text(std::string_view text, size_t cursor) {
    // A command at the beginning of the input is another thing.
    if (text.empty() || text[0] == '/') return {};
    if (cursor > text.size()) cursor = text.size();

    // The slash stands after a space.
    size_t slash = std::string::npos;
    for (size_t i = cursor; i-- > 0;) {
        if (text[i] != '/') continue;
        if (i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1]))) slash = i;
        break;
    }
    if (slash == std::string::npos) return {};

    // The name of the command holds nothing but the characters a name may
    // carry.
    const std::string partial(text.substr(slash + 1, cursor - slash - 1));
    if (partial.empty()) return {};
    for (const char c : partial) {
        if (!name_character(c)) return {};
    }

    // The caret may not stand past the name of the command.
    size_t end = slash + 1;
    while (end < text.size() && name_character(text[end])) ++end;
    if (cursor > end) return {};

    // The first command whose name begins with what was typed completes it.
    const std::string query = cc::to_lower(partial);
    for (const auto& command : commands::all()) {
        if (!commands::is_available(command)) continue;
        if (command.name.size() <= partial.size()) continue;
        if (!cc::starts_with(cc::to_lower(command.name), query)) continue;
        return command.name.substr(partial.size());
    }
    for (const auto& name : commands::known_command_names()) {
        if (name.size() <= partial.size()) continue;
        if (!cc::starts_with(cc::to_lower(name), query)) continue;
        return name.substr(partial.size());
    }
    return {};
}

/// Pads `text` to `width` columns.
std::string pad(std::string_view text, size_t width) {
    const size_t visible = cc::display_width(text);
    if (visible >= width) return std::string(text);
    return std::string(text) + std::string(width - visible, ' ');
}

/// The lines the input is drawn as.
///
/// The text is broken at newlines and then wrapped to the width of the
/// box. The byte offset of the start of every line is kept, so that the
/// caret can be placed.
struct Layout {
    std::vector<std::string> lines;  ///< The text of every line.
    std::vector<size_t> starts;      ///< The byte offset of every line.
};

/// One word of a line of the input.
struct Word {
    size_t start;  ///< The first byte of the word.
    size_t end;    ///< The byte past the last one.
    size_t width;  ///< The columns the word takes.
};

/// One row of a line of the input.
struct Row {
    size_t start;  ///< The first byte of the row.
    size_t end;    ///< The byte past the last one.
    size_t width;  ///< The columns the row takes.
};

/// Splits a word that is wider than the row across the rows.
///
/// The row that is being filled is filled first, the way `wrap-ansi` fills
/// it: the word is cut where the row ends, and the rest of it stands at
/// the beginning of the next row.
void split_word(std::string_view word, size_t start, size_t width, std::vector<Row>& rows) {
    size_t used = 0;    // The bytes of the word that are placed.
    size_t visible = rows.back().width;

    for (const std::string& cluster : cc::graphemes(word)) {
        const size_t cluster_width = cc::display_width(cluster);
        if (visible + cluster_width <= width) {
            // The character fits in the row.
            used += cluster.size();
            visible += cluster_width;
            rows.back().end += cluster.size();
            continue;
        }
        // The character does not fit: a new row starts with it.
        rows.push_back({start + used, start + used, 0});
        used += cluster.size();
        visible = cluster_width;
        rows.back().end = start + used;
    }
}

/// Wraps one line of the text to the width of the box.
///
/// The line is broken the way `wrap-ansi` breaks it: a word that does not
/// fit moves to the next row, and a word that is wider than the row is
/// split across the rows.
void wrap_line(std::string_view text, size_t start, size_t end, size_t width, Layout& out) {
    // The words of the line, split on the spaces.
    std::vector<Word> words;
    size_t word_start = start;
    for (size_t i = start; i <= end; ++i) {
        if (i == end || text[i] == ' ') {
            words.push_back(
                {word_start, i, cc::display_width(text.substr(word_start, i - word_start))});
            word_start = i + 1;
        }
    }

    // The rows the line is broken into.
    std::vector<Row> rows;
    rows.push_back({start, start, 0});

    for (size_t index = 0; index < words.size(); ++index) {
        const Word& word = words[index];

        if (index != 0) {
            if (rows.back().width >= width) {
                // The row is full: the word starts a new one, and the space
                // that stands before it belongs to the new row.
                rows.push_back({word.start - 1, word.start - 1, 0});
            }
            // The space that stands between the words belongs to the row.
            rows.back().end = word.start;
            rows.back().width += 1;
        }

        if (word.width > width) {
            // The word is wider than the row. It goes on the row that is
            // left when it needs no more rows there than it would need on a
            // row of its own.
            const size_t left = width - rows.back().width;
            const size_t here = 1 + (word.width - left - 1) / width;
            const size_t next = (word.width - 1) / width;
            if (next < here) rows.push_back({word.start, word.start, 0});
            split_word(text.substr(word.start, word.end - word.start), word.start, width, rows);
            continue;
        }

        if (rows.back().width + word.width > width && rows.back().width > 0 && word.width > 0) {
            // The word does not fit in the row: it starts a new one.
            rows.push_back({word.start, word.start, 0});
        }

        rows.back().end = word.end;
        rows.back().width += word.width;
    }

    for (size_t index = 0; index < rows.size(); ++index) {
        Row row = rows[index];

        // The row that follows the first one of the line starts with the
        // space that broke it; the reference leaves the space out.
        if (index > 0) {
            while (row.start < row.end && std::isspace(static_cast<unsigned char>(text[row.start]))) {
                ++row.start;
            }
        }

        out.lines.push_back(std::string(text.substr(row.start, row.end - row.start)));
        out.starts.push_back(row.start);
    }
}

/// Lays the text out in the lines it is drawn as.
Layout layout_input(std::string_view text, size_t width) {
    Layout out;
    if (width == 0) width = 1;

    // The text is broken at the newlines first, and every line is wrapped
    // on its own.
    size_t content = 0;  // The first byte of the current line.
    size_t i = 0;
    while (true) {
        if (i >= text.size()) {
            wrap_line(text, content, text.size(), width, out);
            break;
        }

        const char ch = text[i];
        if (ch == '\r' || ch == '\n') {
            // The line ends here.
            wrap_line(text, content, i, width, out);

            size_t next = i + 1;
            if (ch == '\r' && next < text.size() && text[next] == '\n') ++next;
            content = next;
            i = next;
            continue;
        }
        ++i;
    }

    return out;
}

/// True when the user has written a newline with the backslash and the
/// return key.
///
/// The reference remembers this and shortens the instruction in the
/// shortcuts menu.
bool& used_backslash_return() {
    static bool used = false;
    return used;
}

/// The file the example commands name.
///
/// The reference names one of the files the session touches often; until it
/// knows one, it names a placeholder.
constexpr std::string_view kFrequentFile = "<filepath>";

/// The number of lines of the input that are drawn at least.
///
/// The reference keeps three lines in the box, however small the screen is.
constexpr size_t kMinInputViewportLines = 3;

/// The number of lines the footer of the box takes.
///
/// The reference leaves this much room below the input for the footer.
constexpr size_t kPromptFooterLines = 5;

/// The indent of the lines that sit below the box.
constexpr size_t kFooterIndent = 2;

/// Wraps a text that carries colours of its own at `width` columns.
///
/// The escape sequences take no room on the screen, so they are copied as
/// they are. The colours that are in force where a line is broken are set
/// again at the beginning of the next line, so that the text keeps its
/// colour.
///
/// The reference wraps the text of the footer, so a hint that does not fit
/// takes a row of its own for every part of it that does not.
std::vector<std::string> wrap_styled(std::string_view text, size_t width) {
    std::vector<std::string> out;
    if (width == 0) {
        out.emplace_back(text);
        return out;
    }

    std::string line;
    std::string style;  // the colours that are in force
    size_t cells = 0;   // the columns the line takes

    // The place of the last space of the line: the line is broken there.
    // The place is kept twice, because the text and the line do not have
    // the same offsets: the escape sequences are in the line and not in the
    // text.
    size_t space_in_text = std::string::npos;
    size_t space_in_line = std::string::npos;

    const auto push = [&]() {
        out.push_back(line);
        // The colours are set again on the next line.
        line = style;
        cells = 0;
        space_in_text = std::string::npos;
        space_in_line = std::string::npos;
    };

    size_t at = 0;
    while (at < text.size()) {
        // An escape sequence is copied as it is; it takes no room.
        if (text[at] == '\x1b') {
            size_t end = at + 1;
            if (end < text.size() && text[end] == '[') {
                ++end;
                while (end < text.size() && (text[end] < '@' || text[end] > '~')) ++end;
                if (end < text.size()) ++end;
            }
            const std::string sequence(text.substr(at, end - at));
            line += sequence;
            // A reset takes every colour away; the other sequences set them.
            if (sequence == "\x1b[0m" || sequence == "\x1b[m") {
                style.clear();
            } else {
                style += sequence;
            }
            at = end;
            continue;
        }

        const char c = text[at];
        if (c == ' ') {
            space_in_text = at;
            space_in_line = line.size();
        }

        // The row is full: the line is broken at the last space, or in the
        // middle of the word when there is no space to break at.
        if (cells + 1 > width && !line.empty()) {
            if (space_in_text != std::string::npos) {
                const size_t next = space_in_text + 1;
                line.erase(space_in_line);
                push();
                at = next;
                continue;
            }
            push();
        }

        line.push_back(c);
        ++cells;
        ++at;
    }

    out.push_back(line);
    return out;
}

/// The indent the callers put the shortcuts menu at.
///
/// The menu of the prompt box and the one of the help pane are both drawn
/// two columns to the right.
constexpr size_t kMenuIndent = 2;

/// The first line of the input that is drawn.
///
/// The box may not be taller than the screen, so only the lines around the
/// caret are drawn. The reference keeps half of the room above the caret
/// and half of it below.
size_t viewport_start(size_t cursor_line, size_t lines, size_t max_visible) {
    if (max_visible == 0 || lines <= max_visible) return 0;

    const size_t half = max_visible / 2;
    size_t start = cursor_line > half ? cursor_line - half : 0;
    const size_t end = std::min(lines, start + max_visible);
    if (end - start < max_visible) start = end > max_visible ? end - max_visible : 0;
    return start;
}

/// The number of lines of the input that may be drawn.
///
/// The box may not be taller than the screen, so the input is cut down to
/// the lines around the caret. The reference keeps half of the screen for
/// the input and the rest for the footer.
size_t max_visible_lines() {
    const int rows = Terminal::instance().height();
    if (rows <= 0) return kMinInputViewportLines;

    const size_t half = static_cast<size_t>(rows / 2);
    const size_t room = half > kPromptFooterLines ? half - kPromptFooterLines : 0;
    return std::max(kMinInputViewportLines, room);
}

/// One keybinding: the context it belongs to, the key and the action.
struct Binding {
    /// The context the binding belongs to, e.g. `Chat`.
    std::string context;
    /// The key, e.g. `ctrl+shift+-`.
    std::string key;
    /// The action the key does, e.g. `chat:undo`.
    std::string action;
};

/// The bindings the CLI ships with.
///
/// The reference reads them from `keybindings/defaultBindings.ts`. The last
/// binding of an action in a context is the one that is written down, so
/// it is the second binding of `chat:undo` that the menu names.
const std::vector<Binding>& built_in_bindings() {
    static const std::vector<Binding> bindings = {
        {"Global", "ctrl+o", "app:toggleTranscript"},
        {"Global", "ctrl+t", "app:toggleTodos"},
        {"Chat", "shift+tab", "chat:cycleMode"},
        {"Chat", "meta+p", "chat:modelPicker"},
        {"Chat", "meta+o", "chat:fastMode"},
        {"Chat", "ctrl+g", "chat:externalEditor"},
        {"Chat", "ctrl+s", "chat:stash"},
        {"Chat", "ctrl+v", "chat:imagePaste"},
        {"Chat", "ctrl+_", "chat:undo"},
        {"Chat", "ctrl+shift+-", "chat:undo"},
    };
    return bindings;
}

/// The bindings the user wrote in `~/.claude/keybindings.json`.
///
/// The file holds a list of blocks, each of them a context and the keys
/// that do something in it. A key that is bound to nothing is left out.
std::vector<Binding> user_bindings() {
    std::vector<Binding> bindings;

    const auto contents = cc::read_file(cc::path_join(cc::claude_config_dir(), "keybindings.json"));
    if (!contents) return bindings;

    const auto parsed = cc::json_parse(*contents, boost::json::value(boost::json::object{}));
    if (!parsed.is_object()) return bindings;

    const auto& root = parsed.as_object();
    const auto blocks = root.find("bindings");
    if (blocks == root.end() || !blocks->value().is_array()) return bindings;

    for (const auto& block : blocks->value().as_array()) {
        if (!block.is_object()) continue;
        const auto& object = block.as_object();

        const auto context = object.find("context");
        if (context == object.end() || !context->value().is_string()) continue;

        const auto keys = object.find("bindings");
        if (keys == object.end() || !keys->value().is_object()) continue;

        for (const auto& entry : keys->value().as_object()) {
            if (!entry.value().is_string()) continue;
            bindings.push_back({std::string(context->value().as_string().c_str()),
                                std::string(entry.key()),
                                std::string(entry.value().as_string().c_str())});
        }
    }

    return bindings;
}

/// The bindings of the session: the ones the CLI ships with and the ones of
/// the user.
///
/// They are read once: the menu of the shortcuts is drawn again on every
/// frame, and the file of the user is not read again for every frame.
const std::vector<Binding>& session_bindings() {
    static const std::vector<Binding> bindings = [] {
        std::vector<Binding> all = built_in_bindings();
        const std::vector<Binding> user = user_bindings();
        all.insert(all.end(), user.begin(), user.end());
        return all;
    }();
    return bindings;
}

/// The name of a key, the way the reference writes it.
std::string key_display_name(std::string_view key) {
    if (key == "escape") return "Esc";
    if (key == " ") return "Space";
    if (key == "tab") return "tab";
    if (key == "enter") return "Enter";
    if (key == "backspace") return "Backspace";
    if (key == "delete") return "Delete";
    if (key == "up") return "↑";
    if (key == "down") return "↓";
    if (key == "left") return "←";
    if (key == "right") return "→";
    if (key == "pageup") return "PageUp";
    if (key == "pagedown") return "PageDown";
    if (key == "home") return "Home";
    if (key == "end") return "End";
    return std::string(key);
}

/// The name of a binding, the way the reference writes it.
///
/// The modifiers come first, in the order of the reference, and the name of
/// the key last: `ctrl + shift + -`.
std::string binding_display_name(std::string_view binding) {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool meta = false;
    bool cmd = false;
    std::string key;

    for (const std::string& part : cc::split(binding, '+')) {
        const std::string lower = cc::to_lower(part);
        if (lower == "ctrl" || lower == "control") {
            ctrl = true;
        } else if (lower == "alt" || lower == "opt" || lower == "option") {
            alt = true;
        } else if (lower == "shift") {
            shift = true;
        } else if (lower == "meta") {
            meta = true;
        } else if (lower == "cmd" || lower == "command" || lower == "super" || lower == "win") {
            cmd = true;
        } else {
            key = part;
        }
    }

    std::string out;
    const auto add = [&](const std::string& name) {
        if (!out.empty()) out += "+";
        out += name;
    };
    if (ctrl) add("ctrl");
    if (alt) add("alt");
    if (shift) add("shift");
    if (meta) add("meta");
    if (cmd) add("cmd");
    if (!key.empty()) add(key_display_name(key));

    // The reference writes the name of a shortcut with spaces around the
    // plus signs.
    std::string spaced;
    for (const char c : out) {
        if (c == '+') {
            spaced += " + ";
        } else {
            spaced += c;
        }
    }
    return spaced;
}

/// The name of the key an action is bound to.
///
/// The reference resolves the action through the bindings of the session:
/// the last binding of the action in that context wins, so the key the user
/// has bound is the one that is written down.
std::string shortcut_for(const std::string& action, const std::string& context,
                         const std::string& fallback) {
    const auto& bindings = session_bindings();
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
        if (it->action != action || it->context != context) continue;
        return binding_display_name(it->key);
    }
    return binding_display_name(fallback);
}

} // namespace

PromptInput::PromptInput(Theme theme, std::string cwd)
    : theme_(std::move(theme)), cwd_(std::move(cwd)) {
    // The reference picks one of its example commands when the session
    // starts and shows it in the empty prompt box. The file the commands
    // name is one of the files the session touches often; the reference
    // names a placeholder until it knows one.
    const std::string file(kFrequentFile);
    static const std::vector<std::string> kExamples = {
        "fix lint errors",
        "fix typecheck errors",
        "how does " + file + " work?",
        "refactor " + file,
        "how do I log an error?",
        "edit " + file + " to...",
        "write a test for " + file,
        "create a util logging.py that...",
    };

    std::random_device seed;
    std::mt19937 engine(seed());
    std::uniform_int_distribution<size_t> pick(0, kExamples.size() - 1);
    example_command_ = kExamples[pick(engine)];
}

void PromptInput::set_text(std::string text) {
    text_ = std::move(text);
    cursor_ = text_.size();
}

void PromptInput::insert(std::string_view chunk) {
    if (chunk.empty()) return;
    text_.insert(cursor_, chunk);
    cursor_ += chunk.size();
}

void PromptInput::backspace() {
    if (cursor_ == 0) return;
    const size_t previous = previous_boundary(text_, cursor_);
    text_.erase(previous, cursor_ - previous);
    cursor_ = previous;
}

void PromptInput::delete_forward() {
    if (cursor_ >= text_.size()) return;
    const size_t next = next_boundary(text_, cursor_);
    text_.erase(cursor_, next - cursor_);
}

void PromptInput::kill_to_start() {
    if (cursor_ == 0) return;
    text_.erase(0, cursor_);
    cursor_ = 0;
}

void PromptInput::kill_to_end() {
    if (cursor_ >= text_.size()) return;
    text_.erase(cursor_);
}

void PromptInput::delete_word_backwards() {
    if (cursor_ == 0) return;

    // Skip the spaces in front of the cursor, then the word itself.
    size_t at = cursor_;
    while (at > 0 && text_[at - 1] == ' ') --at;
    while (at > 0 && text_[at - 1] != ' ') --at;
    text_.erase(at, cursor_ - at);
    cursor_ = at;
}

void PromptInput::delete_word_forward() {
    if (cursor_ >= text_.size()) return;

    // Skip the spaces under the cursor, then the word itself.
    size_t at = cursor_;
    while (at < text_.size() && text_[at] == ' ') ++at;
    while (at < text_.size() && text_[at] != ' ') ++at;
    text_.erase(cursor_, at - cursor_);
}

void PromptInput::move_left() { cursor_ = previous_boundary(text_, cursor_); }

void PromptInput::move_right() { cursor_ = next_boundary(text_, cursor_); }

void PromptInput::move_home() {
    if (cursor_ == 0) return;
    const size_t newline = text_.rfind('\n', cursor_ - 1);
    cursor_ = (newline == std::string::npos) ? 0 : newline + 1;
}

void PromptInput::move_end() {
    const size_t newline = text_.find('\n', cursor_);
    cursor_ = (newline == std::string::npos) ? text_.size() : newline;
}

bool PromptInput::move_up() {
    if (cursor_ == 0) return false;
    const size_t newline = text_.rfind('\n', cursor_ - 1);
    if (newline == std::string::npos) return false;
    const size_t previous = text_.rfind('\n', newline - 1);
    cursor_ = (previous == std::string::npos) ? 0 : previous + 1;
    return true;
}

bool PromptInput::move_down() {
    const size_t newline = text_.find('\n', cursor_);
    if (newline == std::string::npos) return false;
    cursor_ = newline + 1;
    return true;
}

void PromptInput::insert_newline() {
    used_backslash_return() = true;
    insert("\n");
}

bool PromptInput::has_backslash_before_cursor() const {
    return cursor_ > 0 && text_[cursor_ - 1] == '\\';
}

void PromptInput::clear() {
    text_.clear();
    cursor_ = 0;
}

InputMode PromptInput::mode() const {
    if (!text_.empty() && text_.front() == '!') return InputMode::Bash;
    return InputMode::Prompt;
}

std::string PromptInput::value_for_mode() const {
    if (mode() == InputMode::Bash) return text_.substr(1);
    return text_;
}

std::string PromptInput::placeholder() const {
    // The placeholder is shown only before the first message, the way the
    // reference shows it.
    if (submit_count_ >= 1) return {};
    return "Try \"" + example_command_ + "\"";
}

void PromptInput::note_submit() { ++submit_count_; }

void PromptInput::set_vim_mode(bool enabled, bool insert) {
    vim_enabled_ = enabled;
    vim_insert_ = insert;
}

std::string PromptInput::hint_line(bool loading) const {
    // The reference names the paste in the footer while it is going on.
    if (pasting_) return "Pasting text…";

    // The vim mode names the insert mode in the footer, the way Claude Code
    // does.
    if (vim_enabled_ && vim_insert_) return "-- INSERT --";

    // The reference names the bash mode in the footer.
    if (mode() == InputMode::Bash) return "! for bash mode";
    if (!text_.empty()) return {};
    return loading ? "esc to interrupt" : "? for shortcuts";
}

std::vector<std::string> PromptInput::render_box(int width, bool loading,
                                                  std::string_view argument_hint) const {
    const size_t w = width > 0 ? static_cast<size_t>(width) : 1;
    const bool bash = mode() == InputMode::Bash;

    // The rule is drawn in the colour of the border of the box. The themes
    // that name the colours of the palette of the terminal draw it in the
    // white of the terminal, the way the reference does.
    const Style border_style = Style::fg(bash ? theme_.bash_border : theme_.prompt_border);

    std::vector<std::string> out;
    out.push_back(styled(repeat("─", w), border_style));

    // The prompt character, followed by a space.
    std::string prompt;
    Style prompt_style;
    if (bash) {
        prompt = "! ";
        prompt_style = Style::fg(theme_.bash_border);
    } else {
        prompt = "❯ ";
    }

    // The prompt character is dimmed while a request is running.
    if (loading) prompt_style.dim = true;

    const std::string displayed = value_for_mode();
    const size_t offset = bash ? 1 : 0;
    const size_t cursor = cursor_ > offset ? cursor_ - offset : 0;

    if (displayed.empty()) {
        Style placeholder_style;
        placeholder_style.dim = true;

        // The character under the caret is drawn in reverse video, the way
        // Claude Code draws the placeholder.
        Style caret_style;
        caret_style.inverse = true;

        std::string text = placeholder();
        if (cc::display_width(text) + 2 > w) text = cc::truncate(text, w > 2 ? w - 2 : 1);

        if (text.empty()) {
            // The caret is drawn even when there is no text and no example
            // command: the reference inverts the character under it, and
            // there is none, so it inverts a space.
            out.push_back(styled(prompt, prompt_style) + styled(" ", caret_style));
        } else {
            const auto clusters = cc::graphemes(text);
            const std::string first = clusters.empty() ? std::string() : clusters.front();
            out.push_back(styled(prompt, prompt_style) + styled(first, caret_style) +
                          styled(text.substr(first.size()), placeholder_style));
        }
        out.push_back(styled(repeat("─", w), border_style));
        return out;
    }

    // The text is wrapped to the width of the box, leaving room for the
    // prompt character and the margin on the right.
    //
    // The reference measures the text at four columns less than the width
    // of the terminal: one for the prompt character, one for the space that
    // follows it and two for the margin.
    const size_t text_width = w > 4 ? w - 4 : 1;
    const Layout layout = layout_input(displayed, text_width);

    // The line the caret is on.
    size_t cursor_line = 0;
    for (size_t i = 0; i < layout.starts.size(); ++i) {
        if (cursor >= layout.starts[i]) cursor_line = i;
    }
    const size_t cursor_column =
        cursor > layout.starts[cursor_line] ? cursor - layout.starts[cursor_line] : 0;

    // The character under the caret is drawn in reverse video, the way
    // Claude Code draws it.
    Style cursor_style;
    cursor_style.inverse = true;

    Style dim_style;
    dim_style.dim = true;

    // The commands the user is typing that the session knows are painted in
    // the colour of the suggestions, the way the reference paints them.
    const std::vector<Painted> painted = painted_commands(displayed, theme_);

    // The command that is being typed in the middle of the input is
    // completed right after the caret. The reference draws the first
    // character of the completion in the block of the caret and the rest of
    // it dim.
    std::string ghost;
    std::string ghost_first;
    if (cursor == displayed.size()) {
        ghost = ghost_text(displayed, cursor);
        if (!ghost.empty()) {
            const auto clusters = cc::graphemes(ghost);
            ghost_first = clusters.empty() ? std::string() : clusters.front();
            ghost = ghost.substr(ghost_first.size());
        }
    }

    // Only the lines around the caret are drawn: the box may not be taller
    // than the screen, or the frames pile up in the scrollback.
    const size_t visible = max_visible_lines();
    const size_t first = viewport_start(cursor_line, layout.lines.size(), visible);
    const size_t last = std::min(layout.lines.size(), first + visible);

    for (size_t i = first; i < last; ++i) {
        const std::string& content = layout.lines[i];
        std::string before = content;
        std::string at;
        std::string after;
        size_t after_offset = layout.starts[i];
        if (i == cursor_line) {
            const size_t split = std::min(cursor_column, content.size());
            before = content.substr(0, split);
            const auto clusters = cc::graphemes(content.substr(split));
            if (!clusters.empty()) {
                at = clusters.front();
                after = content.substr(split + at.size());
                after_offset = layout.starts[i] + split + at.size();
            }
        }
        // The prompt character stands at the top of the box, whatever lines
        // of the input are shown.
        std::string line = (i == first) ? styled(prompt, prompt_style) : std::string("  ");
        append_painted(line, before, layout.starts[i], painted);
        if (i == cursor_line) {
            if (at.empty()) {
                if (ghost_first.empty()) {
                    // The caret is at the end of the text: the block is drawn
                    // on a space, the way Claude Code draws it.
                    line += styled(" ", cursor_style);
                } else {
                    // The first character of the completion stands in the
                    // block of the caret, the way Claude Code draws it.
                    line += styled(ghost_first, cursor_style);
                    line += styled(ghost, dim_style);
                }
            } else {
                // The character under the caret is drawn in reverse video,
                // and the rest of the line is left as it is.
                line += styled(at, cursor_style);
                append_painted(line, after, after_offset, painted);
            }
        } else {
            append_painted(line, after, after_offset, painted);
        }
        out.push_back(std::move(line));
    }

    // The hint of the arguments of the command is drawn dim right after the
    // text, the way the reference draws it.
    if (!argument_hint.empty() && !displayed.empty() && out.size() >= 2) {
        // The hint stands after the text, so it is drawn on the last line
        // of the text — or on the last line that is shown, when the text
        // does not fit on the screen.
        size_t line = layout.lines.size() - 1;
        if (line < first || line >= last) line = last - 1;

        // The hint is cut down to the room that is left in the box.
        size_t used = kFooterIndent + cc::display_width(layout.lines[line]);
        if (line == cursor_line) ++used;
        const size_t room = w > used ? w - used : 0;

        std::string hint_text(argument_hint);
        if (!cc::ends_with(displayed, " ")) hint_text = " " + hint_text;
        if (cc::display_width(hint_text) > room) hint_text = cc::truncate(hint_text, room);

        Style hint_style;
        hint_style.dim = true;
        out[line - first + 1] += styled(hint_text, hint_style);
    }

    out.push_back(styled(repeat("─", w), border_style));
    return out;
}

std::vector<std::string> PromptInput::render(int width, bool loading, std::string_view hint,
                                              std::string_view status_line, bool suppress_hint,
                                              std::string_view argument_hint,
                                              std::string_view notification,
                                              int status_line_padding) const {
    std::vector<std::string> out = render_box(width, loading, argument_hint);

    Style dim_style;
    dim_style.dim = true;

    const size_t w = width > 0 ? static_cast<size_t>(width) : 1;
    const bool bash = mode() == InputMode::Bash;

    // The status line sits between the box and the hint, the way Claude
    // Code draws it.
    //
    // The reference leaves it out in the bash mode and while the user is
    // pasting.
    const size_t first_footer_row = out.size();
    if (!status_line.empty() && !bash && !pasting_) {
        // The padding of the setting widens the indent of the status line,
        // the way the reference widens it.
        const size_t indent =
            kFooterIndent +
            (status_line_padding > 0 ? static_cast<size_t>(status_line_padding) : 0);
        const size_t room = w > indent ? w - indent : 1;

        // Every line of the status line is indented on its own, the way the
        // reference indents them.
        for (const auto& row : cc::split(status_line, '\n')) {
            std::string text(row);
            if (cc::display_width(text) > room) text = cc::truncate(text, room);
            out.push_back(std::string(indent, ' ') + styled(text, dim_style));
        }
    }

    // Claude Code hides the usual hint line as soon as a status line is
    // configured, even when the command wrote nothing.
    //
    // The name of the bash mode and the name of the paste are shown all the
    // same, the way the reference shows them.
    const bool vim_insert = vim_enabled_ && vim_insert_;
    const bool bash_hint = bash && !vim_insert && !pasting_;

    std::string text = hint.empty() ? std::string() : std::string(hint);
    bool named_bash = false;
    if (text.empty()) {
        if (bash_hint || pasting_ || !suppress_hint) {
            text = hint_line(loading);
            named_bash = bash_hint;
        }
    }

    if (!text.empty()) {
        // The hint is wrapped at the width of the terminal, the way the
        // reference wraps it: a hint that does not fit takes a row of its
        // own for every part of it that does not.
        const size_t room = w > kFooterIndent ? w - kFooterIndent : 1;
        for (const auto& part : wrap_styled(text, room)) {
            if (named_bash) {
                // The bash mode is named in the colour of the bash border.
                out.push_back(std::string(kFooterIndent, ' ') +
                             styled(part, Style::fg(theme_.bash_border)));
            } else if (part.find('\x1b') != std::string::npos) {
                // The hint already carries its own colours.
                out.push_back(std::string(kFooterIndent, ' ') + part);
            } else {
                out.push_back(std::string(kFooterIndent, ' ') + styled(part, dim_style));
            }
        }
    }

    // The notification is raised at the right of the first row of the
    // footer, the way the reference raises its notifications.
    if (!notification.empty()) {
        // A notification that carries no colour of its own is dim, the way
        // the reference dims it.
        const bool coloured = notification.find('\x1b') != std::string::npos;
        const std::string note = coloured ? std::string(notification)
                                           : styled(notification, dim_style);

        // The notification that does not fit in the row is cut down, the way
        // the reference cuts it down.
        const auto cut = [&](size_t room) {
            if (cc::display_width(note) <= room) return note;
            std::string text = cc::truncate(note, room, "");
            if (coloured) text += kReset;
            return text;
        };

        const auto raise = [&](std::string& row) {
            const size_t used = cc::display_width(row);
            const size_t width = cc::display_width(note);
            const size_t room = w > used + width + kFooterIndent ? w - used - width - kFooterIndent : 0;
            row += std::string(room, ' ') + cut(w > used + kFooterIndent ? w - used - kFooterIndent : 0);
        };

        // Below eighty columns the footer is a column: the notification
        // stands on a row of its own, under the hint.
        if (w < 80) {
            std::string row(kFooterIndent, ' ');
            row += cut(w > kFooterIndent ? w - kFooterIndent : 0);
            out.push_back(std::move(row));
        } else if (first_footer_row < out.size()) {
            raise(out[first_footer_row]);
        } else {
            std::string row;
            raise(row);
            out.push_back(std::move(row));
        }
    }
    return out;
}

std::vector<std::string> PromptInput::shortcuts_menu(int width, bool dim, size_t gap) {
    static const std::vector<std::string> kColumn1 = {
        "! for bash mode",
        "/ for commands",
        "@ for file paths",
        "& for background",
        "/btw for side question",
    };

    // The reference writes the key the action is bound to, so the key the
    // user has bound is the one that is named.
    const std::string cycle_mode = shortcut_for("chat:cycleMode", "Chat", "shift+tab");
    const std::string transcript = shortcut_for("app:toggleTranscript", "Global", "ctrl+o");
    const std::string todos = shortcut_for("app:toggleTodos", "Global", "ctrl+t");
    const std::string undo = shortcut_for("chat:undo", "Chat", "ctrl+_");
    const std::string image_paste = shortcut_for("chat:imagePaste", "Chat", "ctrl+v");
    const std::string model_picker = shortcut_for("chat:modelPicker", "Chat", "alt+p");
    const std::string stash = shortcut_for("chat:stash", "Chat", "ctrl+s");
    const std::string external_editor = shortcut_for("chat:externalEditor", "Chat", "ctrl+g");

    const std::vector<std::string> kColumn2 = {
        "double tap esc to clear input",
        cycle_mode + " to auto-accept edits",
        transcript + " for verbose output",
        todos + " to toggle tasks",
    };
    const std::vector<std::string> kColumn3 = {
        undo + " to undo",
        "ctrl + z to suspend",
        image_paste + " to paste images",
        model_picker + " to switch model",
        stash + " to stash prompt",
        external_editor + " to edit in $EDITOR",
    };

    constexpr size_t kFirstColumnWidth = 24;
    constexpr size_t kSecondColumnWidth = 35;

    // The menu under the prompt box has no gap between its columns; the
    // one of the help pane has a gap of two.
    const size_t kGap = gap;

    // The reference shortens the instruction once the user has written a
    // newline with the backslash and the return key.
    std::vector<std::string> column2_entries = kColumn2;
    column2_entries.push_back(used_backslash_return() ? "\\⏎ for newline"
                                                      : "backslash (\\) + return (⏎) for newline");

    // Every column is an independent stack of lines, so a long entry simply
    // takes two rows in its own column.
    auto flatten_column = [](const std::vector<std::string>& entries, size_t column_width) {
        std::vector<std::string> lines;
        for (const auto& entry : entries) {
            auto wrapped = cc::word_wrap(entry, column_width);
            if (wrapped.empty()) wrapped.push_back(std::string());
            for (auto& line : wrapped) lines.push_back(std::move(line));
        }
        return lines;
    };

    // The third column takes the room that is left, so that its entries are
    // wrapped inside it and not cut off by the terminal.
    //
    // The callers draw the menu two columns to the right, so those columns
    // are taken off the room that is left, the way the reference leaves them
    // out.
    const size_t used = kFirstColumnWidth + kSecondColumnWidth + 2 * kGap + kMenuIndent;
    const size_t third_column_width =
        width > static_cast<int>(used) ? static_cast<size_t>(width) - used : 1;

    const std::vector<std::string> column1 = flatten_column(kColumn1, kFirstColumnWidth);
    const std::vector<std::string> column2 = flatten_column(column2_entries, kSecondColumnWidth);
    const std::vector<std::string> column3 = flatten_column(kColumn3, third_column_width);

    const size_t rows = std::max({column1.size(), column2.size(), column3.size()});

    Style dim_style;
    dim_style.dim = dim;

    std::vector<std::string> out;
    for (size_t row = 0; row < rows; ++row) {
        std::string line;
        const auto append = [&](const std::vector<std::string>& column, size_t column_width) {
            const std::string text = row < column.size() ? column[row] : std::string();
            line += pad(text, column_width);
            line += std::string(kGap, ' ');
        };
        append(column1, kFirstColumnWidth);
        append(column2, kSecondColumnWidth);
        if (row < column3.size()) line += column3[row];
        out.push_back(dim ? styled(line, dim_style) : line);
    }
    return out;
}

} // namespace cc::ui
