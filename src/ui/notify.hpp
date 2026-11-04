#pragma once

#include <string>
#include <vector>

namespace cc::ui {

/// The way the terminal is told that the session wants the user.
///
/// The reference offers the channels of the terminals it knows and lets the
/// user pick one of them in the panel of the configuration.
enum class NotificationChannel {
    Auto,           ///< The channel is picked from the terminal the session runs in.
    Iterm2,         ///< The notification of iTerm2.
    TerminalBell,   ///< The bell of the terminal.
    Iterm2WithBell, ///< The notification of iTerm2 and the bell of the terminal.
    Kitty,          ///< The notification of kitty.
    Ghostty,        ///< The notification of ghostty.
    Disabled,       ///< The terminal is not told at all.
};

/// The channel of the notifications, as it is written in the configuration.
///
/// The channel of `auto` is returned for a name that is not known, the way
/// the reference falls back on it.
NotificationChannel notification_channel(const std::string& name);

/// The name of the channel, as it is written in the configuration.
std::string to_string(NotificationChannel channel);

/// The names of the channels, in the order the reference offers them.
const std::vector<std::string>& notification_channel_names();

/// Tells the terminal that the session wants the user.
///
/// The sequence that is written is the one of the terminal that is asked
/// for. The bell of the terminal is written alone, the way the reference
/// writes it, so that tmux sees it.
void notify_terminal(NotificationChannel channel, const std::string& message,
                     const std::string& title = "Claude Code");

} // namespace cc::ui
