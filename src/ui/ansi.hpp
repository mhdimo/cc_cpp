#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cc::ui {

/// One of the terminal's basic colours.
///
/// The highlighter of the reference uses the colours of the terminal's own
/// palette rather than the ones of the theme, the way `cli-highlight` does.
enum class BasicColor : uint8_t {
    Black = 0,
    Red = 1,
    Green = 2,
    Yellow = 3,
    Blue = 4,
    Magenta = 5,
    Cyan = 6,
    White = 7,
    BrightBlack = 8,
    BrightRed = 9,
    BrightGreen = 10,
    BrightYellow = 11,
    BrightBlue = 12,
    BrightMagenta = 13,
    BrightCyan = 14,
    BrightWhite = 15,
};

/// A 24-bit colour.
struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    /// The colour of the terminal's own palette this one stands for.
    ///
    /// The `ansi` themes of the reference name their colours after the
    /// palette of the terminal, not after a twenty-four bit colour; the
    /// colour of the palette is the one that is drawn.
    std::optional<BasicColor> basic;

    constexpr Rgb() = default;
    constexpr Rgb(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}

    friend constexpr bool operator==(const Rgb&, const Rgb&) = default;

    /// Parses `rgb(r,g,b)` / `#rrggbb`, returning nullopt when unparsable.
    static bool parse(std::string_view text, Rgb& out);
};

/// Text attributes, mirroring the subset of SGR that Claude Code uses.
struct Style {
    std::optional<Rgb> foreground;
    std::optional<Rgb> background;

    /// A colour of the terminal's own palette, which wins over the
    /// twenty-four bit one.
    std::optional<BasicColor> basic_foreground;

    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    bool inverse = false;

    friend bool operator==(const Style&, const Style&) = default;

    /// A style with only a foreground colour set.
    static Style fg(Rgb colour);
    /// A style with only a background colour set.
    static Style bg(Rgb colour);
    /// A style with only a basic foreground colour set.
    static Style fg(BasicColor colour);
};

/// The escape sequence that resets every attribute.
inline constexpr std::string_view kReset = "\x1b[0m";

/// Wraps `text` in the SGR sequence for `style`.
///
/// An empty style returns `text` unchanged, so the common case costs
/// nothing.
std::string styled(std::string_view text, const Style& style);

/// The SGR sequence that selects `style`, without any text.
std::string style_sequence(const Style& style);

/// Moves the cursor up by `lines` rows.
std::string cursor_up(int lines);

/// Moves the cursor down by `lines` rows.
std::string cursor_down(int lines);

/// Erases from the cursor to the end of the screen.
std::string erase_to_end_of_screen();

/// Erases the whole screen.
std::string erase_screen();

/// Moves the cursor to the given column (1-based) on the current row.
std::string cursor_to_column(int column);

/// Hides the cursor.
std::string hide_cursor();

/// Shows the cursor.
std::string show_cursor();

/// Wraps `text` in an OSC 8 hyperlink pointing at `url`.
///
/// A terminal that cannot follow a hyperlink is shown the address alone.
std::string hyperlink(std::string_view text, std::string_view url);

/// True when the terminal can follow an OSC 8 hyperlink.
///
/// The question is asked with the variables the reference asks it with: the
/// program of the terminal, the name it is known by inside `tmux` and the
/// kind of the terminal.
bool supports_hyperlinks();

/// The OSC 52 sequence that puts `text` on the clipboard.
///
/// The terminal has to be willing to accept it; Claude Code writes the
/// sequence to the standard output and the terminal takes it from there.
std::string clipboard_sequence(std::string_view text);

/// Puts `text` on the clipboard.
///
/// The OSC 52 sequence is written to the standard output, and on the
/// platforms that have a clipboard tool the text is copied with it as well,
/// the way Claude Code does.
void copy_to_clipboard(std::string_view text);

/// Removes every ANSI escape sequence from `text`.
std::string strip(std::string_view text);

} // namespace cc::ui
