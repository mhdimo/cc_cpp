#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace cc::ui::code {

/// The scope of a token, the way highlight.js names them.
enum class Scope : uint8_t {
    Keyword,          ///< A word of the language: `if`, `return`, `class`.
    BuiltIn,          ///< A built-in of the language: `print`, `len`.
    Type,             ///< A type of the language: `int`, `String`.
    Literal,          ///< A literal of the language: `true`, `null`.
    Title,            ///< The name of a class or a function.
    Symbol,           ///< A symbolic constant.
    Meta,             ///< A preprocessor directive or a decorator.
    Attr,             ///< The name of an attribute.
    Name,             ///< The name of a tag.
    Variable,         ///< The name of a variable.
    Doctag,           ///< The markup of a documentation comment.
    Section,          ///< The heading of a section of a file.
    MetaKeyword,      ///< A keyword of a preprocessor directive.
    MetaString,       ///< A string of a preprocessor directive.
    TemplateVariable, ///< A variable of a template.
    Params,           ///< The name of a parameter.
    Class,            ///< The name of a class.
    Function,         ///< The name of a function.
    Regexp,           ///< A regular expression.
    Subst,            ///< The text of a string that has its own parts.
    Number,           ///< A number.
    String,           ///< A string.
    Comment,          ///< A comment.
    Tag,              ///< The name of a tag.
};

/// How the comments of a language are written.
enum class CommentStyle : uint8_t {
    None,     ///< The language has no comments.
    CLike,    ///< `//` and `/* */`.
    Hash,     ///< `#`.
    Semi,     ///< `--`.
    Block,    ///< `/* */` only.
    Quote,    ///< `'`.
    Percent,  ///< `%`.
    Xml,      ///< `<!-- -->`.
};

/// How the block comments of a language are written.
enum class BlockComment : uint8_t {
    None,   ///< The language has no block comments.
    Slash,  ///< `/* */`.
    Angle,  ///< `<# #>`.
};

/// The words of one scope.
struct ScopeWords {
    Scope scope = Scope::Keyword;
    const char* const* words = nullptr;
    size_t count = 0;
};

/// The keywords of one language.
struct LanguageDefinition {
    /// The name of the language, the way highlight.js names it.
    const char* name = nullptr;

    /// The names the language answers to, the name itself included.
    const char* const* aliases = nullptr;
    size_t alias_count = 0;

    /// How the comments of the language are written.
    CommentStyle comments = CommentStyle::None;

    /// How the block comments of the language are written.
    BlockComment block_comment = BlockComment::None;

    /// True when a backtick opens a string.
    bool backtick_strings = false;

    /// True when a triple quote opens a string.
    bool triple_strings = false;

    /// The words of the language, per scope.
    std::vector<ScopeWords> scopes;
};

/// The languages the port knows.
const LanguageDefinition* definitions();

/// The number of languages the port knows.
size_t definition_count();

/// The language with that name, or nullptr when there is none.
///
/// The name may be an alias, the way `js` names `javascript`.
const LanguageDefinition* find(std::string_view name);

} // namespace cc::ui::code
