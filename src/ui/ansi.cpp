#include "ui/ansi.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace cc::ui {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool Rgb::parse(std::string_view text, Rgb& out) {
    if (text.size() >= 2 && text[0] == '#') {
        if (text.size() != 7) return false;
        int values[3];
        for (int i = 0; i < 3; ++i) {
            const int hi = hex_value(text[1 + i * 2]);
            const int lo = hex_value(text[2 + i * 2]);
            if (hi < 0 || lo < 0) return false;
            values[i] = hi * 16 + lo;
        }
        out = Rgb{static_cast<uint8_t>(values[0]), static_cast<uint8_t>(values[1]),
                   static_cast<uint8_t>(values[2])};
        return true;
    }

    constexpr std::string_view kPrefix = "rgb(";
    if (text.size() < kPrefix.size() + 2) return false;
    if (text.substr(0, kPrefix.size()) != kPrefix) return false;
    if (text.back() != ')') return false;

    const std::string_view body = text.substr(kPrefix.size(), text.size() - kPrefix.size() - 1);
    int values[3] = {0, 0, 0};
    int index = 0;
    size_t pos = 0;
    while (pos <= body.size() && index < 3) {
        const size_t comma = body.find(',', pos);
        const std::string_view part =
            body.substr(pos, comma == std::string_view::npos ? std::string_view::npos
                                                             : comma - pos);
        if (part.empty()) return false;
        for (char c : part) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        const int value = std::atoi(std::string(part).c_str());
        if (value < 0 || value > 255) return false;
        values[index++] = value;
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    if (index != 3) return false;
    out = Rgb{static_cast<uint8_t>(values[0]), static_cast<uint8_t>(values[1]),
               static_cast<uint8_t>(values[2])};
    return true;
}

Style Style::fg(Rgb colour) {
    Style s;
    // A colour of the terminal's own palette is drawn with the code of the
    // palette, not with a twenty-four bit one.
    if (colour.basic) {
        s.basic_foreground = *colour.basic;
        return s;
    }
    s.foreground = colour;
    return s;
}

Style Style::bg(Rgb colour) {
    Style s;
    s.background = colour;
    return s;
}

Style Style::fg(BasicColor colour) {
    Style s;
    s.basic_foreground = colour;
    return s;
}

std::string style_sequence(const Style& style) {
    std::string out = "\x1b[";
    bool first = true;

    auto emit = [&](const std::string& code) {
        if (!first) out += ';';
        out += code;
        first = false;
    };

    if (style.bold) emit("1");
    if (style.dim) emit("2");
    if (style.italic) emit("3");
    if (style.underline) emit("4");
    if (style.inverse) emit("7");
    if (style.strikethrough) emit("9");

    if (style.foreground) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "38;2;%u;%u;%u", style.foreground->r,
                      style.foreground->g, style.foreground->b);
        emit(buf);
    }
    if (style.basic_foreground) {
        const uint8_t index = static_cast<uint8_t>(*style.basic_foreground);
        // The eight colours are 30 to 37, the bright ones 90 to 97.
        char buf[8];
        if (index < 8) {
            std::snprintf(buf, sizeof(buf), "%u", 30u + index);
        } else {
            std::snprintf(buf, sizeof(buf), "%u", 90u + index - 8u);
        }
        emit(buf);
    }
    if (style.background) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "48;2;%u;%u;%u", style.background->r,
                      style.background->g, style.background->b);
        emit(buf);
    }

    if (first) return {};
    out += 'm';
    return out;
}

std::string styled(std::string_view text, const Style& style) {
    const std::string sequence = style_sequence(style);
    if (sequence.empty()) return std::string(text);
    std::string out;
    out.reserve(sequence.size() + text.size() + kReset.size());
    out += sequence;
    out += text;
    out += kReset;
    return out;
}

std::string cursor_up(int lines) {
    if (lines <= 0) return {};
    return "\x1b[" + std::to_string(lines) + "A";
}

std::string cursor_down(int lines) {
    if (lines <= 0) return {};
    return "\x1b[" + std::to_string(lines) + "B";
}

std::string erase_to_end_of_screen() { return "\x1b[0J"; }
std::string erase_screen() { return "\x1b[2J\x1b[H"; }

std::string cursor_to_column(int column) {
    if (column < 1) column = 1;
    return "\x1b[" + std::to_string(column) + "G";
}

std::string hide_cursor() { return "\x1b[?25l"; }
std::string show_cursor() { return "\x1b[?25h"; }

std::string hyperlink(std::string_view text, std::string_view url) {
    if (url.empty()) return std::string(text);
    // A terminal that cannot follow a hyperlink is shown the address alone,
    // the way the reference shows it.
    if (!supports_hyperlinks()) return std::string(url);
    // The BEL is the terminator that is more widely supported, the way the
    // reference writes it.
    std::string out;
    out += "\x1b]8;;";
    out += url;
    out += "\a";
    out += text;
    out += "\x1b]8;;\a";
    return out;
}

bool supports_hyperlinks() {
    // The reference asks the terminal with the variables it has, the way the
    // library of the reference does.
    if (const char* no = std::getenv("NO_HYPERLINK"); no != nullptr && *no != '\0') return false;
    if (const char* force = std::getenv("FORCE_HYPERLINK"); force != nullptr && *force != '\0') {
        return true;
    }
    if (!isatty(STDOUT_FILENO)) return false;

    // The terminals the reference knows about: the ones that are named by
    // `TERM_PROGRAM` or by `LC_TERMINAL` (the latter is kept inside tmux).
    // Apple Terminal is not one of them: it cannot follow a hyperlink, and
    // the reference shows the address alone there.
    static const char* const kPrograms[] = {"iTerm.app", "iTerm2",  "Hyper",   "kitty",
                                            "ghostty",   "alacritty", "WezTerm", "vscode"};
    for (const char* name : {std::getenv("TERM_PROGRAM"), std::getenv("LC_TERMINAL")}) {
        if (name == nullptr) continue;
        for (const char* known : kPrograms) {
            if (std::string(name) == known) return true;
        }
    }
    // The terminal of kitty is the only one the reference knows by `TERM`.
    if (const char* term = std::getenv("TERM"); term != nullptr) {
        const std::string name = term;
        if (name == "alacritty" || name.find("kitty") != std::string::npos) return true;
    }
    return false;
}

std::string clipboard_sequence(std::string_view text) {
    // OSC 52 carries the text base64-encoded, in the "c" clipboard.
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out = "\x1b]52;c;";

    size_t i = 0;
    while (i + 2 < text.size()) {
        const unsigned value = (static_cast<unsigned char>(text[i]) << 16) |
                               (static_cast<unsigned char>(text[i + 1]) << 8) |
                               static_cast<unsigned char>(text[i + 2]);
        out.push_back(kAlphabet[(value >> 18) & 0x3f]);
        out.push_back(kAlphabet[(value >> 12) & 0x3f]);
        out.push_back(kAlphabet[(value >> 6) & 0x3f]);
        out.push_back(kAlphabet[value & 0x3f]);
        i += 3;
    }

    if (i < text.size()) {
        const size_t left = text.size() - i;
        unsigned value = static_cast<unsigned char>(text[i]) << 16;
        if (left > 1) value |= static_cast<unsigned char>(text[i + 1]) << 8;
        out.push_back(kAlphabet[(value >> 18) & 0x3f]);
        out.push_back(kAlphabet[(value >> 12) & 0x3f]);
        out.push_back(left > 1 ? kAlphabet[(value >> 6) & 0x3f] : '=');
        out.push_back('=');
    }

    out.push_back('\a');
    return out;
}

void copy_to_clipboard(std::string_view text) {
    if (text.empty()) return;

    // The sequence goes to the standard output, where the terminal picks it
    // up.
    const std::string sequence = clipboard_sequence(text);
    std::fwrite(sequence.data(), 1, sequence.size(), stdout);
    std::fflush(stdout);

    // A terminal that does not take the sequence is helped along by the
    // clipboard tool of the platform.
#if defined(__APPLE__)
    const char* tool = "pbcopy";
#elif defined(__linux__)
    const char* tool = "xclip";
#else
    const char* tool = nullptr;
#endif

    if (tool == nullptr) return;

    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) return;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        return;
    }

    if (pid == 0) {
        // The child forks once more and leaves, so the tool is reparented
        // to the init process and does not become a zombie.
        ::close(pipe_fds[1]);
        const pid_t grandchild = ::fork();
        if (grandchild < 0) ::_exit(127);
        if (grandchild > 0) ::_exit(0);

        ::dup2(pipe_fds[0], STDIN_FILENO);
        ::close(pipe_fds[0]);
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) {
            ::dup2(null, STDOUT_FILENO);
            ::dup2(null, STDERR_FILENO);
            ::close(null);
        }
#if defined(__APPLE__)
        ::execlp(tool, tool, static_cast<char*>(nullptr));
#else
        ::execlp(tool, tool, "-selection", "clipboard", "-i", static_cast<char*>(nullptr));
#endif
        ::_exit(127);
    }

    ::close(pipe_fds[0]);
    // The text is written to the tool, and the pipe is closed so the tool
    // knows that there is nothing more coming.
    size_t written = 0;
    while (written < text.size()) {
        const ssize_t count = ::write(pipe_fds[1], text.data() + written, text.size() - written);
        if (count <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        written += static_cast<size_t>(count);
    }
    ::close(pipe_fds[1]);

    // The child leaves on its own, so the wait does not last.
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

std::string strip(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\x1b') {
            // CSI sequences: ESC [ ... final byte in 0x40..0x7E.
            if (i + 1 < text.size() && text[i + 1] == '[') {
                size_t j = i + 2;
                while (j < text.size() && !(text[j] >= 0x40 && text[j] <= 0x7E)) ++j;
                i = (j < text.size()) ? j + 1 : text.size();
                continue;
            }
            // OSC sequences: ESC ] ... BEL or ESC \.
            if (i + 1 < text.size() && text[i + 1] == ']') {
                size_t j = i + 2;
                while (j < text.size() && text[j] != '\a') {
                    if (text[j] == '\x1b' && j + 1 < text.size() && text[j + 1] == '\\') {
                        j += 1;
                        break;
                    }
                    ++j;
                }
                i = (j < text.size()) ? j + 1 : text.size();
                continue;
            }
            // Any other two-byte escape sequence.
            i = std::min(text.size(), i + 2);
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

} // namespace cc::ui
