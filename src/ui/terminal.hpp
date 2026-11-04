#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc::ui {

/// A key press, decoded from the terminal's input stream.
struct Key {
    enum class Type {
        None,
        Character,  ///< A printable character (including UTF-8 text).
        Enter,
        Backspace,
        Delete,
        Tab,
        ShiftTab,
        Escape,
        Up,
        Down,
        ShiftUp,   ///< Shift+Up.
        ShiftDown, ///< Shift+Down.
        Left,
        Right,
        Home,
        End,
        PageUp,
        PageDown,
        CtrlC,
        CtrlD,
        CtrlA,
        CtrlE,
        CtrlK,
        CtrlU,
        CtrlW,
        CtrlL,
        CtrlR,
        CtrlO,
        CtrlN, ///< Ctrl+N: the next entry of the list of the commands.
        CtrlP, ///< Ctrl+P: the entry before the one that is chosen.
        CtrlG,
        CtrlF,
        CtrlB,
        CtrlD_Alt,
        CtrlZ,
        CtrlY,
        CtrlX,
        CtrlT,
        CtrlS,
        CtrlC_Alt,
        F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        Paste,       ///< A bracketed-paste payload: the whole pasted block.
        MouseClick,  ///< A click of the mouse: the button was let go.
        MouseDown,   ///< The button of the mouse went down.
        MouseDrag,   ///< The mouse moved while the button was down.
        MouseScrollUp,   ///< The wheel of the mouse was turned up.
        MouseScrollDown, ///< The wheel of the mouse was turned down.
        Unknown,
    };

    Type type = Type::None;

    /// The character for `Type::Character`, or the payload for `Type::Paste`.
    ///
    /// A paste payload holds the whole block the terminal pasted, with its
    /// newlines in it.
    std::string text;

    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    /// The column of a click of the mouse, counted from one.
    int mouse_x = 0;

    /// The row of a click of the mouse, counted from one.
    int mouse_y = 0;

    bool is_printable() const { return type == Type::Character; }
};

/// One place of the screen.
struct ScreenPosition {
    /// The row of the place, counted from zero.
    int row = 0;

    /// The column of the place, counted from zero.
    int column = 0;
};

/// The terminal Claude Code draws on.
///
/// Claude Code keeps two regions on screen: the transcript, which is written
/// once and stays in the terminal scrollback, and a dynamic region at the
/// bottom (the prompt box, the spinner) that is redrawn in place.
class Terminal {
public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    static Terminal& instance();

    /// True when stdout is a terminal and the UI can be drawn.
    bool interactive() const { return interactive_; }

    /// The terminal size in columns and rows.
    int width() const;
    int height() const;

    /// Puts the terminal into raw mode and starts decoding input.
    ///
    /// Returns false when the terminal cannot be switched to raw mode, in
    /// which case the caller should fall back to line-based input.
    bool enter_raw_mode();

    /// Restores the terminal's original mode.
    ///
    /// The terminal's settings are put back right away, so the shell that
    /// takes over sees a terminal that echoes and sends signals again.
    void leave_raw_mode();

    /// Switches to the full-screen layout.
    ///
    /// The alternate screen of the terminal becomes the session's own
    /// window: the transcript is kept in memory, the whole screen is drawn
    /// again whenever something changes, and the conversation scrolls in
    /// place. This is the layout the reference calls the full-screen one.
    ///
    /// Returns false when the terminal cannot be switched, in which case the
    /// session keeps the classic layout.
    bool enter_fullscreen();

    /// Leaves the full-screen layout and gives the terminal back.
    void leave_fullscreen();

    /// True when the full-screen layout is on.
    bool fullscreen() const { return fullscreen_; }

    /// Moves the view of the transcript up or down.
    ///
    /// A positive number moves the view up. The view follows the end of the
    /// conversation again once it reaches the bottom.
    void scroll(int lines);

    /// Moves the view back to the end of the conversation.
    void scroll_to_bottom();

    /// Replaces a line of the transcript of the full-screen layout.
    ///
    /// The line is named by its index, counted from the beginning of the
    /// transcript. Nothing happens in the classic layout, where the
    /// transcript is in the scrollback of the terminal.
    void replace_transcript_line(size_t index, std::string_view line);

    /// The row a line of the transcript is on, counted from one.
    ///
    /// The row is zero when the line is not on the screen.
    int transcript_line_row(size_t index) const;

    /// The row the dynamic region starts on, counted from one.
    int dynamic_row() const { return dynamic_row_ + 1; }

    /// True when the terminal was resized since the last call.
    ///
    /// The screen has to be drawn again when it was: the region of the
    /// prompt is measured against the size of the terminal.
    bool took_resize();

    /// Draws the screen again, even when the content is the same.
    ///
    /// The screen is drawn again when the terminal was resized, the way
    /// the reference draws it again.
    void invalidate() { invalidated_ = true; }

    /// True when the view has been moved away from the end of the
    /// conversation.
    bool scrolled_up() const { return scroll_offset_ > 0; }

    /// Reads the next key press, waiting at most `timeout_ms`.
    ///
    /// Returns `std::nullopt` on timeout or end of input.
    ///
    /// A block the user pasted arrives as a single key of type `Paste` that
    /// carries the whole block, so the newlines in it are never mistaken for
    /// the Enter that commits the prompt.
    std::optional<Key> read_key(int timeout_ms);

    /// True when the input of the terminal has closed and no more keys will
    /// come.
    ///
    /// The session waits for a key that will never arrive when the terminal
    /// is gone, so the loops that wait for one ask this before they wait
    /// again.
    bool closed() const;

    /// Writes text to the transcript region. The text is committed to the
    /// terminal scrollback and is never redrawn.
    void write_static(std::string_view text);

    /// Writes a line to the transcript region.
    void write_static_line(std::string_view text);

    /// Draws the answer of the model that is still growing.
    ///
    /// The answer stands right under the text that came before it, the way
    /// the reference streams it: the text grows in the place where it
    /// began. In the classic layout the answer is the first part of the
    /// dynamic region; in the full-screen layout it is a part of the
    /// transcript, which is kept in memory.
    ///
    /// The lines are drawn with the next `render_dynamic`, so that the
    /// screen is drawn only once.
    void set_live_lines(std::vector<std::string> lines);

    /// Writes the answer that grew to the transcript and takes the answer
    /// that was growing away.
    ///
    /// The answer is written and the answer that grew is taken away in the
    /// same drawing, so that the answer is never on the screen twice.
    void commit_live(std::string_view text);

    /// Redraws the dynamic region with `lines`.
    ///
    /// The previous dynamic region is erased first, so this can be called on
    /// every frame.
    ///
    /// When `pinned` is set the region is pushed down to the bottom of the
    /// screen, the way the prompt box of Claude Code sits there, and the
    /// space above it is left empty. The answer that grows stands in front
    /// of that space, so that it grows right under the text that came
    /// before it.
    void render_dynamic(const std::vector<std::string>& lines, bool pinned = false);

    /// Writes a single line to the dynamic region.
    void render_dynamic_line(std::string_view text);

    /// Commits the dynamic region to the transcript and clears the dynamic
    /// region, so the next `render_dynamic` starts from a clean slate.
    void commit_dynamic();

    /// Clears the dynamic region without committing it.
    void clear_dynamic();

    /// The number of rows a region of lines takes on the screen.
    ///
    /// A line that is wider than the terminal takes as many rows as it
    /// needs, so a region of few lines may take many rows. The region of
    /// the screen may never be taller than the screen itself, so this is
    /// how the height of a region is measured.
    int rows_of_lines(const std::vector<std::string>& lines) const;

    /// Flushes the output stream.
    void flush();

    /// Shows the cursor.
    void show_cursor();

    /// Hides the cursor.
    void hide_cursor();

private:
    /// Erases the dynamic region from the screen.
    ///
    /// The escape sequence that wipes the region is returned, so that it can
    /// be written together with what comes after it: the region does not
    /// disappear from the screen for a moment.
    std::string erase_dynamic();

    /// Draws the dynamic region again, in the place the old one was.
    void redraw_dynamic();

    /// The region, as it is written to the screen.
    ///
    /// The text of the region is returned, so that it can be written
    /// together with the text that stands above it: the screen never shows
    /// the region without the text that came before it.
    std::string region_frame();

    /// Measures the dynamic region again, after the terminal was resized.
    ///
    /// The terminal wraps the text that is on the screen again when it is
    /// resized, so the region takes a different number of rows: the rows it
    /// takes are measured again, at the width of the terminal now.
    void refit_dynamic();

    /// Wraps the lines of the transcript again, at the width of the terminal
    /// now.
    ///
    /// The transcript of the full-screen layout is kept in memory, so that
    /// the screen can be drawn again: the lines of it are wrapped again when
    /// the terminal is resized.
    void rebuild_transcript_rows();

    /// Writes the dynamic region to the screen.
    ///
    /// The text of the region is returned, so that it can be written
    /// together with the wipe that comes before it.
    ///
    /// The row the region starts at is remembered, so that the next redraw
    /// erases it exactly. When the region was drawn past the bottom of the
    /// screen the screen scrolled, and the region ends at the bottom row.
    std::string draw_dynamic(const std::vector<std::string>& content);

    /// The number of rows `text` takes on the screen.
    ///
    /// A line that is wider than the terminal takes as many rows as it
    /// needs.
    int rows_of(std::string_view text) const;

    /// The number of rows the answer that is growing takes on the screen.
    int live_rows() const;

    /// Adds text to the transcript of the full-screen layout.
    void append_transcript(std::string_view text);

    /// Draws the whole screen again in the full-screen layout.
    void redraw_fullscreen();

    /// Keeps the selection of the mouse up to date.
    ///
    /// The reference lets the user select text with the mouse in its
    /// full-screen layout, and puts the text that was selected in the
    /// clipboard when the button is let go.
    void handle_mouse(const Key& key);

    /// The text the mouse has selected.
    std::string selected_text() const;

    /// The text of every row of the screen of the full-screen layout.
    ///
    /// The rows are kept so that the text the user selects may be read
    /// back.
    std::vector<std::string> screen_rows_;

    /// Where the selection of the mouse began.
    std::optional<ScreenPosition> selection_start_;

    /// Where the selection of the mouse stands.
    std::optional<ScreenPosition> selection_end_;

    /// The lines of the answer of the model that is still growing.
    ///
    /// They stand right under the text that came before them, the way the
    /// reference streams the answer.
    std::vector<std::string> live_lines_;

    /// True when the answer that grows has changed since the last drawing.
    bool live_changed_ = false;

    bool interactive_ = false;
    bool raw_mode_ = false;
    int dynamic_height_ = 0;

    /// True when the full-screen layout is on.
    bool fullscreen_ = false;

    /// The lines of the transcript, in the full-screen layout.
    ///
    /// The lines are the ones that were written; they are wrapped again
    /// when the terminal is resized, so that the conversation follows the
    /// width of the terminal.
    std::vector<std::string> transcript_;

    /// The rows the lines of the transcript take on the screen.
    ///
    /// The rows are the lines of the transcript wrapped at the width of the
    /// terminal; they are what the screen is drawn from.
    std::vector<std::string> transcript_rows_;

    /// The number of rows every line of the transcript takes.
    ///
    /// The number tells where the rows of a line begin, so that a line can
    /// be replaced and its rows wrapped again.
    std::vector<size_t> transcript_line_rows_;

    /// The text of the last line of the transcript, when it has no newline
    /// yet.
    std::string transcript_tail_;

    /// How far the view has been moved up from the end of the transcript.
    int scroll_offset_ = 0;

    /// The row the cursor is on, counted from the top of the screen.
    int cursor_row_ = 0;

    /// The row the dynamic region starts on.
    int dynamic_row_ = 0;

    /// The lines the dynamic region currently holds.
    std::vector<std::string> dynamic_lines_;

    /// The lines of the dynamic region, without the empty lines that pad it
    /// to the bottom of the screen.
    std::vector<std::string> dynamic_content_;

    /// True when the dynamic region is pinned to the bottom of the screen.
    bool dynamic_pinned_ = false;

    /// True when the screen has to be drawn again, even when the content
    /// of the region did not change.
    bool invalidated_ = false;

    /// True when the region has to be put in the place the model knows, the
    /// next time it is drawn.
    ///
    /// The terminal wraps the text that is on the screen again when it is
    /// resized, so the region may stand in another place than the one the
    /// model knows.
    bool dynamic_anchored_ = false;

    // Opaque state for the input decoder; kept out of the header so the
    // FTXUI dependency stays an implementation detail.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Wraps `text` to `width` columns, preserving existing newlines and
/// indenting continuation lines by `indent` spaces.
std::vector<std::string> wrap_lines(std::string_view text, int width, int indent = 0);

} // namespace cc::ui
