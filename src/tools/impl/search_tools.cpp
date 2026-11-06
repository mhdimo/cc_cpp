#include "tools/impl/search_tools.hpp"

#include "core/error.hpp"
#include "tools/tool_use_context.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace cc::tools {
namespace {

/// The largest number of files the Glob tool returns.
constexpr size_t kGlobMaxResults = 100;

/// The number of entries the Grep tool returns when `head_limit` is unset.
constexpr int64_t kDefaultHeadLimit = 250;

/// The number of context lines shown on either side of a match.
constexpr int64_t kDefaultContextLines = 0;

/// The working directory the session runs in.
std::string session_cwd(const ToolUseContext& context) {
    return context.options.cwd.empty() ? cc::cwd() : context.options.cwd;
}

/// Expresses `path` relative to `base`, keeping the absolute path when the
/// file lies outside of it.
std::string to_relative_path(const std::string& path, const std::string& base) {
    const std::string rel = relative(path, base);
    if (rel.size() >= 2 && rel[0] == '.' && rel[1] == '.') return path;
    return rel;
}

/// Sorts the paths by modification time, oldest first, with the path as the
/// tiebreaker. This is the order ripgrep's `--sort=modified` produces; the
/// Grep tool wants the opposite order.
void sort_by_modification_time(std::vector<std::string>& paths, bool newest_first) {
    std::vector<std::pair<int64_t, std::string>> keyed;
    keyed.reserve(paths.size());
    for (const auto& path : paths) keyed.emplace_back(file_state(path).mtime_ms, path);

    std::sort(keyed.begin(), keyed.end(), [newest_first](const auto& a, const auto& b) {
        if (a.first != b.first) return newest_first ? a.first > b.first : a.first < b.first;
        return a.second < b.second;
    });

    for (size_t i = 0; i < paths.size(); ++i) paths[i] = keyed[i].second;
}

/// The outcome of applying an offset and a head limit to a list of results.
struct Limited {
    std::vector<std::string> items;
    std::optional<int64_t> applied_limit;
    size_t from = 0; ///< the index of the first item that was kept
    size_t to = 0;   ///< the index just past the last item that was kept
};

/// Applies `offset` and `limit` the way Claude Code does: a limit of zero
/// means unlimited, and the limit is only reported when it really truncated
/// the results.
Limited apply_head_limit(const std::vector<std::string>& items, int64_t limit, int64_t offset) {
    Limited out;
    const size_t from = std::min(offset > 0 ? static_cast<size_t>(offset) : 0, items.size());

    if (limit == 0) {
        out.items.assign(items.begin() + static_cast<std::ptrdiff_t>(from), items.end());
        out.from = from;
        out.to = items.size();
        return out;
    }

    const int64_t effective = limit < 0 ? kDefaultHeadLimit : limit;
    const size_t to = std::min(items.size(), from + static_cast<size_t>(effective));
    out.items.assign(items.begin() + static_cast<std::ptrdiff_t>(from),
                     items.begin() + static_cast<std::ptrdiff_t>(to));
    out.from = from;
    out.to = to;
    if (static_cast<int64_t>(items.size()) - offset > effective) out.applied_limit = effective;
    return out;
}

/// Formats the pagination note Claude Code appends to a truncated result.
std::string format_limit_info(const std::optional<int64_t>& applied_limit, int64_t applied_offset) {
    std::vector<std::string> parts;
    if (applied_limit.has_value()) parts.push_back("limit: " + std::to_string(*applied_limit));
    if (applied_offset > 0) parts.push_back("offset: " + std::to_string(applied_offset));
    return join(parts, ", ");
}

/// Splits the `glob` parameter into the individual patterns ripgrep would
/// receive. Patterns with braces are kept whole.
std::vector<std::string> split_glob_patterns(std::string_view glob_filter) {
    std::vector<std::string> patterns;
    for (const auto& raw : split_any(glob_filter, " \t\r\n")) {
        if (raw.empty()) continue;
        if (raw.find('{') != std::string::npos && raw.find('}') != std::string::npos) {
            patterns.push_back(raw);
            continue;
        }
        for (const auto& part : split(raw, ',')) {
            if (!part.empty()) patterns.push_back(part);
        }
    }
    return patterns;
}

/// The file extensions ripgrep's `--type` flag selects.
const std::vector<std::pair<std::string, std::vector<std::string>>>& grep_file_types() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> types = {
        {"c", {"*.c", "*.h"}},
        {"cpp", {"*.cpp", "*.cc", "*.cxx", "*.c++", "*.hpp", "*.hh", "*.hxx", "*.h++"}},
        {"cs", {"*.cs"}},
        {"css", {"*.css"}},
        {"dart", {"*.dart"}},
        {"docker", {"*Dockerfile*"}},
        {"ex", {"*.ex", "*.exs"}},
        {"go", {"*.go"}},
        {"hs", {"*.hs"}},
        {"html", {"*.html", "*.htm"}},
        {"java", {"*.java"}},
        {"js", {"*.js", "*.jsx", "*.mjs", "*.cjs"}},
        {"json", {"*.json"}},
        {"kt", {"*.kt", "*.kts"}},
        {"lua", {"*.lua"}},
        {"make", {"Makefile", "*.mk"}},
        {"md", {"*.md", "*.markdown"}},
        {"perl", {"*.pl", "*.pm"}},
        {"php", {"*.php"}},
        {"py", {"*.py", "*.pyi", "*.pyw"}},
        {"r", {"*.r"}},
        {"rb", {"*.rb", "*.rake"}},
        {"rs", {"*.rs"}},
        {"scala", {"*.scala"}},
        {"sh", {"*.sh", "*.bash", "*.zsh"}},
        {"sql", {"*.sql"}},
        {"swift", {"*.swift"}},
        {"toml", {"*.toml"}},
        {"ts", {"*.ts", "*.tsx", "*.mts", "*.cts"}},
        {"vim", {"*.vim"}},
        {"xml", {"*.xml"}},
        {"yaml", {"*.yaml", "*.yml"}},
        {"zig", {"*.zig"}},
    };
    return types;
}

/// The patterns that select the files a search looks at.
struct SearchFilters {
    /// The patterns coming from the `glob` parameter.
    std::vector<std::string> globs;
    /// The patterns coming from the `type` parameter.
    std::vector<std::string> types;
};

/// True when `file` matches one of the glob patterns.
bool matches_any(const std::string& file, const std::string& root,
                 const std::vector<std::string>& patterns) {
    if (patterns.empty()) return true;
    const std::string name = basename(file);
    const std::string rel = relative(file, root);
    for (const auto& pattern : patterns) {
        if (glob_match(name, pattern)) return true;
        if (!rel.empty() && glob_match(rel, pattern)) return true;
        if (glob_match(file, pattern)) return true;
    }
    return false;
}

/// True when the file passes every filter.
bool matches_filters(const std::string& file, const std::string& root,
                     const SearchFilters& filters) {
    if (!matches_any(file, root, filters.globs)) return false;
    if (!matches_any(file, root, filters.types)) return false;
    return true;
}

/// One matching line.
struct GrepHit {
    std::string file; ///< the absolute path of the file
    int line = 0;     ///< the 1-based line number
    std::string text; ///< the full line, without the terminator
};

/// True when the pattern holds no regular expression metacharacters.
bool is_literal_pattern(std::string_view pattern) {
    static const std::string specials = "\\^$.*+?()[]{}|";
    return pattern.find_first_of(specials) == std::string_view::npos;
}

/// Rewrites the pattern so that `.` also matches newlines, the way ripgrep's
/// `--multiline-dotall` does.
std::string dotall_pattern(std::string_view pattern) {
    std::string out;
    out.reserve(pattern.size());
    bool in_class = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            out.push_back(c);
            out.push_back(pattern[i + 1]);
            ++i;
            continue;
        }
        if (c == '[') in_class = true;
        else if (c == ']') in_class = false;
        if (c == '.' && !in_class) {
            out += "[\\s\\S]";
            continue;
        }
        out.push_back(c);
    }
    return out;
}

/// The line number of the character at `pos` in `text`.
int line_number_at(const std::string& text, size_t pos) {
    int line = 1;
    for (size_t i = 0; i < pos && i < text.size(); ++i) {
        if (text[i] == '\n') ++line;
    }
    return line;
}

/// Searches the files under `root` and returns every matching line.
std::vector<GrepHit> search_lines(const std::string& pattern, const std::string& root,
                                   const SearchFilters& filters, bool case_insensitive,
                                   bool multiline) {
    std::vector<GrepHit> hits;

    if (!multiline) {
        // The search helper matches a single glob against the file name and
        // the full path, which is all it can do for a name-only pattern.
        const bool simple_glob = filters.globs.size() == 1 && filters.types.empty() &&
                                 filters.globs.front().find('/') == std::string::npos;
        const std::string inline_filter = simple_glob ? filters.globs.front() : std::string();

        std::vector<GrepMatch> matches;
        if (is_literal_pattern(pattern)) {
            matches = grep_literal(pattern, root, inline_filter, 0, case_insensitive);
        } else {
            matches = grep(pattern, root, inline_filter, 0, case_insensitive);
        }

        for (const auto& match : matches) {
            if (!matches_filters(match.file, root, filters)) continue;
            hits.push_back(GrepHit{match.file, match.line, match.text});
        }
        return hits;
    }

    // Multiline mode matches the pattern against the whole file, so the
    // search cannot be delegated to the line-based helper.
    std::regex re;
    try {
        auto flags = std::regex::ECMAScript | std::regex::multiline;
        if (case_insensitive) flags |= std::regex::icase;
        re.assign(dotall_pattern(pattern), flags);
    } catch (const std::regex_error&) {
        return hits;
    }

    for (const auto& file : walk_files(root, 0)) {
        if (!matches_filters(file, root, filters)) continue;
        const auto contents = read_file(file);
        if (!contents) continue;

        std::regex_iterator<std::string::const_iterator> it(contents->begin(), contents->end(), re);
        const std::regex_iterator<std::string::const_iterator> end;
        for (; it != end; ++it) {
            const std::smatch& match = *it;
            const size_t pos = static_cast<size_t>(match.position(0));
            const size_t eol = contents->find('\n', pos);
            const size_t length = (eol == std::string::npos ? contents->size() : eol) - pos;
            hits.push_back(GrepHit{file, line_number_at(*contents, pos),
                                   contents->substr(pos, length)});
        }
    }
    return hits;
}

/// The rendered output of the `content` mode.
struct ContentOutput {
    std::vector<std::string> lines; ///< the lines, in ripgrep's format
    std::vector<size_t> line_files; ///< the index of the file each line came from
    std::vector<std::string> files;  ///< the files, in the order they appear
};

/// Builds the output lines of the `content` mode, in ripgrep's format.
ContentOutput build_content_lines(const std::vector<GrepHit>& hits, const std::string& cwd,
                                   int before, int after, bool show_line_numbers) {
    ContentOutput out;

    std::vector<std::string> file_order;
    std::map<std::string, std::vector<GrepHit>> by_file;
    for (const auto& hit : hits) {
        auto [it, inserted] = by_file.emplace(hit.file, std::vector<GrepHit>{});
        if (inserted) file_order.push_back(hit.file);
        it->second.push_back(hit);
    }

    const bool with_context = before > 0 || after > 0;
    bool first_group = true;

    for (const auto& file : file_order) {
        auto& file_hits = by_file[file];
        std::sort(file_hits.begin(), file_hits.end(),
                  [](const GrepHit& a, const GrepHit& b) { return a.line < b.line; });

        const auto contents = read_file(file);
        const std::vector<std::string> text_lines =
            contents ? split_lines(*contents) : std::vector<std::string>{};
        const size_t total = text_lines.size();
        const std::string rel = to_relative_path(file, cwd);

        const size_t file_index = out.files.size();
        out.files.push_back(rel);

        // The ranges of lines to show, with the context lines folded in.
        std::vector<std::pair<size_t, size_t>> ranges;
        for (const auto& hit : file_hits) {
            if (total == 0) break;
            size_t start = hit.line > before ? static_cast<size_t>(hit.line - before) : 1;
            size_t end = static_cast<size_t>(hit.line) + static_cast<size_t>(after);
            if (end > total) end = total;
            if (!ranges.empty() && start <= ranges.back().second + 1) {
                ranges.back().second = std::max(ranges.back().second, end);
            } else {
                ranges.emplace_back(start, end);
            }
        }

        size_t next_hit = 0;
        for (const auto& [start, end] : ranges) {
            if (with_context && !first_group) {
                out.lines.push_back("--");
                out.line_files.push_back(file_index);
            }
            first_group = false;

            for (size_t line = start; line <= end; ++line) {
                bool is_match = false;
                std::string text;
                while (next_hit < file_hits.size() &&
                       static_cast<size_t>(file_hits[next_hit].line) < line) {
                    ++next_hit;
                }
                if (next_hit < file_hits.size() &&
                    static_cast<size_t>(file_hits[next_hit].line) == line) {
                    is_match = true;
                    text = file_hits[next_hit].text;
                    ++next_hit;
                } else if (line <= total) {
                    text = text_lines[line - 1];
                }

                std::string entry = rel + (is_match ? ":" : "-");
                if (show_line_numbers) entry += std::to_string(line) + ":";
                entry += text;
                out.lines.push_back(std::move(entry));
                out.line_files.push_back(file_index);
            }
        }

        // A file we could not read still shows its matching lines.
        if (total == 0) {
            for (const auto& hit : file_hits) {
                std::string entry = rel + ":";
                if (show_line_numbers) entry += std::to_string(hit.line) + ":";
                entry += hit.text;
                out.lines.push_back(std::move(entry));
                out.line_files.push_back(file_index);
            }
        }
    }

    return out;
}

/// Counts the files the lines between `from` and `to` came from.
size_t count_files(const ContentOutput& output, size_t from, size_t to) {
    std::vector<bool> seen(output.files.size(), false);
    size_t count = 0;
    for (size_t i = from; i < to && i < output.line_files.size(); ++i) {
        const size_t index = output.line_files[i];
        if (!seen[index]) {
            seen[index] = true;
            ++count;
        }
    }
    return count;
}

} // namespace

// ---------------------------------------------------------------------------
// Glob
// ---------------------------------------------------------------------------

boost::json::object GlobTool::input_schema() const {
    return schema_object(
        {
            {"pattern", schema_string("The glob pattern to match files against")},
            {"path",
             schema_string(
                 "The directory to search in. If not specified, the current working directory will "
                 "be used. IMPORTANT: Omit this field to use the default directory. DO NOT enter "
                 "\"undefined\" or \"null\" - simply omit it for the default behavior. Must be a "
                 "valid directory path if provided.")},
        },
        {"pattern"});
}

std::string GlobTool::prompt() const { return glob_tool_prompt(); }

ToolResult GlobTool::call(const boost::json::object& input, ToolUseContext& context) {
    const std::string pattern = jget_string(input, "pattern");
    const std::string requested = jget_string(input, "path");
    const std::string cwd = session_cwd(context);

    std::string root = cwd;
    if (!requested.empty()) {
        const std::string expanded = resolve(expand_path(requested), cwd);
        if (!file_exists(expanded)) {
            std::string message = "Directory does not exist: " + requested + ". " +
                                  std::string(kFileNotFoundCwdNote) + " " + cwd + ".";
            const std::string suggestion = did_you_mean(expanded, cwd);
            if (!suggestion.empty()) message += " " + suggestion;
            throw ToolError(message, "Glob");
        }
        if (!is_directory(expanded)) {
            throw ToolError("Path is not a directory: " + requested, "Glob");
        }
        root = expanded;
    }

    std::vector<std::string> matches = glob(pattern, root, 0);
    sort_by_modification_time(matches, false);

    const bool truncated = matches.size() > kGlobMaxResults;
    if (truncated) matches.resize(kGlobMaxResults);

    ToolResult result;

    // The UI renders a one-line summary of the search, so the result carries
    // the list of the files that were found next to the text.
    boost::json::object payload;
    payload["mode"] = "files_with_matches";

    if (matches.empty()) {
        payload["filenames"] = boost::json::array();
        payload["numFiles"] = 0;
        result.content = "No files found";
        result.tool_use_result = boost::json::value(std::move(payload));
        return result;
    }

    std::vector<std::string> filenames;
    filenames.reserve(matches.size());
    for (const auto& match : matches) filenames.push_back(to_relative_path(match, cwd));

    result.content = join(filenames, "\n");
    if (truncated) {
        result.content +=
            "\n(Results are truncated. Consider using a more specific path or pattern.)";
    }

    boost::json::array file_list;
    file_list.reserve(filenames.size());
    for (const auto& name : filenames) file_list.emplace_back(name);
    payload["filenames"] = std::move(file_list);
    payload["numFiles"] = static_cast<int64_t>(filenames.size());
    result.tool_use_result = boost::json::value(std::move(payload));
    return result;
}

// ---------------------------------------------------------------------------
// Grep
// ---------------------------------------------------------------------------

boost::json::object GrepTool::input_schema() const {
    return schema_object(
        {
            {"pattern", schema_string("The regular expression pattern to search for in file "
                                      "contents")},
            {"path", schema_string("File or directory to search in (rg PATH). Defaults to current "
                                   "working directory.")},
            {"glob", schema_string("Glob pattern to filter files (e.g. \"*.js\", "
                                   "\"*.{ts,tsx}\") - maps to rg --glob")},
            {"output_mode",
             schema_enum("Output mode: \"content\" shows matching lines (supports -A/-B/-C "
                         "context, -n line numbers, head_limit), \"files_with_matches\" shows file "
                         "paths (supports head_limit), \"count\" shows match counts (supports "
                         "head_limit). Defaults to \"files_with_matches\".",
                         {"content", "files_with_matches", "count"})},
            {"-B", schema_number("Number of lines to show before each match (rg -B). Requires "
                                 "output_mode: \"content\", ignored otherwise.")},
            {"-A", schema_number("Number of lines to show after each match (rg -A). Requires "
                                 "output_mode: \"content\", ignored otherwise.")},
            {"-C", schema_number("Alias for context.")},
            {"context", schema_number("Number of lines to show before and after each match (rg "
                                      "-C). Requires output_mode: \"content\", ignored "
                                      "otherwise.")},
            {"-n", schema_boolean("Show line numbers in output (rg -n). Requires output_mode: "
                                  "\"content\", ignored otherwise. Defaults to true.")},
            {"-i", schema_boolean("Case insensitive search (rg -i)")},
            {"type", schema_string("File type to search (rg --type). Common types: js, py, rust, "
                                   "go, java, etc. More efficient than include for standard file "
                                   "types.")},
            {"head_limit",
             schema_number("Limit output to first N lines/entries, equivalent to \"| head -N\". "
                           "Works across all output modes: content (limits output lines), "
                           "files_with_matches (limits file paths), count (limits count "
                           "entries). Defaults to 250 when unspecified. Pass 0 for unlimited (use "
                           "sparingly — large result sets waste context).")},
            {"offset", schema_number("Skip first N lines/entries before applying head_limit, "
                                     "equivalent to \"| tail -n +N | head -N\". Works across all "
                                     "output modes. Defaults to 0.")},
            {"multiline", schema_boolean("Enable multiline mode where . matches newlines and "
                                         "patterns can span lines (rg -U --multiline-dotall). "
                                         "Default: false.")},
        },
        {"pattern"});
}

std::string GrepTool::prompt() const { return grep_tool_prompt(); }

ToolResult GrepTool::call(const boost::json::object& input, ToolUseContext& context) {
    const std::string pattern = jget_string(input, "pattern");
    const std::string requested = jget_string(input, "path");
    const std::string glob_filter = jget_string(input, "glob");
    const std::string output_mode = jget_string(input, "output_mode", "files_with_matches");
    const std::string type_filter = jget_string(input, "type");
    const int64_t before_arg = jget_int(input, "-B", kDefaultContextLines);
    const int64_t after_arg = jget_int(input, "-A", kDefaultContextLines);
    const int64_t context_c = jget_int(input, "-C", -1);
    const int64_t context_arg = jget_int(input, "context", -1);
    const bool show_line_numbers = jget_bool(input, "-n", true);
    const bool case_insensitive = jget_bool(input, "-i", false);
    const int64_t head_limit = jget_int(input, "head_limit", -1);
    const int64_t offset = jget_int(input, "offset", 0);
    const bool multiline = jget_bool(input, "multiline", false);

    const std::string cwd = session_cwd(context);

    std::string root = cwd;
    if (!requested.empty()) {
        const std::string expanded = resolve(expand_path(requested), cwd);
        if (!file_exists(expanded)) {
            std::string message = "Path does not exist: " + requested + ". " +
                                  std::string(kFileNotFoundCwdNote) + " " + cwd + ".";
            const std::string suggestion = did_you_mean(expanded, cwd);
            if (!suggestion.empty()) message += " " + suggestion;
            throw ToolError(message, "Grep");
        }
        root = expanded;
    }

    // The context flags: `context` wins over `-C`, which wins over `-A`/`-B`.
    int before = static_cast<int>(std::max<int64_t>(before_arg, 0));
    int after = static_cast<int>(std::max<int64_t>(after_arg, 0));
    if (context_arg >= 0) {
        before = after = static_cast<int>(context_arg);
    } else if (context_c >= 0) {
        before = after = static_cast<int>(context_c);
    }

    SearchFilters filters;
    filters.globs = split_glob_patterns(glob_filter);
    if (!type_filter.empty()) {
        for (const auto& [name, patterns] : grep_file_types()) {
            if (name == type_filter) {
                filters.types = patterns;
                break;
            }
        }
    }

    const std::vector<GrepHit> hits =
        search_lines(pattern, root, filters, case_insensitive, multiline);

    ToolResult result;

    if (output_mode == "content") {
        const ContentOutput all_lines =
            build_content_lines(hits, cwd, before, after, show_line_numbers);
        const Limited limited = apply_head_limit(all_lines.lines, head_limit, offset);

        std::string content = join(limited.items, "\n");
        if (content.empty()) content = "No matches found";

        const std::string limit_info = format_limit_info(limited.applied_limit, offset);
        if (!limit_info.empty()) {
            content += "\n\n[Showing results with pagination = " + limit_info + "]";
        }
        result.content = content;

        // The UI counts the lines the search produced, and the files they
        // came from.
        boost::json::object payload;
        payload["mode"] = "content";
        payload["numFiles"] = static_cast<int64_t>(count_files(all_lines, limited.from, limited.to));
        payload["numLines"] = static_cast<int64_t>(limited.items.size());
        payload["content"] = content;
        result.tool_use_result = boost::json::value(std::move(payload));
        return result;
    }

    if (output_mode == "count") {
        // One entry per file, holding the number of matches in it.
        std::vector<std::string> files;
        std::map<std::string, int64_t> counts;
        for (const auto& hit : hits) {
            if (counts.find(hit.file) == counts.end()) files.push_back(hit.file);
            ++counts[hit.file];
        }

        std::vector<std::string> count_lines;
        count_lines.reserve(files.size());
        for (const auto& file : files) {
            count_lines.push_back(file + ":" + std::to_string(counts[file]));
        }

        const Limited limited = apply_head_limit(count_lines, head_limit, offset);

        std::vector<std::string> final_lines;
        final_lines.reserve(limited.items.size());
        boost::json::array file_list;
        int64_t total_matches = 0;
        int64_t file_count = 0;
        for (const auto& line : limited.items) {
            const size_t colon = line.rfind(':');
            if (colon == std::string::npos) {
                final_lines.push_back(line);
                continue;
            }
            const std::string file = to_relative_path(line.substr(0, colon), cwd);
            const std::string count_text = line.substr(colon + 1);
            final_lines.push_back(file + ":" + count_text);
            file_list.emplace_back(file);

            const int64_t count = parse_int(count_text).value_or(0);
            total_matches += count;
            ++file_count;
        }

        std::string content = join(final_lines, "\n");
        if (content.empty()) content = "No matches found";

        const std::string limit_info = format_limit_info(limited.applied_limit, offset);
        content += "\n\nFound " + std::to_string(total_matches) + " total " +
                   (total_matches == 1 ? "occurrence" : "occurrences") + " across " +
                   std::to_string(file_count) + " " + (file_count == 1 ? "file" : "files") + ".";
        if (!limit_info.empty()) content += " with pagination = " + limit_info;
        result.content = content;

        boost::json::object payload;
        payload["mode"] = "count";
        payload["filenames"] = std::move(file_list);
        payload["numFiles"] = file_count;
        payload["numMatches"] = total_matches;
        result.tool_use_result = boost::json::value(std::move(payload));
        return result;
    }

    // The default mode: the files that matched, newest first.
    std::vector<std::string> files;
    std::map<std::string, bool> seen;
    for (const auto& hit : hits) {
        if (seen.emplace(hit.file, true).second) files.push_back(hit.file);
    }
    sort_by_modification_time(files, true);

    const Limited limited = apply_head_limit(files, head_limit, offset);

    boost::json::object payload;
    payload["mode"] = "files_with_matches";

    if (limited.items.empty()) {
        payload["filenames"] = boost::json::array();
        payload["numFiles"] = 0;
        result.content = "No files found";
        result.tool_use_result = boost::json::value(std::move(payload));
        return result;
    }

    std::vector<std::string> filenames;
    filenames.reserve(limited.items.size());
    for (const auto& file : limited.items) filenames.push_back(to_relative_path(file, cwd));

    const std::string limit_info = format_limit_info(limited.applied_limit, offset);
    result.content = "Found " + std::to_string(filenames.size()) + " " +
                     (filenames.size() == 1 ? "file" : "files") +
                     (limit_info.empty() ? "" : " " + limit_info) + "\n" + join(filenames, "\n");

    boost::json::array file_list;
    file_list.reserve(filenames.size());
    for (const auto& name : filenames) file_list.emplace_back(name);
    payload["filenames"] = std::move(file_list);
    payload["numFiles"] = static_cast<int64_t>(filenames.size());
    result.tool_use_result = boost::json::value(std::move(payload));
    return result;
}

} // namespace cc::tools
