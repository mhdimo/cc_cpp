#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc {

/// A pattern in the grammar of JavaScript.
///
/// The patterns of the hooks are written in the grammar of JavaScript, which
/// knows more than the grammar of `std::regex`: the groups may be named, the
/// pattern may look behind, and the escapes are read the way JavaScript
/// reads them.
///
/// The pattern is searched for anywhere in the text, the way JavaScript
/// searches for it.
class JsRegex {
public:
    /// The pattern, when it is one the port understands.
    ///
    /// The failure is reported through `error`.
    static std::optional<JsRegex> compile(std::string_view pattern, std::string* error);

    /// The pattern, with the flags of JavaScript.
    ///
    /// The flags are the letters `i` (the match ignores the case), `m` (the
    /// anchors hold at the ends of the lines), `s` (the dot holds the line
    /// ends) and `u` (the escapes of the properties of the Unicode standard
    /// are read as such).
    static std::optional<JsRegex> compile(std::string_view pattern, std::string_view flags,
                                          std::string* error);

    /// True when the text holds a match of the pattern.
    bool search(std::string_view text) const;

private:
    /// One piece of a pattern.
    struct Piece;
    using PiecePtr = std::shared_ptr<Piece>;

    /// The text a group captured: where it starts and where it ends.
    using Capture = std::pair<size_t, size_t>;

    /// The text the groups have captured.
    using Captures = std::shared_ptr<const std::vector<Capture>>;

    /// The continuation of a match.
    ///
    /// It is given the position after the pieces and the text the groups
    /// have captured; the match succeeds when the continuation says so.
    using Continuation = std::function<bool(size_t, const Captures&)>;

    /// The parser of a pattern.
    struct Parser;

    /// Matches the pieces from `index` on, at `position`.
    bool match(const std::vector<PiecePtr>& pieces, size_t index, std::string_view text,
               size_t position, const Captures& captures, const Continuation& next) const;

    /// Matches a piece that is repeated.
    bool match_repeat(const Piece& piece, const std::vector<PiecePtr>& pieces, size_t index,
                      std::string_view text, size_t position, size_t count,
                      const Captures& captures, const Continuation& next) const;

    /// True when the piece of the lookaround holds at the position.
    bool lookaround_matches(const Piece& piece, std::string_view text, size_t position,
                             const Captures& captures) const;

    /// True when the character at the position is in the class of the piece.
    bool class_matches(const Piece& piece, std::string_view text, size_t position) const;

    /// True when the two characters are the same, the way the flag of the
    /// case tells them.
    bool same_character(uint32_t one, uint32_t other) const;

    /// True when the character is in the range, the way the flag of the case
    /// tells it.
    bool in_range(uint32_t code, uint32_t first, uint32_t last) const;

    /// True when the two pieces of text are the same, the way the flag of
    /// the case tells them.
    bool same_text(std::string_view one, size_t one_position, std::string_view other, size_t other_position,
                   size_t length) const;

    /// True when the position is at the end of a line.
    bool at_line_end(std::string_view text, size_t position) const;

    /// True when the position is at the start of a line.
    bool at_line_start(std::string_view text, size_t position) const;

    /// The pieces of the pattern.
    std::vector<PiecePtr> root_;

    /// The number of the groups that capture.
    int group_count_ = 0;

    /// True when the match ignores the case.
    bool case_insensitive_ = false;

    /// True when the dot holds the line ends.
    bool dot_all_ = false;

    /// True when the anchors hold at the ends of the lines.
    bool multiline_ = false;

    /// True when the escapes of the properties are read as such.
    bool unicode_ = false;
};

} // namespace cc
