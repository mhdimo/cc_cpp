#include "ui/markdown.hpp"

#include "ui/code_highlight.hpp"

#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cc::ui {
namespace {

// ---------------------------------------------------------------------------
// Internal representation
// ---------------------------------------------------------------------------

/// How a run of text is coloured when the markdown is finally rendered.
enum class PieceKind {
    Plain,       ///< Ordinary text.
    InlineCode,  ///< A `code span`.
    Code,        ///< A line inside a fenced code block.
    Link,        ///< The label of a markdown link.
};

/// A run of text that shares a single style.
struct Piece {
    std::string text;
    Style style;
    PieceKind kind = PieceKind::Plain;
    std::string url;  ///< The target of a link piece.
};

/// The pieces that make up one output line.
using Pieces = std::vector<Piece>;

/// One output line of the rendered markdown.
using Line = Pieces;

// ---------------------------------------------------------------------------
// Line level helpers
// ---------------------------------------------------------------------------

/// True when the line holds nothing but whitespace.
bool is_blank(std::string_view line) { return cc::trim(line).empty(); }

/// The number of columns the reference keeps free around a table.
///
/// The margin covers the indentation of the message and the races a
/// terminal resize causes; without it a table flickers.
constexpr size_t kTableSafetyMargin = 4;

/// The narrowest a table column may be.
constexpr size_t kMinColumnWidth = 3;

/// The number of lines a table row may take before the table is drawn as
/// a list of key and value pairs instead.
constexpr size_t kMaxRowLines = 4;

/// Replaces every run of whitespace with a single space.
std::string collapse_whitespace(std::string_view text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

/// Repeats `unit` `count` times.
std::string repeat(std::string_view unit, size_t count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (size_t i = 0; i < count; ++i) out += unit;
    return out;
}

/// The number of leading spaces of a line.
size_t leading_spaces(std::string_view line) {
    size_t count = 0;
    while (count < line.size() && line[count] == ' ') ++count;
    return count;
}

/// A markdown heading.
struct Heading {
    int level;
    std::string text;
};

/// A fenced code block delimiter.
struct Fence {
    char ch;
    size_t length;
    std::string info;
};

/// A list item marker.
struct ListMarker {
    size_t indent = 0;      ///< Leading spaces of the marker.
    bool ordered = false;    ///< True for `1.` style markers.
    int number = 0;          ///< The number of an ordered marker.
    size_t marker_len = 0;   ///< The length of the marker text itself.
    std::string content;     ///< The text that follows the marker.
    size_t content_col = 0;  ///< The column the content starts at.
};

/// The links that the text defines, by their name.
///
/// The name of a link is written in lower case, the way the reference writes
/// it: `[Foo]` and `[foo]` are the same link.
using LinkDefinitions = std::map<std::string, std::string>;

/// True when the text is a tag of HTML.
///
/// The reference throws the tags of HTML away, the way the lexer of the
/// reference does: a tag is a name that begins with a letter, and it may
/// carry attributes behind it.
bool is_html_tag(std::string_view text) {
    size_t i = 0;
    // A tag that closes another one.
    if (i < text.size() && text[i] == '/') ++i;
    if (i >= text.size() || std::isalpha(static_cast<unsigned char>(text[i])) == 0) return false;
    ++i;
    while (i < text.size() &&
           (std::isalnum(static_cast<unsigned char>(text[i])) != 0 || text[i] == '_' ||
            text[i] == '-' || text[i] == ':')) {
        ++i;
    }

    // The attributes of the tag.
    while (i < text.size()) {
        size_t j = i;
        while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j])) != 0) ++j;
        if (j == i) break;  // The tag ends here.
        if (j >= text.size()) return false;
        if (std::isalpha(static_cast<unsigned char>(text[j])) == 0 && text[j] != '_' &&
            text[j] != ':') {
            return false;
        }
        ++j;
        while (j < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[j])) != 0 || text[j] == '_' ||
                text[j] == '.' || text[j] == '-' || text[j] == ':')) {
            ++j;
        }
        // The value of the attribute.
        if (j < text.size() && text[j] == '=') {
            ++j;
            while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j])) != 0) ++j;
            if (j < text.size() && (text[j] == '"' || text[j] == '\'')) {
                const size_t close = text.find(text[j], j + 1);
                if (close == std::string_view::npos) return false;
                j = close + 1;
            } else {
                while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j])) == 0 &&
                       text[j] != '"' && text[j] != '\'' && text[j] != '`' && text[j] != '=') {
                    ++j;
                }
            }
        }
        i = j;
    }

    // What is left of the tag is a slash and spaces.
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) ++i;
    if (i < text.size() && text[i] == '/') ++i;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) ++i;
    return i == text.size();
}

/// The names of the tags that open a block of HTML.
///
/// The reference throws a block of HTML away when it begins with one of
/// these tags, the way the lexer of the reference does.
bool opens_html_block(std::string_view line) {
    size_t i = leading_spaces(line);
    if (i > 3 || i >= line.size() || line[i] != '<') return false;
    ++i;
    if (i < line.size() && line[i] == '/') ++i;
    if (i >= line.size()) return false;
    // A comment is a block of HTML as well.
    if (line.compare(i, 3, "!--") == 0) return true;
    if (std::isalpha(static_cast<unsigned char>(line[i])) == 0) return false;

    size_t j = i;
    while (j < line.size() &&
           (std::isalnum(static_cast<unsigned char>(line[j])) != 0 || line[j] == '_' ||
            line[j] == '-')) {
        ++j;
    }
    const std::string name(line.substr(i, j - i));
    static const char* const kTags[] = {
        "address",  "article", "aside",   "base",      "basefont", "blockquote", "body",
        "caption",  "center",  "col",     "colgroup",  "dd",       "details",    "dialog",
        "dir",      "div",     "dl",      "dt",        "fieldset", "figcaption", "figure",
        "footer",   "form",    "frame",   "frameset",  "h1",       "h2",         "h3",
        "h4",       "h5",      "h6",      "head",      "header",   "hr",         "html",
        "iframe",   "legend",  "li",      "link",      "main",     "menu",       "menuitem",
        "meta",     "nav",     "noframes", "ol",       "optgroup", "option",     "p",
        "param",    "search",  "section", "summary",   "table",    "tbody",      "td",
        "tfoot",    "th",      "thead",   "title",     "tr",       "track",      "ul"};
    for (const char* tag : kTags) {
        if (name == tag) return true;
    }
    return false;
}

/// Parses an ATX heading, returning nothing when the line is not one.
std::optional<Heading> match_heading(std::string_view line) {
    size_t i = leading_spaces(line);
    if (i > 3) return std::nullopt;
    size_t start = i;
    int level = 0;
    while (i < line.size() && line[i] == '#') {
        ++level;
        ++i;
    }
    if (level == 0 || level > 6) return std::nullopt;
    if (i < line.size() && line[i] != ' ') return std::nullopt;

    std::string text(cc::trim(line.substr(i)));
    // A closing run of hashes is decoration and is dropped.
    size_t end = text.size();
    while (end > 0 && text[end - 1] == '#') --end;
    if (end < text.size()) {
        text = cc::trim(text.substr(0, end));
    }
    (void)start;
    return Heading{level, text};
}

/// Parses a code fence, returning nothing when the line is not one.
std::optional<Fence> match_fence(std::string_view line) {
    size_t i = leading_spaces(line);
    if (i > 3 || i >= line.size()) return std::nullopt;
    const char ch = line[i];
    if (ch != '`' && ch != '~') return std::nullopt;
    size_t j = i;
    while (j < line.size() && line[j] == ch) ++j;
    if (j - i < 3) return std::nullopt;
    std::string info(cc::trim(line.substr(j)));
    if (ch == '`' && info.find('`') != std::string::npos) return std::nullopt;
    return Fence{ch, j - i, info};
}

/// True when the line is a thematic break.
bool match_rule(std::string_view line) {
    const std::string text = cc::trim(line);
    if (text.size() < 3) return false;
    const char ch = text[0];
    if (ch != '-' && ch != '*' && ch != '_') return false;
    for (char c : text) {
        if (c != ch) return false;
    }
    return true;
}

/// True when the line is the underline of a heading written with one.
///
/// The reference reads a line of `=` or `-` under a line of text as a
/// heading, the way markdown does.
bool match_setext_underline(std::string_view line) {
    const std::string text = cc::trim(line);
    if (text.empty()) return false;
    const char ch = text.front();
    if (ch != '=' && ch != '-') return false;
    for (const char c : text) {
        if (c != ch) return false;
    }
    return true;
}

/// Parses a list item marker, returning nothing when the line is not one.
std::optional<ListMarker> match_list_item(std::string_view line) {
    const size_t indent = leading_spaces(line);
    if (indent >= line.size()) return std::nullopt;

    ListMarker marker;
    marker.indent = indent;
    size_t i = indent;
    if (line[i] == '-' || line[i] == '*' || line[i] == '+') {
        marker.marker_len = 1;
    } else if (std::isdigit(static_cast<unsigned char>(line[i]))) {
        size_t j = i;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j]))) ++j;
        if (j >= line.size() || (line[j] != '.' && line[j] != ')')) return std::nullopt;
        marker.ordered = true;
        marker.number = std::atoi(std::string(line.substr(i, j - i)).c_str());
        marker.marker_len = (j - i) + 1;
    } else {
        return std::nullopt;
    }

    if (i + marker.marker_len >= line.size()) return std::nullopt;
    if (line[i + marker.marker_len] != ' ') return std::nullopt;

    size_t content = i + marker.marker_len + 1;
    while (content < line.size() && line[content] == ' ') ++content;
    marker.content = std::string(line.substr(content));
    marker.content_col = content;
    return marker;
}

/// True when the line is a blockquote.
bool match_quote(std::string_view line) {
    const size_t indent = leading_spaces(line);
    if (indent > 3 || indent >= line.size()) return false;
    return line[indent] == '>';
}

/// Splits a table row into its cells.
std::vector<std::string> split_table_row(std::string_view line) {
    std::string text = cc::trim(line);
    if (!text.empty() && text.front() == '|') text.erase(text.begin());
    if (!text.empty() && text.back() == '|') text.pop_back();

    std::vector<std::string> cells;
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '|') {
            current.push_back('|');
            ++i;
            continue;
        }
        if (text[i] == '|') {
            cells.push_back(cc::trim(current));
            current.clear();
            continue;
        }
        current.push_back(text[i]);
    }
    cells.push_back(cc::trim(current));
    return cells;
}

/// True when the line is a table delimiter row such as `| --- | :-: |`.
bool is_table_delimiter(std::string_view line) {
    const std::string text = cc::trim(line);
    if (text.empty() || text.find('|') == std::string::npos) return false;
    for (const auto& cell : split_table_row(text)) {
        if (cell.empty()) return false;
        size_t i = 0;
        if (cell[i] == ':') ++i;
        size_t dashes = 0;
        while (i < cell.size() && cell[i] == '-') {
            ++dashes;
            ++i;
        }
        if (dashes == 0) return false;
        if (i < cell.size() && cell[i] == ':') ++i;
        if (i != cell.size()) return false;
    }
    return true;
}

/// True when a table begins at `i`.
///
/// A table is a line that holds a bar and a line of dashes behind it.
bool starts_table(const std::vector<std::string>& src, size_t i) {
    return i + 1 < src.size() && src[i].find('|') != std::string::npos &&
           is_table_delimiter(src[i + 1]);
}

/// True when the line starts a block of its own, ending a paragraph.
bool starts_new_block(std::string_view line) {
    return match_heading(line).has_value() || match_fence(line).has_value() || match_rule(line) ||
           match_quote(line) || match_list_item(line).has_value();
}

/// The definition of a link, e.g. `[label]: https://example.com`.
struct LinkDefinition {
    std::string label;
    std::string url;
};

/// Parses the definition of a link, returning nothing when the line is not one.
std::optional<LinkDefinition> match_link_definition(std::string_view line) {
    size_t i = leading_spaces(line);
    if (i > 3 || i >= line.size() || line[i] != '[') return std::nullopt;
    const size_t close = line.find(']', i + 1);
    if (close == std::string_view::npos || close == i + 1) return std::nullopt;
    const std::string label(line.substr(i + 1, close - i - 1));
    size_t j = close + 1;
    if (j >= line.size() || line[j] != ':') return std::nullopt;
    ++j;
    while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
    if (j >= line.size()) return std::nullopt;

    std::string url;
    if (line[j] == '<') {
        const size_t end = line.find('>', j + 1);
        if (end == std::string_view::npos) return std::nullopt;
        url = std::string(line.substr(j + 1, end - j - 1));
    } else {
        size_t end = j;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
        url = std::string(line.substr(j, end - j));
    }
    if (url.empty()) return std::nullopt;
    return LinkDefinition{label, url};
}

/// Takes the tick of a task list off the text of an item.
///
/// The reference reads the tick as the mark of the item and does not draw
/// it: `- [ ] todo` is drawn as `- todo`.
std::string strip_task_tick(std::string_view text) {
    if (text.size() < 4 || text[0] != '[' || text[2] != ']' || text[3] != ' ') {
        return std::string(text);
    }
    const char tick = text[1];
    if (tick != ' ' && tick != 'x' && tick != 'X') return std::string(text);
    size_t i = 4;
    while (i < text.size() && text[i] == ' ') ++i;
    return std::string(text.substr(i));
}

// ---------------------------------------------------------------------------
// Inline parsing
// ---------------------------------------------------------------------------

/// A markdown link that was found in the text.
struct Link {
    std::string text;
    std::string url;
    size_t end = 0;  ///< The offset just past the closing parenthesis.
};

/// Parses a `[label](url)` link starting at `pos`.
std::optional<Link> parse_link(std::string_view text, size_t pos) {
    if (pos >= text.size() || text[pos] != '[') return std::nullopt;

    size_t depth = 0;
    size_t i = pos;
    for (; i < text.size(); ++i) {
        if (text[i] == '[') ++depth;
        if (text[i] == ']') {
            --depth;
            if (depth == 0) break;
        }
    }
    if (i >= text.size()) return std::nullopt;

    const std::string label(text.substr(pos + 1, i - pos - 1));
    size_t j = i + 1;
    if (j >= text.size() || text[j] != '(') return std::nullopt;
    ++j;
    size_t paren = 0;
    size_t k = j;
    for (; k < text.size(); ++k) {
        if (text[k] == '(') ++paren;
        if (text[k] == ')') {
            if (paren == 0) break;
            --paren;
        }
    }
    if (k >= text.size()) return std::nullopt;

    std::string target(cc::trim(text.substr(j, k - j)));
    // Drop an optional title, e.g. `url "title"`.
    const size_t space = target.find(' ');
    if (space != std::string::npos) target = target.substr(0, space);
    if (target.size() >= 2 && target.front() == '<' && target.back() == '>') {
        target = target.substr(1, target.size() - 2);
    }
    return Link{label, target, k + 1};
}

/// Parses a reference to a link that was defined, starting at `pos`.
///
/// The reference resolves `[label]` and `[label][name]` into the link that
/// the text defined under that name.
std::optional<Link> parse_reference_link(std::string_view text, size_t pos,
                                         const LinkDefinitions& links) {
    if (pos >= text.size() || text[pos] != '[') return std::nullopt;

    size_t depth = 0;
    size_t i = pos;
    for (; i < text.size(); ++i) {
        if (text[i] == '[') ++depth;
        if (text[i] == ']') {
            --depth;
            if (depth == 0) break;
        }
    }
    if (i >= text.size()) return std::nullopt;

    const std::string label(text.substr(pos + 1, i - pos - 1));
    size_t j = i + 1;
    std::string name = label;
    if (j < text.size() && text[j] == '[') {
        size_t inner = 0;
        size_t k = j;
        for (; k < text.size(); ++k) {
            if (text[k] == '[') ++inner;
            if (text[k] == ']') {
                --inner;
                if (inner == 0) break;
            }
        }
        if (k >= text.size()) return std::nullopt;
        const std::string second(text.substr(j + 1, k - j - 1));
        if (!second.empty()) name = second;
        j = k + 1;
    }

    const auto found = links.find(cc::to_lower(name));
    if (found == links.end()) return std::nullopt;
    return Link{label, found->second, j};
}

/// True when `c` may open an emphasis run.
bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}/// True when `c` may be part of the name of a repository.
bool is_repo_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
}

/// The number of bytes of the character that starts at `i`.
///
/// A character that is not the start of a UTF-8 sequence is a byte of its
/// own, so that a broken text is still walked to its end.
size_t char_length(std::string_view text, size_t i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    size_t length = 1;
    if ((c & 0xE0) == 0xC0) {
        length = 2;
    } else if ((c & 0xF0) == 0xE0) {
        length = 3;
    } else if ((c & 0xF8) == 0xF0) {
        length = 4;
    }
    if (i + length > text.size()) return 1;
    // The bytes that follow must be the continuation of the character.
    for (size_t k = 1; k < length; ++k) {
        if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return 1;
    }
    return length;
}

/// The pieces of a text with the references to GitHub issues turned into
/// links.
///
/// The reference turns `owner/repo#123` into a link to the issue. The name
/// of the owner may not hold a dot, so that a host name is not mistaken for
/// one.
Pieces linkify_issue_references(std::string_view text) {
    Pieces out;

    // The reference leaves the text as it is when the terminal cannot follow
    // a hyperlink: the name of the issue is lost in the address otherwise.
    if (!supports_hyperlinks()) {
        if (!text.empty()) out.push_back(Piece{std::string(text), Style{}, PieceKind::Plain, {}});
        return out;
    }

    size_t i = 0;
    while (i < text.size()) {
        // The reference has to start at the beginning of the text or after
        // a character that is not part of a name.
        if (i > 0 && (is_word_char(text[i - 1]) || text[i - 1] == '.' || text[i - 1] == '/' ||
                      text[i - 1] == '-')) {
            const size_t length = char_length(text, i);
            out.push_back(Piece{std::string(text.substr(i, length)), Style{}, PieceKind::Plain, {}});
            i += length;
            continue;
        }

        // The name of the owner.
        size_t j = i;
        if (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) != 0)) {
            ++j;
            while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) != 0 ||
                                       text[j] == '_' || text[j] == '-')) {
                ++j;
            }
        }

        // The name of the repository.
        size_t owner_end = j;
        if (j < text.size() && text[j] == '/') {
            ++j;
            if (j < text.size() && std::isalnum(static_cast<unsigned char>(text[j])) != 0) {
                ++j;
                while (j < text.size() && is_repo_char(text[j])) ++j;
            } else {
                j = owner_end;
            }
        } else {
            j = owner_end;
        }

        // The number of the issue.
        const size_t repo_end = j;
        if (j < text.size() && text[j] == '#') {
            ++j;
            const size_t number_start = j;
            while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j])) != 0) ++j;
            if (j > number_start && (j == text.size() || !is_word_char(text[j]))) {
                const std::string repo(text.substr(i, repo_end - i));
                const std::string number(text.substr(number_start, j - number_start));
                out.push_back(Piece{repo + "#" + number, Style{}, PieceKind::Link,
                                    "https://github.com/" + repo + "/issues/" + number});
                i = j;
                continue;
            }
            j = repo_end;
        }

        // Nothing was matched: the character is kept as it is. The text is
        // walked a character at a time, so that a character of more than one
        // byte is never broken in two.
        if (j == i) {
            const size_t length = char_length(text, i);
            out.push_back(Piece{std::string(text.substr(i, length)), Style{}, PieceKind::Plain, {}});
            i += length;
            continue;
        }

        out.push_back(Piece{std::string(text.substr(i, j - i)), Style{}, PieceKind::Plain, {}});
        i = j;
    }

    return out;
}

/// True when `c` is one of the characters a backslash may escape.
///
/// The reference reads a backslash before anything else as a literal
/// backslash; only the punctuation of markdown is escaped.
bool is_escapable(char c) {
    return std::ispunct(static_cast<unsigned char>(c)) != 0;
}

/// The end of a bare address.
///
/// The address ends at a space or at a character that cannot be part of it.
/// The punctuation that ends a sentence is not part of the address either.
size_t url_end(std::string_view text, size_t start) {
    size_t i = start;
    while (i < text.size()) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '<' || c == '>') break;
        ++i;
    }
    while (i > start) {
        const char c = text[i - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '\'' ||
            c == '"') {
            --i;
            continue;
        }
        // A closing bracket belongs to the address only when it closes one
        // that the address opened.
        if (c == ')') {
            const std::string_view address = text.substr(start, i - start);
            if (std::count(address.begin(), address.end(), '(') >=
                std::count(address.begin(), address.end(), ')')) {
                --i;
                continue;
            }
        }
        break;
    }
    return i;
}

/// True when the text at `i` begins an address.
///
/// The reference takes `www.` and `ftp://` as well as `http://` and
/// `https://`, the way the rule of the lexer of the reference does.
bool starts_url(std::string_view text, size_t i) {
    static const std::string_view kSchemes[] = {"https://", "http://", "ftp://"};
    for (const std::string_view scheme : kSchemes) {
        if (text.compare(i, scheme.size(), scheme) == 0) return true;
    }
    return text.compare(i, 4, "www.") == 0;
}

/// Parses the inline markup of a piece of text.
Pieces parse_inline(std::string_view text, const LinkDefinitions& links = {}) {
    Pieces out;
    std::string literal;

    auto flush = [&] {
        if (!literal.empty()) {
            for (auto& piece : linkify_issue_references(literal)) out.push_back(std::move(piece));
            literal.clear();
        }
    };
    auto append = [&](Pieces inner) {
        for (auto& piece : inner) out.push_back(std::move(piece));
    };

    size_t i = 0;
    while (i < text.size()) {
        const char ch = text[i];

        // Backslash escapes. Only the punctuation of markdown is escaped;
        // a backslash before anything else is a literal backslash.
        if (ch == '\\' && i + 1 < text.size() && is_escapable(text[i + 1])) {
            literal.push_back(text[i + 1]);
            i += 2;
            continue;
        }

        // Code spans. The span is written with a run of backticks, and it
        // ends at a run of the same length.
        if (ch == '`') {
            size_t ticks = 0;
            while (i + ticks < text.size() && text[i + ticks] == '`') ++ticks;
            const std::string marker(ticks, '`');
            const size_t close = text.find(marker, i + ticks);
            if (close != std::string_view::npos) {
                std::string_view code = text.substr(i + ticks, close - i - ticks);
                // A space at both ends is part of the way the span is
                // written, not part of the code.
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ') {
                    code = code.substr(1, code.size() - 2);
                }
                flush();
                out.push_back(
                    Piece{std::string(code), Style{}, PieceKind::InlineCode, {}});
                i = close + ticks;
                continue;
            }
        }

        // Images render as their target.
        if (ch == '!' && i + 1 < text.size() && text[i + 1] == '[') {
            if (auto link = parse_link(text, i + 1)) {
                flush();
                out.push_back(Piece{link->url, Style{}, PieceKind::Plain, {}});
                i = link->end;
                continue;
            }
        }

        // Links.
        if (ch == '[') {
            if (auto link = parse_link(text, i)) {
                flush();
                // The reference shows the address of a mail link as plain
                // text rather than as a link.
                if (link->url.rfind("mailto:", 0) == 0) {
                    out.push_back(Piece{link->url.substr(7), Style{}, PieceKind::Plain, {}});
                    i = link->end;
                    continue;
                }
                // A link without a label shows its address.
                if (link->text.empty()) {
                    out.push_back(Piece{link->url, Style{}, PieceKind::Link, link->url});
                    i = link->end;
                    continue;
                }
                Pieces inner = parse_inline(link->text, links);
                for (auto& piece : inner) {
                    piece.kind = PieceKind::Link;
                    piece.url = link->url;
                }
                append(std::move(inner));
                i = link->end;
                continue;
            }
            // A reference to a link that the text defined.
            if (auto link = parse_reference_link(text, i, links)) {
                flush();
                if (link->text.empty()) {
                    out.push_back(Piece{link->url, Style{}, PieceKind::Link, link->url});
                } else {
                    Pieces inner = parse_inline(link->text, links);
                    for (auto& piece : inner) {
                        piece.kind = PieceKind::Link;
                        piece.url = link->url;
                    }
                    append(std::move(inner));
                }
                i = link->end;
                continue;
            }
        }

        // Addresses written between angle brackets.
        if (ch == '<') {
            const size_t close = text.find('>', i + 1);
            if (close != std::string_view::npos) {
                const std::string_view inner = text.substr(i + 1, close - i - 1);
                // A tag of HTML is thrown away, the way the reference throws
                // it away.
                if (is_html_tag(inner)) {
                    flush();
                    i = close + 1;
                    continue;
                }
                const bool address = inner.find(' ') == std::string_view::npos &&
                                     inner.find('\n') == std::string_view::npos &&
                                     !inner.empty();
                if (address && inner.find(':') != std::string_view::npos) {
                    flush();
                    const std::string url(inner);
                    if (url.rfind("mailto:", 0) == 0) {
                        out.push_back(Piece{url.substr(7), Style{}, PieceKind::Plain, {}});
                    } else {
                        out.push_back(Piece{url, Style{}, PieceKind::Link, url});
                    }
                    i = close + 1;
                    continue;
                }
                if (address && inner.find('@') != std::string_view::npos) {
                    flush();
                    out.push_back(Piece{std::string(inner), Style{}, PieceKind::Plain, {}});
                    i = close + 1;
                    continue;
                }
            }
        }

        // A bare address.
        if ((ch == 'h' || ch == 'H' || ch == 'w' || ch == 'W' || ch == 'f' || ch == 'F') &&
            (i == 0 || !is_word_char(text[i - 1])) && starts_url(text, i)) {
            const size_t end = url_end(text, i);
            if (end > i + 8) {
                flush();
                const std::string url(text.substr(i, end - i));
                // The address of a link that begins with "www." is written
                // with the scheme in front of it, the way the reference writes
                // it.
                const std::string href = url.rfind("www.", 0) == 0 ? "http://" + url : url;
                out.push_back(Piece{url, Style{}, PieceKind::Link, href});
                i = end;
                continue;
            }
        }

        // Strong and italic emphasis together.
        if ((ch == '*' || ch == '_') && i + 2 < text.size() && text[i + 1] == ch &&
            text[i + 2] == ch) {
            const std::string marker(3, ch);
            const size_t close = text.find(marker, i + 3);
            const bool word_before = ch == '_' && i > 0 && is_word_char(text[i - 1]);
            if (close != std::string_view::npos && !word_before && close > i + 3) {
                flush();
                Pieces inner = parse_inline(text.substr(i + 3, close - i - 3), links);
                for (auto& piece : inner) {
                    piece.style.bold = true;
                    piece.style.italic = true;
                }
                append(std::move(inner));
                i = close + 3;
                continue;
            }
        }

        // Strong emphasis.
        if ((ch == '*' || ch == '_') && i + 1 < text.size() && text[i + 1] == ch) {
            const std::string marker(2, ch);
            const size_t close = text.find(marker, i + 2);
            const bool word_before = ch == '_' && i > 0 && is_word_char(text[i - 1]);
            if (close != std::string_view::npos && !word_before && close > i + 2) {
                flush();
                Pieces inner = parse_inline(text.substr(i + 2, close - i - 2), links);
                for (auto& piece : inner) piece.style.bold = true;
                append(std::move(inner));
                i = close + 2;
                continue;
            }
        }

        // Emphasis.
        if (ch == '*' || ch == '_') {
            const size_t close = text.find(ch, i + 1);
            const bool word_before = ch == '_' && i > 0 && is_word_char(text[i - 1]);
            const bool word_after = ch == '_' && close + 1 < text.size() && is_word_char(text[close + 1]);
            if (close != std::string_view::npos && !word_before && !word_after && close > i + 1) {
                flush();
                Pieces inner = parse_inline(text.substr(i + 1, close - i - 1), links);
                for (auto& piece : inner) piece.style.italic = true;
                append(std::move(inner));
                i = close + 1;
                continue;
            }
        }

        literal.push_back(ch);
        ++i;
    }

    flush();
    return out;
}

// ---------------------------------------------------------------------------
// Wrapping
// ---------------------------------------------------------------------------

/// A word, a run of spaces, or a fragment of a word.
struct Atom {
    std::string text;
    bool space = false;
    Style style;
    PieceKind kind = PieceKind::Plain;
    std::string url;
};

/// Splits the pieces into atoms the wrapper can move around freely.
std::vector<Atom> to_atoms(const Pieces& pieces) {
    std::vector<Atom> atoms;
    for (const auto& piece : pieces) {
        std::string current;
        bool current_is_space = false;
        auto push = [&] {
            if (current.empty()) return;
            atoms.push_back(Atom{current, current_is_space, piece.style, piece.kind, piece.url});
            current.clear();
        };
        for (char ch : piece.text) {
            const bool is_space = (ch == ' ' || ch == '\t');
            if (!current.empty() && is_space != current_is_space) push();
            if (current.empty()) current_is_space = is_space;
            current.push_back(ch);
        }
        push();
    }
    return atoms;
}

/// Splits `text` into chunks that each fit into `width` columns.
std::vector<std::string> hard_split(std::string_view text, size_t width) {
    std::vector<std::string> chunks;
    if (width == 0) return chunks;
    std::string current;
    size_t current_width = 0;
    for (const auto& cluster : cc::graphemes(text)) {
        const size_t cluster_width = cc::display_width(cluster);
        if (current_width + cluster_width > width && !current.empty()) {
            chunks.push_back(current);
            current.clear();
            current_width = 0;
        }
        current += cluster;
        current_width += cluster_width;
    }
    if (!current.empty()) chunks.push_back(current);
    return chunks;
}

/// Wraps the atoms to `width` columns.
std::vector<std::vector<Atom>> wrap_atoms(const std::vector<Atom>& atoms, size_t width) {
    std::vector<std::vector<Atom>> lines;
    if (width == 0) width = 1;

    std::vector<Atom> current;
    size_t current_width = 0;

    auto flush = [&] {
        while (!current.empty() && current.back().space) current.pop_back();
        if (!current.empty()) lines.push_back(current);
        current.clear();
        current_width = 0;
    };

    // The word that is being looked at: the run of the atoms that are not
    // separated by a space. The reference breaks the line at a space only, so
    // the punctuation of a word stays with the word it belongs to: a line
    // never ends with a lone "?" or a lone ".".
    std::vector<Atom> word;
    size_t word_width = 0;

    auto flush_word = [&] {
        if (word.empty()) return;
        if (!current.empty() && current_width + word_width > width) flush();
        if (word_width > width) {
            // The word does not fit on a line of its own: it is cut in pieces.
            size_t room = width;
            for (const auto& atom : word) {
                const size_t atom_width = cc::display_width(atom.text);
                if (atom_width <= room) {
                    current.push_back(atom);
                    current_width += atom_width;
                    room -= atom_width;
                    continue;
                }
                for (const auto& chunk : hard_split(atom.text, room)) {
                    current.push_back(Atom{chunk, false, atom.style, atom.kind, atom.url});
                    current_width += cc::display_width(chunk);
                    flush();
                    room = width;
                }
            }
            word.clear();
            word_width = 0;
            return;
        }
        for (const auto& atom : word) {
            current.push_back(atom);
            current_width += cc::display_width(atom.text);
        }
        word.clear();
        word_width = 0;
    };

    for (const auto& atom : atoms) {
        if (atom.space) {
            flush_word();
            if (current.empty()) {
                // The indentation of a line of code is part of it, so it is
                // kept. The indentation of a paragraph is not.
                if (atom.kind == PieceKind::Code) {
                    current.push_back(atom);
                    current_width += cc::display_width(atom.text);
                }
                continue;
            }
            current.push_back(atom);
            current_width += cc::display_width(atom.text);
            continue;
        }
        word.push_back(atom);
        word_width += cc::display_width(atom.text);
    }
    flush_word();
    flush();
    return lines;
}

/// Merges neighbouring atoms that share a style back into pieces.
Pieces atoms_to_pieces(const std::vector<Atom>& atoms) {
    Pieces pieces;
    for (const auto& atom : atoms) {
        if (!pieces.empty()) {
            Piece& last = pieces.back();
            if (last.style == atom.style && last.kind == atom.kind && last.url == atom.url) {
                last.text += atom.text;
                continue;
            }
        }
        pieces.push_back(Piece{atom.text, atom.style, atom.kind, atom.url});
    }
    return pieces;
}

/// Wraps the pieces to `width` columns.
std::vector<Line> wrap_pieces(const Pieces& pieces, size_t width) {
    std::vector<Line> lines;
    for (const auto& atoms : wrap_atoms(to_atoms(pieces), width)) {
        lines.push_back(atoms_to_pieces(atoms));
    }
    return lines;
}

/// Adds `indent` spaces in front of every line.
void indent_lines(std::vector<Line>& lines, size_t indent) {
    if (indent == 0) return;
    const std::string padding(indent, ' ');
    for (auto& line : lines) {
        line.insert(line.begin(), Piece{padding, Style{}, PieceKind::Plain, {}});
    }
}

// ---------------------------------------------------------------------------
// Syntax highlighting
// ---------------------------------------------------------------------------

/// Highlights the code of a block.
///
/// The reference highlights the code with `cli-highlight`, which wraps
/// highlight.js; the words of a language come from highlight.js and the
/// colours from the theme of `cli-highlight`.
Pieces highlight_code_pieces(std::string_view code, std::string_view language) {
    Pieces out;
    for (const auto& token : code::highlight(code, language)) {
        out.push_back(Piece{token.text, token.style, PieceKind::Code, {}});
    }
    return out;
}

/// Splits the pieces of a block into the lines they are drawn as.
std::vector<Pieces> split_into_lines(const Pieces& pieces) {
    std::vector<Pieces> lines;
    Pieces line;
    for (const auto& piece : pieces) {
        size_t start = 0;
        for (;;) {
            const size_t newline = piece.text.find('\n', start);
            if (newline == std::string::npos) break;
            line.push_back(Piece{piece.text.substr(start, newline - start), piece.style,
                                 piece.kind, piece.url});
            lines.push_back(std::move(line));
            line.clear();
            start = newline + 1;
        }
        if (start < piece.text.size()) {
            line.push_back(Piece{piece.text.substr(start), piece.style, piece.kind, piece.url});
        }
    }
    lines.push_back(std::move(line));
    return lines;
}

// ---------------------------------------------------------------------------
// Block rendering
// ---------------------------------------------------------------------------

/// Renders markdown into lines of styled pieces.
class MarkdownRenderer {
public:
    MarkdownRenderer(int width, Theme theme, int terminal_width = 0)
        : width_(width > 0 ? width : 1),
          terminal_width_(terminal_width > 0 ? terminal_width : (width > 0 ? width : 1)),
          theme_(std::move(theme)) {}

    /// Renders the markdown, returning one entry per output line.
    std::vector<Line> render(std::string_view text) {
        const std::vector<std::string> lines = cc::split_lines(text);
        // The definitions of the links are read first, so that a reference
        // that stands before the definition is resolved as well.
        for (const auto& line : lines) {
            if (auto definition = match_link_definition(line)) {
                links_.emplace(cc::to_lower(definition->label), definition->url);
            }
        }
        return render_blocks(lines, 0, 0);
    }

private:
    /// Renders a sequence of blocks.
    ///
    /// The reference leaves a blank line between two blocks when the text
    /// had one, and always under a heading. A rule is not followed by a
    /// blank line: the reference writes it without a line break of its own.
    ///
    /// The prefix of the item of a list is carried in, so that the
    /// paragraphs that follow the first one are drawn as items of the list
    /// as well.
    std::vector<Line> render_blocks(const std::vector<std::string>& src, int level, size_t indent,
                                    const std::string& item_prefix = {}) {
        std::vector<Line> out;
        size_t i = 0;
        // True when the text had a blank line before the next block.
        bool blank_before = false;
        // True when the block that came before was a heading, which always
        // leaves a blank line under it.
        bool heading_before = false;
        // True when the block that came before was a table, which is drawn
        // in a panel of its own.
        bool table_before = false;
        while (i < src.size()) {
            if (is_blank(src[i])) {
                blank_before = true;
                ++i;
                continue;
            }
            const bool gap = !out.empty() && (blank_before || heading_before || table_before);
            if (gap) out.push_back(Line{});
            blank_before = false;
            heading_before = false;
            table_before = false;

            // The definition of a link is thrown away: the reference keeps
            // the link it names and draws nothing for the line.
            if (auto definition = match_link_definition(src[i])) {
                links_.emplace(cc::to_lower(definition->label), definition->url);
                ++i;
                continue;
            }

            // A block of HTML is thrown away as well.
            if (opens_html_block(src[i])) {
                while (i < src.size() && !is_blank(src[i])) ++i;
                continue;
            }

            if (match_fence(src[i])) {
                i = render_code(src, i, out);
                continue;
            }
            if (auto heading = match_heading(src[i])) {
                out.push_back(render_heading(*heading));
                // The reference leaves a blank line under a heading, whether
                // the text had one or not.
                heading_before = true;
                ++i;
                continue;
            }
            if (match_rule(src[i])) {
                out.push_back(Pieces{Piece{"---", Style{}, PieceKind::Plain, {}}});
                ++i;
                // The rule is written without a line break of its own: the
                // empty lines that follow it end its line, and the block
                // that comes next starts under it.
                continue;
            }
            if (match_quote(src[i])) {
                i = render_quote(src, i, level, indent, out);
                continue;
            }
            if (match_list_item(src[i])) {
                i = render_list(src, i, level, out);
                continue;
            }
            if (starts_table(src, i)) {
                // The reference draws a table in a panel of its own, so there
                // is always an empty line above it.
                if (!gap && !out.empty()) out.push_back(Line{});
                i = render_table(src, i, out);
                table_before = true;
                continue;
            }

            // A heading written with a line of `=` or `-` under it.
            if (i + 1 < src.size() && !is_blank(src[i]) && !starts_new_block(src[i]) &&
                match_setext_underline(src[i + 1])) {
                const int level = cc::trim(src[i + 1]).front() == '=' ? 1 : 2;
                out.push_back(render_heading(Heading{level, cc::trim(src[i])}));
                out.push_back(Line{});
                i += 2;
                continue;
            }

            i = render_paragraph(src, i, indent, out, item_prefix);
        }
        return out;
    }

    /// Renders a heading.
    Line render_heading(const Heading& heading) {
        Pieces pieces = parse_inline(heading.text, links_);
        for (auto& piece : pieces) {
            piece.style.bold = true;
            if (heading.level == 1) {
                piece.style.italic = true;
                piece.style.underline = true;
            }
        }
        Line line;
        for (auto& wrapped : wrap_pieces(pieces, static_cast<size_t>(width_))) {
            line.insert(line.end(), wrapped.begin(), wrapped.end());
        }
        return line;
    }

    /// Renders a fenced code block.
    size_t render_code(const std::vector<std::string>& src, size_t start, std::vector<Line>& out) {
        const Fence fence = *match_fence(src[start]);
        const std::string language = fence.info;
        // The indent of the fence is taken off the lines of the code, the way
        // the lexer of the reference takes it off.
        const std::string indent = src[start].substr(0, src[start].find(fence.ch));
        size_t i = start + 1;
        std::vector<std::string> code;
        while (i < src.size()) {
            const std::string trimmed = cc::trim(src[i]);
            bool closing = trimmed.size() >= fence.length;
            for (size_t k = 0; k < trimmed.size() && closing; ++k) {
                if (trimmed[k] != fence.ch) closing = false;
            }
            if (closing) {
                ++i;
                break;
            }
            code.push_back(src[i]);
            ++i;
        }

        // The whole block is highlighted at once, so that a comment or a
        // string that runs over a line is coloured as one.
        std::string text;
        for (size_t k = 0; k < code.size(); ++k) {
            if (k > 0) text += "\n";
            std::string line = cc::trim_end(code[k]);
            // The indent of the fence is taken off the line, the way the
            // lexer of the reference takes it off.
            if (!indent.empty() && fence.ch == '`') {
                const size_t at = line.find(indent);
                if (at != std::string::npos) line.erase(at, indent.size());
            }
            text += line;
        }

        for (const auto& pieces : split_into_lines(highlight_code_pieces(text, language))) {
            std::vector<Line> wrapped = wrap_pieces(pieces, static_cast<size_t>(width_));
            if (wrapped.empty()) {
                out.push_back(Pieces{Piece{std::string(), Style{}, PieceKind::Code, {}}});
                continue;
            }
            for (auto& line : wrapped) out.push_back(std::move(line));
        }
        return i;
    }

    /// Renders a paragraph.
    ///
    /// The line breaks the model wrote are kept: the reference does not
    /// reflow the text of a paragraph.
    ///
    /// The prefix of the item of a list is carried in: the paragraphs that
    /// follow the first one are drawn as items of the list as well.
    size_t render_paragraph(const std::vector<std::string>& src, size_t start, size_t indent,
                            std::vector<Line>& out, const std::string& item_prefix = {}) {
        std::vector<std::string> lines;
        size_t i = start;
        for (; i < src.size(); ++i) {
            if (is_blank(src[i])) break;
            if (i > start && (starts_new_block(src[i]) || starts_table(src, i) ||
                              opens_html_block(src[i]))) {
                break;
            }
            lines.push_back(cc::trim(src[i]));
        }

        // The width of the marker of an item of a list is kept free in front
        // of the first line of the paragraph.
        const size_t hanging = item_prefix.empty() ? 0 : cc::display_width(item_prefix);
        for (size_t k = 0; k < lines.size(); ++k) {
            const bool first = k == 0;
            const size_t room = first && hanging > 0
                                    ? (static_cast<size_t>(width_) > hanging
                                           ? static_cast<size_t>(width_) - hanging
                                           : 1)
                                    : static_cast<size_t>(width_);
            Pieces pieces = parse_inline(lines[k], links_);
            std::vector<Line> wrapped = wrap_pieces(pieces, room);
            // The prefix of the item carries the indent of the level, so the
            // paragraph is not indented a second time.
            if (hanging == 0) indent_lines(wrapped, indent);
            for (size_t w = 0; w < wrapped.size(); ++w) {
                Line line = wrapped[w];
                if (first && w == 0 && hanging > 0) {
                    line.insert(line.begin(),
                                Piece{item_prefix, Style{}, PieceKind::Plain, {}});
                }
                out.push_back(std::move(line));
            }
        }
        return i;
    }

    /// Renders a blockquote.
    size_t render_quote(const std::vector<std::string>& src, size_t start, int level, size_t indent,
                        std::vector<Line>& out) {
        std::vector<std::string> inner;
        size_t i = start;
        while (i < src.size() && match_quote(src[i])) {
            std::string_view line = src[i];
            const size_t offset = leading_spaces(line);
            line = line.substr(offset + 1);
            if (!line.empty() && line.front() == ' ') line.remove_prefix(1);
            inner.push_back(std::string(line));
            ++i;
        }

        std::vector<Line> content = render_blocks(inner, level, 0);
        for (auto& line : content) {
            if (line.empty()) {
                out.push_back(Line{});
                continue;
            }
            Line quoted;
            Style bar_style;
            bar_style.dim = true;
            if (indent > 0) {
                quoted.push_back(Piece{std::string(indent, ' '), Style{}, PieceKind::Plain, {}});
            }
            quoted.push_back(Piece{"▎", bar_style, PieceKind::Plain, {}});
            quoted.push_back(Piece{" ", Style{}, PieceKind::Plain, {}});
            for (auto& piece : line) {
                piece.style.italic = true;
                quoted.push_back(std::move(piece));
            }
            out.push_back(std::move(quoted));
        }
        return i;
    }

    /// Renders a list, starting at `start`.
    size_t render_list(const std::vector<std::string>& src, size_t start, int level, std::vector<Line>& out) {
        const ListMarker first = *match_list_item(src[start]);
        const size_t base_indent = first.indent;
        size_t i = start;
        // The number of the item in the list: the items of an ordered list
        // are numbered from the number of the first one, the way the
        // reference numbers them.
        size_t number = 0;
        while (i < src.size()) {
            if (is_blank(src[i])) {
                size_t j = i;
                while (j < src.size() && is_blank(src[j])) ++j;
                if (j < src.size()) {
                    auto marker = match_list_item(src[j]);
                    if (marker && marker->indent <= base_indent && marker->ordered == first.ordered) {
                        i = j;
                        continue;
                    }
                }
                break;
            }
            auto marker = match_list_item(src[i]);
            if (!marker || marker->indent > base_indent) break;
            if (marker->ordered != first.ordered) break;

            std::vector<std::string> content;
            content.push_back(strip_task_tick(marker->content));
            const size_t content_col = marker->content_col;
            ++i;
            while (i < src.size()) {
                if (is_blank(src[i])) {
                    size_t j = i;
                    while (j < src.size() && is_blank(src[j])) ++j;
                    if (j < src.size() && leading_spaces(src[j]) >= content_col) {
                        for (size_t k = i; k < j; ++k) content.push_back(std::string());
                        i = j;
                        continue;
                    }
                    break;
                }
                if (leading_spaces(src[i]) >= content_col) {
                    content.push_back(std::string(src[i].substr(content_col)));
                    ++i;
                    continue;
                }
                auto next = match_list_item(src[i]);
                if (next && next->indent <= base_indent) break;
                if (starts_new_block(src[i])) break;
                content.push_back(cc::trim(src[i]));
                ++i;
            }

            ListMarker item = *marker;
            if (item.ordered) {
                item.number = first.number + static_cast<int>(number);
            }
            render_list_item(content, item, level, out);
            ++number;
        }
        return i;
    }

    /// Renders a single list item.
    void render_list_item(const std::vector<std::string>& content, const ListMarker& marker, int level,
                          std::vector<Line>& out) {
        const std::string marker_text =
            marker.ordered ? ordered_marker(level + 1, marker.number) + "." : std::string("-");
        const std::string indent(static_cast<size_t>(level) * 2, ' ');
        const std::string prefix = indent + marker_text + " ";
        const size_t hanging = cc::display_width(prefix);

        // The first paragraph of the item carries the marker.
        size_t split = 0;
        while (split < content.size() && !is_blank(content[split])) {
            if (split > 0 && starts_new_block(content[split])) break;
            ++split;
        }

        // The reference keeps the line breaks the model wrote: the marker is
        // only in front of the first line and the lines that follow start at
        // the left margin.
        for (size_t k = 0; k < split; ++k) {
            const bool first = k == 0;
            const size_t room = first ? static_cast<size_t>(width_) - hanging : static_cast<size_t>(width_);
            Pieces pieces = parse_inline(cc::trim(content[k]), links_);
            std::vector<Line> wrapped = wrap_pieces(pieces, room);
            for (size_t w = 0; w < wrapped.size(); ++w) {
                Line line = wrapped[w];
                if (first && w == 0) {
                    line.insert(line.begin(), Piece{prefix, Style{}, PieceKind::Plain, {}});
                }
                out.push_back(std::move(line));
            }
        }

        // Anything after the first paragraph is a nested block. The
        // paragraphs that follow are drawn as items of the list as well, and
        // the empty line that stood before them is drawn too.
        if (split < content.size()) {
            const std::vector<std::string> rest(content.begin() + static_cast<long>(split), content.end());
            if (!rest.empty() && is_blank(rest[0])) out.push_back(Line{});
            std::vector<Line> nested = render_blocks(rest, level + 1, indent.size(), prefix);
            for (auto& line : nested) out.push_back(std::move(line));
        }
    }

    /// Renders a table.
    size_t render_table(const std::vector<std::string>& src, size_t start, std::vector<Line>& out) {
        const std::vector<std::string> header = split_table_row(src[start]);
        std::vector<std::vector<std::string>> rows;
        size_t i = start + 2;
        for (; i < src.size(); ++i) {
            if (is_blank(src[i])) break;
            if (src[i].find('|') == std::string::npos) break;
            rows.push_back(split_table_row(src[i]));
        }

        const size_t columns = header.size();
        std::vector<std::string> aligns(columns, "left");
        const std::vector<std::string> delimiters = split_table_row(src[start + 1]);
        for (size_t c = 0; c < columns && c < delimiters.size(); ++c) {
            const std::string& cell = delimiters[c];
            const bool left = !cell.empty() && cell.front() == ':';
            const bool right = !cell.empty() && cell.back() == ':';
            if (left && right) aligns[c] = "center";
            else if (right) aligns[c] = "right";
            else aligns[c] = "left";
        }

        // Compute the column widths.
        std::vector<size_t> ideal(columns, kMinColumnWidth);
        std::vector<size_t> minimum(columns, kMinColumnWidth);
        auto measure = [&](const std::vector<std::string>& cells, size_t column) {
            if (column >= cells.size()) return;
            // The width is measured on the text as it is drawn, without the
            // marks of the markdown, the way the reference measures it.
            std::string text;
            for (const auto& piece : parse_inline(cc::trim(cells[column]), links_)) {
                text += piece.text;
            }
            ideal[column] = std::max(ideal[column], cc::display_width(text));
            size_t longest_word = 0;
            std::string current;
            for (char ch : text) {
                if (ch == ' ') {
                    longest_word = std::max(longest_word, cc::display_width(current));
                    current.clear();
                } else {
                    current.push_back(ch);
                }
            }
            longest_word = std::max(longest_word, cc::display_width(current));
            minimum[column] = std::max(minimum[column], std::min(longest_word, ideal[column]));
        };
        for (size_t c = 0; c < columns; ++c) measure(header, c);
        for (const auto& row : rows) {
            for (size_t c = 0; c < columns; ++c) measure(row, c);
        }

        // The border takes one column and every column takes three more.
        // The reference measures the table against the width of the terminal,
        // and not against the width of the text the answer is drawn in.
        const size_t overhead = 1 + columns * 3;
        const size_t terminal = static_cast<size_t>(terminal_width_);
        const size_t room = terminal > overhead + kTableSafetyMargin ? terminal - overhead - kTableSafetyMargin : 0;
        const size_t available = std::max(room, columns * kMinColumnWidth);
        size_t total_ideal = 0;
        size_t total_minimum = 0;
        for (size_t c = 0; c < columns; ++c) {
            total_ideal += ideal[c];
            total_minimum += minimum[c];
        }

        std::vector<size_t> widths(columns, kMinColumnWidth);
        if (total_ideal <= available) {
            widths = ideal;
        } else if (total_minimum <= available) {
            const size_t extra = available - total_minimum;
            size_t overflow_total = 0;
            for (size_t c = 0; c < columns; ++c) overflow_total += ideal[c] - minimum[c];
            for (size_t c = 0; c < columns; ++c) {
                if (overflow_total == 0) {
                    widths[c] = minimum[c];
                } else {
                    const size_t share = (ideal[c] - minimum[c]) * extra / overflow_total;
                    widths[c] = minimum[c] + share;
                }
            }
        } else {
            for (size_t c = 0; c < columns; ++c) {
                widths[c] = std::max<size_t>(kMinColumnWidth,
                                             minimum[c] * available / std::max<size_t>(total_minimum, 1));
            }
        }

        // A table whose rows would grow too tall is drawn as a list of key
        // and value pairs instead.
        if (table_row_height(widths, header, rows) > kMaxRowLines) {
            render_table_vertical(header, rows, out);
            return i;
        }

        // The frame of the table: the border above the header, the borders
        // between the rows and the border below the last one.
        out.push_back(table_border(widths, "top"));

        // The header row.
        std::vector<Line> header_lines =
            render_table_row(header, widths, aligns, true);
        for (auto& line : header_lines) out.push_back(std::move(line));
        out.push_back(table_border(widths, "middle"));

        // The body rows.
        for (size_t r = 0; r < rows.size(); ++r) {
            std::vector<Line> row_lines = render_table_row(rows[r], widths, aligns, false);
            for (auto& line : row_lines) out.push_back(std::move(line));
            if (r + 1 < rows.size()) out.push_back(table_border(widths, "middle"));
        }
        out.push_back(table_border(widths, "bottom"));
        return i;
    }

    /// The number of lines the tallest cell of the table takes.
    size_t table_row_height(const std::vector<size_t>& widths,
                            const std::vector<std::string>& header,
                            const std::vector<std::vector<std::string>>& rows) const {
        size_t tallest = 1;
        auto measure = [&](const std::vector<std::string>& cells) {
            for (size_t c = 0; c < widths.size(); ++c) {
                const std::string text = c < cells.size() ? cc::trim(cells[c]) : std::string();
                std::vector<std::string> wrapped = cc::word_wrap(text, widths[c]);
                if (wrapped.empty()) wrapped.push_back(std::string());
                tallest = std::max(tallest, wrapped.size());
            }
        };
        measure(header);
        for (const auto& row : rows) measure(row);
        return tallest;
    }

    /// Renders a table as a list of key and value pairs.
    ///
    /// The reference falls back to this shape when the rows of a table
    /// would need too many lines to stay readable.
    void render_table_vertical(const std::vector<std::string>& header,
                               const std::vector<std::vector<std::string>>& rows,
                               std::vector<Line>& out) const {
        const size_t width = static_cast<size_t>(terminal_width_);
        const size_t separator_width = width > 1 ? std::min(width - 1, size_t{40}) : 0;
        const std::string separator = repeat("─", separator_width);

        Style label_style;
        label_style.bold = true;

        for (size_t r = 0; r < rows.size(); ++r) {
            if (r > 0) {
                out.push_back(Pieces{Piece{separator, Style{}, PieceKind::Plain, {}}});
            }
            for (size_t c = 0; c < rows[r].size(); ++c) {
                const std::string label = c < header.size() ? cc::trim(header[c]) : std::string();
                const std::string value = collapse_whitespace(cc::trim(rows[r][c]));

                // The first line leaves room for the label, the lines that
                // follow use the whole width.
                const size_t first_width = width > label.size() + 3 ? width - label.size() - 3 : 10;
                const size_t rest_width = width > 3 ? width - 3 : 10;

                std::vector<std::string> first_pass = cc::word_wrap(value, std::max(first_width, size_t{10}));
                if (first_pass.empty()) first_pass.push_back(std::string());

                std::vector<std::string> wrapped;
                wrapped.push_back(first_pass.front());
                if (first_pass.size() > 1 && rest_width > first_width) {
                    std::string rest;
                    for (size_t k = 1; k < first_pass.size(); ++k) {
                        if (!rest.empty()) rest.push_back(' ');
                        rest += cc::trim(first_pass[k]);
                    }
                    for (auto& line : cc::word_wrap(rest, rest_width)) wrapped.push_back(std::move(line));
                } else {
                    for (size_t k = 1; k < first_pass.size(); ++k) wrapped.push_back(first_pass[k]);
                }

                Line line;
                line.push_back(Piece{label + ":", label_style, PieceKind::Plain, {}});
                line.push_back(Piece{" " + wrapped.front(), Style{}, PieceKind::Plain, {}});
                out.push_back(std::move(line));

                for (size_t k = 1; k < wrapped.size(); ++k) {
                    const std::string text = cc::trim(wrapped[k]);
                    if (text.empty()) continue;
                    out.push_back(Pieces{Piece{"  " + text, Style{}, PieceKind::Plain, {}}});
                }
            }
        }
    }

    /// Renders one row of a table.
    std::vector<Line> render_table_row(const std::vector<std::string>& cells,
                                        const std::vector<size_t>& widths,
                                        const std::vector<std::string>& aligns, bool header) {
        // The cell is parsed for the inline markup, the way the reference
        // parses it.
        std::vector<std::vector<Line>> wrapped(cells.size());
        size_t height = 1;
        for (size_t c = 0; c < cells.size(); ++c) {
            const std::string text = c < cells.size() ? cc::trim(cells[c]) : std::string();
            wrapped[c] = wrap_pieces(parse_inline(text, links_), widths[c]);
            if (wrapped[c].empty()) wrapped[c].push_back(Line{});
            height = std::max(height, wrapped[c].size());
        }

        std::vector<Line> lines;
        for (size_t row = 0; row < height; ++row) {
            Line line;
            line.push_back(Piece{"│", Style{}, PieceKind::Plain, {}});
            for (size_t c = 0; c < cells.size(); ++c) {
                // A cell that is shorter than the row sits in the middle of
                // it, the way the reference centres it.
                const size_t offset = (height - wrapped[c].size()) / 2;
                Line content;
                size_t visible = 0;
                if (row >= offset && row - offset < wrapped[c].size()) {
                    content = wrapped[c][row - offset];
                    for (const auto& piece : content) visible += cc::display_width(piece.text);
                }
                const size_t padding = widths[c] > visible ? widths[c] - visible : 0;
                const std::string align = header ? "center" : aligns[c];
                size_t left = 0;
                if (align == "right") {
                    left = padding;
                } else if (align == "center") {
                    left = padding / 2;
                }
                const size_t right = padding - left;

                line.push_back(Piece{" ", Style{}, PieceKind::Plain, {}});
                if (left > 0) line.push_back(Piece{std::string(left, ' '), Style{}, PieceKind::Plain, {}});
                for (auto& piece : content) line.push_back(std::move(piece));
                if (right > 0) line.push_back(Piece{std::string(right, ' '), Style{}, PieceKind::Plain, {}});
                line.push_back(Piece{" │", Style{}, PieceKind::Plain, {}});
            }
            lines.push_back(std::move(line));
        }
        return lines;
    }

    /// Builds a horizontal table border.
    Line table_border(const std::vector<size_t>& widths, std::string_view kind) {
        const char* left = "┌";
        const char* cross = "┬";
        const char* right = "┐";
        if (kind == "middle") {
            left = "├";
            cross = "┼";
            right = "┤";
        } else if (kind == "bottom") {
            left = "└";
            cross = "┴";
            right = "┘";
        }
        Line line;
        line.push_back(Piece{left, Style{}, PieceKind::Plain, {}});
        for (size_t c = 0; c < widths.size(); ++c) {
            line.push_back(Piece{repeat("─", widths[c] + 2), Style{}, PieceKind::Plain, {}});
            line.push_back(Piece{c + 1 < widths.size() ? cross : right, Style{}, PieceKind::Plain, {}});
        }
        return line;
    }

    /// The marker of an ordered list item, e.g. `1`, `a` or `iv`.
    static std::string ordered_marker(int depth, int number) {
        switch (depth) {
            case 2:
                return number_to_letter(number);
            case 3:
                return number_to_roman(number);
            default:
                return std::to_string(number);
        }
    }

    /// Converts a number to a lower case letter, `1` becomes `a`.
    static std::string number_to_letter(int number) {
        std::string result;
        while (number > 0) {
            --number;
            result.insert(result.begin(), static_cast<char>('a' + (number % 26)));
            number /= 26;
        }
        return result;
    }

    /// Converts a number to a lower case roman numeral.
    static std::string number_to_roman(int number) {
        static const std::pair<int, std::string_view> kValues[] = {
            {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
            {50, "l"},   {40, "xl"},  {10, "x"},  {9, "ix"},   {5, "v"},  {4, "iv"},
            {1, "i"},
        };
        std::string result;
        for (const auto& [value, numeral] : kValues) {
            while (number >= value) {
                result += numeral;
                number -= value;
            }
        }
        return result;
    }

    int width_;
    /// The width of the terminal, which is wider than the text of the answer.
    ///
    /// The reference measures a table against the width of the terminal and
    /// not against the width of the text it is drawn in.
    int terminal_width_;
    /// The links that the text defined, by their name.
    LinkDefinitions links_;
    Theme theme_ = Theme::dark();
};

/// Applies the theme colours to a piece.
std::string style_piece(const Piece& piece, const Theme& theme) {
    switch (piece.kind) {
        case PieceKind::InlineCode: {
            Style style = piece.style;
            if (!style.foreground && !style.basic_foreground) style.foreground = theme.permission;
            return styled(piece.text, style);
        }
        case PieceKind::Code: {
            Style style = piece.style;
            if (!style.foreground && !style.basic_foreground) style.foreground = theme.text;
            return styled(piece.text, style);
        }
        case PieceKind::Link: {
            // The reference draws a link in the terminal's own blue, which
            // is what the ANSI code for it asks for.
            std::string label;
            if (piece.style.foreground) {
                label = styled(piece.text, piece.style);
            } else {
                label = "\x1b[34m" + std::string(piece.text) + "\x1b[0m";
            }
            return hyperlink(label, piece.url);
        }
        case PieceKind::Plain:
        default:
            return styled(piece.text, piece.style);
    }
}

/// Removes the blocks of XML the reference hides.
///
/// The reference strips the `<commit_analysis>`, `<context>`,
/// `<function_analysis>` and `<pr_analysis>` blocks before the text is
/// rendered, and trims what is left of it.
std::string strip_prompt_xml_tags(std::string_view text) {
    static const char* const kTags[] = {"commit_analysis", "context", "function_analysis",
                                         "pr_analysis"};
    std::string out(text);
    for (const char* tag : kTags) {
        const std::string open = std::string("<") + tag + ">";
        const std::string close = std::string("</") + tag + ">";
        size_t at = 0;
        while ((at = out.find(open, at)) != std::string::npos) {
            const size_t end = out.find(close, at + open.size());
            if (end == std::string::npos) break;
            size_t stop = end + close.size();
            if (stop < out.size() && out[stop] == '\n') ++stop;
            out.erase(at, stop - at);
        }
    }
    return cc::trim(out);
}

/// Puts the block that follows a rule on the rule's own line.
///
/// The reference writes the rule without a line break of its own, so the
/// block that comes after it starts on the line of the rule.
void join_rules(std::vector<Line>& lines) {
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        const Line& rule = lines[i];
        if (rule.size() != 1 || rule[0].text != "---" || rule[0].kind != PieceKind::Plain) continue;
        Line& next = lines[i + 1];
        for (auto& piece : next) lines[i].push_back(std::move(piece));
        lines.erase(lines.begin() + static_cast<long>(i) + 1);
    }
}

} // namespace

std::vector<std::string> render_markdown(std::string_view text, const Theme& theme, int width,
                                         int terminal_width, bool dim) {
    MarkdownRenderer renderer(width, theme, terminal_width);
    std::vector<Line> lines = renderer.render(strip_prompt_xml_tags(text));
    join_rules(lines);

    // The reference trims the text it renders, so that no blank line is
    // left at the beginning or at the end.
    size_t first = 0;
    while (first < lines.size() && lines[first].empty()) ++first;
    size_t last = lines.size();
    while (last > first && lines[last - 1].empty()) --last;

    std::vector<std::string> out;
    out.reserve(last - first);
    for (size_t i = first; i < last; ++i) {
        std::string rendered;
        for (const auto& piece : lines[i]) {
            // The dim colour of the theme is used for the thinking of the
            // model, the way the reference uses it.
            if (dim) {
                Piece dim_piece = piece;
                dim_piece.style.dim = true;
                rendered += style_piece(dim_piece, theme);
            } else {
                rendered += style_piece(piece, theme);
            }
        }
        out.push_back(std::move(rendered));
    }
    return out;
}

std::vector<std::string> render_assistant_text(std::string_view text, const Theme& theme,
                                                int width, bool with_marker) {
    // The marker takes two columns of its own, the way the reference gives
    // it, so the text is wrapped in what is left. The columns are kept even
    // when the marker is not drawn: the text then continues under the marker
    // of the block above it.
    constexpr int kGutter = 2;
    const auto lines = render_markdown(text, theme, std::max(1, width - kGutter), width);

    Style marker;
    marker.foreground = theme.text;

    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i == 0 && with_marker) {
            out.push_back(styled(std::string(figures::kBlackCircle), marker) + " " + lines[i]);
        } else {
            out.push_back("  " + lines[i]);
        }
    }
    return out;
}

std::vector<StyledSpan> markdown_spans(std::string_view text, int width) {
    MarkdownRenderer renderer(width, Theme::dark());
    std::vector<Line> lines = renderer.render(strip_prompt_xml_tags(text));
    join_rules(lines);

    std::vector<StyledSpan> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        for (const auto& piece : lines[i]) {
            out.push_back(StyledSpan{piece.text, piece.style});
        }
        if (i + 1 < lines.size()) out.push_back(StyledSpan{"\n", Style{}});
    }
    return out;
}

std::vector<std::string> highlight_code(std::string_view code, std::string_view info,
                                        const Theme& theme, int width) {
    (void)width;
    (void)theme;

    // The whole block is highlighted at once, so that a comment or a string
    // that runs over a line is coloured as one.
    std::vector<std::string> out;
    std::string line;
    for (const auto& token : code::highlight(code, info)) {
        size_t start = 0;
        for (;;) {
            const size_t newline = token.text.find('\n', start);
            if (newline == std::string::npos) break;
            line += styled(token.text.substr(start, newline - start), token.style);
            out.push_back(std::move(line));
            line.clear();
            start = newline + 1;
        }
        line += styled(token.text.substr(start), token.style);
    }
    if (!line.empty()) out.push_back(std::move(line));
    return out;
}

} // namespace cc::ui
