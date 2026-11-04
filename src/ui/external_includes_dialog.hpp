#pragma once

#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// The dialog Claude Code shows when a project pulls files in from outside
/// its folder.
///
/// The dialog asks whether the files outside the folder of the session may be
/// read; the answer is remembered, so the question is only asked once.
class ExternalIncludesDialog {
public:
    /// The dialog, with the files the project pulls in.
    ///
    /// The dialog is drawn with its own frame when it stands on its own, and
    /// without one when it is a panel of the configuration.
    ExternalIncludesDialog(Theme theme, std::vector<std::string> includes, bool standalone = true);

    /// Renders the dialog.
    std::vector<std::string> render(int width) const;

    /// Runs the dialog until the user chooses.
    ///
    /// Returns true when the user allows the files to be read, false when
    /// they do not. The reference treats the escape key as a refusal, the
    /// way its dialog does.
    bool run(Terminal& terminal);

private:
    Theme theme_;

    /// The files the project pulls in from outside its folder.
    std::vector<std::string> includes_;

    /// True when the dialog is drawn on its own, with its own frame.
    bool standalone_ = true;

    /// The index of the highlighted choice.
    size_t selected_ = 0;
};

} // namespace cc::ui
