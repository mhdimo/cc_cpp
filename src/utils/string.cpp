#include "utils/string.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace cc {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

} // namespace

std::string trim_start(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && is_space(s[i])) ++i;
    return std::string(s.substr(i));
}

std::string trim_end(std::string_view s) {
    size_t n = s.size();
    while (n > 0 && is_space(s[n - 1])) --n;
    return std::string(s.substr(0, n));
}

std::string trim(std::string_view s) { return trim_end(trim_start(s)); }

std::vector<std::string> split(std::string_view s, char sep, bool skip_empty) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            std::string piece(s.substr(start));
            if (!skip_empty || !piece.empty()) out.push_back(std::move(piece));
            break;
        }
        std::string piece(s.substr(start, pos - start));
        if (!skip_empty || !piece.empty()) out.push_back(std::move(piece));
        start = pos + 1;
    }
    return out;
}

std::pair<std::string, std::string> split_once(std::string_view s, char sep) {
    const size_t at = s.find(sep);
    if (at == std::string_view::npos) return {std::string(s), {}};
    return {std::string(s.substr(0, at)), std::string(s.substr(at + 1))};
}

std::vector<std::string> split_any(std::string_view s, std::string_view seps) {
    std::vector<std::string> out;
    std::string current;
    for (char c : s) {
        if (seps.find(c) != std::string_view::npos) {
            if (!current.empty()) out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    std::string current;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\n') {
            out.push_back(current);
            current.clear();
        } else if (c == '\r') {
            out.push_back(current);
            current.clear();
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) out.push_back(current);
    return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out.append(sep);
        out += parts[i];
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(std::string_view s, std::string_view needle) {
    return s.find(needle) != std::string_view::npos;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string replace_all(std::string_view s, std::string_view from, std::string_view to) {
    if (from.empty()) return std::string(s);
    std::string out;
    size_t start = 0;
    while (true) {
        const size_t pos = s.find(from, start);
        if (pos == std::string_view::npos) {
            out.append(s.substr(start));
            break;
        }
        out.append(s.substr(start, pos - start));
        out.append(to);
        start = pos + from.size();
    }
    return out;
}

std::string truncate(std::string_view s, size_t max, std::string_view ellipsis) {
    // The width is measured in columns and not in bytes, so that the text
    // that is kept fills the room it is given.
    if (cc::display_width(s) <= max) return std::string(s);
    const size_t ellipsis_width = cc::display_width(ellipsis);
    if (max <= ellipsis_width) return std::string(ellipsis);
    const size_t room = max - ellipsis_width;

    std::string out;
    size_t columns = 0;
    for (const auto& cluster : cc::graphemes(s)) {
        const size_t width = cc::display_width(cluster);
        if (columns + width > room) break;
        out += cluster;
        columns += width;
    }
    out.append(ellipsis);
    return out;
}

/// Cuts a path in the middle, so that the folder and the name of the file
/// both stay visible.
///
/// The reference keeps the first and the last part of the path and puts the
/// ellipsis between them.
std::string truncate_path_middle(std::string_view path, size_t max) {
    if (cc::display_width(path) <= max) return std::string(path);

    const std::vector<std::string> parts = cc::split(path, '/');
    if (parts.size() == 1) return truncate(path, max);

    const std::string& first = parts.front();
    const std::string& last = parts.back();
    const size_t first_width = cc::display_width(first);
    const size_t last_width = cc::display_width(last);

    // There is no room for the last part: it is cut on its own.
    if (first.empty() && 1 + 1 + last_width >= max) {
        return "/" + truncate(last, std::max<size_t>(1, max - 1));
    }
    if (!first.empty() && 1 + 1 + 1 + last_width >= max) {
        return "…/" + truncate(last, std::max<size_t>(1, max - 2));
    }

    if (parts.size() == 2) {
        const size_t room = max > 1 + 1 + last_width ? max - 1 - 1 - last_width : 0;
        return truncate(first, room) + "…/" + last;
    }

    // The middle parts are kept as long as there is room for them.
    size_t room = max > first_width + last_width + 3 ? max - first_width - last_width - 3 : 0;
    if (room == 0) {
        const size_t first_room = max > last_width + 3 ? max - last_width - 3 : 0;
        return truncate(first, first_room) + "/…/" + last;
    }

    std::vector<std::string> middle;
    for (size_t i = parts.size() - 1; i-- > 1;) {
        const size_t width = cc::display_width(parts[i]);
        if (!parts[i].empty() && width + 1 <= room) {
            middle.insert(middle.begin(), parts[i]);
            room -= width + 1;
        } else {
            break;
        }
    }
    if (middle.empty()) return first + "/…/" + last;

    std::string out = first + "/…/";
    for (const auto& part : middle) {
        out += part + "/";
    }
    out += last;
    return out;
}

std::string truncate_middle(std::string_view s, size_t max) {
    if (s.size() <= max) return std::string(s);
    if (max < 5) return std::string(s.substr(0, max));
    const size_t head = (max - 3) / 2;
    const size_t tail = max - 3 - head;
    std::string out(s.substr(0, head));
    out += "...";
    out.append(s.substr(s.size() - tail));
    return out;
}

std::string format_file_size(size_t bytes) {
    const double kilobytes = static_cast<double>(bytes) / 1024.0;
    if (kilobytes < 1.0) return std::to_string(bytes) + " bytes";

    // The decimal is left out when it is zero, the way the reference leaves
    // it out.
    const auto one_decimal = [](double value) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(1) << value;
        std::string text = out.str();
        if (text.size() > 2 && text.compare(text.size() - 2, 2, ".0") == 0) {
            text.erase(text.size() - 2);
        }
        return text;
    };

    if (kilobytes < 1024.0) return one_decimal(kilobytes) + "KB";

    const double megabytes = kilobytes / 1024.0;
    if (megabytes < 1024.0) return one_decimal(megabytes) + "MB";

    return one_decimal(megabytes / 1024.0) + "GB";
}

std::string format_number(int64_t number) {
    if (number < 1000) return std::to_string(number);

    static const char* kUnits[] = {"k", "m", "b", "t"};
    const size_t units = sizeof(kUnits) / sizeof(kUnits[0]);

    double value = static_cast<double>(number);
    size_t divisions = 0;
    while (value >= 1000.0 && divisions + 1 < units) {
        value /= 1000.0;
        ++divisions;
    }

    // The number is shown with one decimal, and a value that rounds up to a
    // thousand is carried into the next unit, the way the compact notation
    // of the reference does: 999999 becomes "1.0m".
    if (std::round(value * 10.0) / 10.0 >= 1000.0 && divisions < units) {
        value /= 1000.0;
        ++divisions;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << value << kUnits[divisions - 1];
    return out.str();
}

std::string wrap_line(std::string_view text, size_t width) {    if (width == 0) return std::string(text);
    if (text.size() <= width) return std::string(text);

    std::string out;
    size_t line_start = 0;
    size_t last_space = std::string_view::npos;

    auto flush = [&](size_t end) {
        if (!out.empty()) out.push_back('\n');
        out.append(text.substr(line_start, end - line_start));
    };

    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ' ') last_space = i;
        // The line is full when the character would not fit in it: the
        // reference fills the row the way `wrap-ansi` does, so a line of
        // the width of the row is a line that is allowed.
        if (i - line_start + 1 > width) {
            if (last_space != std::string_view::npos && last_space > line_start) {
                flush(last_space);
                line_start = last_space + 1;
                last_space = std::string_view::npos;
            } else {
                // No break opportunity: hard-break the word.
                flush(i);
                line_start = i;
                last_space = std::string_view::npos;
            }
        }
    }
    if (line_start < text.size()) {
        if (!out.empty()) out.push_back('\n');
        out.append(text.substr(line_start));
    }
    return out;
}

std::vector<std::string> word_wrap(std::string_view text, size_t width) {
    std::vector<std::string> out;
    for (const auto& paragraph : split_lines(text)) {
        if (paragraph.size() <= width) {
            out.push_back(paragraph);
            continue;
        }
        for (auto& line : split_lines(wrap_line(paragraph, width))) {
            out.push_back(std::move(line));
        }
    }
    return out;
}

size_t count_lines(std::string_view text) {
    if (text.empty()) return 0;
    size_t lines = 1;
    for (char c : text) {
        if (c == '\n') ++lines;
    }
    // A trailing newline does not start a new line.
    if (text.back() == '\n') --lines;
    return lines;
}

std::optional<int64_t> parse_int(std::string_view s) {
    const std::string t = trim(s);
    if (t.empty()) return std::nullopt;
    size_t i = 0;
    if (t[0] == '+' || t[0] == '-') i = 1;
    if (i >= t.size()) return std::nullopt;
    for (size_t j = i; j < t.size(); ++j) {
        if (!std::isdigit(static_cast<unsigned char>(t[j]))) return std::nullopt;
    }
    return std::strtoll(t.c_str(), nullptr, 10);
}

std::optional<bool> parse_bool(std::string_view s) {
    const std::string t = to_lower(trim(s));
    if (t == "true" || t == "1" || t == "yes" || t == "on") return true;
    if (t == "false" || t == "0" || t == "no" || t == "off") return false;
    return std::nullopt;
}

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0x0F]);
                    out.push_back(hex[c & 0x0F]);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string quote_json(std::string_view s) {
    return "\"" + escape_json(s) + "\"";
}

std::string expand_home(std::string_view path) {
    if (path.empty() || path[0] != '~') return std::string(path);
    if (path.size() > 1 && path[1] != '/') return std::string(path);
    const char* home = std::getenv("HOME");
    if (!home || *home == '\0') return std::string(path);
    std::string out(home);
    out.append(path.substr(1));
    return out;
}

size_t edit_distance(std::string_view a, std::string_view b) {
    const size_t n = b.size();
    std::vector<size_t> prev(n + 1);
    std::vector<size_t> cur(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            const size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(cur);
    }
    return prev[n];
}

std::optional<std::string> closest_match(std::string_view needle,
                                         const std::vector<std::string>& candidates,
                                         size_t max_distance) {
    std::optional<std::string> best;
    size_t best_distance = max_distance + 1;
    for (const auto& candidate : candidates) {
        const size_t d = edit_distance(needle, candidate);
        if (d < best_distance) {
            best_distance = d;
            best = candidate;
        }
    }
    if (best && best_distance <= max_distance) return best;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// UTF-8 and ANSI
// ---------------------------------------------------------------------------

std::vector<uint32_t> utf8_codepoints(std::string_view s) {
    std::vector<uint32_t> out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0;
        size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            len = 4;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + len > s.size()) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        bool valid = true;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!valid) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string utf8_encode(uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

size_t utf8_length(std::string_view s) { return utf8_codepoints(s).size(); }

std::vector<std::string> graphemes(std::string_view s) {
    std::vector<std::string> out;
    const auto cps = utf8_codepoints(s);
    for (size_t i = 0; i < cps.size(); ++i) {
        std::string cluster = utf8_encode(cps[i]);
        // Absorb combining marks, variation selectors, zero-width joiners and
        // emoji modifiers into the preceding cluster.
        while (i + 1 < cps.size()) {
            const uint32_t next = cps[i + 1];
            const bool combining = (next >= 0x0300 && next <= 0x036F) ||
                                   (next >= 0x1AB0 && next <= 0x1AFF) ||
                                   (next >= 0x20D0 && next <= 0x20FF) ||
                                   (next >= 0xFE00 && next <= 0xFE0F) ||
                                   (next >= 0x1F3FB && next <= 0x1F3FF) ||
                                   next == 0x200D || next == 0xFEFF;
            if (!combining) break;
            cluster += utf8_encode(next);
            ++i;
        }
        out.push_back(std::move(cluster));
    }
    return out;
}

namespace {

/// True for East Asian Wide / Fullwidth code points, which occupy two
/// terminal columns.
bool is_wide(uint32_t cp) {
    return (cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
           (cp >= 0x2E80 && cp <= 0x303E) ||   // CJK radicals, Kangxi
           (cp >= 0x3041 && cp <= 0x33FF) ||   // Hiragana .. CJK compat
           (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK ext A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK unified
           (cp >= 0xA000 && cp <= 0xA4CF) ||   // Yi
           (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul syllables
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK compat ideographs
           (cp >= 0xFE30 && cp <= 0xFE6F) ||   // CJK compat forms
           (cp >= 0xFF00 && cp <= 0xFF60) ||   // Fullwidth forms
           (cp >= 0xFFE0 && cp <= 0xFFE6) ||
           (cp >= 0x1F300 && cp <= 0x1F64F) || // Emoji
           (cp >= 0x1F900 && cp <= 0x1F9FF) ||
           (cp >= 0x20000 && cp <= 0x3FFFD);    // CJK ext B+
}

} // namespace

std::string strip_ansi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\x1b') {
            size_t j = i + 1;
            if (j < s.size() && s[j] == '[') {
                ++j;
                while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;
                if (j < s.size()) ++j; // consume the final byte
                i = j;
                continue;
            }
            if (j < s.size() && s[j] == ']') {
                // OSC: terminated by BEL or ST (ESC \).
                ++j;
                while (j < s.size()) {
                    if (s[j] == '\x07') {
                        ++j;
                        break;
                    }
                    if (s[j] == '\x1b' && j + 1 < s.size() && s[j + 1] == '\\') {
                        j += 2;
                        break;
                    }
                    ++j;
                }
                i = j;
                continue;
            }
            // Two-character escape sequence.
            i = j < s.size() ? j + 1 : j;
            continue;
        }
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

size_t display_width(std::string_view s) {
    size_t width = 0;
    for (const auto& cluster : graphemes(strip_ansi(s))) {
        const auto cps = utf8_codepoints(cluster);
        if (cps.empty()) continue;
        const uint32_t cp = cps[0];
        if (cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F)) continue; // zero width
        width += is_wide(cp) ? 2 : 1;
    }
    return width;
}

} // namespace cc
