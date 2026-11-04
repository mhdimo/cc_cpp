#pragma once

#include "ui/ansi.hpp"
#include "ui/figures.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// The mode the prompt box is in.
enum class InputMode {
    Prompt,  ///< Normal input.
    Bash,    ///< The input starts with `!` and is run as a shell command.
};

/// The prompt box: the bar the user types into.
///
/// The box renders the same way Claude Code renders it: a horizontal rule
/// above and below the input, a `❯` pointer in front of the text, and a
/// hint line underneath.
class PromptInput {
public:
    PromptInput(Theme theme, std::string cwd);

    /// The current text.
    const std::string& text() const { return text_; }

    /// Replaces the text.
    void set_text(std::string text);

    /// Appends a character to the text.
    void insert(std::string_view chunk);

    /// Removes the character before the cursor.
    void backspace();

    /// Deletes the character under the cursor.
    void delete_forward();

    /// Moves the cursor one position left.
    void move_left();

    /// Moves the cursor one position right.
    void move_right();

    /// Moves the cursor to the start of the line.
    void move_home();

    /// Moves the cursor to the end of the line.
    void move_end();

    /// Moves the cursor to the start of the previous line, if there is one.
    bool move_up();

    /// Moves the cursor to the start of the next line, if there is one.
    bool move_down();

    /// Inserts a newline.
    void insert_newline();

    /// True when the character just before the cursor is a backslash.
    ///
    /// Claude Code lets the user write a backslash before the return key to
    /// put a newline in the prompt instead of sending it.
    bool has_backslash_before_cursor() const;

    /// Removes the text before the cursor.
    void kill_to_start();

    /// Removes the text after the cursor.
    void kill_to_end();

    /// Removes the word before the cursor.
    void delete_word_backwards();

    /// Clears the text and the cursor.
    void clear();

    /// The mode the input is in, derived from its first character.
    InputMode mode() const;

    /// The text with the mode character removed.
    std::string value_for_mode() const;

    /// The placeholder shown while the input is empty.
    ///
    /// Claude Code shows one of its example commands here, picked once per
    /// session, and drops it as soon as the user has sent a message.
    std::string placeholder() const;

    /// Records that the user sent a message.
    ///
    /// The placeholder is shown only before the first message.
    void note_submit();

    /// The hint line shown below the input.
    ///
    /// Claude Code shows `? for shortcuts` while the input is empty and
    /// `esc to interrupt` while a request is running.
    std::string hint_line(bool loading) const;

    /// The lines that make up the prompt box, including the hint line.
    ///
    /// `hint` replaces the usual hint line when it is not empty, which is
    /// how the "press again to exit" message is shown.
    ///
    /// `argument_hint` is the hint of the arguments of the command that is
    /// being typed; it is drawn dim right after the text.
    ///
    /// `status_line` is drawn between the box and the hint, the way Claude
    /// Code draws the user's own status line. `status_line_padding` is the
    /// padding of the setting of the status line, which widens its indent.
    /// `suppress_hint` says whether the usual hint line is left out, which
    /// is what happens as soon as a status line is configured or the
    /// suggestions are showing.
    ///
    /// `notification` is raised at the right of the footer row, the way
    /// Claude Code raises its notifications.
    std::vector<std::string> render(int width, bool loading, std::string_view hint = {},
                                    std::string_view status_line = {},
                                    bool suppress_hint = false,
                                    std::string_view argument_hint = {},
                                    std::string_view notification = {},
                                    int status_line_padding = 0) const;

    /// The lines that make up the prompt box, without the hint line.
    ///
    /// `argument_hint` is the hint of the arguments of the command that is
    /// being typed; it is drawn dim right after the text.
    std::vector<std::string> render_box(int width, bool loading,
                                         std::string_view argument_hint = {}) const;

    /// Says that the user is pasting.
    ///
    /// Claude Code names the paste in the footer while it is going on, and
    /// leaves the status line out for that time.
    void set_pasting(bool pasting) { pasting_ = pasting; }

    /// True while the user is pasting.
    bool pasting() const { return pasting_; }

    /// The `?` shortcuts menu, as Claude Code shows it.
    ///
    /// The menu under the prompt box is dim, the one in the help overlay is
    /// not, the way the reference draws them.
    static std::vector<std::string> shortcuts_menu(int width, bool dim = true, size_t gap = 0);

    /// Says whether the vim mode is on and which mode the prompt box is in.
    ///
    /// The marker of the insert mode is shown while the vim mode is on, the
    /// way Claude Code shows it.
    void set_vim_mode(bool enabled, bool insert);

    /// The text of the input and the place of the cursor, so that the vim
    /// mode may work on them.
    std::string& text() { return text_; }
    size_t& cursor() { return cursor_; }

private:
    Theme theme_;
    std::string cwd_;
    std::string text_;

    /// True when the vim mode is on.
    bool vim_enabled_ = false;

    /// True when the prompt box is in the insert mode of the vim mode.
    bool vim_insert_ = true;

    /// True while the user is pasting.
    bool pasting_ = false;

    size_t cursor_ = 0;

    /// The example command the placeholder names.
    std::string example_command_;

    /// How many messages the user has sent.
    size_t submit_count_ = 0;
};

} // namespace cc::ui
