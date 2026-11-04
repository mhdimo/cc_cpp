#include "ui/terminal.hpp"

#include "config/global_config.hpp"
#include "ui/ansi.hpp"
#include "ui/clipboard.hpp"
#include "utils/string.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/component/receiver.hpp>
#include <ftxui/component/task.hpp>
#include <ftxui/screen/terminal.hpp>

#include "ftxui/component/terminal_input_parser.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>

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

/// Translates an FTXUI event into the key the UI cares about.
Key translate(ftxui::Event event) {
    Key key;
    key.type = Key::Type::Unknown;

    if (event == ftxui::Event::Return) {
        key.type = Key::Type::Enter;
        return key;
    }
    if (event == ftxui::Event::Backspace) {
        key.type = Key::Type::Backspace;
        return key;
    }
    if (event == ftxui::Event::Delete) {
        key.type = Key::Type::Delete;
        return key;
    }
    if (event == ftxui::Event::Tab) {
        key.type = Key::Type::Tab;
        return key;
    }
    if (event == ftxui::Event::TabReverse) {
        key.type = Key::Type::ShiftTab;
        return key;
    }
    if (event == ftxui::Event::Escape) {
        key.type = Key::Type::Escape;
        return key;
    }
    if (event == ftxui::Event::ArrowUp) {
        key.type = Key::Type::Up;
        return key;
    }
    if (event == ftxui::Event::ArrowDown) {
        key.type = Key::Type::Down;
        return key;
    }
    if (event == ftxui::Event::ArrowLeft) {
        key.type = Key::Type::Left;
        return key;
    }
    if (event == ftxui::Event::ArrowRight) {
        key.type = Key::Type::Right;
        return key;
    }
    if (event == ftxui::Event::Home) {
        key.type = Key::Type::Home;
        return key;
    }
    if (event == ftxui::Event::End) {
        key.type = Key::Type::End;
        return key;
    }
    if (event == ftxui::Event::PageUp) {
        key.type = Key::Type::PageUp;
        return key;
    }
    if (event == ftxui::Event::PageDown) {
        key.type = Key::Type::PageDown;
        return key;
    }

    // A click of the mouse. Only the left button counts, and only when it
    // is let go, so that a click is one event and not two.
    if (event.is_mouse()) {
        const ftxui::Mouse mouse = event.mouse();
        if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Released) {
            key.type = Key::Type::MouseClick;
            key.mouse_x = mouse.x;
            key.mouse_y = mouse.y;
        }
        return key;
    }

    if (event.is_character()) {
        const std::string& text = event.character();
        if (text.size() == 1) {
            const unsigned char c = static_cast<unsigned char>(text[0]);
            // Control characters arrive as their own byte.
            if (c == 3) {
                key.type = Key::Type::CtrlC;
                return key;
            }
            if (c == 4) {
                key.type = Key::Type::CtrlD;
                return key;
            }
            if (c == 1) {
                key.type = Key::Type::CtrlA;
                return key;
            }
            if (c == 5) {
                key.type = Key::Type::CtrlE;
                return key;
            }
            if (c == 11) {
                key.type = Key::Type::CtrlK;
                return key;
            }
            if (c == 21) {
                key.type = Key::Type::CtrlU;
                return key;
            }
            if (c == 23) {
                key.type = Key::Type::CtrlW;
                return key;
            }
            if (c == 12) {
                key.type = Key::Type::CtrlL;
                return key;
            }
            if (c == 18) {
                key.type = Key::Type::CtrlR;
                return key;
            }
            if (c == 15) {
                key.type = Key::Type::CtrlO;
                return key;
            }
            if (c == 14) {
                key.type = Key::Type::CtrlN;
                return key;
            }
            if (c == 16) {
                key.type = Key::Type::CtrlP;
                return key;
            }
            if (c == 7) {
                key.type = Key::Type::CtrlG;
                return key;
            }
            if (c == 6) {
                key.type = Key::Type::CtrlF;
                return key;
            }
            if (c == 2) {
                key.type = Key::Type::CtrlB;
                return key;
            }
            if (c == 26) {
                key.type = Key::Type::CtrlZ;
                return key;
            }
            if (c == 25) {
                key.type = Key::Type::CtrlY;
                return key;
            }
            if (c == 24) {
                key.type = Key::Type::CtrlX;
                return key;
            }
            if (c == 20) {
                key.type = Key::Type::CtrlT;
                return key;
            }
            if (c == 19) {
                key.type = Key::Type::CtrlS;
                return key;
            }
            if (c == 16) {
                key.type = Key::Type::CtrlP;
                return key;
            }
            if (c == 14) {
                key.type = Key::Type::CtrlN;
                return key;
            }
            if (c < 0x20) {
                key.type = Key::Type::Unknown;
                return key;
            }
        }
        key.type = Key::Type::Character;
        key.text = text;
        return key;
    }

    // FTXUI reports control characters pressed with a modifier as "special"
    // events whose input is a single byte.
    const std::string& input = event.input();
    if (input.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(input[0]);
        if (c == 3) key.type = Key::Type::CtrlC;
        else if (c == 4) key.type = Key::Type::CtrlD;
        else if (c == 1) key.type = Key::Type::CtrlA;
        else if (c == 5) key.type = Key::Type::CtrlE;
        else if (c == 11) key.type = Key::Type::CtrlK;
        else if (c == 21) key.type = Key::Type::CtrlU;
        else if (c == 23) key.type = Key::Type::CtrlW;
        else if (c == 12) key.type = Key::Type::CtrlL;
        else if (c == 18) key.type = Key::Type::CtrlR;
        else if (c == 15) key.type = Key::Type::CtrlO;
        else if (c == 14) key.type = Key::Type::CtrlN;
        else if (c == 16) key.type = Key::Type::CtrlP;
        else if (c == 7) key.type = Key::Type::CtrlG;
        else if (c == 6) key.type = Key::Type::CtrlF;
        else if (c == 2) key.type = Key::Type::CtrlB;
        else if (c == 26) key.type = Key::Type::CtrlZ;
        else if (c == 25) key.type = Key::Type::CtrlY;
        else if (c == 24) key.type = Key::Type::CtrlX;
        else if (c == 20) key.type = Key::Type::CtrlT;
        else if (c == 19) key.type = Key::Type::CtrlS;
        else if (c == 16) key.type = Key::Type::CtrlP;
        else if (c == 14) key.type = Key::Type::CtrlN;
        else return key;
        return key;
    }

    // The arrows with a modifier arrive as special events whose input is
    // the whole sequence: shift+up is "\x1b[1;2A".
    if (input == "\x1b[1;2A") {
        key.type = Key::Type::ShiftUp;
        key.shift = true;
        return key;
    }
    if (input == "\x1b[1;2B") {
        key.type = Key::Type::ShiftDown;
        key.shift = true;
        return key;
    }

    // The arrows with the control and the meta modifier arrive the same
    // way: ctrl+up is "\x1b[1;5A" and meta+up is "\x1b[1;3A".
    if (input == "\x1b[1;5A" || input == "\x1b[5A") {
        key.type = Key::Type::Up;
        key.ctrl = true;
        return key;
    }
    if (input == "\x1b[1;5B" || input == "\x1b[5B") {
        key.type = Key::Type::Down;
        key.ctrl = true;
        return key;
    }
    if (input == "\x1b[1;3A") {
        key.type = Key::Type::Up;
        key.alt = true;
        return key;
    }
    if (input == "\x1b[1;3B") {
        key.type = Key::Type::Down;
        key.alt = true;
        return key;
    }

    // An escape followed by a single character is that character with the
    // Meta (Alt) modifier, the way the reference reads it
    // (`ink/parse-keypress.ts`). Without this the pair would be dropped and
    // both the Escape and the character would be lost.
    if (input.size() == 2 && input[0] == '\x1b') {
        const unsigned char c = static_cast<unsigned char>(input[1]);
        if (c == 0x7f || c == 0x08) {
            key.type = Key::Type::Backspace;
            key.alt = true;
            return key;
        }
        const bool printable = c == ' ' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                               (c >= 'A' && c <= 'Z');
        if (printable) {
            key.type = Key::Type::Character;
            key.text = std::string(1, input[1]);
            key.alt = true;
            return key;
        }
    }

    // Two escapes in a row are a single Escape with the Meta modifier.
    if (input == "\x1b\x1b") {
        key.type = Key::Type::Escape;
        key.alt = true;
        return key;
    }

    return key;
}

/// Reads a mouse event of the SGR mode.
///
/// The sequence is `\x1b[<button;column;rowM` for a press and
/// `\x1b[<button;column;rowm` for a release. The bit that says that the
/// mouse moved is told apart from the others, the way the terminals tell it
/// apart.
bool parse_sgr_mouse(const std::string& data, size_t at, size_t* end, Key* key) {
    if (data.compare(at, 3, "\x1b[<") != 0) return false;

    size_t i = at + 3;
    int numbers[3] = {0, 0, 0};
    int count = 0;
    while (i < data.size() && count < 3) {
        if (data[i] == ';') {
            ++count;
            ++i;
            continue;
        }
        // The end of the numbers is the letter that ends the sequence.
        if (data[i] < '0' || data[i] > '9') break;
        numbers[count] = numbers[count] * 10 + (data[i] - '0');
        ++i;
    }
    if (count != 2 || i >= data.size()) return false;

    const char terminator = data[i];
    if (terminator != 'M' && terminator != 'm') return false;
    *end = i + 1;

    // The event is not one the port uses; the caller drops it.
    key->type = Key::Type::Unknown;

    // The wheel is the sixth bit of the button; it scrolls the list and it
    // is not a press of the left button.
    if ((numbers[0] & 64) != 0) {
        // The wheel is turned up by the button 64 and down by the button 65.
        key->type = (numbers[0] & 3) == 0 ? Key::Type::MouseScrollUp : Key::Type::MouseScrollDown;
        return true;
    }

    // The button of the left one is the only one that counts, and the bit
    // that says that the mouse moved is the one of the motion.
    const int button = numbers[0] & 3;
    const bool moved = (numbers[0] & 32) != 0;
    if (button != 0) return true;

    if (terminator == 'm') {
        // The release of the left button is the click.
        key->type = Key::Type::MouseClick;
    } else if (moved) {
        // The mouse moved while the button was down: the selection grows.
        key->type = Key::Type::MouseDrag;
    } else {
        // The button went down: the selection begins.
        key->type = Key::Type::MouseDown;
    }

    key->mouse_x = numbers[1];
    key->mouse_y = numbers[2];
    return true;
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

/// Moves the cursor to a row and a column of the screen.
///
/// The row and the column count from one.
std::string cursor_to(int row, int column) {
    if (row < 1) row = 1;
    if (column < 1) column = 1;
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(column) + "H";
}

} // namespace

struct Terminal::Impl {
    ftxui::Receiver<ftxui::Task> receiver = ftxui::MakeReceiver<ftxui::Task>();
    ftxui::Sender<ftxui::Task> sender;
    std::unique_ptr<ftxui::TerminalInputParser> parser;

    /// Input that has been decoded but not yet handed to the caller.
    std::deque<PendingInput> pending;

    /// Keys that have been decoded and are waiting to be handed over.
    std::deque<Key> ready;

    /// Bytes that were read but not handed to the parser, because they could
    /// still grow into a bracketed-paste marker.
    std::string carry;

    /// The bytes of a mouse event that has not arrived whole.
    ///
    /// The terminal reports the mouse in the SGR mode; the bytes of an
    /// event that is still coming are held back, so that they are never
    /// read as text.
    std::string mouse_carry;

    /// True while a bracketed paste is being collected.
    bool in_paste = false;

    /// The text collected so far by the current paste.
    std::string paste;

    /// When the last byte was read from the terminal.
    std::chrono::steady_clock::time_point last_input = std::chrono::steady_clock::now();

    /// True when the terminal closed its input and no more keys will come.
    bool closed = false;

    Impl() {
        sender = receiver->MakeSender();
        parser = std::make_unique<ftxui::TerminalInputParser>(std::move(sender));
    }

    /// Queues raw bytes for the key parser.
    ///
    /// The CAN and SUB bytes are taken out of the run: the parser drops them,
    /// so the keys they stand for would never reach the caller.
    void push_bytes(std::string bytes) {
        size_t start = 0;
        while (start < bytes.size()) {
            const size_t stop = bytes.find_first_of("\x18\x1a", start);
            const size_t end = stop == std::string::npos ? bytes.size() : stop;
            push_raw_bytes(bytes.substr(start, end - start));
            if (stop == std::string::npos) return;
            push_key(bytes[stop] == '\x18' ? Key::Type::CtrlX : Key::Type::CtrlZ);
            start = stop + 1;
        }
    }

    /// Takes the mouse events out of `data` and queues them as keys.
    ///
    /// The terminal reports the mouse in the SGR mode, and the key parser of
    /// the terminal does not read that form: the events would end up in the
    /// prompt box as plain text. The bytes of an event that has not arrived
    /// whole are held back until the rest of it arrives, so that the event
    /// is not read as text either.
    std::string take_mouse_events(std::string data) {
        size_t at = 0;
        while (true) {
            const size_t start = data.find("\x1b[<", at);
            if (start == std::string::npos) break;

            size_t end = 0;
            Key key;
            if (!parse_sgr_mouse(data, start, &end, &key)) {
                // The event is not whole: the bytes are held back.
                mouse_carry = data.substr(start);
                data.erase(start);
                break;
            }
            data.erase(start, end - start);
            at = start;
            if (key.type != Key::Type::Unknown) push_key(key);
        }
        return data;
    }

    /// Queues raw bytes for the key parser.
    void push_raw_bytes(std::string bytes) {
        if (bytes.empty()) return;
        PendingInput item;
        item.bytes = std::move(bytes);
        pending.push_back(std::move(item));
    }

    /// Queues a key that was decoded from the raw bytes.
    void push_key(Key::Type type) {
        PendingInput item;
        item.is_key = true;
        item.key.type = type;
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

        // The bytes of a mouse event that had not arrived whole come first.
        if (!mouse_carry.empty()) {
            data = mouse_carry + data;
            mouse_carry.clear();
        }

        // The mouse events are taken out before anything else: their bytes
        // must never reach the key parser.
        data = take_mouse_events(data);

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
    void collect_events() {
        ftxui::Task task;
        while (receiver->ReceiveNonBlocking(&task)) {
            const auto* event = std::get_if<ftxui::Event>(&task);
            if (event == nullptr) continue;
            Key key = translate(*event);
            if (key.type == Key::Type::Unknown) continue;
            ready.push_back(std::move(key));
        }
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
                parser->Timeout(kEscapeTimeoutMs);
                collect_events();
                ready.push_back(std::move(item.key));
                continue;
            }

            for (const char c : item.bytes) parser->Add(c);
            collect_events();
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

        // The mouse event never completed: the bytes are handed over to the
        // parser, which drops them; they must not be read as text.
        if (!mouse_carry.empty()) {
            const std::string bytes = std::move(mouse_carry);
            mouse_carry.clear();
            for (const char c : bytes) parser->Add(c);
        }

        // The escape sequence never completed: hand the bytes that were
        // held back to the parser and let it turn them into a key.
        if (!carry.empty()) {
            const std::string bytes = std::move(carry);
            carry.clear();
            for (const char c : bytes) parser->Add(c);
        }

        // The parser may still be holding a sequence that never completed,
        // such as an escape followed by a single character.
        parser->Timeout(kEscapeTimeoutMs);
        collect_events();
    }

    /// Forgets everything that was read but not yet handed over.
    void reset() {
        pending.clear();
        ready.clear();
        carry.clear();
        mouse_carry.clear();
        paste.clear();
        in_paste = false;
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

int Terminal::width() const {
    const auto size = ftxui::Terminal::Size();
    return size.dimx > 0 ? size.dimx : 80;
}

int Terminal::height() const {
    const auto size = ftxui::Terminal::Size();
    return size.dimy > 0 ? size.dimy : 24;
}

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

    // The terminal wrapped the text that is on the screen again, so the
    // region takes a different number of rows now: it is measured again.
    refit_dynamic();

    // The transcript of the full-screen layout is kept in memory, so its
    // lines are wrapped again at the width of the terminal now.
    if (fullscreen_) rebuild_transcript_rows();

    return true;
}

/// Measures the dynamic region again, after the terminal was resized.
///
/// The terminal wraps the text that is on the screen again when it is
/// resized: a line that was wider than the terminal takes a row for every
/// part of it that does not fit, and the number of those rows changes with
/// the width. The region is measured again, so that the erase covers it
/// exactly and the region is drawn in the place it stands.
void Terminal::refit_dynamic() {
    if (dynamic_height_ <= 0 || dynamic_lines_.empty()) return;

    dynamic_height_ = rows_of_lines(dynamic_lines_);

    if (dynamic_pinned_) {
        // The region stands at the bottom of the screen, so it begins
        // where the rows it takes end.
        dynamic_row_ = std::max(0, height() - dynamic_height_);
    }

    // The cursor stands at the end of the last line of the region.
    cursor_row_ = std::min(std::max(0, height() - 1), dynamic_row_ + dynamic_height_ - 1);

    // The region has to be put in the place the model knows, the next time
    // it is drawn: the terminal may have moved it when it wrapped the text
    // that was on the screen again.
    dynamic_anchored_ = true;
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

    out.append(text);

    // The transcript has to end on a line boundary, otherwise the dynamic
    // region is put back in the middle of a line.
    if (text.back() != '\n') out.push_back('\n');

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

std::string Terminal::erase_dynamic() {
    if (dynamic_height_ <= 0) return {};

    // The terminal may have been resized since the region was drawn, so the
    // region is measured again: the rows it takes now are the rows that are
    // on the screen, and the wipe has to cover them exactly. A wipe that
    // covers fewer rows leaves the frames of the old region in the
    // scrollback.
    took_resize();
    const int rows = dynamic_lines_.empty() ? dynamic_height_ : rows_of_lines(dynamic_lines_);

    // The cursor sits at the end of the last dynamic line. Move back to the
    // first one and return to the first column, so the wipe covers the whole
    // line rather than everything to the right of the cursor.
    const int max_up = std::max(0, height() - 1);
    const int up = std::min(rows - 1, max_up);
    std::string out = cursor_up(up);
    out += "\r";
    out += erase_to_end_of_screen();
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
    // The region starts where the cursor is, which is where the previous
    // region ended once it is erased.
    const int start = cursor_row_;

    // The answer that grows stands in front of the region: it grows right
    // under the text that came before it, the way the reference streams it.
    std::vector<std::string> content = live_lines_;
    content.insert(content.end(), dynamic_content_.begin(), dynamic_content_.end());

    std::string out;
    if (dynamic_pinned_) {
        // The terminal wrapped the text that was on the screen again when it
        // was resized, so the region may stand in another place than the one
        // the model knows. It is put in the place the model knows, so that
        // the box of the prompt stands at the bottom of the screen still.
        if (dynamic_anchored_) out += cursor_to(start + 1, 1);

        // The region is pushed down to the bottom of the screen, so that the
        // prompt box sits there the way Claude Code puts it. The empty
        // lines above it are part of the region, so they are erased and
        // drawn with it.
        const int room = height() - start - rows_of_lines(content);
        if (room > 0) {
            // The empty space goes between the answer that grows and the
            // rest of the region.
            content.insert(content.begin() + static_cast<std::ptrdiff_t>(live_lines_.size()),
                           static_cast<size_t>(room), std::string());
        }
    }

    // The region stands where the model knows now: the next frame is drawn
    // where the cursor is.
    dynamic_anchored_ = false;

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
    int rows = 0;
    for (const auto& line : split_lines(text)) {
        const size_t cells = cc::display_width(line);
        rows += cells == 0 ? 1 : static_cast<int>((cells + columns - 1) / columns);
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

void Terminal::commit_live(std::string_view text) {
    // The answer that grew is taken away: it becomes a part of the
    // transcript.
    live_lines_.clear();
    live_changed_ = true;

    if (fullscreen_) {
        // The answer is written to the transcript of the window and the
        // answer that grew is taken away in the same drawing, so that the
        // answer is never on the screen twice.
        append_transcript(text);
        redraw_fullscreen();
        return;
    }

    write_static(text);
}

void Terminal::render_dynamic(const std::vector<std::string>& lines, bool pinned) {
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

    // The screen was resized: it is drawn again, even when the content of
    // the region did not change.
    took_resize();

    // Redrawing the same content would flicker, so it is skipped — unless
    // the screen has to be drawn again, after a resize, or the answer that
    // grows has changed.
    if (!invalidated_ && !live_changed_ && dynamic_height_ > 0 && lines == dynamic_content_) return;
    invalidated_ = false;
    live_changed_ = false;

    dynamic_content_ = lines;
    dynamic_pinned_ = pinned;
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
    const int rows = height();
    std::vector<std::string> bottom = dynamic_lines_;
    while (rows_of_lines(bottom) > rows && !bottom.empty()) bottom.erase(bottom.begin());
    const int window = std::max(0, rows - rows_of_lines(bottom) - live_rows());

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
    const int rows = std::max(1, height());
    const int window = std::max(0, rows - rows_of_lines(dynamic_lines_) - live_rows());

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
        auto rows = screen_rows(line, width());
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
void Terminal::rebuild_transcript_rows() {
    transcript_rows_.clear();
    transcript_line_rows_.clear();
    for (const auto& line : transcript_) {
        auto rows = screen_rows(line, width());
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
    const int region_rows = rows_of_lines(bottom);

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
    const int window = std::max(0, rows - region_rows);

    // The content of the window is the transcript and the answer that grows:
    // the answer stands right under the transcript.
    const size_t total = transcript_rows_.size() + live.size();
    size_t end = total;
    if (scroll_offset_ > 0) {
        end = total - std::min<size_t>(static_cast<size_t>(scroll_offset_), total);
    }

    // The transcript is cut so that it fits in the window it has.
    std::vector<std::string> shown;
    for (size_t i = end; i > 0; --i) {
        const size_t index = i - 1;
        const std::string& row = index < transcript_rows_.size()
                                     ? transcript_rows_[index]
                                     : live[index - transcript_rows_.size()];
        shown.insert(shown.begin(), row);
        if (rows_of_lines(shown) > window) {
            shown.erase(shown.begin());
            break;
        }
    }

    std::string out = "\x1b[H";

    // The text of every row of the screen is kept, so that the text the
    // user selects with the mouse may be read back.
    screen_rows_.clear();

    // The conversation stands at the top of the screen, the way it grows
    // there, and the region of the prompt stands at the bottom of it: the
    // empty rows are between the two.
    bool first = true;
    for (const auto& line : shown) {
        if (!first) out += "\r\n";
        first = false;
        out += line;
        out += "\x1b[K";

        // The colours of the line take no room on the screen, so they are
        // taken away: the columns of the row are the columns of the screen.
        for (const auto& row : screen_rows(cc::strip_ansi(line), width())) {
            screen_rows_.push_back(row);
        }
    }

    const int used = rows_of_lines(shown);
    for (int i = 0; i < window - used; ++i) {
        if (!first) out += "\r\n";
        first = false;
        out += "\x1b[K";
        screen_rows_.emplace_back();
    }

    for (const auto& line : bottom) {
        if (!first) out += "\r\n";
        first = false;
        out += line;
        out += "\x1b[K";

        // The colours of the line take no room on the screen, so they are
        // taken away: the columns of the row are the columns of the screen.
        for (const auto& row : screen_rows(cc::strip_ansi(line), width())) {
            screen_rows_.push_back(row);
        }
    }

    // Whatever is below the last line is stale, so it is cleared.
    out += "\x1b[J";

    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
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
