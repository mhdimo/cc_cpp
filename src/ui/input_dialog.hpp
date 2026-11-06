#pragma once

#include "ui/prompt_input.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace cc::ui {

/// How the dialog ended.
enum class InputDialogOutcome {
    /// The user confirmed the value.
    Confirmed,
    /// The user left the dialog with the escape key.
    Cancelled,
    /// The user interrupted the field with the Ctrl-C.
    Interrupted,
};

/// What the dialog returned.
struct InputDialogResult {
    /// How the dialog ended.
    InputDialogOutcome outcome = InputDialogOutcome::Confirmed;
    /// The value of the field, when the user confirmed it.
    std::string value;
    /// True when the user asked for the next field with the tab key.
    bool next_field = false;
};

/// The dialog that asks the user for a value.
///
/// The dialog is a rule across the top, the title, the subtitle, the field
/// with the cursor in it, the number of the field and the keys that save it.
class InputDialog {
public:
    InputDialog(Theme theme, std::string title, std::string subtitle, std::string value = {});

    /// Sets the field the dialog asks for.
    ///
    /// The reference gives the field a title of its own, a description of
    /// its own, and says which field of how many it is.
    void set_field(std::string label, std::string description, size_t index, size_t count);

    /// Sets the line that stands above the field.
    ///
    /// The reference says "Describe the issue below:" above the field of
    /// the report.
    void set_lead(std::string lead);

    /// Sets the hint of the keys.
    ///
    /// The reference says "Enter to continue" in the field of the report.
    void set_hint(std::string hint);

    /// Says whether the Ctrl-C leaves the dialog.
    ///
    /// The reference binds it in the dialog of the design system, and
    /// leaves it to the field where a field is being written in.
    void set_exit_on_ctrl_c(bool active) { exit_on_ctrl_c_ = active; }

    /// Renders the dialog.
    std::vector<std::string> render(int width) const;

    /// Runs the dialog until the user confirms it.
    InputDialogResult run(Terminal& terminal);

private:
    Theme theme_;

    /// The title of the dialog.
    std::string title_;

    /// The subtitle of the dialog.
    std::string subtitle_;

    /// The line that stands above the field.
    std::string lead_;

    /// The title of the field.
    std::string field_label_;

    /// The description of the field.
    std::string field_description_;

    /// The number of the field, counting from one, and the number of the
    /// fields there are.
    size_t field_index_ = 0;
    size_t field_count_ = 0;

    /// The hint of the keys.
    std::string hint_ = "Enter to confirm · Esc to cancel";

    /// True when the Ctrl-C leaves the dialog on the second press.
    bool exit_on_ctrl_c_ = true;

    /// True when the user pressed the Ctrl-C and has to press it again.
    bool ctrl_c_pending_ = false;

    /// When the first press is forgotten.
    std::chrono::steady_clock::time_point exit_deadline_{};

    PromptInput input_;
};

} // namespace cc::ui
