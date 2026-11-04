#pragma once

#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace cc {

/// Removes leading and trailing whitespace (including \r and \n).
std::string trim(std::string_view s);

/// Removes leading whitespace only.
std::string trim_start(std::string_view s);

/// Removes trailing whitespace only.
std::string trim_end(std::string_view s);

/// Splits on `sep`. Empty fields are preserved unless `skip_empty` is set.
std::vector<std::string> split(std::string_view s, char sep, bool skip_empty = false);

/// Splits on any character in `seps`, dropping empty fields.
std::vector<std::string> split_any(std::string_view s, std::string_view seps);

/// Splits at the first occurrence of `sep`.
///
/// The second element is empty when the separator is not found.
std::pair<std::string, std::string> split_once(std::string_view s, char sep);

/// Splits into lines, handling \n, \r\n and \r terminators. A trailing
/// terminator does not produce a final empty line.
std::vector<std::string> split_lines(std::string_view s);

/// Joins with `sep`.
std::string join(const std::vector<std::string>& parts, std::string_view sep);

/// True when `s` starts with `prefix`.
bool starts_with(std::string_view s, std::string_view prefix);

/// True when `s` ends with `suffix`.
bool ends_with(std::string_view s, std::string_view suffix);

/// True when `s` contains `needle`.
bool contains(std::string_view s, std::string_view needle);

/// Case-insensitive comparison (ASCII only).
bool iequals(std::string_view a, std::string_view b);

/// Lowercases an ASCII string.
std::string to_lower(std::string_view s);

/// Replaces every occurrence of `from` with `to`.
std::string replace_all(std::string_view s, std::string_view from, std::string_view to);

/// Truncates to `max` characters, appending an ellipsis when shortened.
std::string truncate(std::string_view s, size_t max, std::string_view ellipsis = "…");

/// Cuts a path in the middle, so that the folder and the name of the file
/// both stay visible.
///
/// The reference keeps the first and the last part of the path and puts the
/// ellipsis between them.
std::string truncate_path_middle(std::string_view path, size_t max);

/// Truncates in the middle, keeping `head` characters at the start and
/// `tail` at the end. Used for long paths.
std::string truncate_middle(std::string_view s, size_t max);

/// Writes a size of a file the way Claude Code writes it.
///
/// The wording is `412 bytes`, `1.2KB`, `4.5MB` or `1.1GB`; a whole
/// number keeps no decimal.
std::string format_file_size(size_t bytes);

/// Writes a number the way Claude Code writes it.
///
/// The wording is `900`, `1.3k`, `12.0k` or `1.0m`; a number of a
/// thousand or more always keeps its one decimal.
std::string format_number(int64_t number);

/// Wraps text to `width` columns, breaking on whitespace where possible.
/// Existing newlines start a new paragraph and are preserved.
std::vector<std::string> word_wrap(std::string_view text, size_t width);

/// Wraps a single line to `width` columns, breaking on whitespace where
/// possible and hard-breaking words that are longer than the width.
std::string wrap_line(std::string_view text, size_t width);

/// Counts the number of lines in `text`.
size_t count_lines(std::string_view text);

/// Parses a leading integer, returning `fallback` when there is none.
std::optional<int64_t> parse_int(std::string_view s);

/// Parses a boolean from the usual textual spellings ("true", "1", "yes",
/// "on" and their negatives). Returns `fallback` when unrecognised.
std::optional<bool> parse_bool(std::string_view s);

/// Escapes a string for inclusion in a JSON string literal.
std::string escape_json(std::string_view s);

/// Quotes a string as a JSON string literal, including the surrounding
/// double quotes.
std::string quote_json(std::string_view s);

/// Expands a leading `~` to the user's home directory.
std::string expand_home(std::string_view path);

/// Levenshtein distance, used for "did you mean" suggestions.
size_t edit_distance(std::string_view a, std::string_view b);

/// Returns the closest match to `needle` among `candidates` when the
/// distance is within `max_distance`, otherwise `std::nullopt`.
std::optional<std::string> closest_match(std::string_view needle,
                                         const std::vector<std::string>& candidates,
                                         size_t max_distance = 3);

/// Strips ANSI escape sequences from a string.
std::string strip_ansi(std::string_view s);

/// The display width of a string, ignoring ANSI sequences and counting
/// wide (CJK) code points as two columns.
size_t display_width(std::string_view s);

/// Splits a string into grapheme-ish clusters: a base code point plus any
/// following combining marks, variation selectors and zero-width joiners.
std::vector<std::string> graphemes(std::string_view s);

/// Decodes UTF-8 into code points. Invalid bytes become U+FFFD.
std::vector<uint32_t> utf8_codepoints(std::string_view s);

/// Encodes a code point as UTF-8.
std::string utf8_encode(uint32_t cp);

/// Number of code points in a UTF-8 string.
size_t utf8_length(std::string_view s);

} // namespace cc
