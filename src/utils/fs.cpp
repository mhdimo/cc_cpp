#include "utils/fs.hpp"

#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <system_error>

namespace fs = std::filesystem;

namespace cc {
namespace {

std::string errno_message(const std::error_code& ec) { return ec.message(); }

/// Matches one path segment against one pattern segment, supporting `*`,
/// `?` and character classes.
bool match_segment(std::string_view text, std::string_view pattern) {
    // Iterative wildcard match with backtracking on the last `*`.
    size_t t = 0;
    size_t p = 0;
    size_t star_p = std::string_view::npos;
    size_t star_t = 0;

    while (t < text.size()) {
        if (p < pattern.size()) {
            const char pc = pattern[p];
            if (pc == '*') {
                star_p = p++;
                star_t = t;
                continue;
            }
            if (pc == '?') {
                ++p;
                ++t;
                continue;
            }
            if (pc == '[') {
                const size_t close = pattern.find(']', p + 2);
                if (close != std::string_view::npos) {
                    std::string_view cls = pattern.substr(p + 1, close - p - 1);
                    bool negate = !cls.empty() && (cls.front() == '!' || cls.front() == '^');
                    if (negate) cls.remove_prefix(1);
                    bool matched = false;
                    for (size_t i = 0; i < cls.size(); ++i) {
                        if (i + 2 < cls.size() && cls[i + 1] == '-') {
                            if (text[t] >= cls[i] && text[t] <= cls[i + 2]) {
                                matched = true;
                                break;
                            }
                            i += 2;
                        } else if (cls[i] == text[t]) {
                            matched = true;
                            break;
                        }
                    }
                    if (matched == !negate) {
                        p = close + 1;
                        ++t;
                        continue;
                    }
                }
            } else if (pc == text[t]) {
                ++p;
                ++t;
                continue;
            }
        }
        if (star_p != std::string_view::npos) {
            p = star_p + 1;
            ++star_t;
            t = star_t;
            continue;
        }
        return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

/// Splits a pattern into segments, expanding `{a,b}` alternatives into
/// separate patterns.
std::vector<std::string> expand_braces(std::string_view pattern) {
    const size_t open = pattern.find('{');
    if (open == std::string_view::npos) return {std::string(pattern)};

    int depth = 0;
    size_t close = std::string_view::npos;
    for (size_t i = open; i < pattern.size(); ++i) {
        if (pattern[i] == '{') ++depth;
        if (pattern[i] == '}') {
            --depth;
            if (depth == 0) {
                close = i;
                break;
            }
        }
    }
    if (close == std::string_view::npos) return {std::string(pattern)};

    const std::string prefix(pattern.substr(0, open));
    const std::string suffix(pattern.substr(close + 1));

    std::vector<std::string> alternatives;
    int inner_depth = 0;
    size_t start = open + 1;
    for (size_t i = open + 1; i < close; ++i) {
        if (pattern[i] == '{') ++inner_depth;
        if (pattern[i] == '}') --inner_depth;
        if (pattern[i] == ',' && inner_depth == 0) {
            alternatives.emplace_back(pattern.substr(start, i - start));
            start = i + 1;
        }
    }
    alternatives.emplace_back(pattern.substr(start, close - start));

    std::vector<std::string> out;
    for (const auto& alt : alternatives) {
        for (const auto& expanded : expand_braces(prefix + alt + suffix)) {
            out.push_back(expanded);
        }
    }
    return out;
}

/// Matches a path against a pattern, honouring `**` as "zero or more
/// directory segments".
bool match_path(std::string_view path, std::string_view pattern) {
    const std::vector<std::string> pparts = path_components(pattern);
    const std::vector<std::string> tparts = path_components(path);

    // Recursive descent over the pattern segments.
    struct Frame {
        size_t pi;
        size_t ti;
    };
    std::vector<Frame> stack{{0, 0}};

    while (!stack.empty()) {
        const Frame f = stack.back();
        stack.pop_back();

        if (f.pi == pparts.size()) {
            if (f.ti == tparts.size()) return true;
            continue;
        }

        const std::string& seg = pparts[f.pi];
        if (seg == "**") {
            // `**` may consume zero or more path segments.
            for (size_t skip = f.ti; skip <= tparts.size(); ++skip) {
                stack.push_back({f.pi + 1, skip});
            }
            continue;
        }

        if (f.ti >= tparts.size()) continue;
        if (!match_segment(tparts[f.ti], seg)) continue;
        stack.push_back({f.pi + 1, f.ti + 1});
    }
    return false;
}

} // namespace

FileState file_state(const std::string& path) {
    FileState state;
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (ec || !fs::exists(status)) return state;
    state.exists = true;
    state.size = static_cast<int64_t>(fs::file_size(path, ec));
    if (ec) state.size = 0;
    const auto mtime = fs::last_write_time(path, ec);
    if (!ec) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            mtime.time_since_epoch())
                            .count();
        state.mtime_ms = static_cast<int64_t>(ms);
    }
    return state;
}

bool file_exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

bool is_file(const std::string& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec;
}

bool is_directory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec;
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return contents;
}

std::optional<std::vector<uint8_t>> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (in.bad()) return std::nullopt;
    return data;
}

bool write_file(const std::string& path, std::string_view contents, std::string* error) {
    const std::string dir = dirname(path);
    if (!dir.empty() && !is_directory(dir)) {
        if (!mkdirp(dir, error)) return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot open " + path + " for writing";
        return false;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!out) {
        if (error) *error = "failed while writing " + path;
        return false;
    }
    return true;
}

bool append_file(const std::string& path, std::string_view contents, std::string* error) {
    const std::string dir = dirname(path);
    if (!dir.empty() && !is_directory(dir)) {
        if (!mkdirp(dir, error)) return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) {
        if (error) *error = "cannot open " + path + " for appending";
        return false;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(out);
}

bool mkdirp(const std::string& path, std::string* error) {
    if (path.empty()) return true;
    std::error_code ec;
    if (fs::exists(path, ec)) return true;
    fs::create_directories(path, ec);
    if (ec) {
        if (error) *error = errno_message(ec);
        return false;
    }
    return true;
}

std::vector<std::string> list_dir(const std::string& path) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return out;
    for (const auto& entry : fs::directory_iterator(path, ec)) {
        out.push_back(entry.path().filename().string());
    }
    if (ec) return {};
    std::sort(out.begin(), out.end());
    return out;
}

bool remove_file(const std::string& path) {
    std::error_code ec;
    return fs::remove(path, ec) && !ec;
}

bool remove_dir_all(const std::string& path) {
    std::error_code ec;
    fs::remove_all(path, ec);
    return !ec;
}

bool copy_recursive(const std::string& from, const std::string& to, std::string* error) {
    std::error_code ec;
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) *error = errno_message(ec);
        return false;
    }
    return true;
}

bool move_path(const std::string& from, const std::string& to, std::string* error) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (ec) {
        if (error) *error = errno_message(ec);
        return false;
    }
    return true;
}

size_t count_file_lines(const std::string& path) {
    const auto contents = read_file(path);
    if (!contents) return 0;
    return count_lines(*contents);
}

bool glob_match(std::string_view path, std::string_view pattern) {
    for (const auto& expanded : expand_braces(pattern)) {
        if (match_path(path, expanded)) return true;
    }
    return false;
}

bool is_ignored_dir_name(std::string_view name) {
    static const std::vector<std::string> ignored = {
        ".git",     "node_modules", ".venv",     "venv",       "__pycache__",
        ".mypy_cache", ".pytest_cache", ".ruff_cache", "dist",  "build",
        ".next",    ".cache",       ".turbo",    "target",     ".gradle",
        ".idea",    ".vscode",      "coverage",  ".DS_Store",  ".tox",
    };
    for (const auto& name_ : ignored) {
        if (name == name_) return true;
    }
    return false;
}

std::vector<std::string> walk_files(const std::string& root, size_t limit) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;

    std::vector<std::string> stack{root};
    while (!stack.empty()) {
        const std::string dir = stack.back();
        stack.pop_back();
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            const std::string name = entry.path().filename().string();
            std::error_code sub_ec;
            if (entry.is_directory(sub_ec)) {
                if (is_ignored_dir_name(name) || name == ".git") continue;
                stack.push_back(entry.path().string());
            } else if (entry.is_regular_file(sub_ec)) {
                out.push_back(entry.path().string());
                if (limit && out.size() >= limit) return out;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> glob(const std::string& pattern, const std::string& root,
                              size_t limit) {
    std::vector<std::string> out;

    // A pattern with no wildcard components is a literal path.
    const bool has_magic =
        pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos ||
        pattern.find('[') != std::string::npos || pattern.find('{') != std::string::npos;

    if (!has_magic) {
        const std::string target = resolve(pattern, root);
        if (file_exists(target)) out.push_back(target);
        return out;
    }

    // Determine the fixed directory prefix of the pattern so the walk can
    // start there instead of at the filesystem root.
    std::string fixed = pattern;
    size_t magic_pos = fixed.size();
    for (size_t i = 0; i < fixed.size(); ++i) {
        if (fixed[i] == '*' || fixed[i] == '?' || fixed[i] == '[' || fixed[i] == '{') {
            magic_pos = i;
            break;
        }
    }
    std::string base = fixed.substr(0, magic_pos);
    if (const size_t slash = base.find_last_of('/'); slash != std::string::npos) {
        base = base.substr(0, slash);
    } else {
        base = ".";
    }

    const std::string search_root = resolve(base, root);
    const std::string relative_pattern =
        base == "." ? pattern : pattern.substr(base.size() + (base == "/" ? 0 : 1));

    for (const auto& candidate : walk_files(search_root, 0)) {
        const std::string relative_path = cc::relative(candidate, search_root);
        if (relative_path.empty()) continue;
        if (glob_match(relative_path, relative_pattern)) {
            out.push_back(candidate);
            if (limit && out.size() >= limit) break;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

std::vector<GrepMatch> grep_impl(const std::function<bool(const std::string&)>& matcher,
                                  const std::string& root, std::string_view glob_filter,
                                  size_t limit) {
    std::vector<GrepMatch> out;

    const std::string search_root = resolve(root.empty() ? "." : root);
    if (is_file(search_root)) {
        const auto contents = read_file(search_root);
        if (contents) {
            int line_no = 0;
            for (const auto& line : split_lines(*contents)) {
                ++line_no;
                if (matcher(line)) {
                    GrepMatch m;
                    m.file = search_root;
                    m.line = line_no;
                    m.text = line;
                    out.push_back(std::move(m));
                    if (limit && out.size() >= limit) return out;
                }
            }
        }
        return out;
    }

    for (const auto& file : walk_files(search_root, 0)) {
        if (!glob_filter.empty() && !glob_match(basename(file), glob_filter) &&
            !glob_match(file, glob_filter)) {
            continue;
        }
        const auto contents = read_file(file);
        if (!contents) continue;
        int line_no = 0;
        for (const auto& line : split_lines(*contents)) {
            ++line_no;
            if (matcher(line)) {
                GrepMatch m;
                m.file = file;
                m.line = line_no;
                m.text = line;
                out.push_back(std::move(m));
                if (limit && out.size() >= limit) return out;
            }
        }
    }
    return out;
}

} // namespace

std::vector<GrepMatch> grep(const std::string& pattern, const std::string& root,
                             std::string_view glob_filter, size_t limit,
                             bool case_insensitive) {
    auto flags = std::regex::ECMAScript;
    if (case_insensitive) flags |= std::regex::icase;
    std::regex re;
    try {
        re.assign(pattern, flags);
    } catch (const std::regex_error&) {
        return {};
    }
    return grep_impl(
        [&re](const std::string& line) { return std::regex_search(line, re); }, root,
        glob_filter, limit);
}

std::vector<GrepMatch> grep_literal(const std::string& needle, const std::string& root,
                                     std::string_view glob_filter, size_t limit,
                                     bool case_insensitive) {
    const std::string lowered = to_lower(needle);
    return grep_impl(
        [&](const std::string& line) {
            if (case_insensitive) return to_lower(line).find(lowered) != std::string::npos;
            return line.find(needle) != std::string::npos;
        },
        root, glob_filter, limit);
}

} // namespace cc
