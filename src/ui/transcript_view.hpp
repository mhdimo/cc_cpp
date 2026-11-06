#pragma once

#include "ui/ansi.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// One entry of the transcript.
///
/// The entry holds the lines the message is drawn with, the same lines the
/// conversation itself shows.
struct TranscriptEntry {
    /// The lines of the entry.
    std::vector<std::string> lines;
    /// True when the entry is only shown when everything is shown.
    bool hidden = false;
};

/// The full-screen view of the conversation.
///
/// The whole conversation is listed, the way Claude Code shows it when the
/// detailed transcript is toggled on.
class TranscriptView {
public:
    TranscriptView(Theme theme, std::vector<TranscriptEntry> entries);

    /// Renders the view.
    std::vector<std::string> render(int width, int height) const;

    /// Runs the view until the user dismisses it.
    ///
    /// `show_all` tells the view whether everything is shown; it is toggled
    /// with Ctrl+E. The callback reports the new state so the caller can
    /// rebuild the entries.
    void run(Terminal& terminal, bool& show_all);

    /// Shows everything, or only the last messages.
    ///
    /// The older messages join the transcript when everything is shown.
    void set_show_all(bool show_all) { show_all_ = show_all; }

private:
    /// The lines of the conversation, without the footer.
    ///
    /// The result is kept between the frames, because the view redraws
    /// itself on every tick and rendering the markdown each time would be
    /// a waste.
    const std::vector<std::string>& body_lines(int width) const;

    /// The largest offset the view can be scrolled to.
    int max_scroll(int width, int height) const;

    /// The rule that names the messages that are hidden.
    ///
    /// The reference draws the title of the rule in the middle of it, with
    /// the rule on both sides of it.
    std::string truncation_rule(size_t hidden_count, bool show_all, int width) const;

    Theme theme_;
    std::vector<TranscriptEntry> entries_;

    /// True when everything is shown, the hidden entries included.
    bool show_all_ = false;

    /// The number of lines the view is scrolled down by.
    int scroll_ = 0;

    /// The rendered conversation.
    mutable std::vector<std::string> body_;
    /// The width the conversation was rendered for.
    mutable int body_width_ = -1;
    /// The state the conversation was rendered for.
    mutable bool body_show_all_ = false;
};

} // namespace cc::ui
