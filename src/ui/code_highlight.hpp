#pragma once

#include "ui/ansi.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cc::ui::code {

/// One piece of highlighted code.
struct Token {
    std::string text;
    Style style;
};

/// The language that the tag of a fence names.
///
/// The tag of the fence may carry attributes after the language
/// (`rust,ignore`); the language is the run of characters in front of them,
/// the way the reference reads it out of the tag. The name is the one the
/// tag writes, in lower case. A language the port does not know has no name
/// here: the code is then drawn as plain text, the way the reference draws
/// it.
std::string language_of(std::string_view tag);

/// True when the tag of a fence names a language the port knows.
///
/// A tag that carries attributes after the language (`rust,ignore`) names no
/// language, the way the reference reads it: the language is the part of the
/// tag in front of the attributes.
bool names_a_language(std::string_view tag);

/// Highlights the code of a block.
///
/// The language is the name of the fence of the block. The reference falls
/// back to plain text when it does not know the language.
///
/// The colours are the ones of the theme of `cli-highlight`, the highlighter
/// the reference uses: the colours of the terminal's own palette rather than
/// the ones of the theme of Claude Code.
std::vector<Token> highlight(std::string_view code, std::string_view language);

} // namespace cc::ui::code
