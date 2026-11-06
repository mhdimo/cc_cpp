#pragma once

#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// The view Claude Code shows for `/mobile`.
///
/// It is a pane below the transcript: a rule, the code of the address of the
/// application, and the way to get it. The tab switches between the two
/// applications.
class MobileView {
public:
    MobileView(Theme theme);

    /// Renders the view.
    std::vector<std::string> render(int width, int height) const;

    /// Runs the view until the user dismisses it.
    void run(Terminal& terminal);

    /// Says whether the view is drawn in the modal slot.
    ///
    /// The reference floats the panes of the slash commands in the modal
    /// slot of the full-screen layout: they have a rule of their own and
    /// their contents sit three columns in.
    void set_modal(bool modal);

    /// The address of the application that is showing.
    std::string url() const;

private:
    /// The code of the address of the application that is showing.
    std::vector<std::string> code() const;

    Theme theme_;
    /// True when the application of the iPhone is showing.
    bool ios_ = true;
    /// True when the view is drawn in the modal slot.
    bool modal_ = false;
};

} // namespace cc::ui
