#pragma once

#include "ui/ansi.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace cc::ui {

/// The picker of the conversations that are resumed.
///
/// The reference opens the picker with a rule, the heading "Resume
/// Session", a field the conversations are searched in, the conversations
/// themselves — the title of every one of them with the time it was
/// written on under it — and the keys that do something at the bottom.
class SessionPicker {
public:
    /// One conversation of the list.
    struct Entry {
        /// The id of the session.
        std::string id;

        /// The title of the conversation.
        std::string title;

        /// The time the conversation was written on, the way the reference
        /// writes it: "2 hours ago".
        std::string when;

        /// The branch the conversation ran on.
        std::string branch;

        /// The size of the transcript of the conversation, the way the
        /// reference writes it: "1.4KB".
        std::string size;

        /// The number of the messages of the conversation.
        size_t messages = 0;

        /// The folder the conversation ran in, when it is not the folder
        /// the session runs in.
        std::string path;
    };

    /// The picker of the conversations of the folder.
    ///
    /// The second list holds the conversations of every folder of the
    /// machine; Ctrl+A switches between the two, the way the reference
    /// switches between them. The name of the branch the session runs on
    /// is kept as well: Ctrl+B shows the conversations of that branch
    /// only.
    SessionPicker(Theme theme, std::vector<Entry> entries, std::vector<Entry> all_entries = {},
                  std::string current_branch = {});

    /// The lines of the picker.
    std::vector<std::string> render(int width) const;

    /// Runs the picker until the user picks a conversation.
    ///
    /// Returns the id of the conversation; the string is empty when the
    /// picker was dismissed.
    std::string run(Terminal& terminal);

private:
    /// The conversations that match the search of the user.
    ///
    /// The branch filter is applied as well: only the conversations of the
    /// branch of the session are shown when it is on.
    std::vector<size_t> matching() const;

    /// The conversations that are shown: the ones of the folder, or the
    /// ones of every folder.
    const std::vector<Entry>& shown() const;

    /// The row the list starts on, counted from the top of the picker.
    ///
    /// The rule, the empty line, the heading, the three lines of the field
    /// and the empty line under it come before the list.
    static constexpr int kFirstRow = 7;

    /// The row of the list a conversation stands on, counted from the top
    /// of the picker.
    ///
    /// Every conversation takes two rows: its title and the line under it.
    /// The count is -1 when the conversation is not shown.
    int row_of(size_t index) const;

    /// Moves the window of the list, so that the chosen conversation is
    /// shown.
    ///
    /// The window follows the highlight, the way the reference shows it.
    void scroll_to_selection();

    /// The keys that do something, the way the reference writes them.
    std::string hints() const;

    Theme theme_;

    /// The conversations of the folder the session runs in.
    std::vector<Entry> entries_;

    /// The conversations of every folder of the machine.
    std::vector<Entry> all_entries_;

    /// The branch the session runs on.
    std::string current_branch_;

    /// True when the conversations of every folder are shown.
    bool all_ = false;

    /// True when only the conversations of the branch of the session are
    /// shown.
    bool branch_only_ = false;

    /// The search of the user.
    std::string query_;

    /// The index of the conversation that is highlighted, in the list of the
    /// ones that match the search.
    size_t selected_ = 0;

    /// The first conversation that is shown.
    size_t first_ = 0;
};

} // namespace cc::ui
