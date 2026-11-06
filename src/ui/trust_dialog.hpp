#pragma once

#include "ui/ansi.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace cc::ui {

/// The dialog Claude Code shows the first time it runs in a folder.
///
/// The dialog asks whether the user trusts the folder; the answer is
/// remembered, so the question is only asked once.
class TrustDialog {
public:
    TrustDialog(Theme theme, std::string cwd);

    /// Renders the dialog.
    std::vector<std::string> render(int width) const;

    /// Runs the dialog until the user chooses.
    ///
    /// Returns true when the user trusts the folder, false when they do
    /// not.
    bool run(Terminal& terminal);

private:
    Theme theme_;
    std::string cwd_;
    /// The index of the highlighted choice.
    size_t selected_ = 0;

    /// True when the user pressed the Ctrl-C and has to press it again.
    ///
    /// The reference asks for the key twice and shows a message in between.
    bool ctrl_c_pending_ = false;

    /// The name of the key that has to be pressed again.
    std::string exit_key_ = "Ctrl-C";

    /// When the first press is forgotten.
    ///
    /// The reference forgets it after eight hundred milliseconds.
    std::chrono::steady_clock::time_point exit_deadline_{};
};

} // namespace cc::ui
