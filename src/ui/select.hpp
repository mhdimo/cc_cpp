#pragma once

#include "ui/ansi.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// One entry in a picker.
struct SelectOption {
    /// The text shown for the entry.
    std::string label;
    /// The explanation shown underneath the label. May be empty.
    std::string description;
};

/// A list the user picks from, the way Claude Code shows its pickers.
///
/// The highlighted entry is marked with `❯`; the up and down arrows move
/// the highlight, Enter picks the entry and Escape dismisses the picker.
class Select {
public:
    Select(Theme theme, std::vector<SelectOption> options, std::string title = {},
           size_t visible_count = 10);

    /// Renders the picker.
    std::vector<std::string> render(int width) const;

    /// Runs the picker until the user picks something.
    ///
    /// Returns true when the user picked an option, false when they
    /// dismissed the picker with Escape.
    bool run(Terminal& terminal);

    /// The index of the highlighted option.
    size_t selected() const { return selected_; }

    /// Moves the highlight to `index`.
    ///
    /// The index is clamped to the options that are there.
    void set_selected(size_t index);

    /// The label of the highlighted option.
    std::string selected_label() const;

    /// The number of options.
    size_t size() const { return options_.size(); }

private:
    Theme theme_;

    /// The title of the picker, when it has one.
    std::string title_;

    std::vector<SelectOption> options_;

    /// The index of the highlighted option.
    size_t selected_ = 0;

    /// The number of options that are shown at once.
    size_t visible_count_ = 10;

    /// The index of the first option that is shown.
    size_t first_ = 0;
};

} // namespace cc::ui
