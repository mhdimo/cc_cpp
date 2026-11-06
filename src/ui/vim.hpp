#pragma once

#include "ui/terminal.hpp"

#include <string>
#include <vector>

namespace cc::ui::vim {

/// The mode the prompt box is in.
enum class Mode {
    /// The text is written as it is typed.
    Insert,
    /// The keys are the commands of vim.
    Normal,
};

/// What the vim mode did with a key.
enum class Outcome {
    /// The key was a key of vim and was handled.
    Handled,
    /// The key was not a key of vim; the caller should handle it.
    Unhandled,
    /// The key asked for the input to be sent.
    Submit,
};

/// The command the normal mode is waiting for.
///
/// The command is a small state machine of its own: a count may be typed
/// before a motion, an operator waits for a motion, and a find waits for
/// the character that is looked for.
struct Command {
    /// The kind of the command.
    enum class Type {
        /// Nothing is being waited for.
        Idle,
        /// A count is being typed.
        Count,
        /// An operator waits for a motion.
        Operator,
        /// A count is being typed after an operator.
        OperatorCount,
        /// An operator waits for the character it looks for.
        OperatorFind,
        /// An operator waits for a text object.
        OperatorTextObject,
        /// A character is being looked for.
        Find,
        /// The `g` of a `gg` was typed.
        G,
        /// An operator waits for the `g` of a `gg`.
        OperatorG,
        /// A character is to replace the one under the cursor.
        Replace,
        /// The line is to be indented.
        Indent,
    };

    /// The kind of the command.
    Type type = Type::Idle;

    /// The operator: `d`, `c` or `y`.
    char op = 0;

    /// The count of the command.
    int count = 1;

    /// The digits of a count that is being typed.
    std::string digits;

    /// The character that is looked for: `f`, `F`, `t` or `T`.
    char find = 0;

    /// The scope of a text object: `i` or `a`.
    char scope = 0;

    /// The direction of an indent: `>` or `<`.
    char direction = 0;
};

/// The state of the vim mode.
///
/// The state holds the mode, the command that is being typed, the register
/// of the yank and the undo of the changes.
class State {
public:
    /// The mode the prompt box is in.
    Mode mode() const { return mode_; }

    /// Puts the prompt box in a mode.
    void set_mode(Mode mode);

    /// True when the vim mode is on.
    ///
    /// The mode is off until the user turns it on with `/vim`.
    bool enabled() const { return enabled_; }

    /// Turns the vim mode on or off.
    void set_enabled(bool enabled) { enabled_ = enabled; }

    /// Handles a key while the vim mode is on.
    ///
    /// The text and the cursor are changed in place; the undo of the change
    /// is remembered, so that `u` may take it back.
    Outcome handle(const Key& key, std::string& text, size_t& cursor);

private:
    /// A change that may be taken back.
    struct Change {
        std::string text;
        size_t cursor = 0;
    };

    /// Remembers the text and the cursor, so that the change may be taken
    /// back.
    void remember(const std::string& text, size_t cursor);

    /// Handles a key of the normal mode.
    Outcome handle_normal(const Key& key, std::string& text, size_t& cursor);

    /// Handles a key of the insert mode.
    Outcome handle_insert(const Key& key, std::string& text, size_t& cursor);

    /// Moves the cursor to where a motion says.
    void move(const std::string& motion, int count, std::string& text, size_t& cursor) const;

    /// Runs an operator over a motion.
    void operate(char op, const std::string& motion, int count, std::string& text, size_t& cursor);

    /// Runs an operator over the whole line.
    void operate_line(char op, int count, std::string& text, size_t& cursor);

    /// Moves the cursor to the character that is looked for.
    void move_find(char find, const std::string& character, int count, std::string& text,
                   size_t& cursor) const;

    /// Runs an operator over the character that is looked for.
    void operate_find(char find, const std::string& character, int count, std::string& text,
                      size_t& cursor);

    /// Runs an operator over a text object.
    void operate_text_object(char scope, const std::string& type, int count, std::string& text,
                             size_t& cursor);

    /// Indents the line the cursor is in, or takes the indent away.
    void indent(char direction, int count, std::string& text, size_t& cursor);

    /// Deletes the text between two places and returns it.
    std::string take(size_t from, size_t to, std::string& text, size_t& cursor);

    /// True when the mode is on.
    bool enabled_ = false;

    /// The mode the prompt box is in.
    Mode mode_ = Mode::Insert;

    /// The command that is being typed.
    Command command_;

    /// The text that was yanked last.
    std::string register_;

    /// True when the register holds whole lines.
    bool register_is_linewise_ = false;

    /// The changes that were made, so that they may be taken back.
    std::vector<Change> undo_;

    /// The changes that were taken back, so that they may be made again.
    std::vector<Change> redo_;
};

} // namespace cc::ui::vim
