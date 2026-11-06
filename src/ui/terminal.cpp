#include "ui/terminal.hpp"

#include "config/global_config.hpp"
#include "ui/ansi.hpp"
#include "ui/clipboard.hpp"
#include "utils/string.hpp"

#include <maya/platform/io.hpp>
#include <maya/terminal/input.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <utility>
#include <variant>

#ifndef _WIN32
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace cc::ui {
namespace {

/// The flag the signal of the resize sets.
///
/// The handler of the signal may only set the flag: everything else is
/// done by the thread that owns the terminal.
std::atomic<bool> g_resized{false};

/// The handler of the signal of the resize.
extern "C" void on_resize(int) { g_resized.store(true, std::memory_order_relaxed); }


#ifndef _WIN32
/// The terminal's original settings, captured before raw mode is enabled.
struct RawModeGuard {
    termios saved{};
    bool valid = false;

    /// Puts the saved settings back and forgets them.
    ///
    /// Forgetting them matters: entering raw mode again has to capture the
    /// original settings, not the raw ones that were saved by mistake.
    void restore() {
        if (!valid) return;
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        valid = false;
    }

    ~RawModeGuard() { restore(); }
};

/// The guard that holds the terminal's original settings.
///
/// It lives for the whole run of the program, so the settings are restored
/// even when the process dies without leaving raw mode.
RawModeGuard& raw_mode_guard() {
    static RawModeGuard guard;
    return guard;
}
#endif

/// The marker the terminal puts in front of a pasted block.
constexpr std::string_view kPasteStart = "\x1b[200~";

/// The marker the terminal puts behind a pasted block.
constexpr std::string_view kPasteEnd = "\x1b[201~";

/// The length of both bracketed-paste markers.
constexpr size_t kPasteMarkerSize = 6;

/// How long the terminal waits for the rest of an escape sequence before it
/// treats what it has as a lone Escape.
///
/// The reference waits for the same 50 ms before it flushes an incomplete
/// escape sequence (`NORMAL_TIMEOUT` in `ink/components/App.tsx`).
constexpr int kEscapeTimeoutMs = 50;

/// How long the terminal waits for the end of a bracketed paste before it
/// hands over the text it has collected.
///
/// The reference waits for 500 ms (`PASTE_TIMEOUT` in
/// `ink/components/App.tsx`).
constexpr int kPasteTimeoutMs = 500;

/// The prefixes of the bracketed-paste markers.
///
/// A read can end in the middle of a marker, so the bytes that could still
/// grow into one are held back until the next read completes them.
const std::vector<std::string>& paste_marker_prefixes() {
    static const std::vector<std::string> prefixes = {
        "\x1b", "\x1b[", "\x1b[2", "\x1b[20", "\x1b[200", "\x1b[201",
    };
    return prefixes;
}

/// The length of the longest suffix of `text` that could still grow into a
/// bracketed-paste marker, or zero when there is no such suffix.
size_t marker_prefix_length(std::string_view text) {
    const size_t limit = std::min(kPasteMarkerSize - 1, text.size());
    for (size_t len = limit; len > 0; --len) {
        for (const std::string& prefix : paste_marker_prefixes()) {
            if (prefix.size() == len && text.compare(text.size() - len, len, prefix) == 0) {
                return len;
            }
        }
    }
    return 0;
}

/// Normalises a pasted payload the way the reference does before it lands in
/// the prompt: escape sequences are stripped, carriage returns become
/// newlines and tabs become four spaces.
///
/// See `onTextPaste` in `components/PromptInput/PromptInput.tsx` of the
/// reference.
std::string normalise_paste(std::string_view text) {
    const std::string plain = strip(text);
    std::string out;
    out.reserve(plain.size());
    for (const char c : plain) {
        if (c == '\r') {
            out.push_back('\n');
        } else if (c == '\t') {
            out.append("    ");
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// The type of the key that a control character stands for.
///
/// The terminal sends the control of a letter as the letter itself with the
/// control held: Ctrl-C is the letter C.
Key::Type control_key(char32_t letter) {
    switch (letter) {
    case U'a': return Key::Type::CtrlA;
    case U'b': return Key::Type::CtrlB;
    case U'c': return Key::Type::CtrlC;
    case U'd': return Key::Type::CtrlD;
    case U'e': return Key::Type::CtrlE;
    case U'f': return Key::Type::CtrlF;
    case U'g': return Key::Type::CtrlG;
    case U'k': return Key::Type::CtrlK;
    case U'l': return Key::Type::CtrlL;
    case U'n': return Key::Type::CtrlN;
    case U'o': return Key::Type::CtrlO;
    case U'p': return Key::Type::CtrlP;
    case U'r': return Key::Type::CtrlR;
    case U's': return Key::Type::CtrlS;
    case U't': return Key::Type::CtrlT;
    case U'u': return Key::Type::CtrlU;
    case U'w': return Key::Type::CtrlW;
    case U'x': return Key::Type::CtrlX;
    case U'y': return Key::Type::CtrlY;
    case U'z': return Key::Type::CtrlZ;
    default: return Key::Type::Unknown;
    }
}

/// True when the event carries the bytes of an escape sequence.
///
/// The parser hands over a question mark for a sequence it cannot read, and
/// the question mark is not a character the user typed: it is the mark of a
/// sequence the terminal sent and the UI does not know.
bool is_escape_sequence(const maya::KeyEvent& event) {
    return !event.raw_sequence.empty() && event.raw_sequence[0] == '\x1b';
}

/// True when the character is one the user can type with the option held.
///
/// The terminal sends the option with a letter, a digit or the space; the
/// option with anything else is a key of its own, and the UI has no use for
/// it.
bool is_alt_character(char32_t codepoint) {
    return codepoint == U' ' || (codepoint >= U'0' && codepoint <= U'9') ||
           (codepoint >= U'a' && codepoint <= U'z') || (codepoint >= U'A' && codepoint <= U'Z');
}

/// Translates a key of maya into the key the UI cares about.
///
/// A key that the UI has no use for is `Key::Type::Unknown`, and the caller
/// drops it.
Key translate(const maya::KeyEvent& event) {
    Key key;
    key.type = Key::Type::Unknown;

    if (const auto* character = std::get_if<maya::CharKey>(&event.key)) {
        const char32_t codepoint = character->codepoint;

        // The control of a letter arrives as the letter itself: Ctrl-C is
        // the letter C with the control held.
        if (event.mods.ctrl) {
            // The command of the keyboard of Apple with the backspace
            // arrives as the control of the letter H with the option held:
            // the terminal tells the two apart by nothing at all.
            if (event.mods.alt && codepoint == U'h') {
                key.type = Key::Type::Backspace;
                key.cmd = true;
                return key;
            }
            // The option with the control is a key of its own, and the UI
            // has no use for it.
            if (event.mods.alt) return key;

            key.type = control_key(codepoint);
            return key;
        }

        // The parser hands over a question mark for every sequence it
        // cannot read: the terminal sent something the UI does not know,
        // and the question mark is not something the user typed.
        if (codepoint == U'?' && is_escape_sequence(event)) return key;

        // A character that was cut in half is the mark of a lost byte, not
        // a character: the replacement one is three bytes long, and the
        // broken one is shorter.
        if (codepoint == 0xFFFD && event.raw_sequence.size() < 3) return key;

        // The option with a character that cannot be typed with it is a
        // key of its own.
        if (event.mods.alt && !is_alt_character(codepoint)) return key;

        key.type = Key::Type::Character;
        key.text = cc::utf8_encode(codepoint);
        key.alt = event.mods.alt;
        return key;
    }

    const maya::SpecialKey special = std::get<maya::SpecialKey>(event.key);
    switch (special) {
    case maya::SpecialKey::Enter:
        // The Enter with a modifier is not the Enter of the prompt.
        if (event.mods.ctrl || event.mods.alt || event.mods.shift) break;
        key.type = Key::Type::Enter;
        break;
    case maya::SpecialKey::Tab:
        if (event.mods.ctrl || event.mods.alt) break;
        key.type = Key::Type::Tab;
        break;
    case maya::SpecialKey::BackTab:
        key.type = Key::Type::ShiftTab;
        key.shift = true;
        break;
    case maya::SpecialKey::Backspace:
        // The control of the backspace is the control of the letter H,
        // which the terminal sends as a byte of its own: the terminal
        // reads it as the backspace and nothing more.
        key.type = Key::Type::Backspace;
        key.shift = event.mods.shift;
        key.alt = event.mods.alt;
        key.cmd = event.mods.super_;
        break;
    case maya::SpecialKey::Delete:
        key.type = Key::Type::Delete;
        key.ctrl = event.mods.ctrl;
        key.shift = event.mods.shift;
        key.alt = event.mods.alt;
        key.cmd = event.mods.super_;
        break;
    case maya::SpecialKey::Up:
    case maya::SpecialKey::Down: {
        const bool up = special == maya::SpecialKey::Up;
        // The shift with the arrow is a key of its own.
        if (event.mods.shift) {
            if (event.mods.ctrl || event.mods.alt) break;
            key.type = up ? Key::Type::ShiftUp : Key::Type::ShiftDown;
            key.shift = true;
            break;
        }
        key.type = up ? Key::Type::Up : Key::Type::Down;
        key.ctrl = event.mods.ctrl;
        key.alt = event.mods.alt;
        break;
    }
    case maya::SpecialKey::Left:
        if (event.mods.ctrl || event.mods.alt || event.mods.shift) break;
        key.type = Key::Type::Left;
        break;
    case maya::SpecialKey::Right:
        if (event.mods.ctrl || event.mods.alt || event.mods.shift) break;
        key.type = Key::Type::Right;
        break;
    case maya::SpecialKey::Home:
        if (event.mods.ctrl || event.mods.alt || event.mods.shift) break;
        key.type = Key::Type::Home;
        break;
    case maya::SpecialKey::End:
        if (event.mods.ctrl || event.mods.alt || event.mods.shift) break;
        key.type = Key::Type::End;
        break;
    case maya::SpecialKey::PageUp:
        if (event.mods.ctrl || event.mods.alt || event.mods.shift) break;
        key.type = Key::Type::PageUp;
        break;
    case maya::SpecialKey::PageDown:
        if (event.mods.ctrl || event.mods.alt || event.mods.shift) break;
        key.type = Key::Type::PageDown;
        break;
    case maya::SpecialKey::Escape:
        key.type = Key::Type::Escape;
        key.alt = event.mods.alt;
        break;
    case maya::SpecialKey::F1: key.type = Key::Type::F1; break;
    case maya::SpecialKey::F2: key.type = Key::Type::F2; break;
    case maya::SpecialKey::F3: key.type = Key::Type::F3; break;
    case maya::SpecialKey::F4: key.type = Key::Type::F4; break;
    case maya::SpecialKey::F5: key.type = Key::Type::F5; break;
    case maya::SpecialKey::F6: key.type = Key::Type::F6; break;
    case maya::SpecialKey::F7: key.type = Key::Type::F7; break;
    case maya::SpecialKey::F8: key.type = Key::Type::F8; break;
    case maya::SpecialKey::F9: key.type = Key::Type::F9; break;
    case maya::SpecialKey::F10: key.type = Key::Type::F10; break;
    case maya::SpecialKey::F11: key.type = Key::Type::F11; break;
    case maya::SpecialKey::F12: key.type = Key::Type::F12; break;
    case maya::SpecialKey::Insert:
        // The UI has no use for the insert key.
        break;
    }
    return key;
}

/// Translates an event of the mouse into the key the UI cares about.
Key translate(const maya::MouseEvent& event) {
    Key key;
    key.type = Key::Type::Unknown;

    // The wheel is the sixth bit of the button; it scrolls the list and it
    // is not a press of the left button. The wheel that goes sideways is
    // a wheel that goes down, the way the terminal reports it.
    if (event.button == maya::MouseButton::ScrollUp) {
        key.type = Key::Type::MouseScrollUp;
        return key;
    }
    if (event.button == maya::MouseButton::ScrollDown || event.button == maya::MouseButton::ScrollLeft ||
        event.button == maya::MouseButton::ScrollRight) {
        key.type = Key::Type::MouseScrollDown;
        return key;
    }

    // Only the left button counts.
    if (event.button != maya::MouseButton::Left) return key;

    switch (event.kind) {
    case maya::MouseEventKind::Press:
        // The button went down: the selection begins.
        key.type = Key::Type::MouseDown;
        break;
    case maya::MouseEventKind::Release:
        // The release of the button is the click.
        key.type = Key::Type::MouseClick;
        break;
    case maya::MouseEventKind::Move:
        // The mouse moved while the button was down: the selection grows.
        key.type = Key::Type::MouseDrag;
        break;
    }

    // The columns and the rows count from one.
    key.mouse_x = event.x.value;
    key.mouse_y = event.y.value;
    return key;
}

/// Translates an event of maya into the key the UI cares about.
Key translate(const maya::Event& event) {
    if (const auto* key = std::get_if<maya::KeyEvent>(&event)) return translate(*key);
    if (const auto* mouse = std::get_if<maya::MouseEvent>(&event)) return translate(*mouse);
    if (const auto* paste = std::get_if<maya::PasteEvent>(&event)) {
        Key key;
        key.type = Key::Type::Paste;
        key.text = normalise_paste(paste->content);
        return key;
    }

    // The focus and the size of the terminal are not keys.
    Key key;
    key.type = Key::Type::Unknown;
    return key;
}
/// A piece of input that has been decoded but not yet handed to the caller.
struct PendingInput {
    /// True when the item carries a decoded key, false when it carries raw
    /// bytes that still have to go through the key parser.
    bool is_key = false;

    /// The decoded key, when `is_key` is set.
    Key key;

    /// The raw bytes, when `is_key` is not set.
    std::string bytes;
};

/// The rows a line takes on the screen.
///
/// A line that is wider than the screen takes as many rows as it needs. The
/// escape sequences of the colours take no room, so they stay with the text
/// they belong to.
std::vector<std::string> screen_rows(std::string_view line, int width) {
    if (width <= 0) return {std::string(line)};

    std::vector<std::string> rows;
    std::string current;
    size_t cells = 0;

    const auto clusters = cc::graphemes(line);
    for (size_t i = 0; i < clusters.size(); ++i) {
        const std::string& cluster = clusters[i];

        // An escape sequence is copied as it is; it takes no room.
        if (cluster.size() == 1 && cluster[0] == '\x1b') {
            std::string sequence = cluster;
            while (++i < clusters.size()) {
                sequence += clusters[i];
                if (clusters[i].size() == 1 && clusters[i][0] == 'm') break;
            }
            current += sequence;
            continue;
        }

        const size_t cell = cc::display_width(cluster);
        if (cells + cell > static_cast<size_t>(width) && cells > 0) {
            rows.push_back(current);
            current.clear();
            cells = 0;
        }
        current += cluster;
        cells += cell;
    }

    rows.push_back(current);
    return rows;
}

/// Wraps a line again, at the width of the terminal.
///
/// The line is broken where a word ends, so that a word is never cut in
/// half, and the text that stands at the beginning of the line stands at
/// the beginning of the rows that follow it: a paragraph keeps its place
/// in the gutter when the terminal is resized.
///
/// The escape sequences of the colours take no room. The row that follows a
/// break carries the colour that was in force where the break was made.
std::vector<std::string> reflow_line(std::string_view line, int width) {
    if (width <= 0) return {std::string(line)};

    // The text that stands at the beginning of the line: the gutter of a
    // paragraph, or the margin of a list.
    size_t indent = 0;
    while (indent < line.size() && line[indent] == ' ') ++indent;
    const int room = width - static_cast<int>(indent);
    if (room <= 0) return screen_rows(line, width);

    const auto clusters = cc::graphemes(line.substr(indent));

    std::vector<std::string> rows;
    std::string row(indent, ' ');
    std::string style;  // the colour that is in force
    size_t cells = 0;
    size_t space = std::string::npos;  // where the last space of the row is

    for (size_t i = 0; i < clusters.size(); ++i) {
        const std::string& cluster = clusters[i];

        // An escape sequence is copied as it is; it takes no room.
        if (cluster.size() == 1 && cluster[0] == '\x1b') {
            std::string sequence = cluster;
            while (++i < clusters.size()) {
                sequence += clusters[i];
                if (clusters[i].size() == 1 && clusters[i][0] == 'm') break;
            }
            row += sequence;
            // The row that follows a break carries the colour that is in
            // force here.
            if (sequence == "\x1b[0m" || sequence == "\x1b[m") {
                style.clear();
            } else {
                style += sequence;
            }
            continue;
        }

        const size_t cell = cc::display_width(cluster);
        if (cells + cell > static_cast<size_t>(room) && cells > 0) {
            // The row is full: the line is broken where the last space of
            // the row is, so that the word is not cut in half.
            std::string rest;
            if (space != std::string::npos) {
                rest = row.substr(space + 1);
                row.erase(space);
            }
            rows.push_back(row);
            row = std::string(indent, ' ') + style + rest;
            cells = cc::display_width(cc::strip_ansi(rest));
            space = std::string::npos;
        }

        if (cluster == " ") space = row.size();
        row += cluster;
        cells += cell;
    }

    rows.push_back(row);
    return rows;
}

/// Moves the cursor to a row and a column of the screen.
///
/// The row and the column count from one.
std::string cursor_to(int row, int column) {
    if (row < 1) row = 1;
    if (column < 1) column = 1;
    // The corner of the screen is the place the cursor comes back to.
    if (row == 1 && column == 1) return "\x1b[H";
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(column) + "H";
}

/// The wipe of the rest of a row, after the row was written.
///
/// A row that fills the screen is left alone: the cursor stands at the last
/// column of the row and waits for the next character to wrap, and the wipe
/// would take that last cell away.
std::string erase_to_end_of_row(std::string_view row, int columns) {
    if (columns > 0 && cc::display_width(cc::strip_ansi(row)) >= static_cast<size_t>(columns)) {
        return {};
    }
    // The wipe is done with the colours of the terminal and not with the
    // ones of the row: a row that ends with a background of its own would
    // paint the rest of the row with it.
    return "\x1b[0m\x1b[K";
}

} // namespace

struct Terminal::Impl {
    /// The decoder of the terminal.
    ///
    /// It reads the bytes that come from the terminal and turns them into
    /// keys, mouse events and pastes.
    maya::InputParser parser;

    /// Input that has been decoded but not yet handed to the caller.
    std::deque<PendingInput> pending;

    /// Keys that have been decoded and are waiting to be handed over.
    std::deque<Key> ready;

    /// Bytes that were read but not handed to the parser, because they could
    /// still grow into a bracketed-paste marker.
    std::string carry;

    /// The last byte that was handed over to the parser.
    ///
    /// The byte tells whether the parser is holding an escape or a sequence
    /// that never came: an escape is one byte long, and the sequence that
    /// follows it is not.
    char last_fed_ = '\0';

    /// True while a bracketed paste is being collected.
    bool in_paste = false;

    /// The text collected so far by the current paste.
    std::string paste;

    /// When the last byte was read from the terminal.
    std::chrono::steady_clock::time_point last_input = std::chrono::steady_clock::now();

    /// True when the terminal closed its input and no more keys will come.
    bool closed = false;

    /// Queues raw bytes for the key parser.
    void push_bytes(std::string bytes) {
        if (bytes.empty()) return;
        PendingInput item;
        item.bytes = std::move(bytes);
        pending.push_back(std::move(item));
    }

    /// Queues a key that carries more than its type.
    void push_key(Key key) {
        PendingInput item;
        item.is_key = true;
        item.key = std::move(key);
        pending.push_back(std::move(item));
    }

    /// Queues a decoded paste.
    void push_paste(std::string text) {
        PendingInput item;
        item.is_key = true;
        item.key.type = Key::Type::Paste;
        item.key.text = normalise_paste(text);
        pending.push_back(std::move(item));
    }

    /// Splits the raw bytes into ordinary input and bracketed pastes.
    ///
    /// The paste markers and the text they wrap never reach the key parser:
    /// a paste is handed over as a single key, so the newlines it contains
    /// cannot be mistaken for the Enter that commits the prompt.
    void scan(std::string_view chunk) {
        std::string data = std::move(carry);
        carry.clear();
        data.append(chunk);

        // The mouse events and the escape sequences are read by the parser
        // itself, so only the pastes are taken out here.

        size_t i = 0;
        while (i < data.size()) {
            if (in_paste) {
                const size_t end = data.find(kPasteEnd, i);
                if (end == std::string::npos) {
                    // The end marker may still be coming, so the bytes that
                    // could grow into one are held back.
                    const size_t keep = marker_prefix_length(std::string_view(data).substr(i));
                    paste.append(data, i, data.size() - keep - i);
                    carry = data.substr(data.size() - keep);
                    break;
                }
                paste.append(data, i, end - i);
                push_paste(std::move(paste));
                paste.clear();
                in_paste = false;
                i = end + kPasteEnd.size();
                continue;
            }

            const size_t start = data.find(kPasteStart, i);
            const size_t end = data.find(kPasteEnd, i);
            if (start == std::string::npos && end == std::string::npos) {
                const size_t keep = marker_prefix_length(std::string_view(data).substr(i));
                push_bytes(data.substr(i, data.size() - keep - i));
                carry = data.substr(data.size() - keep);
                break;
            }

            const bool starts_paste = start != std::string::npos && (end == std::string::npos || start <= end);
            const size_t at = starts_paste ? start : end;
            push_bytes(data.substr(i, at - i));
            if (starts_paste) {
                in_paste = true;
                paste.clear();
            } else {
                // An end marker without a start: the reference hands over an
                // empty paste in that case too.
                push_paste(std::string());
            }
            i = at + kPasteMarkerSize;
        }
    }

    /// Moves the events the key parser produced into the ready queue.
    void collect_events(const std::vector<maya::Event>& events) {
        for (const maya::Event& event : events) {
            Key key = translate(event);
            if (key.type == Key::Type::Unknown) continue;
            ready.push_back(std::move(key));
        }
    }

    /// Hands the bytes over to the key parser and collects the events.
    ///
    /// The bytes are handed over in small pieces: the parser reads a long
    /// run of plain text as a paste of its own, and the port reads a run of
    /// text as the text it is.
    void feed_parser(std::string_view bytes) {
        constexpr size_t kChunk = 32;
        size_t at = 0;
        while (at < bytes.size()) {
            const size_t end = std::min(at + kChunk, bytes.size());
            collect_events(parser.feed(bytes.substr(at, end - at)));
            at = end;
        }
        if (!bytes.empty()) last_fed_ = bytes.back();
    }

    /// Hands over the next decoded key, if there is one.
    std::optional<Key> deliver() {
        while (true) {
            if (!ready.empty()) {
                Key key = std::move(ready.front());
                ready.pop_front();
                return key;
            }
            if (pending.empty()) return std::nullopt;

            PendingInput item = std::move(pending.front());
            pending.pop_front();

            if (item.is_key) {
                // The parser may still be holding an escape sequence that
                // the paste interrupted. Flushing it keeps it from swallowing
                // the input that follows the paste.
                collect_events(parser.flush_timeout());
                ready.push_back(std::move(item.key));
                continue;
            }

            feed_parser(item.bytes);
        }
    }

    /// The moment the terminal's silence has to end an unfinished escape
    /// sequence or paste.
    std::chrono::steady_clock::time_point flush_deadline() const {
        const int timeout = in_paste ? kPasteTimeoutMs : kEscapeTimeoutMs;
        return last_input + std::chrono::milliseconds(timeout);
    }

    /// Finishes the input that the terminal left unfinished.
    ///
    /// A lone Escape is handed over once the terminal has been quiet for
    /// `kEscapeTimeoutMs`, and a paste that was never closed is handed over
    /// after `kPasteTimeoutMs`.
    void flush_incomplete() {
        // The silence is measured as a duration, so a wait that is a fraction
        // of a millisecond short of the timeout does not count as a timeout.
        const auto idle = std::chrono::steady_clock::now() - last_input;

        if (in_paste) {
            if (idle < std::chrono::milliseconds(kPasteTimeoutMs)) return;
            push_paste(paste + carry);
            paste.clear();
            carry.clear();
            in_paste = false;
            return;
        }

        if (idle < std::chrono::milliseconds(kEscapeTimeoutMs)) return;

        // The bytes that were held back never became a paste marker.
        //
        // A lone escape is the escape key: the terminal reads it so when
        // nothing follows it. The escape that was held back behind another
        // escape is the escape with the option held — the terminal reads
        // the two of them as one key. Every other sequence is one the
        // terminal cannot read, and the terminal drops it.
        if (!carry.empty()) {
            const std::string bytes = std::move(carry);
            carry.clear();

            if (bytes == "\x1b") {
                Key key;
                key.type = Key::Type::Escape;
                if (!parser.has_pending()) {
                    push_key(std::move(key));
                } else if (last_fed_ == '\x1b') {
                    key.alt = true;
                    push_key(std::move(key));
                }
            }

            // The parser keeps nothing of what it was holding: the sequence
            // it was in the middle of never came.
            parser.reset();
        }

        // The parser may still be holding a sequence that never completed,
        // such as an escape followed by a single character.
        collect_events(parser.flush_timeout());
    }

    /// Forgets everything that was read but not yet handed over.
    void reset() {
        pending.clear();
        ready.clear();
        carry.clear();
        paste.clear();
        in_paste = false;
        parser.reset();
        last_input = std::chrono::steady_clock::now();
    }
};

Terminal::Terminal() : impl_(std::make_unique<Impl>()) {
#ifndef _WIN32
    interactive_ = isatty(STDOUT_FILENO) != 0 && isatty(STDIN_FILENO) != 0;
#endif

    // The output of a terminal is line buffered: the buffer of the standard
    // output is written out at every newline of it. A frame of the interface
    // holds many lines, so it would reach the terminal in pieces, and the
    // user would see the region of the screen without its text for a moment.
    //
    // The buffer is made bigger, and every frame is written out at once, the
    // way the reference writes it.
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
}

Terminal::~Terminal() { leave_raw_mode(); }

Terminal& Terminal::instance() {
    static Terminal terminal;
    return terminal;
}

/// The size of the terminal in columns and rows.
///
/// A terminal that does not tell its size, or that tells a size with no rows
/// in it, is a terminal of eighty columns and twenty-four rows: the two
/// numbers are one size, and one of them alone is not a size.
std::pair<int, int> terminal_size() {
    const maya::Size size = maya::platform::query_terminal_size(maya::platform::stdout_handle());
    if (size.width.value <= 0 || size.height.value <= 0) return {80, 24};
    return {size.width.value, size.height.value};
}

int Terminal::width() const { return terminal_size().first; }

int Terminal::height() const { return terminal_size().second; }

bool Terminal::enter_raw_mode() {
#ifndef _WIN32
    if (raw_mode_) return true;
    if (!interactive_) return false;

    RawModeGuard& guard = raw_mode_guard();
    if (tcgetattr(STDIN_FILENO, &guard.saved) != 0) return false;
    guard.valid = true;

    termios raw = guard.saved;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // `OPOST` stays on so that a newline still returns the carriage; without
    // it every line would start one column further right than the last.
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
    raw_mode_ = true;

    impl_->reset();

    // The cursor of the terminal is hidden: the prompt draws its own caret,
    // the way Claude Code draws it. The cursor of the terminal would sit at
    // the end of the box and never go away.
    std::fputs(cc::ui::hide_cursor().c_str(), stdout);

    // Bracketed paste, so multi-line pastes arrive as a single payload.
    std::fputs("\x1b[?2004h", stdout);
    std::fflush(stdout);

    // The screen is drawn again when the terminal is resized, the way the
    // reference draws it again.
    struct sigaction action {};
    action.sa_handler = on_resize;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGWINCH, &action, nullptr);

    return true;
#else
    return false;
#endif
}

void Terminal::leave_raw_mode() {
#ifndef _WIN32
    if (!raw_mode_) return;
    raw_mode_ = false;
    std::fputs("\x1b[?2004l", stdout);
    // The cursor of the terminal belongs to the shell that takes over.
    std::fputs(cc::ui::show_cursor().c_str(), stdout);
    std::fflush(stdout);

    // The original settings are put back here rather than in the guard's
    // destructor, which only runs when the process exits: the terminal has
    // to be usable again as soon as raw mode is left.
    raw_mode_guard().restore();
#endif
}

std::optional<Key> Terminal::read_key(int timeout_ms) {
#ifndef _WIN32
    if (!raw_mode_) return std::nullopt;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    // Whether the terminal has been asked for its bytes. A wait of no time
    // asks the terminal once, so that a key that is waiting is read even
    // when the caller does not want to wait for it: the session reads the
    // keys while the answer is running, and the keys would never arrive if
    // the terminal was never asked.
    bool polled = false;

    while (true) {
        // A key that has already been decoded is handed over right away.
        if (auto key = impl_->deliver()) {
            handle_mouse(*key);
            return key;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline && polled) {
            // The wait is over. Whatever the terminal left unfinished is
            // finished now and handed over as well.
            impl_->flush_incomplete();
            if (auto key = impl_->deliver()) {
                handle_mouse(*key);
                return key;
            }
            return std::nullopt;
        }

        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

        pollfd fd{};
        fd.fd = STDIN_FILENO;
        fd.events = POLLIN;

        // The wait does not outlast the moment the terminal's silence has to
        // end an unfinished escape sequence or paste, so a lone Escape is
        // handed over as soon as it can be told apart from a longer
        // sequence.
        const auto until_flush = impl_->flush_deadline() - now;
        int wait_ms = static_cast<int>(wait.count());
        if (until_flush > std::chrono::milliseconds::zero()) {
            const auto flush_wait = std::chrono::duration_cast<std::chrono::milliseconds>(until_flush);
            wait_ms = static_cast<int>(std::min(wait, flush_wait).count());
        }

        const int ready = poll(&fd, 1, wait_ms);
        polled = true;
        if (ready > 0) {
            char buffer[256];
            const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) {
                // A signal arrived: the terminal is not closed.
                continue;
            }
            if (count <= 0) {
                // The terminal closed: there is nothing left to read.
                impl_->closed = true;
                impl_->flush_incomplete();
                if (auto key = impl_->deliver()) {
                    handle_mouse(*key);
                    return key;
                }
                return std::nullopt;
            }
            if (count > 0) {
                impl_->last_input = std::chrono::steady_clock::now();
                impl_->scan(std::string_view(buffer, static_cast<size_t>(count)));
            }
        }

        // Nothing arrived for a while: the terminal may have left an escape
        // sequence or a paste unfinished.
        impl_->flush_incomplete();
    }
#else
    (void)timeout_ms;
    return std::nullopt;
#endif
}

bool Terminal::closed() const { return impl_->closed; }

bool Terminal::took_resize() {
    if (!g_resized.exchange(false, std::memory_order_relaxed)) return false;

    // The screen is drawn again, even when the content of the region did
    // not change: the region is measured against the size of the terminal.
    invalidated_ = true;

    if (fullscreen_) {
        // The transcript is kept in memory in this layout, so that the
        // conversation follows the width of the terminal: the rows of it
        // are wrapped again at the width the terminal has now.
        rebuild_transcript_rows();
        screen_frame_.clear();
        return true;
    }

    // The transcript is in the scrollback of the terminal, and the terminal
    // has wrapped it again by itself: it is left alone here. Writing it
    // again would put the same text on the screen a second time, and the
    // frames of the old screen would pile up in the scrollback.
    redraw_after_resize();

    return true;
}

/// Draws the region again, after the terminal was resized.
///
/// The terminal has wrapped the text that was on the screen again: the rows
/// of the region take a different number of rows now, and the region stands
/// at the end of the buffer of the terminal, where the screen shows it.
///
/// The transcript is left alone: it is in the scrollback of the terminal,
/// and the terminal has wrapped it again by itself.
void Terminal::redraw_after_resize() {
    // A width change reflows the terminal's existing rows, including the
    // dynamic region. The row count we kept before the resize can no longer
    // locate that region reliably, so wipe_region() can erase transcript
    // text or leave old wrapped rows behind. Rebuild the inline screen from
    // the transcript we retain, like the full-screen renderer does.
    std::string replay;
    auto append_text = [&](std::string_view text) {
        for (size_t i = 0; i < text.size();) {
            if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
                size_t end = i + 2;
                while (end < text.size() && !(text[end] >= '@' && text[end] <= '~')) ++end;
                if (end < text.size()) {
                    // Keep styling, but omit cursor/screen controls that
                    // were written through write_static() as one-off actions.
                    if (text[end] == 'm') replay.append(text.substr(i, end - i + 1));
                    i = end + 1;
                    continue;
                }
            }
            replay.push_back(text[i++]);
        }
    };
    for (const auto& line : transcript_) {
        append_text(line);
        replay.push_back('\n');
    }
    append_text(transcript_tail_);
    if (!replay.empty() && replay.back() != '\n') replay.push_back('\n');

    std::string out = "\x1b[3J\x1b[2J\x1b[H";
    out += replay;
    cursor_row_ = std::min(rows_of(replay), std::max(0, height() - 1));
    dynamic_height_ = 0;
    dynamic_lines_.clear();
    out += region_frame();

    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

void Terminal::write_static(std::string_view text) {
    if (text.empty()) return;

    // The terminal may have been resized: the region is measured again
    // before anything is written, so that the text lands where the region
    // stood.
    took_resize();

    // In the full-screen layout the transcript is kept in memory and the
    // whole screen is drawn again, the way the reference draws it.
    if (fullscreen_) {
        append_transcript(text);
        redraw_fullscreen();
        return;
    }

    // The transcript is kept in memory in this layout as well: the terminal
    // wraps the text that is on the screen again when it is resized, and
    // the region of the prompt has to stand where the transcript ends then.
    //
    // The transcript has to end on a line boundary, otherwise the dynamic
    // region is put back in the middle of a line.
    std::string written(text);
    if (written.back() != '\n') written.push_back('\n');
    append_transcript(written);

    // The dynamic region sits below the cursor, so it has to be taken down
    // before the transcript is written and put back afterwards. The lines
    // are kept, because erasing the region clears them.
    //
    // The wipe, the text and the region that comes back are written in one
    // go, so that the screen never shows the region without the text that
    // stands above it.
    const bool had_dynamic = dynamic_height_ > 0;
    std::vector<std::string> saved;
    std::string out;
    if (had_dynamic) {
        saved = dynamic_content_;
        out = erase_dynamic();
    }

    out.append(written);

    // The cursor moved down by as many rows as the text took. The terminal
    // scrolls when the text reaches the bottom, so the row is clamped.
    cursor_row_ = std::min(cursor_row_ + rows_of(text), std::max(0, height() - 1));

    if (had_dynamic) {
        dynamic_content_ = std::move(saved);
        out += region_frame();
    }

    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

void Terminal::write_static_line(std::string_view text) {
    std::string out(text);
    out.push_back('\n');
    write_static(out);
}

std::string Terminal::wipe_region() {
    std::string out;

    // The rows of the region are wiped one by one, from the last one to the
    // first one. The wipe is measured from the cursor, which stands at the
    // end of the last row of the region: the region is wiped where it is,
    // and the rows the user has scrolled up to are left alone.
    //
    // The wipe covers the region and nothing else: the rows below it are
    // the empty rows of the screen, and they are left as they are.
    const int rows = dynamic_lines_.empty() ? dynamic_height_ : rows_of_lines(dynamic_lines_);
    const int max_up = std::max(0, height() - 1);
    const int up = std::min(rows - 1, max_up);

    // The wipe begins at the first column of the row: the cursor stands at
    // the end of the text of the last row of the region, and the text that
    // is to the left of it belongs to the transcript, not to the region.
    out += "\r";
    for (int i = 0; i <= up; ++i) {
        out += "\x1b[K";
        if (i < up) {
            out += "\r";
            out += cursor_up(1);
        }
    }

    return out;
}

std::string Terminal::erase_dynamic() {
    if (dynamic_height_ <= 0) return {};

    std::string out = wipe_region();

    dynamic_height_ = 0;
    dynamic_lines_.clear();
    cursor_row_ = dynamic_row_;
    return out;
}

void Terminal::redraw_dynamic() {
    // The old region is erased first, so the new one is drawn in its place.
    // The wipe and the new region are written in one go, so that the region
    // does not disappear from the screen for a moment.
    std::string out = erase_dynamic();
    out += region_frame();
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

std::string Terminal::region_frame() {
    // The answer that grows stands in front of the region: it grows right
    // under the text that came before it, the way the reference streams it.
    std::vector<std::string> content = live_lines_;
    content.insert(content.end(), dynamic_content_.begin(), dynamic_content_.end());

    // The region is drawn where the cursor is: the wipe before it left the
    // cursor at the first row of the region.
    std::string out;
    out += draw_dynamic(content);
    return out;
}

std::string Terminal::draw_dynamic(const std::vector<std::string>& content) {
    if (content.empty()) return {};

    // The region starts where the cursor is.
    const int start = cursor_row_;

    std::string out;
    for (size_t i = 0; i < content.size(); ++i) {
        if (i > 0) out.push_back('\n');
        out += content[i];
        // The rest of the row is wiped: the row may hold the text of the
        // region before it, and the text that was there is not part of the
        // region now. A row that fills the terminal is left alone: the
        // cursor stands at the last column of it and waits for the next
        // character to wrap, and the wipe would take that cell away.
        out += erase_to_end_of_row(content[i], width());
    }
    // The region may take more rows than it has lines: a line that is wider
    // than the terminal takes a row for every part of it that does not fit.
    // The erase has to know how tall the region really is, or the frames
    // pile up in the scrollback.
    dynamic_height_ = rows_of_lines(content);
    dynamic_lines_ = content;

    // The region may have been drawn past the bottom of the screen, in
    // which case the screen scrolled and the region ends at the bottom row.
    // The row it starts at is then the row it ends at minus its height —
    // the row it really is at, and not the one it was drawn from.
    if (start + dynamic_height_ - 1 >= height() - 1) {
        dynamic_row_ = std::max(0, height() - dynamic_height_);
        cursor_row_ = std::max(0, height() - 1);
    } else {
        dynamic_row_ = start;
        cursor_row_ = start + dynamic_height_ - 1;
    }
    return out;
}

/// The number of rows `text` takes on the screen.
///
/// A line that is wider than the terminal takes as many rows as it needs.
int Terminal::rows_of(std::string_view text) const {
    const int columns = std::max(1, width());
    // An empty line is a line all the same: it takes a row of the screen.
    if (text.empty()) return 1;

    // The rows are the rows the terminal wraps the text into: a line that
    // is wider than the terminal takes a row for every part of it that does
    // not fit, and the escapes of the colours take no room.
    int rows = 0;
    for (const auto& line : split_lines(text)) {
        rows += static_cast<int>(screen_rows(line, columns).size());
    }
    return rows;
}

int Terminal::rows_of_lines(const std::vector<std::string>& lines) const {
    int rows = 0;
    for (const auto& line : lines) rows += rows_of(line);
    return rows;
}

int Terminal::live_rows() const {
    return rows_of_lines(live_lines_);
}

void Terminal::set_live_lines(std::vector<std::string> lines) {
    if (lines == live_lines_) return;
    live_lines_ = std::move(lines);
    live_changed_ = true;
}

void Terminal::commit_live(std::string_view text) { commit_live(text, {}); }

void Terminal::commit_live(std::string_view text, std::vector<std::string> growing) {
    if (fullscreen_) {
        // The answer is written to the transcript of the window and the
        // answer that grew is taken away in the same drawing, so that the
        // answer is never on the screen twice: the part that is written and
        // the part that is still growing are put on the screen together.
        live_lines_ = std::move(growing);
        live_changed_ = true;
        append_transcript(text);
        redraw_fullscreen();
        return;
    }

    // In the other layout the region of the prompt holds the answer that
    // grows: it is taken away, so that the text is written where the region
    // stood.
    clear_dynamic();
    write_static(text);
}

void Terminal::render_dynamic(const std::vector<std::string>& lines) {
    // In the full-screen layout the region is at the bottom of the screen
    // and the whole screen is drawn again.
    if (fullscreen_) {
        // The screen was resized: it is drawn again, even when the content
        // of the region did not change — the region is measured against the
        // size of the terminal.
        const bool resized = took_resize();
        if (!resized && !live_changed_ && dynamic_lines_ == lines) return;
        dynamic_lines_ = lines;
        dynamic_height_ = static_cast<int>(dynamic_lines_.size());
        live_changed_ = false;
        redraw_fullscreen();
        return;
    }

    // The content of the region is kept before the resize is handled: the
    // region is drawn again when the terminal was resized, and the content
    // it draws is the one that was just handed over. The box of the prompt
    // is rendered at the width the terminal has now — a box of the old width
    // would be drawn once more and left behind.
    const bool same = lines == dynamic_content_;
    dynamic_content_ = lines;

    // The screen was resized: it is drawn again, even when the content of
    // the region did not change.
    const bool resized = took_resize();

    // Redrawing the same content would flicker, so it is skipped — unless
    // the screen has to be drawn again, after a resize, or the answer that
    // grows has changed.
    if (!invalidated_ && !live_changed_ && dynamic_height_ > 0 && same) return;
    invalidated_ = false;
    live_changed_ = false;

    // The region was drawn again when the terminal was resized, with the
    // content that was just handed over: it is not drawn a second time.
    if (resized) {
        std::fflush(stdout);
        return;
    }

    redraw_dynamic();
    std::fflush(stdout);
}

void Terminal::render_dynamic_line(std::string_view text) {
    render_dynamic({std::string(text)});
}

void Terminal::commit_dynamic() {
    if (dynamic_height_ <= 0) return;

    if (fullscreen_) {
        // The region becomes part of the transcript of the window.
        for (const auto& line : dynamic_lines_) {
            transcript_.push_back(line);
            auto rows = screen_rows(line, width());
            transcript_line_rows_.push_back(rows.size());
            for (auto& row : rows) {
                transcript_rows_.push_back(std::move(row));
            }
        }
        dynamic_height_ = 0;
        dynamic_lines_.clear();
        redraw_fullscreen();
        return;
    }

    // The dynamic region becomes part of the transcript, so a newline is
    // written and the region is forgotten.
    std::fwrite("\n", 1, 1, stdout);
    std::fflush(stdout);

    // The newline is a part of the transcript as well, so that the rows of
    // the transcript are the rows of the text that is on the screen.
    append_transcript("\n");

    dynamic_height_ = 0;
    dynamic_lines_.clear();
    cursor_row_ = std::min(cursor_row_ + 1, std::max(0, height() - 1));
}

void Terminal::clear_dynamic() {
    if (fullscreen_) {
        // The answer that grows stands in the transcript of the window, so
        // it stays: only the region of the prompt is taken away.
        if (dynamic_lines_.empty()) return;
        dynamic_height_ = 0;
        dynamic_lines_.clear();
        redraw_fullscreen();
        return;
    }
    // The answer that grows is a part of the region in this layout, so it
    // goes away with the region.
    live_lines_.clear();
    live_changed_ = true;
    const std::string wipe = erase_dynamic();
    std::fwrite(wipe.data(), 1, wipe.size(), stdout);
    std::fflush(stdout);
}

void Terminal::flush() {
    if (fullscreen_) redraw_fullscreen();
    std::fflush(stdout);
}

// The cursor is shown and hidden with a plain escape sequence: the cursor's
// visibility does not depend on the position, and the sequence must not
// become a line of the transcript.
void Terminal::show_cursor() {
    const std::string sequence = cc::ui::show_cursor();
    std::fwrite(sequence.data(), 1, sequence.size(), stdout);
    std::fflush(stdout);
}

void Terminal::hide_cursor() {
    const std::string sequence = cc::ui::hide_cursor();
    std::fwrite(sequence.data(), 1, sequence.size(), stdout);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// The full-screen layout
// ---------------------------------------------------------------------------

bool Terminal::enter_fullscreen() {
    if (!interactive_) return false;
    if (fullscreen_) return true;

    // The alternate screen becomes the session's own window; the screen of
    // the terminal is kept and comes back when the session ends.
    //
    // The mouse is reported as well, the way the reference reports it in
    // its full-screen layout: the mascot answers a click, and the user may
    // select text with the mouse.
    //
    // The mouse is reported when a button goes down, when it is let go and
    // when it moves; the reference asks for the same.
    std::fputs("\x1b[?1049h\x1b[2J\x1b[H\x1b[?1000h\x1b[?1002h\x1b[?1003h\x1b[?1006h", stdout);
    std::fflush(stdout);

    fullscreen_ = true;
    transcript_.clear();
    transcript_rows_.clear();
    transcript_line_rows_.clear();
    transcript_tail_.clear();
    scroll_offset_ = 0;
    screen_frame_.clear();
    content_rows_ = 0;
    content_first_ = 0;
    screen_window_ = 0;
    dynamic_height_ = 0;
    dynamic_lines_.clear();
    cursor_row_ = 0;
    return true;
}

void Terminal::leave_fullscreen() {
    if (!fullscreen_) return;
    fullscreen_ = false;

    // The screen of the terminal comes back, with what it held before the
    // session took the alternate one over. The mouse stops being reported.
    std::fputs("\x1b[?1006l\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?1049l", stdout);
    std::fflush(stdout);

    transcript_.clear();
    transcript_rows_.clear();
    transcript_line_rows_.clear();
    transcript_tail_.clear();
    scroll_offset_ = 0;
    screen_frame_.clear();
    content_rows_ = 0;
    content_first_ = 0;
    screen_window_ = 0;
    dynamic_height_ = 0;
    dynamic_lines_.clear();
    live_lines_.clear();
    cursor_row_ = 0;
    screen_rows_.clear();
    selection_start_.reset();
    selection_end_.reset();
}

void Terminal::scroll(int lines) {
    if (!fullscreen_ || lines == 0) return;

    // The region of the prompt stands at the bottom of the screen; the rest
    // of the screen holds the transcript and the answer that grows.
    const int window = conversation_rows();

    const int offset = scroll_offset_ + lines;
    if (offset <= 0) {
        scroll_offset_ = 0;
    } else {
        // The view cannot move above the beginning of the transcript: the
        // first screenful of it stays on the screen.
        const int content = static_cast<int>(transcript_rows_.size()) + live_rows();
        const int room = std::max(0, content - window);
        scroll_offset_ = std::min(offset, room);
    }

    redraw_fullscreen();
}

void Terminal::scroll_to_bottom() {
    if (!fullscreen_ || scroll_offset_ == 0) return;
    scroll_offset_ = 0;
    redraw_fullscreen();
}

void Terminal::replace_transcript_line(size_t index, std::string_view line) {
    if (!fullscreen_ || index >= transcript_.size()) return;
    transcript_[index] = std::string(line);

    // The rows of the line are wrapped again: a line that is wider than the
    // terminal takes a row for every part of it that does not fit.
    size_t first_row = 0;
    for (size_t i = 0; i < index; ++i) first_row += transcript_line_rows_[i];
    const size_t old_rows = transcript_line_rows_[index];

    auto rows = screen_rows(transcript_[index], width());
    transcript_line_rows_[index] = rows.size();

    // The rows of the line are taken out and put back, wrapped again. The
    // rows of the lines around it stay where they are.
    transcript_rows_.erase(transcript_rows_.begin() + static_cast<std::ptrdiff_t>(first_row),
                           transcript_rows_.begin() +
                               static_cast<std::ptrdiff_t>(first_row + old_rows));
    transcript_rows_.insert(transcript_rows_.begin() + static_cast<std::ptrdiff_t>(first_row),
                            std::make_move_iterator(rows.begin()),
                            std::make_move_iterator(rows.end()));
}

int Terminal::transcript_line_row(size_t index) const {
    if (!fullscreen_ || index >= transcript_.size()) return 0;

    // The row the line begins on: the lines before it take a row each.
    size_t row = 0;
    for (size_t i = 0; i < index; ++i) row += transcript_line_rows_[i];

    // The rows that are on the screen are the last ones of the transcript.
    // The answer that grows stands under the transcript, so it takes rows
    // of the window too.
    const int window = conversation_rows();

    const size_t total = transcript_rows_.size() + static_cast<size_t>(live_rows());
    size_t end = total;
    if (scroll_offset_ > 0) {
        end = total - std::min<size_t>(static_cast<size_t>(scroll_offset_), total);
    }
    const size_t start = end > static_cast<size_t>(window) ? end - static_cast<size_t>(window) : 0;

    if (row < start || row >= end) return 0;
    return static_cast<int>(row - start) + 1;
}

void Terminal::append_transcript(std::string_view text) {
    // The text is split into the lines it holds; the last one may be
    // unfinished, in which case it waits for the rest of it.
    std::string rest = transcript_tail_;
    rest.append(text);
    transcript_tail_.clear();

    size_t start = 0;
    while (true) {
        const size_t end = rest.find('\n', start);
        if (end == std::string::npos) break;

        const std::string line = rest.substr(start, end - start);
        start = end + 1;

        // The line is kept as it was written, so that it can be wrapped
        // again when the terminal is resized. The rows it takes now are
        // kept beside it.
        transcript_.push_back(line);
        auto rows = reflow_line(line, width());
        transcript_line_rows_.push_back(rows.size());
        for (auto& row : rows) {
            transcript_rows_.push_back(std::move(row));
        }
    }

    transcript_tail_ = rest.substr(start);
}

/// Wraps the lines of the transcript again, at the width of the terminal
/// now.
///
/// The transcript of the full-screen layout is kept in memory, so that the
/// screen can be drawn again: the conversation has to follow the width of
/// the terminal, the way the scrollback of the other layout does.
///
/// The lines are broken where a word ends, and the text that stood at the
/// beginning of a line stands at the beginning of the rows that follow it.
void Terminal::rebuild_transcript_rows() {
    transcript_rows_.clear();
    transcript_line_rows_.clear();
    for (const auto& line : transcript_) {
        auto rows = reflow_line(line, width());
        transcript_line_rows_.push_back(rows.size());
        for (auto& row : rows) {
            transcript_rows_.push_back(std::move(row));
        }
    }
}

void Terminal::redraw_fullscreen() {
    const int rows = std::max(1, height());

    // The dynamic region sits at the bottom of the screen. A region that is
    // taller than the screen is cut: the lines that do not fit are left
    // out, the way the answer that does not fit is left out of the
    // scrollback.
    std::vector<std::string> bottom = dynamic_lines_;
    while (rows_of_lines(bottom) > rows && !bottom.empty()) {
        bottom.erase(bottom.begin());
    }

    // The rows of the region of the prompt.
    std::vector<std::string> region;
    for (const auto& line : bottom) {
        for (auto& row : screen_rows(line, width())) {
            region.push_back(std::move(row));
        }
    }
    if (static_cast<int>(region.size()) > rows) {
        region.erase(region.begin(), region.end() - rows);
    }

    // The answer that grows stands under the transcript, the way the
    // reference draws it: the text grows in the place where it began.
    std::vector<std::string> live;
    for (const auto& line : live_lines_) {
        for (auto& row : screen_rows(line, width())) {
            live.push_back(std::move(row));
        }
    }

    // The rest of the screen holds the transcript. The view shows its end,
    // unless it has been moved up.
    const int window = std::max(0, rows - static_cast<int>(region.size()));

    // The content of the window is the transcript and the answer that grows:
    // the answer stands right under the transcript.
    const size_t total = transcript_rows_.size() + live.size();

    // The content grew at its end, and the view stays where it is: the
    // answer that streams below it does not move the conversation under the
    // eyes of the user.
    if (scroll_offset_ > 0 && total > content_rows_) {
        scroll_offset_ += static_cast<int>(total - content_rows_);
    }
    content_rows_ = total;

    size_t end = total;
    if (scroll_offset_ > 0) {
        end = total - std::min<size_t>(static_cast<size_t>(scroll_offset_), total);
    }

    // The rows of the conversation are the last rows of the content. The
    // empty rows under them belong to it as well: the region of the prompt
    // stands at the bottom of the screen.
    const size_t first = end > static_cast<size_t>(window) ? end - static_cast<size_t>(window) : 0;
    std::vector<std::string> conversation;
    conversation.reserve(static_cast<size_t>(window));
    for (size_t index = first; index < end; ++index) {
        conversation.push_back(index < transcript_rows_.size()
                                   ? transcript_rows_[index]
                                   : live[index - transcript_rows_.size()]);
    }
    conversation.resize(static_cast<size_t>(window));

    // The rows of the screen as they were drawn last: the conversation and
    // the region of the prompt.
    const bool same = !screen_frame_.empty() && screen_window_ == window &&
                      screen_frame_.size() == static_cast<size_t>(rows);
    std::vector<std::string> old_conversation;
    std::vector<std::string> old_region;
    if (same) {
        old_conversation.assign(screen_frame_.begin(),
                                screen_frame_.begin() + static_cast<std::ptrdiff_t>(window));
        old_region.assign(screen_frame_.begin() + static_cast<std::ptrdiff_t>(window),
                          screen_frame_.end());
    }

    // How far the conversation moved: the view was moved up or down, or the
    // content grew at its end. The first row of the conversation tells how
    // far it moved — a conversation that is shorter than the window grows
    // downwards and does not move at all.
    //
    // The terminal moves the rows itself, so that the screen is not painted
    // row by row — a screen that is painted row by row flickers, and the
    // banner with the mascot in it flickers most of all.
    const int moved = same ? static_cast<int>(first) - static_cast<int>(content_first_) : 0;
    content_first_ = first;

    // The rows the screen holds once the terminal has moved them.
    std::vector<std::string> expected(conversation.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        const long source = static_cast<long>(i) + moved;
        expected[i] = (source >= 0 && source < static_cast<long>(old_conversation.size()))
                          ? old_conversation[static_cast<size_t>(source)]
                          : std::string();
    }

    std::string out;

    // The conversation is moved by the terminal itself: the rows of the
    // region are deleted or inserted, and only the rows that are new are
    // written. The region of the prompt stands below the region and is left
    // alone by the move.
    if (moved != 0 && same && window > 0 && std::abs(moved) <= window) {
        out += "\x1b[1;" + std::to_string(window) + "r";
        out += cursor_to(1, 1);
        out += "\x1b[" + std::to_string(moved > 0 ? moved : -moved) + (moved > 0 ? "M" : "L");
        out += "\x1b[r";
    }

    // The rows that changed are written. The rows that did not change are
    // left as they are: the screen is written again only where it is
    // different, so that a screen that does not change does not flicker.
    for (size_t i = 0; i < conversation.size(); ++i) {
        if (conversation[i] == expected[i]) continue;
        out += cursor_to(static_cast<int>(i) + 1, 1);
        out += conversation[i];
        out += erase_to_end_of_row(conversation[i], width());
    }
    for (size_t i = 0; i < region.size(); ++i) {
        if (i < old_region.size() && region[i] == old_region[i]) continue;
        out += cursor_to(static_cast<int>(window) + static_cast<int>(i) + 1, 1);
        out += region[i];
        out += erase_to_end_of_row(region[i], width());
    }

    if (out.empty()) {
        // Nothing changed: the screen stands as it is.
        screen_window_ = window;
        return;
    }

    // The cursor stands at the end of the last row of the screen, the way
    // it stood when the whole screen was written.
    const std::string& last = region.empty()
                                   ? (conversation.empty() ? std::string() : conversation.back())
                                   : region.back();
    const size_t columns = cc::display_width(cc::strip_ansi(last));
    out += cursor_to(static_cast<int>(rows), static_cast<int>(columns) + 1);

    // The text of every row of the screen is kept, so that the text the
    // user selects with the mouse may be read back. The colours of a row
    // take no room on the screen, so they are taken away: the columns of
    // the row are the columns of the screen.
    screen_rows_.clear();
    for (const auto& row : conversation) {
        for (const auto& plain : screen_rows(cc::strip_ansi(row), width())) {
            screen_rows_.push_back(plain);
        }
    }
    for (const auto& row : region) {
        for (const auto& plain : screen_rows(cc::strip_ansi(row), width())) {
            screen_rows_.push_back(plain);
        }
    }

    screen_frame_ = conversation;
    for (auto& row : region) screen_frame_.push_back(std::move(row));
    screen_window_ = window;

    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

/// The number of rows the conversation takes on the screen.
///
/// The region of the prompt stands at the bottom of the screen; the
/// conversation takes the rows that are left above it.
int Terminal::conversation_rows() const {
    const int rows = std::max(1, height());
    std::vector<std::string> bottom = dynamic_lines_;
    while (rows_of_lines(bottom) > rows && !bottom.empty()) bottom.erase(bottom.begin());
    int region_rows = 0;
    for (const auto& line : bottom) {
        region_rows += static_cast<int>(screen_rows(line, width()).size());
    }
    return std::max(0, rows - region_rows);
}

/// Keeps the selection of the mouse up to date.
///
/// The reference lets the user select text with the mouse in its
/// full-screen layout, and puts the text that was selected in the clipboard
/// when the button is let go.
void Terminal::handle_mouse(const Key& key) {
    // The selection belongs to the full-screen layout: in the other one the
    // terminal of the user owns the selection, the way the reference leaves
    // it to the terminal.
    if (!fullscreen_) return;

    switch (key.type) {
    case Key::Type::MouseDown: {
        // The button went down: the selection begins where the mouse is.
        ScreenPosition position;
        position.row = key.mouse_y - 1;
        position.column = key.mouse_x - 1;
        selection_start_ = position;
        selection_end_ = position;
        break;
    }
    case Key::Type::MouseDrag:
        // The mouse moved with the button down: the selection grows.
        if (!selection_start_) break;
        selection_end_ = ScreenPosition{key.mouse_y - 1, key.mouse_x - 1};
        break;
    case Key::Type::MouseClick: {
        // The button was let go: the text that was selected goes into the
        // clipboard.
        const std::string text = selected_text();
        selection_start_.reset();
        selection_end_.reset();

        if (text.empty()) break;
        if (!config::global_flag("copyOnSelect", true)) break;
        copy_to_clipboard(text);
        break;
    }
    default:
        break;
    }
}

/// The text the mouse has selected.
///
/// The selection runs from the point the button went down to the point the
/// mouse stands at, whichever way the mouse went.
std::string Terminal::selected_text() const {
    if (!selection_start_ || !selection_end_) return {};

    ScreenPosition from = *selection_start_;
    ScreenPosition to = *selection_end_;
    if (to.row < from.row || (to.row == from.row && to.column < from.column)) {
        std::swap(from, to);
    }

    std::string out;
    for (int row = from.row; row <= to.row; ++row) {
        if (row < 0 || static_cast<size_t>(row) >= screen_rows_.size()) continue;

        const std::string& text = screen_rows_[static_cast<size_t>(row)];

        size_t first = row == from.row ? static_cast<size_t>(std::max(from.column, 0)) : 0;
        size_t last = row == to.row ? static_cast<size_t>(std::max(to.column, 0)) + 1 : text.size();
        if (first > text.size()) first = text.size();
        if (last > text.size()) last = text.size();
        if (first >= last) continue;

        if (!out.empty()) out += "\n";
        out += text.substr(first, last - first);
    }

    return out;
}

std::vector<std::string> wrap_lines(std::string_view text, int width, int indent) {
    std::vector<std::string> out;
    if (width <= 0) width = 80;

    const std::string pad(indent > 0 ? static_cast<size_t>(indent) : 0, ' ');
    for (const auto& line : split_lines(text)) {
        if (line.empty()) {
            out.push_back(pad);
            continue;
        }
        for (const auto& wrapped : word_wrap(line, static_cast<size_t>(width))) {
            out.push_back(pad + wrapped);
        }
    }
    return out;
}

} // namespace cc::ui
