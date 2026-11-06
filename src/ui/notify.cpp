#include "ui/notify.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace cc::ui {
namespace {

/// The escape that ends an operating system command.
constexpr char kBel = '\a';

/// The escape that starts an operating system command.
constexpr std::string_view kOsc = "\x1b]";

/// The escape that ends an operating system command in kitty.
constexpr std::string_view kSt = "\x1b\\";

/// The number of the notification of kitty.
constexpr int kKitty = 99;

/// The number of the notification of ghostty.
constexpr int kGhostty = 777;

/// The number of the notification of iTerm2.
constexpr int kIterm2 = 9;

/// The name of the terminal the session runs in.
///
/// The reference reads the name of the terminal from the environment, the
/// way the port does it for the terminal it draws in.
std::string terminal_program() {
    if (const char* program = std::getenv("TERM_PROGRAM")) {
        if (*program) return program;
    }
    if (const char* program = std::getenv("LC_TERMINAL")) {
        if (*program) return program;
    }
    return {};
}

/// Writes a sequence to the terminal.
void write_raw(const std::string& sequence) {
    std::fwrite(sequence.data(), 1, sequence.size(), stdout);
    std::fflush(stdout);
}

/// The notification of iTerm2.
///
/// The reference writes the message under the title, with two empty lines
/// above it.
std::string iterm2_sequence(const std::string& message, const std::string& title) {
    const std::string text = title.empty() ? message : title + ":\n" + message;
    return std::string(kOsc) + std::to_string(kIterm2) + ";" + "\n\n" + text + kBel;
}

/// The notification of kitty.
///
/// The reference writes the title and the body in two commands, and closes
/// the notification with a third one.
std::vector<std::string> kitty_sequences(const std::string& message, const std::string& title,
                                          int id) {
    const std::string end(kSt);
    const std::string head = std::string(kOsc) + std::to_string(kKitty) + ";";

    std::vector<std::string> out;
    out.push_back(head + "i=" + std::to_string(id) + ":d=0:p=title" + title + end);
    out.push_back(head + "i=" + std::to_string(id) + ":p=body" + message + end);
    out.push_back(head + "i=" + std::to_string(id) + ":d=1:a=focus" + end);
    return out;
}

/// The notification of ghostty.
std::string ghostty_sequence(const std::string& message, const std::string& title) {
    return std::string(kOsc) + std::to_string(kGhostty) + ";notify;" + title + ";" + message + kBel;
}

/// The channel that is used when the user asked for the automatic one.
///
/// The reference picks the notification of the terminal the session runs in;
/// the bell of the terminal is used when the terminal is the one of Apple.
NotificationChannel automatic_channel() {
    const std::string terminal = terminal_program();
    if (terminal == "iTerm.app") return NotificationChannel::Iterm2;
    if (terminal == "kitty") return NotificationChannel::Kitty;
    if (terminal == "ghostty") return NotificationChannel::Ghostty;
    return NotificationChannel::TerminalBell;
}

} // namespace

NotificationChannel notification_channel(const std::string& name) {
    if (name == "iterm2") return NotificationChannel::Iterm2;
    if (name == "terminal_bell") return NotificationChannel::TerminalBell;
    if (name == "iterm2_with_bell") return NotificationChannel::Iterm2WithBell;
    if (name == "kitty") return NotificationChannel::Kitty;
    if (name == "ghostty") return NotificationChannel::Ghostty;
    if (name == "notifications_disabled") return NotificationChannel::Disabled;
    return NotificationChannel::Auto;
}

std::string to_string(NotificationChannel channel) {
    switch (channel) {
    case NotificationChannel::Iterm2:
        return "iterm2";
    case NotificationChannel::TerminalBell:
        return "terminal_bell";
    case NotificationChannel::Iterm2WithBell:
        return "iterm2_with_bell";
    case NotificationChannel::Kitty:
        return "kitty";
    case NotificationChannel::Ghostty:
        return "ghostty";
    case NotificationChannel::Disabled:
        return "notifications_disabled";
    case NotificationChannel::Auto:
        break;
    }
    return "auto";
}

const std::vector<std::string>& notification_channel_names() {
    static const std::vector<std::string> names = {
        "auto", "iterm2", "terminal_bell", "iterm2_with_bell", "kitty", "ghostty",
        "notifications_disabled",
    };
    return names;
}

void notify_terminal(NotificationChannel channel, const std::string& message,
                     const std::string& title) {
    if (channel == NotificationChannel::Auto) channel = automatic_channel();
    if (channel == NotificationChannel::Disabled) return;

    switch (channel) {
    case NotificationChannel::Iterm2:
        write_raw(iterm2_sequence(message, title));
        return;
    case NotificationChannel::Iterm2WithBell:
        write_raw(iterm2_sequence(message, title));
        write_raw(std::string(1, kBel));
        return;
    case NotificationChannel::Kitty: {
        // The number of the notification is at random, the way the
        // reference picks it.
        const int id = static_cast<int>(std::rand() % 10000);
        for (const auto& sequence : kitty_sequences(message, title, id)) write_raw(sequence);
        return;
    }
    case NotificationChannel::Ghostty:
        write_raw(ghostty_sequence(message, title));
        return;
    case NotificationChannel::TerminalBell:
        write_raw(std::string(1, kBel));
        return;
    default:
        return;
    }
}

} // namespace cc::ui
