#include "ui/tool_display.hpp"

#include "tools/impl/agent_tool.hpp"
#include "tools/tool_names.hpp"
#include "ui/diff.hpp"
#include "ui/figures.hpp"
#include "ui/markdown.hpp"
#include "ui/spinner.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace cc::ui {
namespace {

/// The dot Claude Code puts in front of a tool call.
#if defined(__APPLE__)
constexpr std::string_view kToolDot = "⏺";
#else
constexpr std::string_view kToolDot = "●";
#endif

/// The width of the gutter that precedes a tool result.
constexpr size_t kResultGutter = 5;

/// The gutter that precedes the first line of a tool result.
constexpr std::string_view kResultGutterText = "  ⎿  ";

/// The number of lines of a tool's output the reference shows.
constexpr size_t kOutputLines = 3;

/// The directory the plan files live in.
///
/// Claude Code keeps the plan of a session in a file of its own and names
/// the tools that touch it in a way of their own.
std::string plans_directory() { return cc::path_join(cc::claude_config_dir(), "plans"); }

/// True when the path names a plan file.
bool is_plan_file(const std::string& path) {
    if (path.empty()) return false;
    return cc::starts_with(path, plans_directory());
}

/// The number of lines of context the reference shows around a change.
constexpr size_t kDiffContextLines = 3;

/// Cuts a diff down to the hunks Claude Code shows.
///
/// A hunk is a change with three lines of context on either side of it; two
/// changes that lie close to each other belong to the same hunk.
std::vector<std::vector<DiffLine>> diff_hunks(const std::vector<DiffLine>& lines) {
    // The places where the diff changes.
    std::vector<std::pair<size_t, size_t>> changes;
    for (size_t i = 0; i < lines.size();) {
        if (lines[i].kind == DiffLine::Kind::Context) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < lines.size() && lines[i].kind != DiffLine::Kind::Context) ++i;
        changes.push_back({start, i});
    }
    if (changes.empty()) return {};

    // Two changes that are no further apart than twice the context belong to
    // the same hunk.
    std::vector<std::pair<size_t, size_t>> hunks;
    for (const auto& change : changes) {
        if (!hunks.empty() && change.first - hunks.back().second <= 2 * kDiffContextLines) {
            hunks.back().second = change.second;
        } else {
            hunks.push_back(change);
        }
    }

    std::vector<std::vector<DiffLine>> out;
    out.reserve(hunks.size());
    for (const auto& hunk : hunks) {
        const size_t from = hunk.first > kDiffContextLines ? hunk.first - kDiffContextLines : 0;
        const size_t to = std::min(lines.size(), hunk.second + kDiffContextLines);
        out.emplace_back(lines.begin() + static_cast<long>(from), lines.begin() + static_cast<long>(to));
    }
    return out;
}

/// Renders the hunks of a diff, the way Claude Code shows them.
///
/// The hunks are separated by three dots, the way the reference separates
/// them.
std::vector<std::string> render_diff_hunks(const std::vector<DiffLine>& lines, const Theme& theme,
                                            int width) {
    Style dim;
    dim.foreground = theme.inactive;

    std::vector<std::string> out;
    bool first = true;
    for (const auto& hunk : diff_hunks(lines)) {
        if (!first) out.push_back(styled("...", dim));
        first = false;
        for (const auto& line : render_diff(hunk, theme, width)) {
            out.push_back(line);
        }
    }
    return out;
}

/// The text that is inside a tag of the reference's own.
///
/// The reference takes the text out of the tag, so that the tag is never
/// shown to the user. An empty string is returned when the text carries no
/// such tag.
std::string extract_tag(const std::string& text, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const size_t at = text.find(open);
    if (at == std::string::npos) return {};
    const size_t start = at + open.size();
    const size_t end = text.find(close, start);
    if (end == std::string::npos) return {};
    return text.substr(start, end - start);
}

/// Removes the block of the sandbox violations from `text`.
std::string remove_sandbox_violation_tags(const std::string& text) {
    const std::string open = "<sandbox_violations>";
    const std::string close = "</sandbox_violations>";
    std::string out;
    size_t at = 0;
    while (true) {
        const size_t start = text.find(open, at);
        if (start == std::string::npos) break;
        const size_t end = text.find(close, start);
        if (end == std::string::npos) break;
        out.append(text, at, start - at);
        at = end + close.size();
    }
    out.append(text, at, std::string::npos);
    return out;
}

/// Removes the tags of the error from `text`.
///
/// The reference takes the `<error>` and the `</error>` out of the text of
/// the error, and keeps the rest of it.
std::string remove_error_tags(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 7, "<error>") == 0) {
            i += 7;
            continue;
        }
        if (text.compare(i, 8, "</error>") == 0) {
            i += 8;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

/// One line of a tool's output.
struct OutputLine {
    std::string text;
    Style style;
};

/// Removes the whitespace and the optional escapes from a JSON text.
///
/// The reference compares the text with the text it parsed to make sure
/// that nothing was lost on the way.
std::string normalize_json(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        out.push_back(c);
    }

    // The escape of the slash is optional, so it does not count.
    std::string fixed;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\\' && i + 1 < out.size() && out[i + 1] == '/') continue;
        fixed.push_back(out[i]);
    }
    return fixed;
}

/// Pretty-prints a JSON value with two spaces of indentation.
std::string pretty_json(const boost::json::value& value, size_t depth = 0) {
    const std::string indent(depth * 2, ' ');
    const std::string inner((depth + 1) * 2, ' ');

    if (value.is_object()) {
        const auto& object = value.as_object();
        if (object.empty()) return "{}";

        std::string out = "{\n";
        bool first = true;
        for (const auto& entry : object) {
            if (!first) out += ",\n";
            first = false;
            out += inner + boost::json::serialize(entry.key()) + ": " + pretty_json(entry.value(), depth + 1);
        }
        out += "\n" + indent + "}";
        return out;
    }

    if (value.is_array()) {
        const auto& array = value.as_array();
        if (array.empty()) return "[]";

        std::string out = "[\n";
        bool first = true;
        for (const auto& item : array) {
            if (!first) out += ",\n";
            first = false;
            out += inner + pretty_json(item, depth + 1);
        }
        out += "\n" + indent + "]";
        return out;
    }

    return boost::json::serialize(value);
}

/// Pretty-prints the line when it holds JSON.
///
/// Claude Code formats the output of a command when the output is JSON.
std::vector<std::string> format_json_lines(std::string_view line) {
    boost::system::error_code ec;
    const boost::json::value parsed = boost::json::parse(line, ec);
    if (ec) return {std::string(line)};

    // The round trip has to be lossless: a number that does not survive it
    // is left alone.
    if (normalize_json(line) != normalize_json(boost::json::serialize(parsed))) {
        return {std::string(line)};
    }
    return cc::split_lines(pretty_json(parsed));
}

/// Cuts a line of output into the chunks of `width` columns.
///
/// The reference cuts the line at the exact column and not at a space, so
/// that the output fills the width of the gutter.
std::vector<std::string> cut_output_line(const std::string& text, size_t width) {
    std::vector<std::string> out;
    if (width == 0) {
        out.push_back(cc::trim_end(text));
        return out;
    }
    // The line is walked a character at a time, so that a character of more
    // than one byte is never cut in two.
    const std::vector<std::string> clusters = cc::graphemes(text);
    size_t index = 0;
    while (index < clusters.size()) {
        std::string chunk;
        size_t columns = 0;
        while (index < clusters.size()) {
            const size_t size = cc::display_width(clusters[index]);
            if (columns + size > width) break;
            chunk += clusters[index];
            columns += size;
            ++index;
        }
        if (chunk.empty()) {
            // A character that is wider than the line is kept whole.
            chunk = clusters[index];
            ++index;
        }
        out.push_back(cc::trim_end(chunk));
    }
    return out;
}

/// Shows the lines of a tool's output, cut short the way Claude Code cuts
/// it short.
///
/// The transcript shows the whole output: it is the view the "ctrl+o to
/// expand" of the output refers to, so the output is not cut short there.
std::vector<std::string> render_output_lines(std::vector<OutputLine> lines, const Theme& theme,
                                              int width, bool truncate = true) {
    Style gutter_style;
    gutter_style.foreground = theme.inactive;

    // The reference leaves room for the gutter and for the terminal's own
    // margin, and shows three lines of the output.
    const size_t wrap_width = width > 10 ? static_cast<size_t>(width) - 10 : 10;

    std::vector<std::string> pieces;
    for (const auto& line : lines) {
        for (const auto& source : cc::split_lines(line.text)) {
            for (auto& piece : cut_output_line(source, wrap_width)) {
                pieces.push_back(styled(piece, line.style));
            }
        }
    }

    // The output is cut short, and the number of lines that are left is
    // said, the way the reference says it. A single line that is left is
    // shown rather than counted.
    size_t remaining = 0;
    if (!truncate) {
        // The transcript shows the whole output.
    } else if (pieces.size() > kOutputLines + 1) {
        remaining = pieces.size() - kOutputLines;
        // Only the lines above the fold are shown, the way the reference
        // shows them.
        pieces.resize(kOutputLines);
    } else if (pieces.size() > kOutputLines) {
        remaining = 0;
        pieces.resize(kOutputLines + 1);
    } else {
        pieces.resize(std::min<size_t>(pieces.size(), kOutputLines));
    }

    std::vector<std::string> out;
    bool first = true;
    for (const auto& piece : pieces) {
        const std::string gutter =
            first ? styled(std::string(kResultGutterText), gutter_style)
                  : std::string(kResultGutter, ' ');
        out.push_back(gutter + piece);
        first = false;
    }
    if (out.empty()) {
        out.push_back(styled(std::string(kResultGutterText), gutter_style));
    }

    if (remaining > 0) {
        Style dim;
        dim.foreground = theme.inactive;
        out.push_back(std::string(kResultGutter, ' ') +
                      styled("… +" + std::to_string(remaining) + " lines (ctrl+o to expand)", dim));
    }
    return out;
}

/// The output of a shell command.
///
/// The standard output and the standard error are two blocks of their own:
/// each one gets its own gutter and is cut short on its own. The standard
/// error is shown in its own colour, the way Claude Code shows it.
std::vector<std::string> bash_summary(const boost::json::object& payload, const Theme& theme,
                                       int width, bool truncate) {
    Style normal;
    normal.foreground = theme.text;

    Style error;
    error.foreground = theme.error;

    Style dim;
    dim.foreground = theme.inactive;

    const auto collect = [](const std::string& text, const Style& style) {
        std::vector<OutputLine> lines;
        for (const auto& raw : cc::split_lines(text)) {
            for (auto& piece : format_json_lines(raw)) {
                lines.push_back({std::move(piece), style});
            }
        }
        return lines;
    };

    const std::string stdout = jget_string(payload, "stdout");
    std::string stderr = jget_string(payload, "stderr");

    // The line that says the folder of the session was put back is taken
    // out of the standard error and shown on its own, in the dim colour,
    // the way the reference shows it.
    std::string warning;
    {
        const size_t at = stderr.find("Shell cwd was reset to ");
        if (at != std::string::npos) {
            const size_t end = stderr.find('\n', at);
            const size_t length = end == std::string::npos ? std::string::npos : end - at;
            warning = stderr.substr(at, length);
            stderr.erase(at, length == std::string::npos ? length : length + 1);
        }
    }

    std::vector<std::string> out;
    if (!stdout.empty()) {
        for (auto& line : render_output_lines(collect(stdout, normal), theme, width, truncate)) {
            out.push_back(std::move(line));
        }
    }
    if (!cc::trim(stderr).empty()) {
        for (auto& line : render_output_lines(collect(stderr, error), theme, width, truncate)) {
            out.push_back(std::move(line));
        }
    }
    if (!warning.empty()) {
        for (auto& line : render_output_lines({OutputLine{warning, dim}}, theme, width, truncate)) {
            out.push_back(std::move(line));
        }
    }
    if (out.empty()) {
        // The reference says so when the command printed nothing.
        out = render_output_lines({OutputLine{"(No output)", dim}}, theme, width);
    }
    return out;
}

} // namespace

/// Shows a file path the way Claude Code does.
///
/// A path inside the working directory is shown relative to it, a path in
/// the home directory with a tilde, and anything else in full.
std::string relative_path(const std::string& path, const std::string& cwd) {
    if (path.empty()) return path;
    const std::string base = cwd.empty() ? cc::cwd() : cwd;
    const std::string absolute = cc::resolve(path, base);
    const std::string rel = cc::relative(absolute, base);
    if (!rel.empty() && rel != "." && !cc::starts_with(rel, "..")) return rel;
    return cc::tilde_path(path);
}

std::string user_facing_tool_name(const Tool& tool, const boost::json::object& input) {
    const std::string_view name = tool.name();
    if (name == cc::tools::READ) {
        if (is_plan_file(cc::jget_string(input, "file_path"))) return "Reading Plan";
        return "Read";
    }
    if (name == cc::tools::EDIT) {
        if (is_plan_file(cc::jget_string(input, "file_path"))) return "Updated plan";
        // An empty `old_string` means the file did not exist yet.
        if (input.contains("old_string") && cc::jget_string(input, "old_string").empty()) return "Create";
        return "Update";
    }
    if (name == cc::tools::WRITE) {
        if (is_plan_file(cc::jget_string(input, "file_path"))) return "Updated plan";
        return "Write";
    }
    if (name == cc::tools::BASH) return "Bash";
    // Both search tools are called "Search", the way Claude Code names them.
    if (name == cc::tools::GLOB || name == cc::tools::GREP) return "Search";
    if (name == cc::tools::ASK_USER_QUESTION) return "AskUserQuestion";
    if (name == cc::tools::AGENT) {
        // The agent of the tool is named by the kind of agent it is; the
        // general-purpose one and the worker are called "Agent".
        const std::string type = cc::jget_string(input, "subagent_type");
        if (!type.empty() && type != cc::tools::kGeneralPurposeAgentType && type != "worker") {
            return type;
        }
        return "Agent";
    }
    return std::string(name);
}

namespace {

/// The number of lines of a command the reference shows.
constexpr size_t kMaxCommandDisplayLines = 2;

/// The number of characters of a command the reference shows.
constexpr size_t kMaxCommandDisplayChars = 160;

/// Truncates a command the way the reference does: by lines first, then by
/// characters.
std::string truncate_command(const std::string& command) {
    const auto lines = cc::split_lines(command);
    std::string truncated = command;
    bool truncated_by_lines = false;

    if (lines.size() > kMaxCommandDisplayLines) {
        truncated.clear();
        for (size_t i = 0; i < kMaxCommandDisplayLines; ++i) {
            if (i > 0) truncated.push_back('\n');
            truncated += lines[i];
        }
        truncated_by_lines = true;
    }

    if (truncated.size() > kMaxCommandDisplayChars) {
        truncated = truncated.substr(0, kMaxCommandDisplayChars);
        truncated_by_lines = true;
    }

    return truncated_by_lines ? truncated + "…" : truncated;
}

} // namespace

/// The `file://` URL of a path, the way the reference links to it.
std::string file_url(const std::string& path) {
    if (path.empty()) return {};

    std::string url = "file://";
    // The path is already absolute, so it follows the host straight away.
    for (const char c : path) {
        if (c == ' ') {
            url += "%20";
        } else {
            url += c;
        }
    }
    return url;
}

std::string user_facing_tool_args(const Tool& tool, const boost::json::object& input,
                                  const std::string& cwd) {
    const std::string_view name = tool.name();
    if (name == cc::tools::READ || name == cc::tools::EDIT || name == cc::tools::WRITE) {
        const std::string path = cc::jget_string(input, "file_path");
        // A file of the plan is named by the tool that writes it, so the
        // path is left out of it.
        if (name != cc::tools::READ && is_plan_file(path)) return {};
        // The reference links the path, so the terminal recognises it.
        return hyperlink(relative_path(path, cwd), file_url(cc::resolve(path, cwd)));
    }
    if (name == cc::tools::BASH) {
        return truncate_command(cc::jget_string(input, "command"));
    }
    if (name == cc::tools::GLOB || name == cc::tools::GREP) {
        // The search tools show the pattern, and the directory when there
        // is one.
        std::string args = "pattern: \"" + cc::jget_string(input, "pattern") + "\"";
        const std::string path = cc::jget_string(input, "path");
        if (!path.empty()) {
            args += ", path: \"" + relative_path(path, cwd) + "\"";
        }
        return args;
    }
    if (name == cc::tools::AGENT) {
        // The reference names the agent by its description.
        const std::string description = cc::jget_string(input, "description");
        if (!description.empty()) return description;
        return cc::jget_string(input, "prompt");
    }
    return {};
}

std::string render_tool_use_line(const Tool& tool, const boost::json::object& input,
                                 const std::string& cwd, const Theme& theme, ToolCallState state,
                                 bool marker_visible) {
    // The question tool has no name of its own in Claude Code: the block
    // that reports the answers carries the marker instead. The same goes for
    // the todo and plan tools.
    if (tool.name() == cc::tools::ASK_USER_QUESTION) return {};
    if (tool.name() == cc::tools::TODO_WRITE) return {};
    if (tool.name() == cc::tools::EXIT_PLAN_MODE) return {};

    Style dot_style;
    switch (state) {
        case ToolCallState::Queued:
        case ToolCallState::Running:
            dot_style.dim = true;
            break;
        case ToolCallState::Succeeded:
            dot_style.foreground = theme.success;
            break;
        case ToolCallState::Failed:
            dot_style.foreground = theme.error;
            break;
    }

    Style name_style;
    name_style.bold = true;

    std::string line = styled(marker_visible ? kToolDot : std::string(" "), dot_style);
    // The dot column is two cells wide.
    line += ' ';
    line += styled(user_facing_tool_name(tool, input), name_style);

    const std::string args = user_facing_tool_args(tool, input, cwd);
    if (!args.empty()) {
        line += '(';
        line += args;
        line += ')';
    }
    return line;
}

std::vector<std::string> render_diff_result(const std::string& before, const std::string& after,
                                            const Theme& theme, int width) {
    const auto lines = diff_lines(before, after);

    std::vector<std::string> out;
    out.push_back(std::string(figures::kMessageResponse) + styled(diff_summary(lines), Style{}));

    // The gutter is drawn once, and the body of the diff is indented under
    // it, the way the reference draws it. The diff leaves the room of the
    // gutter and of the margin of the terminal, so that it does not wrap.
    for (const auto& line : render_diff_hunks(lines, theme, width > 12 ? width - 12 : width)) {
        out.push_back(std::string(kResultGutter, ' ') + line);
    }
    return out;
}

std::vector<std::string> render_tool_result_lines(const std::string& content, const Theme& theme, int width,
                                                  bool is_error, bool truncate) {
    Style style;
    style.foreground = is_error ? theme.error : theme.text;

    std::vector<OutputLine> lines;
    for (const auto& raw : cc::split_lines(content)) {
        lines.push_back({raw, style});
    }
    return render_output_lines(std::move(lines), theme, width, truncate);
}

namespace {

/// The number of lines of a written file the reference previews.
constexpr size_t kWritePreviewLines = 10;

/// The singular or plural form of `word`.
///
/// The reference keeps the plural when the count is zero, so an empty
/// search reads "Found 0 files".
std::string pluralize(std::string_view word, int64_t count) {
    if (count == 0 || count > 1) return std::string(word);
    return std::string(word.substr(0, word.size() - 1));
}

/// The number of lines in `content`.
///
/// A trailing newline terminates the last line rather than starting a new
/// one, which is how an editor counts them.
size_t count_lines(std::string_view content) {
    if (content.empty()) return 0;
    size_t lines = 1;
    for (const char c : content) {
        if (c == '\n') ++lines;
    }
    if (content.back() == '\n') --lines;
    return lines;
}

/// Puts the `  ⎿  ` gutter in front of the first line and indents the rest.
std::vector<std::string> with_gutter(const std::vector<std::string>& lines, const Theme& theme) {
    Style gutter_style;
    gutter_style.foreground = theme.inactive;

    std::vector<std::string> out;
    out.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string gutter = i == 0 ? styled(std::string(kResultGutterText), gutter_style)
                                          : std::string(kResultGutter, ' ');
        out.push_back(gutter + lines[i]);
    }
    return out;
}

/// The summary of a `Read`: `Read 42 lines`.
std::vector<std::string> read_summary(const boost::json::object& payload, const Theme& theme) {
    Style bold;
    bold.bold = true;

    if (jget_string(payload, "type") == "file_unchanged") {
        Style dim;
        dim.foreground = theme.inactive;
        return {styled("Unchanged since last read", dim)};
    }

    const int64_t lines = jget_int(payload, "numLines", 0);
    return {"Read " + styled(std::to_string(lines), bold) + " " + pluralize("lines", lines)};
}

/// The language a file's extension names, the way the reference's code
/// highlighter picks it.
std::string language_for_path(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return {};
    std::string extension = path.substr(dot + 1);
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension == "cpp" || extension == "cc" || extension == "cxx" || extension == "hpp" ||
        extension == "h") {
        return "cpp";
    }
    if (extension == "py") return "python";
    if (extension == "js" || extension == "jsx" || extension == "mjs") return "js";
    if (extension == "ts" || extension == "tsx") return "ts";
    if (extension == "rb") return "ruby";
    if (extension == "sh" || extension == "zsh") return "bash";
    if (extension == "yml") return "yaml";
    if (extension == "md") return "markdown";
    return extension;
}

/// The summary of a `Write`: `Wrote 12 lines to src/main.cpp`, followed by
/// the first few lines of the file.
///
/// A write that replaced an existing file shows the diff instead, the way
/// Claude Code does.
std::vector<std::string> write_summary(const boost::json::object& payload, const std::string& cwd,
                                       const Theme& theme, int width, bool truncate) {
    Style bold;
    bold.bold = true;

    const std::string content = jget_string(payload, "content");
    const std::string path = jget_string(payload, "filePath");
    const size_t lines = count_lines(content);

    std::vector<std::string> out;

    if (jget_string(payload, "type") == "update") {
        const auto diff = diff_lines(jget_string(payload, "originalFile"), content);
        out.push_back(diff_summary(diff));
        for (const auto& line : render_diff_hunks(diff, theme, width > 12 ? width - 12 : width)) {
            out.push_back(line);
        }
        return out;
    }

    // The reference writes the path relative to the folder the session runs
    // in, so that a file outside of it is shown with the way back to it.
    const std::string base = cwd.empty() ? cc::cwd() : cwd;
    out.push_back("Wrote " + styled(std::to_string(lines), bold) + " lines to " +
                  styled(cc::relative_from(base, cc::resolve(path, base)), bold));

    // The reference previews the first ten lines of the file, in a box
    // that leaves twelve columns of room around it.
    const auto content_lines = cc::split_lines(content);
    const size_t shown = truncate ? std::min(kWritePreviewLines, content_lines.size())
                                  : content_lines.size();
    if (shown > 0) {
        std::string preview;
        for (size_t i = 0; i < shown; ++i) {
            if (i > 0) preview.push_back('\n');
            preview += content_lines[i];
        }
        const int preview_width = width > 12 ? width - 12 : width;
        const auto rendered = highlight_code(preview, language_for_path(path), theme, preview_width);
        for (const auto& line : rendered) {
            out.push_back(line);
        }
    }

    if (truncate && lines > kWritePreviewLines) {
        const size_t extra = lines - kWritePreviewLines;
        Style dim;
        dim.foreground = theme.inactive;
        out.push_back(styled("… +" + std::to_string(extra) + (extra == 1 ? " line" : " lines") +
                                 " (ctrl+o to expand)",
                             dim));
    }
    return out;
}

/// The summary of a `Glob` or a `Grep`: `Found 3 files`.
std::vector<std::string> search_summary(const boost::json::object& payload, const Theme& theme) {
    Style bold;
    bold.bold = true;

    // The reference says that the result can be expanded when something was
    // found.
    Style dim;
    dim.foreground = theme.inactive;
    const auto hint = [&] { return std::string(" ") + styled("(ctrl+o to expand)", dim); };

    const std::string mode = jget_string(payload, "mode", "files_with_matches");

    if (mode == "content") {
        const int64_t lines = jget_int(payload, "numLines", 0);
        return {"Found " + styled(std::to_string(lines) + " ", bold) + pluralize("lines", lines) +
                (lines > 0 ? hint() : std::string())};
    }

    if (mode == "count") {
        const int64_t matches = jget_int(payload, "numMatches", 0);
        const int64_t files = jget_int(payload, "numFiles", 0);
        return {"Found " + styled(std::to_string(matches) + " ", bold) + pluralize("matches", matches) +
                " across " + styled(std::to_string(files) + " ", bold) + pluralize("files", files) +
                (matches > 0 ? hint() : std::string())};
    }

    const int64_t files = jget_int(payload, "numFiles", 0);
    return {"Found " + styled(std::to_string(files) + " ", bold) + pluralize("files", files) +
            (files > 0 ? hint() : std::string())};
}

/// The result of an `ExitPlanMode`: the plan was approved.
///
/// An empty plan only says that the plan mode is over; a plan that has
/// content names the approval and shows where the plan was saved.
std::vector<std::string> plan_result(const boost::json::object& payload, const std::string& cwd,
                                      const Theme& theme, int width) {
    Style marker;
    marker.foreground = theme.plan_mode;

    const std::string plan = jget_string(payload, "plan");
    if (cc::trim(plan).empty()) {
        return {styled(std::string(figures::kBlackCircle), marker) + " Exited plan mode"};
    }

    std::vector<std::string> out;
    out.push_back(styled(std::string(figures::kBlackCircle), marker) + " User approved Claude's plan");

    Style dim;
    dim.dim = true;
    // The plan is one block: the gutter stands in front of its first line
    // only, the way Claude Code draws it.
    std::vector<std::string> block;
    const std::string file_path = jget_string(payload, "filePath");
    if (!file_path.empty()) {
        block.push_back(
            styled("Plan saved to: " + relative_path(file_path, cwd) + " · /plan to edit", dim));
    }
    for (const auto& line : render_markdown(plan, theme, std::max(1, width - 5))) {
        block.push_back(line);
    }
    for (const auto& line : with_gutter(block, theme)) {
        out.push_back(line);
    }
    return out;
}

/// The result of the `Agent` tool: what the sub-agent did.
std::vector<std::string> agent_result(const boost::json::object& payload, const Theme& theme) {
    const int64_t tool_uses = jget_int(payload, "totalToolUseCount", 0);
    const int64_t tokens = jget_int(payload, "totalTokens", 0);
    const int64_t duration = jget_int(payload, "totalDurationMs", 0);

    Style dim;
    dim.foreground = theme.inactive;

    std::vector<std::string> out;
    out.push_back(std::string(figures::kMessageResponse) +
                  "Done (" + std::string(tool_uses == 1 ? "1 tool use" :
                                                          std::to_string(tool_uses) + " tool uses") +
                  " · " + cc::format_number(tokens) + " tokens · " + format_duration(duration) + ")");
    out.push_back(std::string("  ") + styled("(ctrl+o to expand)", dim));
    return out;
}

/// The result of an `AskUserQuestion`: the answers the user gave.
///
/// The reference gives the tool no name of its own, so the block that
/// reports the answers carries the `⏺` marker instead.
std::vector<std::string> question_result(const boost::json::object& payload, const Theme& theme) {
    Style marker;
    marker.foreground = theme.text;

    Style dim;
    dim.foreground = theme.inactive;

    std::vector<std::string> out;
    out.push_back(styled(std::string(figures::kBlackCircle), marker) +
                  " User answered Claude's questions:");

    const auto answers = payload.find("answers");
    if (answers == payload.end() || !answers->value().is_object()) return out;

    // The answers are a block of their own: the gutter stands in front of
    // the first one only, the way Claude Code draws it.
    std::vector<std::string> block;
    for (const auto& [question_text, answer] : answers->value().as_object()) {
        const std::string text = "· " + std::string(question_text) + " → " +
                                 (answer.is_string() ? std::string(answer.as_string().c_str())
                                                     : json_dump(answer));
        block.push_back(styled(text, dim));
    }
    for (const auto& line : with_gutter(block, theme)) {
        out.push_back(line);
    }
    return out;
}

} // namespace

std::vector<std::string> render_tool_result_lines(std::string_view tool_name, const std::string& content,
                                                  const boost::json::value& structured,
                                                  const std::string& cwd, const Theme& theme, int width,
                                                  bool is_error, bool verbose) {
    // The plan tool reports the plan that was approved.
    if (tool_name == cc::tools::EXIT_PLAN_MODE && !is_error) {
        boost::json::object payload;
        if (structured.is_object()) payload = structured.as_object();
        return plan_result(payload, cwd, theme, width);
    }

    if (structured.is_object()) {
        const auto& payload = structured.as_object();
        if (tool_name == cc::tools::READ) {
            return with_gutter(read_summary(payload, theme), theme);
        }
        if (tool_name == cc::tools::WRITE) {
            return with_gutter(write_summary(payload, cwd, theme, width, !verbose), theme);
        }
        if (tool_name == cc::tools::GLOB || tool_name == cc::tools::GREP) {
            return with_gutter(search_summary(payload, theme), theme);
        }
        if (tool_name == cc::tools::ASK_USER_QUESTION) {
            return question_result(payload, theme);
        }
        if (tool_name == cc::tools::AGENT) {
            return agent_result(payload, theme);
        }
        if (tool_name == cc::tools::BASH) {
            return bash_summary(payload, theme, width, !verbose);
        }
    }

    // The todo tool has nothing to show: Claude Code draws no line for it.
    if (tool_name == cc::tools::TODO_WRITE) return {};

    // A tool that failed shows its error, the way the reference shows it.
    if (is_error) return render_tool_error_lines(content, theme, width, verbose);

    // The tool brought no summary of its own: its own output is shown. The
    // transcript asks for the whole of it, the way it asks for the whole of
    // every other result.
    return render_tool_result_lines(content, theme, width, is_error, !verbose);
}

std::vector<std::string> render_tool_error_lines(const std::string& content, const Theme& theme,
                                                  int width, bool verbose) {
    (void)width;

    // The reference takes the text out of the tags it puts around the error,
    // so that the tags are never shown.
    std::string error = extract_tag(content, "tool_use_error");
    if (error.empty()) error = content;
    error = remove_sandbox_violation_tags(error);
    error = remove_error_tags(error);
    error = cc::trim(error);

    // The reference names the error, unless the error names itself.
    if (!verbose && error.find("InputValidationError: ") != std::string::npos) {
        error = "Invalid tool parameters";
    } else if (!cc::starts_with(error, "Error: ") && !cc::starts_with(error, "Cancelled: ")) {
        error = "Error: " + error;
    }

    Style error_style;
    error_style.foreground = theme.error;

    Style gutter_style;
    gutter_style.foreground = theme.inactive;

    const auto all_lines = cc::split_lines(error);

    // The reference shows ten lines of the error.
    constexpr size_t kErrorLines = 10;
    const size_t shown = verbose ? all_lines.size() : std::min(all_lines.size(), kErrorLines);

    std::vector<std::string> out;
    bool first = true;
    for (size_t i = 0; i < shown; ++i) {
        const std::string gutter =
            first ? styled(std::string(kResultGutterText), gutter_style)
                  : std::string(kResultGutter, ' ');
        out.push_back(gutter + styled(all_lines[i], error_style));
        first = false;
    }

    if (!verbose && all_lines.size() > shown) {
        const size_t extra = all_lines.size() - shown;
        Style dim;
        dim.foreground = theme.inactive;
        Style dim_bold;
        dim_bold.foreground = theme.inactive;
        dim_bold.bold = true;
        // The line that says how many lines are left is indented under the
        // error, the way the reference indents it.
        out.push_back(std::string(kResultGutter, ' ') +
                      styled("… +" + std::to_string(extra) + (extra == 1 ? " line" : " lines") + " (",
                             dim) +
                      styled("ctrl+o", dim_bold) + styled(" to see all)", dim));
    }
    return out;
}

} // namespace cc::ui
