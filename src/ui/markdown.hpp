#pragma once

#include "ui/ansi.hpp"
#include "ui/theme.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cc::ui {

/// A run of text with a single style.
struct StyledSpan {
    std::string text;
    Style style;
};

/// Renders markdown into the terminal lines Claude Code shows.
///
/// The renderer handles the subset of GitHub-flavoured markdown the model
/// produces: headings, bold, italic, inline code, fenced code blocks,
/// blockquotes, ordered and unordered lists, horizontal rules, links and
/// tables. Consecutive blocks are separated by one blank line, matching the
/// reference implementation's `gap={1}`.
///
/// `width` is the terminal width used to wrap paragraphs and code blocks.
///
/// `terminal_width` is the width of the terminal itself, when it is wider
/// than the text: the reference measures a table against the width of the
/// terminal and not against the width of the text the answer is drawn in.
///
/// `dim` draws the text in the dim colour of the theme: the reference shows
/// the thinking of the model that way in the transcript.
std::vector<std::string> render_markdown(std::string_view text, const Theme& theme, int width,
                                         int terminal_width = 0, bool dim = false);

/// Renders the assistant's answer the way Claude Code shows it.
///
/// The lines are indented so they line up with the text after the `⏺`
/// marker. `with_marker` puts the marker on the first line; it is only set
/// for the first piece of an answer, because the marker belongs to the
/// message as a whole.
std::vector<std::string> render_assistant_text(std::string_view text, const Theme& theme,
                                                int width, bool with_marker = true);

/// Renders markdown into styled spans, without applying the colours.
///
/// Useful when the caller wants to compose the result with other content.
std::vector<StyledSpan> markdown_spans(std::string_view text, int width);

/// Renders a block of source code with syntax highlighting.
///
/// `info` is the language, the way a markdown fence names it: `cpp`,
/// `python`, `bash`. An empty string means no highlighting at all. The
/// lines are not wrapped.
std::vector<std::string> highlight_code(std::string_view code, std::string_view info,
                                        const Theme& theme, int width);

} // namespace cc::ui
