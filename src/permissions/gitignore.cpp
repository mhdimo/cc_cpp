#include "permissions/gitignore.hpp"

#include "utils/string.hpp"

#include <regex>

namespace cc::permissions {
namespace {

/// The outcome of matching one pattern against a path.
enum class PatternMatch {
    /// The pattern does not match the path.
    None,
    /// The pattern matches the path.
    Match,
    /// The pattern matches the path and takes it back out of the match.
    Negated,
};

/// Escapes the characters that mean something in a regular expression.
std::string escape_regex(std::string_view text) {
    static constexpr std::string_view kSpecial = R"(\.^$|?*+()[]{})";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (kSpecial.find(c) != std::string_view::npos) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

/// The regular expression of one segment of a pattern.
///
/// A `*` matches any text but the separator of the folders, a `?` matches
/// one character, and a class of characters is read as such.
std::string translate_segment(std::string_view segment) {
    std::string out;
    out.reserve(segment.size());

    for (size_t i = 0; i < segment.size(); ++i) {
        const char c = segment[i];

        if (c == '*') {
            out += "[^/]*";
            continue;
        }
        if (c == '?') {
            out += "[^/]";
            continue;
        }
        if (c == '[') {
            // A class of characters: `[abc]`, `[a-z]` or `[!abc]`.
            const size_t close = segment.find(']', i + 1);
            if (close == std::string_view::npos) {
                out += "\\[";
                continue;
            }
            std::string_view body = segment.substr(i + 1, close - i - 1);
            std::string translated = "[";
            if (!body.empty() && (body.front() == '!' || body.front() == '^')) {
                translated += '^';
                body.remove_prefix(1);
            }
            for (const char b : body) {
                if (b == '\\' || b == '[') translated.push_back('\\');
                translated.push_back(b);
            }
            translated += ']';
            out += translated;
            i = close;
            continue;
        }

        out += escape_regex(std::string_view(&c, 1));
    }

    return out;
}

/// The regular expression of a pattern, relative to the root.
///
/// The pattern is split into the segments of the path; a `**` stands for any
/// number of folders.
std::string pattern_to_regex(std::string_view pattern, bool anchored) {
    const std::vector<std::string> segments = cc::split(pattern, '/', true);

    std::string regex;
    if (!anchored) {
        // A pattern that holds no separator matches at every level.
        regex += "(?:.*/)?";
    }

    bool first = true;
    bool separator = false;
    for (size_t i = 0; i < segments.size(); ++i) {
        const std::string& segment = segments[i];
        const bool last = i + 1 == segments.size();

        if (segment == "**") {
            if (!first) regex += '/';
            // "**/" stands for any number of folders, "**" for everything
            // that is left.
            regex += last ? ".*" : "(?:[^/]+/)?";
            first = false;
            // The fragment carries its own separator.
            separator = false;
            continue;
        }

        if (!first && separator) regex += '/';
        regex += translate_segment(segment);
        first = false;
        separator = true;
    }

    return regex;
}

PatternMatch match_pattern(std::string_view pattern, std::string_view path) {
    // The spaces at the end of a pattern are of no account.
    while (!pattern.empty() && (pattern.back() == ' ' || pattern.back() == '\t')) {
        pattern.remove_suffix(1);
    }
    if (pattern.empty()) return PatternMatch::None;

    PatternMatch outcome = PatternMatch::Match;
    if (pattern.front() == '!') {
        outcome = PatternMatch::Negated;
        pattern.remove_prefix(1);
    }
    if (pattern.empty()) return PatternMatch::None;

    // A pattern that ends with a separator names a folder.
    const bool folder_only = pattern.back() == '/';
    if (folder_only) pattern.remove_suffix(1);

    // A pattern that begins with a separator is fixed to the root, and so is
    // a pattern that holds one.
    bool anchored = false;
    if (!pattern.empty() && pattern.front() == '/') {
        anchored = true;
        pattern.remove_prefix(1);
    }
    if (pattern.find('/') != std::string_view::npos) anchored = true;
    if (pattern.empty()) return PatternMatch::None;

    const std::string regex = pattern_to_regex(pattern, anchored);

    try {
        const std::regex matcher("^" + regex + "$");
        const std::string target(path);

        if (!folder_only && std::regex_match(target, matcher)) return outcome;

        // A pattern that names a folder matches everything inside it.
        for (size_t i = 0; i < target.size(); ++i) {
            if (target[i] != '/') continue;
            if (std::regex_match(target.substr(0, i), matcher)) return outcome;
        }
    } catch (const std::regex_error&) {
        return PatternMatch::None;
    }

    return PatternMatch::None;
}

} // namespace

bool gitignore_matches(std::string_view pattern, std::string_view path) {
    return match_pattern(pattern, path) == PatternMatch::Match;
}

std::optional<std::string> gitignore_match(const std::vector<std::string>& patterns,
                                            std::string_view path) {
    bool ignored = false;
    std::string matched;

    for (const std::string& pattern : patterns) {
        switch (match_pattern(pattern, path)) {
        case PatternMatch::None:
            break;
        case PatternMatch::Match:
            ignored = true;
            matched = pattern;
            break;
        case PatternMatch::Negated:
            ignored = false;
            matched.clear();
            break;
        }
    }

    if (!ignored) return std::nullopt;
    return matched;
}

} // namespace cc::permissions
