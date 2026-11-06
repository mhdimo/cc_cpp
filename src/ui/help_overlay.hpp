#pragma once

#include "ui/ansi.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace cc::ui {

/// One command in the help overlay's list.
struct HelpCommand {
    /// The command, with its leading slash.
    std::string name;
    /// The one-line description shown underneath it.
    std::string description;
};

/// The tab the help overlay is showing.
enum class HelpTab {
    General,        ///< The introduction and the keyboard shortcuts.
    Commands,       ///< The built-in commands.
    CustomCommands, ///< The commands the user added.
};

/// The overlay Claude Code shows for `/help`.
///
/// It is a pane below the transcript: a rule, the tab header, the content
/// of the tab and a footer. The left and right arrows move between the
/// tabs; in the command tabs the up and down arrows move through the list.
class HelpOverlay {
public:
    HelpOverlay(Theme theme, std::vector<HelpCommand> builtin, std::vector<HelpCommand> custom);

    /// Renders the overlay.
    std::vector<std::string> render(int width, int height) const;

    /// Handles a key press.
    ///
    /// Returns false when the overlay should be dismissed.
    bool handle_key(const Key& key);

    /// The tab that is showing.
    HelpTab tab() const { return tab_; }

    /// True when the header of the tabs holds the focus.
    ///
    /// The reference paints the tab that is showing in a block of colour
    /// while the header holds the focus, and in inverted text once the list
    /// of the commands has it.
    bool header_focused() const { return header_focused_; }

    /// True when the exit waits for a second Ctrl-C.
    bool exit_pending() const;

private:
    /// The lines of the general tab.
    std::vector<std::string> render_general(int width) const;

    /// The lines of a tab that lists commands.
    ///
    /// The message that says that there is nothing to show is written on the
    /// tab of the commands of the user only.
    std::vector<std::string> render_commands(const std::vector<HelpCommand>& commands,
                                             const std::string& title, int width, int height,
                                             bool show_empty_message) const;

    /// The commands the current tab lists.
    const std::vector<HelpCommand>& current_commands() const;

    Theme theme_;
    std::vector<HelpCommand> builtin_;
    std::vector<HelpCommand> custom_;
    HelpTab tab_ = HelpTab::General;
    /// The highlighted command in the command tabs.
    size_t selected_ = 0;
    /// The first command the list of the commands shows.
    ///
    /// The window of the list is sticky: it moves by one row only when the
    /// highlight passes its edge, the way the reference moves it.
    size_t first_ = 0;
    /// The height of the pane of the last drawing.
    ///
    /// The window of the list of the commands is as long as the pane
    /// allows, so the overlay keeps the height it was drawn at.
    mutable size_t pane_height_ = 0;
    /// True while the header of the tabs holds the focus.
    bool header_focused_ = true;
    /// When the Ctrl-C that asks for a second one was pressed.
    std::chrono::steady_clock::time_point exit_pressed_at_{};
    /// True when the exit waits for a second Ctrl-C.
    bool exit_armed_ = false;
};

} // namespace cc::ui
