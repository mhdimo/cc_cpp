#pragma once

#include "ui/ansi.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace cc::ui {

/// The code that changed when the conversation is rewound.
///
/// The reference counts the files that would change and the lines that
/// would be added and removed, the way a diff does.
struct RewindDiff {
    /// The files that changed.
    std::vector<std::string> files_changed;
    /// The lines that were added.
    int insertions = 0;
    /// The lines that were removed.
    int deletions = 0;
};

/// One prompt the conversation can be rewound to.
struct RewindOption {
    /// The text of the prompt.
    std::string text;
    /// How long ago the prompt was sent, e.g. `2 minutes ago`.
    std::string ago;
    /// True when the option is the prompt that is being written.
    bool current = false;
    /// The code that changed after the prompt.
    ///
    /// It is empty when the prompt cannot be restored; the reference warns
    /// about that in the list of the prompts.
    std::optional<RewindDiff> diff;
    /// The code that a rewind to this prompt would put back.
    std::optional<RewindDiff> restore;
};

/// What the user chose to do with the conversation.
enum class RewindAction {
    None,         ///< The view was dismissed.
    Cancel,       ///< The user chose "Never mind"; the list shows again.
    Restore,      ///< The conversation is rewound to the chosen prompt.
    RestoreCode,  ///< The files are put back, the conversation is left alone.
    RestoreBoth,  ///< The files and the conversation are put back.
    Summarize,    ///< The conversation from the chosen prompt is summarised.
};

/// The view that lets the user rewind the conversation.
///
/// Claude Code opens it when the user presses Escape twice on an empty
/// prompt. It lists the prompts of the session, and the prompt that is
/// picked is then confirmed with a second list.
class MessageSelector {
public:
    MessageSelector(Theme theme, std::vector<RewindOption> options);

    /// Renders the view.
    std::vector<std::string> render(int width) const;

    /// The lines of the confirmation.
    ///
    /// The confirmation is the second view of the rewind: the actions the
    /// user can pick for the prompt that is chosen.
    std::vector<std::string> render_confirm(int width) const;

    /// Runs the view until the user picks a prompt and an action.
    RewindAction run(Terminal& terminal);

    /// The index of the prompt that was picked.
    size_t selected() const { return selected_; }

    /// The prompt that is highlighted.
    void set_selected(size_t index);

    /// The note the user wrote for the summary.
    const std::string& feedback() const { return feedback_; }

private:
    /// The lines of the list of prompts.
    std::vector<std::string> render_list(int width) const;

    Theme theme_;
    std::vector<RewindOption> options_;

    /// The prompt that is highlighted.
    size_t selected_ = 0;

    /// True when the confirmation is showing.
    bool confirming_ = false;

    /// The action that is highlighted in the confirmation.
    size_t action_ = 0;

    /// The note the user writes for the summary.
    std::string feedback_;

    /// The number of prompts that are shown at once.
    ///
    /// The reference shows seven of them; the port shows as many as fit on
    /// the screen, because the region of the view may never be taller than
    /// the screen.
    size_t visible_ = 7;

    /// True when the Ctrl-C that leaves the view waits for its second press.
    bool exiting_ = false;

    /// When the Ctrl-C was pressed the first time.
    std::chrono::steady_clock::time_point exit_at_;
};

} // namespace cc::ui
