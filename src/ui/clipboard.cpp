#include "ui/clipboard.hpp"

#include "utils/hash.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

#include <cstdlib>

namespace cc::ui {
namespace {

/// The escape that starts an operating system command.
constexpr std::string_view kOsc = "\x1b]";

/// The escape that ends an operating system command.
constexpr char kBel = '\a';

/// The escape that ends an operating system command in kitty.
constexpr std::string_view kSt = "\x1b\\";

/// The name of the terminal the session runs in.
std::string terminal_program() {
    if (const char* program = std::getenv("TERM_PROGRAM")) {
        if (*program) return program;
    }
    if (const char* program = std::getenv("LC_TERMINAL")) {
        if (*program) return program;
    }
    return {};
}

/// The escape that goes through tmux.
///
/// The escape of the clipboard is swallowed by tmux, so it is handed to the
/// terminal outside it.
std::string tmux_passthrough(const std::string& payload) {
    std::string escaped;
    escaped.reserve(payload.size() * 2);
    for (const char c : payload) {
        if (c == '\x1b') {
            escaped += "\x1b\x1b";
            continue;
        }
        escaped += c;
    }
    return "\x1bPtmux;" + escaped + "\x1b\\";
}

/// The tool of the system that puts text in the clipboard.
///
/// The tool is not used over a connection of a shell: the text would end up
/// in the clipboard of the machine the shell runs on, and not in the one of
/// the user.
std::string native_copy_tool() {
    if (std::getenv("SSH_CONNECTION") != nullptr) return {};

#if defined(__APPLE__)
    return "pbcopy";
#elif defined(__linux__)
    // The tool of Wayland is asked for first, and the ones of X11 after it.
    for (const char* tool : {"wl-copy", "xclip", "xsel"}) {
        const ProcessOutcome probe = run_process(std::string(tool) + " --version", {}, {}, 2);
        if (!probe.failed && probe.exit_code == 0) return tool;
    }
    return {};
#else
    return {};
#endif
}

} // namespace

std::string clipboard_sequence(const std::string& text) {
    const std::string encoded = base64(std::vector<uint8_t>(text.begin(), text.end()));

    // The terminal of kitty is told with the other end of the escape, so
    // that it does not ring the bell.
    const bool kitty = terminal_program() == "kitty";
    return std::string(kOsc) + "52;c;" + encoded + (kitty ? std::string(kSt) : std::string(1, kBel));
}

void copy_to_clipboard(const std::string& text) {
    if (text.empty()) return;

    // The tool of the system is asked for first: the escape of the clipboard
    // is not followed by every terminal.
    const std::string tool = native_copy_tool();
    if (!tool.empty()) {
        run_process(tool, {}, text, 2);
    }

    // The escape of the clipboard is written, so that the terminal of the
    // user gets the text as well.
    const std::string sequence = clipboard_sequence(text);

    if (std::getenv("TMUX") != nullptr) {
        // The text is put in the buffer of tmux, so that it may be pasted
        // with the prefix of tmux.
        run_process("tmux load-buffer -w -", {}, text, 2);
        std::fputs(tmux_passthrough(sequence).c_str(), stdout);
        std::fflush(stdout);
        return;
    }

    std::fputs(sequence.c_str(), stdout);
    std::fflush(stdout);
}

} // namespace cc::ui
