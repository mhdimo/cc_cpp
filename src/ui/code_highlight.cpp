#include "ui/code_highlight.hpp"

#include "ui/code_languages.hpp"
#include "utils/string.hpp"

#include <cctype>
#include <string>
#include <unordered_map>

namespace cc::ui::code {
namespace {

// ---------------------------------------------------------------------------
// The theme of `cli-highlight`
// ---------------------------------------------------------------------------

/// A style with a colour of the palette and the dim attribute.
Style dim_style(BasicColor colour) {
    Style style = Style::fg(colour);
    style.dim = true;
    return style;
}

/// A style with the italic attribute.
Style italic_style() {
    Style style;
    style.italic = true;
    return style;
}

/// A style with the bold attribute.
Style bold_style() {
    Style style;
    style.bold = true;
    return style;
}

/// A style with the underline attribute.
Style underline_style() {
    Style style;
    style.underline = true;
    return style;
}

/// The style of a scope of highlight.js, the way the theme of `cli-highlight`
/// gives it.
///
/// The theme of `cli-highlight` is the one the reference uses; its colours
/// are the colours of the terminal's own palette, the ones `chalk` names.
const Style* theme_entry(std::string_view scope) {
    static const std::unordered_map<std::string_view, Style> theme = {
        {"keyword", Style::fg(BasicColor::Blue)},
        {"built_in", Style::fg(BasicColor::Cyan)},
        {"type", dim_style(BasicColor::Cyan)},
        {"literal", Style::fg(BasicColor::Blue)},
        {"number", Style::fg(BasicColor::Green)},
        {"regexp", Style::fg(BasicColor::Red)},
        {"string", Style::fg(BasicColor::Red)},
        {"subst", Style{}},
        {"symbol", Style{}},
        {"class", Style::fg(BasicColor::Blue)},
        {"function", Style::fg(BasicColor::Yellow)},
        {"title", Style{}},
        {"title.function", Style::fg(BasicColor::Yellow)},
        {"title.class", Style::fg(BasicColor::Blue)},
        {"params", Style{}},
        {"comment", Style::fg(BasicColor::Green)},
        {"doctag", Style::fg(BasicColor::Green)},
        {"meta", Style::fg(BasicColor::BrightBlack)},
        {"meta-keyword", Style{}},
        {"meta-string", Style{}},
        {"meta.keyword", Style{}},
        {"meta.string", Style{}},
        {"section", Style{}},
        {"tag", Style::fg(BasicColor::BrightBlack)},
        {"name", Style::fg(BasicColor::Blue)},
        {"attr", Style::fg(BasicColor::Cyan)},
        {"attribute", Style{}},
        {"variable", Style{}},
        {"bullet", Style{}},
        {"code", Style{}},
        {"emphasis", italic_style()},
        {"strong", bold_style()},
        {"link", underline_style()},
        {"quote", Style{}},
        {"addition", Style::fg(BasicColor::Green)},
        {"deletion", Style::fg(BasicColor::Red)},
    };
    const auto found = theme.find(scope);
    return found == theme.end() ? nullptr : &found->second;
}

/// The style of a scope, the way the theme of `cli-highlight` gives it.
///
/// The theme names the scopes of highlight.js; a scope the theme does not
/// name is looked up by its first word, the way the highlighter does:
/// `title.function.invoke` has the colour of `title.function`.
Style style_of(std::string_view scope) {
    for (;;) {
        const Style* found = theme_entry(scope);
        if (found != nullptr) return *found;
        const size_t dot = scope.rfind('.');
        if (dot == std::string_view::npos) return Style{};
        scope = scope.substr(0, dot);
    }
}

/// The name of a scope, the way highlight.js names it.
std::string_view scope_name(Scope scope) {
    switch (scope) {
    case Scope::Keyword:
        return "keyword";
    case Scope::BuiltIn:
        return "built_in";
    case Scope::Type:
        return "type";
    case Scope::Literal:
        return "literal";
    case Scope::Title:
        return "title";
    case Scope::Symbol:
        return "symbol";
    case Scope::Meta:
        return "meta";
    case Scope::Attr:
        return "attr";
    case Scope::Name:
        return "name";
    case Scope::Variable:
        return "variable";
    case Scope::Doctag:
        return "doctag";
    case Scope::Section:
        return "section";
    case Scope::MetaKeyword:
        return "meta-keyword";
    case Scope::MetaString:
        return "meta-string";
    case Scope::TemplateVariable:
        return "template-variable";
    case Scope::Params:
        return "params";
    case Scope::Class:
        return "class";
    case Scope::Function:
        return "function";
    case Scope::Regexp:
        return "regexp";
    case Scope::Subst:
        return "subst";
    case Scope::Number:
        return "number";
    case Scope::String:
        return "string";
    case Scope::Comment:
        return "comment";
    case Scope::Tag:
        return "tag";
    }
    return "";
}

// ---------------------------------------------------------------------------
// The words of a language
// ---------------------------------------------------------------------------

/// The scope of a word of a language.
using ScopeName = std::string_view;

/// The words of a language, and the scope of each of them.
using KeywordMap = std::unordered_map<std::string_view, ScopeName>;

/// The words of a language, per scope.
KeywordMap keywords_of(const LanguageDefinition& definition) {
    KeywordMap map;
    for (const auto& scope : definition.scopes) {
        const ScopeName name = scope_name(scope.scope);
        for (size_t i = 0; i < scope.count; ++i) {
            // A word that is in more than one list has the scope of the last
            // list, the way highlight.js compiles the lists of the words.
            map[std::string_view(scope.words[i])] = name;
        }
    }
    return map;
}

/// True when the word names a class, the way highlight.js reads it.
///
/// The languages of highlight.js name the class that follows the keyword
/// that introduces it; the keyword is one of these.
bool names_a_class(std::string_view word) {
    static const char* const kWords[] = {"class",  "struct",  "union",  "enum", "interface",
                                          "trait",  "record",  "protocol", "impl"};
    for (const char* name : kWords) {
        if (word == name) return true;
    }
    return false;
}

/// True when the word is the prefix of a string, the way highlight.js reads
/// it: `f"…"`, `r"…"`, `b"…"` and `L"…"` are one string.
bool is_string_prefix(std::string_view word) {
    if (word.empty()) return false;
    bool seen = false;
    for (const char c : word) {
        if (c == '8') continue;
        if (c != 'r' && c != 'R' && c != 'f' && c != 'F' && c != 'b' && c != 'B' && c != 'u' &&
            c != 'U' && c != 'L') {
            return false;
        }
        seen = true;
    }
    return seen;
}

// ---------------------------------------------------------------------------
// The language of a block
// ---------------------------------------------------------------------------

/// The name of the language, in lower case and without the spaces around it.
std::string language_name(std::string_view tag) {
    return cc::to_lower(cc::trim(tag));
}

/// The language in front of the attributes of the tag.
///
/// The tag of a fence may carry attributes after the language
/// (`rust,ignore`); the language is the run of characters in front of them,
/// the way the reference reads it out of the tag.
std::string_view language_prefix(std::string_view tag) {
    size_t i = 0;
    while (i < tag.size()) {
        const char c = tag[i];
        const bool word = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        if (!word && c != '.' && c != '+' && c != '#' && c != '-') break;
        ++i;
    }
    return tag.substr(0, i);
}

/// The language of the tag, or nullptr when the port does not know it.
const LanguageDefinition* language_of_tag(std::string_view tag) {
    const LanguageDefinition* definition = find(language_name(tag));
    if (definition != nullptr) return definition;
    return find(language_name(language_prefix(tag)));
}

/// The name of the language of the tag, the way the tag writes it.
///
/// The name is the one the reference hands to the highlighter: the tag
/// itself, or the part of it in front of the attributes.
std::string name_of_tag(std::string_view tag) {
    if (find(language_name(tag)) != nullptr) return language_name(tag);
    const std::string prefix = language_name(language_prefix(tag));
    if (find(prefix) != nullptr) return prefix;
    return {};
}

// ---------------------------------------------------------------------------
// The highlighter
// ---------------------------------------------------------------------------

/// True when the character can be part of a word.
bool is_word_character(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}

/// The end of the line.
size_t end_of_line(std::string_view code, size_t start) {
    const size_t newline = code.find('\n', start);
    return newline == std::string_view::npos ? code.size() : newline;
}

/// The end of the string that starts at `start`.
size_t end_of_string(std::string_view code, size_t start, char quote) {
    size_t i = start + 1;
    while (i < code.size()) {
        if (code[i] == '\\') {
            i += 2;
            continue;
        }
        if (code[i] == quote) return i + 1;
        ++i;
    }
    return code.size();
}

/// The end of the string of three quotes that starts at `start`.
size_t end_of_triple_string(std::string_view code, size_t start, char quote) {
    const std::string marker(3, quote);
    size_t i = start + 3;
    while (i < code.size()) {
        if (code[i] == '\\') {
            i += 2;
            continue;
        }
        if (code.compare(i, 3, marker) == 0) return i + 3;
        ++i;
    }
    return code.size();
}

/// The end of the number that starts at `start`.
///
/// The number is the one of the language: the base may be written down, the
/// fraction and the exponent may be there, and the letters that may follow
/// it are the ones of the suffix.
size_t end_of_number(std::string_view code, size_t start) {
    size_t i = start;
    const auto digit = [&](size_t at) {
        return at < code.size() && std::isdigit(static_cast<unsigned char>(code[at])) != 0;
    };

    if (code[i] == '0' && i + 1 < code.size() &&
        (code[i + 1] == 'x' || code[i + 1] == 'X' || code[i + 1] == 'b' || code[i + 1] == 'B' ||
         code[i + 1] == 'o' || code[i + 1] == 'O')) {
        i += 2;
        while (i < code.size() &&
               (std::isalnum(static_cast<unsigned char>(code[i])) != 0 || code[i] == '_')) {
            ++i;
        }
        return i;
    }

    while (digit(i) || (i < code.size() && code[i] == '_')) ++i;
    if (i + 1 < code.size() && code[i] == '.' && digit(i + 1)) {
        ++i;
        while (digit(i) || (i < code.size() && code[i] == '_')) ++i;
    }
    if (i < code.size() && (code[i] == 'e' || code[i] == 'E')) {
        size_t j = i + 1;
        if (j < code.size() && (code[j] == '+' || code[j] == '-')) ++j;
        if (digit(j)) {
            i = j;
            while (digit(i)) ++i;
        }
    }
    // The suffix of the number: the letters that may follow it.
    while (i < code.size() &&
           (code[i] == 'u' || code[i] == 'U' || code[i] == 'l' || code[i] == 'L' || code[i] == 'f' ||
            code[i] == 'F')) {
        ++i;
    }
    return i;
}

/// The highlighter of one language.
class Highlighter {
public:
    explicit Highlighter(const LanguageDefinition& definition)
        : definition_(definition), keywords_(keywords_of(definition)) {}

    /// The tokens of the code.
    std::vector<Token> run(std::string_view code);

private:
    /// Adds text that has no colour of its own.
    void plain(std::string_view text) { pending_.append(text); }

    /// Adds text that has a colour of its own.
    void paint(std::string_view text, const Style& style) {
        if (text.empty()) return;
        flush();
        out_.push_back(Token{std::string(text), style});
    }

    /// Adds the text that has no colour of its own, when there is any.
    void flush() {
        if (pending_.empty()) return;
        out_.push_back(Token{pending_, Style{}});
        pending_.clear();
    }

    /// The end of the comment of the line that starts at `start`, or
    /// `npos` when the line does not begin with one.
    size_t line_comment_end(std::string_view code, size_t start) const;

    /// The end of the comment that is closed by another text and that starts
    /// at `start`, or `npos` when there is none.
    size_t block_comment_end(std::string_view code, size_t start) const;

    /// The end of the string that starts at `start`, or `npos` when the
    /// character does not open one.
    size_t string_end(std::string_view code, size_t start) const;

    /// The end of the preprocessor directive that starts at `start`.
    size_t preprocessor_end(std::string_view code, size_t start);

    const LanguageDefinition& definition_;
    const KeywordMap keywords_;
    std::vector<Token> out_;
    std::string pending_;
};

size_t Highlighter::line_comment_end(std::string_view code, size_t start) const {
    switch (definition_.comments) {
    case CommentStyle::CLike:
        if (code.compare(start, 2, "//") == 0) return end_of_line(code, start);
        break;
    case CommentStyle::Hash:
        if (code[start] == '#') return end_of_line(code, start);
        break;
    case CommentStyle::Semi:
        if (code.compare(start, 2, "--") == 0) return end_of_line(code, start);
        break;
    case CommentStyle::Quote:
        if (code[start] == '\'') return end_of_line(code, start);
        break;
    case CommentStyle::Percent:
        if (code[start] == '%') return end_of_line(code, start);
        break;
    case CommentStyle::None:
    case CommentStyle::Block:
    case CommentStyle::Xml:
        break;
    }
    return std::string_view::npos;
}

size_t Highlighter::block_comment_end(std::string_view code, size_t start) const {
    // The languages of the C family close their comments with `*/`.
    const bool slash = definition_.comments == CommentStyle::CLike ||
                       definition_.comments == CommentStyle::Block ||
                       definition_.block_comment == BlockComment::Slash;
    if (slash && code.compare(start, 2, "/*") == 0) {
        const size_t close = code.find("*/", start + 2);
        return close == std::string_view::npos ? code.size() : close + 2;
    }
    if (definition_.block_comment == BlockComment::Angle && code.compare(start, 2, "<#") == 0) {
        const size_t close = code.find("#>", start + 2);
        return close == std::string_view::npos ? code.size() : close + 2;
    }
    if (definition_.comments == CommentStyle::Xml && code.compare(start, 4, "<!--") == 0) {
        const size_t close = code.find("-->", start + 4);
        return close == std::string_view::npos ? code.size() : close + 3;
    }
    return std::string_view::npos;
}

size_t Highlighter::string_end(std::string_view code, size_t start) const {
    const char quote = code[start];
    if (quote != '"' && quote != '\'' && quote != '`') return std::string_view::npos;
    // The languages that write their comments with a quote have no strings
    // of their own with that quote.
    if (quote == '\'' && definition_.comments == CommentStyle::Quote) return std::string_view::npos;
    if (quote == '`' && !definition_.backtick_strings) return std::string_view::npos;
    const std::string marker(3, quote);
    if (definition_.triple_strings && code.compare(start, 3, marker) == 0) {
        return end_of_triple_string(code, start, quote);
    }
    return end_of_string(code, start, quote);
}

size_t Highlighter::preprocessor_end(std::string_view code, size_t start) {
    // The `#` and the word that follows it.
    size_t i = start + 1;
    while (i < code.size() && (code[i] == ' ' || code[i] == '\t')) ++i;
    while (i < code.size() && (std::isalnum(static_cast<unsigned char>(code[i])) != 0 || code[i] == '_')) {
        ++i;
    }
    paint(code.substr(start, i - start), style_of("meta"));

    // The name of the file that is included keeps the colour of the text:
    // the reference reads it as another language.
    const size_t line = end_of_line(code, i);
    const std::string_view rest = code.substr(i, line - i);
    const size_t open = rest.find('<');
    if (open != std::string_view::npos) {
        const size_t close = rest.find('>', open);
        if (close != std::string_view::npos) {
            paint(rest.substr(0, open), style_of("meta"));
            plain(rest.substr(open, close + 1 - open));
            paint(rest.substr(close + 1), style_of("meta"));
            return line;
        }
    }
    paint(rest, style_of("meta"));
    return line;
}

std::vector<Token> Highlighter::run(std::string_view code) {
    size_t i = 0;
    bool at_line_start = true;
    // True when the word that came before named a class.
    bool after_class = false;

    while (i < code.size()) {
        const char ch = code[i];

        // A preprocessor directive.
        if (ch == '#' && at_line_start && definition_.comments == CommentStyle::CLike) {
            i = preprocessor_end(code, i);
            at_line_start = false;
            after_class = false;
            continue;
        }

        // A comment that runs to the end of the line.
        const size_t line_comment = line_comment_end(code, i);
        if (line_comment != std::string_view::npos) {
            paint(code.substr(i, line_comment - i), style_of("comment"));
            i = line_comment;
            after_class = false;
            continue;
        }

        // A comment that is closed by another text.
        const size_t block_comment = block_comment_end(code, i);
        if (block_comment != std::string_view::npos) {
            paint(code.substr(i, block_comment - i), style_of("comment"));
            i = block_comment;
            after_class = false;
            continue;
        }

        // A string.
        const size_t string = string_end(code, i);
        if (string != std::string_view::npos) {
            paint(code.substr(i, string - i), style_of("string"));
            i = string;
            at_line_start = false;
            after_class = false;
            continue;
        }

        // A number.
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0 ||
            (ch == '.' && i + 1 < code.size() &&
             std::isdigit(static_cast<unsigned char>(code[i + 1])) != 0)) {
            const size_t end = end_of_number(code, i);
            paint(code.substr(i, end - i), style_of("number"));
            i = end;
            at_line_start = false;
            after_class = false;
            continue;
        }

        // A word.
        if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '$') {
            size_t j = i;
            while (j < code.size() && is_word_character(code[j])) ++j;
            const std::string_view word = code.substr(i, j - i);

            const auto found = keywords_.find(word);
            if (found != keywords_.end()) {
                paint(word, style_of(found->second));
                after_class = names_a_class(word);
            } else if (is_string_prefix(word) && j < code.size() &&
                       string_end(code, j) != std::string_view::npos) {
                // The prefix of a string is a part of the string.
                const size_t end = string_end(code, j);
                paint(code.substr(i, end - i), style_of("string"));
                i = end;
                at_line_start = false;
                after_class = false;
                continue;
            } else if (after_class) {
                // The name of the class that the keyword before introduced.
                paint(word, style_of("title.class"));
                after_class = false;
            } else if (j < code.size() && code[j] == '(') {
                // The name of a function.
                paint(word, style_of("title.function"));
                after_class = false;
            } else {
                plain(word);
                after_class = false;
            }
            i = j;
            at_line_start = false;
            continue;
        }

        // Anything else.
        plain(code.substr(i, 1));
        at_line_start = ch == '\n';
        ++i;
    }

    flush();
    return std::move(out_);
}

} // namespace

const LanguageDefinition* find(std::string_view name) {
    for (size_t i = 0; i < definition_count(); ++i) {
        const LanguageDefinition& definition = definitions()[i];
        for (size_t k = 0; k < definition.alias_count; ++k) {
            if (name == definition.aliases[k]) return &definition;
        }
    }
    return nullptr;
}

std::string language_of(std::string_view tag) {
    return name_of_tag(tag);
}

bool names_a_language(std::string_view tag) {
    return find(language_name(tag)) != nullptr;
}

std::vector<Token> highlight(std::string_view code_text, std::string_view language) {
    const LanguageDefinition* definition = language_of_tag(language);
    if (definition == nullptr) {
        // The reference falls back to plain text when it does not know the
        // language.
        return {Token{std::string(code_text), Style{}}};
    }
    if (std::string_view(definition->name) == "plaintext") {
        // The plain text has no rules of its own: the highlighter leaves it
        // alone.
        return {Token{std::string(code_text), Style{}}};
    }
    Highlighter highlighter(*definition);
    return highlighter.run(code_text);
}

} // namespace cc::ui::code
