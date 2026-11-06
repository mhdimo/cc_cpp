#include "tools/impl/file_tools.hpp"

#include "core/error.hpp"
#include "permissions/content_match.hpp"
#include "tools/tool_use_context.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace cc::tools {
namespace {

/// The maximum number of lines the Read tool returns by default.
constexpr int kMaxLinesToRead = 2000;

/// The largest file the Edit tool will modify.
constexpr int64_t kMaxEditFileSize = 1024LL * 1024 * 1024;

/// The device files the Read tool refuses to read.
bool is_blocked_device(const std::string& path) {
    static const std::vector<std::string> blocked = {
        "/dev/zero", "/dev/random", "/dev/urandom", "/dev/full", "/dev/stdin",
        "/dev/tty",  "/dev/console", "/dev/stdout", "/dev/stderr"};
    for (const auto& entry : blocked) {
        if (path == entry) return true;
    }
    if (path.rfind("/dev/fd/", 0) == 0) return true;
    if (path.find("/proc/") != std::string::npos && path.find("/fd/") != std::string::npos) {
        return true;
    }
    return false;
}

/// True when the file looks like binary content.
bool looks_binary(const std::string& contents) {
    const size_t sample = std::min<size_t>(contents.size(), 8000);
    for (size_t i = 0; i < sample; ++i) {
        const unsigned char c = static_cast<unsigned char>(contents[i]);
        if (c == 0) return true;
    }
    return false;
}

/// Numbers the lines of `contents` the way `cat -n` does.
///
/// The compact form is `<line number>\t<line>`; the legacy form is
/// `<line number padded to 6>→<line>`.
std::string add_line_numbers(const std::string& contents, size_t start_line, bool compact) {
    std::string out;
    const auto lines = split_lines(contents);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out.push_back('\n');
        const size_t number = start_line + i;
        const std::string number_text = std::to_string(number);
        if (compact) {
            out += number_text;
            out.push_back('\t');
        } else {
            if (number_text.size() < 6) {
                out.append(6 - number_text.size(), ' ');
            }
            out += number_text;
            out += "→";
        }
        out += lines[i];
    }
    return out;
}

/// The message Claude Code shows when a file is missing.
std::string file_not_found_message(const std::string& path, const std::string& cwd) {
    std::string message = "File does not exist. Note: your current working directory is " + cwd + ".";
    const std::string suggestion = did_you_mean(path, cwd);
    if (!suggestion.empty()) {
        message += " Did you mean " + suggestion + "?";
    }
    return message;
}

/// The typographic quotes Claude Code recognises.
constexpr std::string_view kLeftSingleQuote = "\xE2\x80\x98";  // '
constexpr std::string_view kRightSingleQuote = "\xE2\x80\x99";  // '
constexpr std::string_view kLeftDoubleQuote = "\xE2\x80\x9C";  // "
constexpr std::string_view kRightDoubleQuote = "\xE2\x80\x9D";  // "

/// The number of bytes in the UTF-8 sequence that starts with `lead`.
///
/// A byte that cannot start a sequence counts as a single byte.
size_t utf8_sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

/// The straight quote that stands in for the curly quote at `text[index]`.
///
/// Returns '\0' when the text there is not a curly quote, and reports the
/// length of the sequence it read through `length`.
char straight_quote_for(std::string_view text, size_t index, size_t* length) {
    if (index + 2 >= text.size()) return '\0';
    if (static_cast<unsigned char>(text[index]) != 0xE2) return '\0';
    if (static_cast<unsigned char>(text[index + 1]) != 0x80) return '\0';

    switch (static_cast<unsigned char>(text[index + 2])) {
    case 0x98:
    case 0x99:
        *length = 3;
        return '\'';
    case 0x9C:
    case 0x9D:
        *length = 3;
        return '"';
    default:
        return '\0';
    }
}

/// Maps a byte offset in the quote-normalised text back to the byte offset in
/// the original text.
///
/// Normalising replaces the three-byte curly quotes with a single byte, so
/// the two offsets drift apart as soon as the text holds a curly quote.
size_t original_offset(std::string_view text, size_t normalized_offset) {
    size_t produced = 0;
    size_t i = 0;
    while (i < text.size() && produced < normalized_offset) {
        size_t length = 0;
        const char quote = straight_quote_for(text, i, &length);
        if (quote != '\0') {
            produced += 1;
            i += length;
            continue;
        }
        const size_t sequence =
            std::min(utf8_sequence_length(static_cast<unsigned char>(text[i])), text.size() - i);
        produced += sequence;
        i += sequence;
    }
    return i;
}

/// True when `text` holds `sequence`.
bool contains_utf8(std::string_view text, std::string_view sequence) {
    return text.find(sequence) != std::string_view::npos;
}

/// Splits `text` into its UTF-8 code points, keeping the original bytes.
std::vector<std::string> code_points(std::string_view text) {
    std::vector<std::string> parts;
    for (size_t i = 0; i < text.size();) {
        const size_t length =
            std::min(utf8_sequence_length(static_cast<unsigned char>(text[i])), text.size() - i);
        parts.emplace_back(text.substr(i, length));
        i += length;
    }
    return parts;
}

/// The code point of the UTF-8 sequence `part`.
unsigned int code_point_value(const std::string& part) {
    if (part.empty()) return 0;
    const unsigned char lead = static_cast<unsigned char>(part[0]);
    if (lead < 0x80) return lead;
    if ((lead & 0xE0) == 0xC0 && part.size() >= 2) {
        return ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(part[1]) & 0x3Fu);
    }
    if ((lead & 0xF0) == 0xE0 && part.size() >= 3) {
        return ((lead & 0x0Fu) << 12) | ((static_cast<unsigned char>(part[1]) & 0x3Fu) << 6) |
               (static_cast<unsigned char>(part[2]) & 0x3Fu);
    }
    if ((lead & 0xF8) == 0xF0 && part.size() >= 4) {
        return ((lead & 0x07u) << 18) | ((static_cast<unsigned char>(part[1]) & 0x3Fu) << 12) |
               ((static_cast<unsigned char>(part[2]) & 0x3Fu) << 6) |
               (static_cast<unsigned char>(part[3]) & 0x3Fu);
    }
    return lead;
}

/// True when `code_point` is a letter.
bool is_letter(unsigned int code_point) {
    if (code_point < 128) {
        return std::isalpha(static_cast<int>(code_point)) != 0;
    }
    // The Latin-1 supplement, minus the multiplication and division signs.
    if (code_point >= 0xC0 && code_point <= 0xFF) {
        return code_point != 0xD7 && code_point != 0xF7;
    }
    // The Latin Extended-A block.
    if (code_point >= 0x100 && code_point <= 0x17F) return true;
    return false;
}

/// True when the quote at `index` opens a quotation.
///
/// A quote preceded by whitespace, the start of the text, or opening
/// punctuation opens a quotation; anything else closes one.
bool is_opening_context(const std::vector<std::string>& parts, size_t index) {
    if (index == 0) return true;
    const std::string& previous = parts[index - 1];
    if (previous == " " || previous == "\t" || previous == "\n" || previous == "\r") return true;
    if (previous == "(" || previous == "[" || previous == "{") return true;
    // The em dash and the en dash.
    if (previous == "\xE2\x80\x94" || previous == "\xE2\x80\x93") return true;
    return false;
}

/// Rewrites the straight double quotes of `parts` as curly ones.
std::string apply_curly_double_quotes(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] == "\"") {
            out += is_opening_context(parts, i) ? kLeftDoubleQuote : kRightDoubleQuote;
        } else {
            out += parts[i];
        }
    }
    return out;
}

/// Rewrites the straight single quotes of `parts` as curly ones.
std::string apply_curly_single_quotes(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i] != "'") {
            out += parts[i];
            continue;
        }
        // An apostrophe between two letters is a contraction, not a quote.
        const bool previous_is_letter = i > 0 && is_letter(code_point_value(parts[i - 1]));
        const bool next_is_letter = i + 1 < parts.size() && is_letter(code_point_value(parts[i + 1]));
        if (previous_is_letter && next_is_letter) {
            out += kRightSingleQuote;
        } else {
            out += is_opening_context(parts, i) ? kLeftSingleQuote : kRightSingleQuote;
        }
    }
    return out;
}

/// Replaces curly quotes with straight ones, so a model's ASCII quotes match
/// a file that was written with typographic quotes.
std::string normalize_quotes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        size_t length = 0;
        const char quote = straight_quote_for(text, i, &length);
        if (quote != '\0') {
            out.push_back(quote);
            i += length;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

/// Finds the text in `file` that `old_string` refers to, tolerating a
/// difference in quote style.
///
/// The result is the matched text as it appears in `file`, so that the caller
/// can splice the file without disturbing the rest of it.
std::optional<std::string> find_actual_string(const std::string& file,
                                              const std::string& old_string) {
    if (file.find(old_string) != std::string::npos) return old_string;

    const std::string needle = normalize_quotes(old_string);
    const std::string haystack = normalize_quotes(file);
    const size_t found = haystack.find(needle);
    if (found == std::string::npos) return std::nullopt;

    // The offsets of the normalised text do not match those of the file: a
    // curly quote takes three bytes there and one byte here.
    const size_t start = original_offset(file, found);
    const size_t end = original_offset(file, found + needle.size());
    return file.substr(start, end - start);
}

/// Applies the quote style of the file to `new_string`.
///
/// When the old string was matched through quote normalisation the file uses
/// typographic quotes, and the replacement should keep them.
std::string preserve_quote_style(const std::string& old_string, const std::string& actual_old_string,
                                 const std::string& new_string) {
    // Nothing was normalised, so the file's style does not apply.
    if (old_string == actual_old_string) return new_string;

    const bool has_double = contains_utf8(actual_old_string, kLeftDoubleQuote) ||
                            contains_utf8(actual_old_string, kRightDoubleQuote);
    const bool has_single = contains_utf8(actual_old_string, kLeftSingleQuote) ||
                            contains_utf8(actual_old_string, kRightSingleQuote);
    if (!has_double && !has_single) return new_string;

    std::vector<std::string> parts = code_points(new_string);
    if (has_double) parts = code_points(apply_curly_double_quotes(parts));
    if (has_single) parts = code_points(apply_curly_single_quotes(parts));

    std::string out;
    for (const auto& part : parts) out += part;
    return out;
}

/// Reads an optional integer member, returning `std::nullopt` when absent.
std::optional<int64_t> optional_int(const boost::json::object& input, std::string_view key) {
    const auto it = input.find(std::string(key));
    if (it == input.end()) return std::nullopt;
    const auto& value = it->value();
    if (value.is_int64()) return value.as_int64();
    if (value.is_uint64()) return static_cast<int64_t>(value.as_uint64());
    if (value.is_double()) return static_cast<int64_t>(value.as_double());
    return std::nullopt;
}

/// Builds the payload the UI reads for a `Read` result.
boost::json::object read_payload(const std::string& path, std::string_view type, size_t num_lines,
                                 size_t total_lines) {
    boost::json::object payload;
    payload["type"] = std::string(type);
    payload["filePath"] = path;
    payload["numLines"] = static_cast<int64_t>(num_lines);
    payload["totalLines"] = static_cast<int64_t>(total_lines);
    return payload;
}

/// Counts the lines of `path`, or 0 when it cannot be read.
size_t count_lines_of(const std::string& path) {
    const auto contents = read_file(path);
    if (!contents) return 0;
    return split_lines(*contents).size();
}

/// The result of the rules of the paths that match a file.
///
/// The wording is the one of the reference: a file that may not be read is
/// denied, a file that may not be changed is denied as well.
PermissionResult path_permission_result(const permissions::ContentRuleMatch& match,
                                         const std::string& path, const boost::json::object& input,
                                         bool reading) {
    if (match.deny.has_value()) {
        PermissionResult result;
        result.behavior = PermissionBehaviorKind::Deny;
        result.message = reading ? "Permission to read " + path + " has been denied."
                                 : "Permission to edit " + path + " has been denied.";
        result.decision_reason = permissions::rule_decision_reason(*match.deny);
        return result;
    }

    if (match.ask.has_value()) {
        PermissionResult result;
        result.behavior = PermissionBehaviorKind::Ask;
        result.message =
            reading ? "Claude requested permissions to read from " + path +
                          ", but you haven't granted it yet."
                    : "Claude requested permissions to write to " + path +
                          ", but you haven't granted it yet.";
        result.decision_reason = permissions::rule_decision_reason(*match.ask);
        return result;
    }

    if (match.allow.has_value()) {
        PermissionResult result;
        result.behavior = PermissionBehaviorKind::Allow;
        result.updated_input = input;
        result.decision_reason = permissions::rule_decision_reason(*match.allow);
        return result;
    }

    PermissionResult result;
    result.behavior = PermissionBehaviorKind::Passthrough;
    result.message = create_permission_request_message(reading ? "Read" : "Edit");
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

ReadTool::ReadTool(std::shared_ptr<FileStateTracker> tracker) : tracker_(std::move(tracker)) {}

boost::json::object ReadTool::input_schema() const {
    return schema_object(
        {
            {"file_path", schema_string("The absolute path to the file to read")},
            {"offset",
             schema_integer("The line number to start reading from. Only provide if the file is "
                            "too large to read at once")},
            {"limit", schema_integer("The number of lines to read. Only provide if the file is too "
                                     "large to read at once.")},
            {"pages", schema_string("Page range for PDF files (e.g., \"1-5\", \"3\", \"10-20\"). "
                                    "Only applicable to PDF files. Maximum 20 pages per request.")},
        },
        {"file_path"});
}

std::optional<std::string> ReadTool::get_path(const boost::json::object& input) const {
    const std::string path = jget_string(input, "file_path");
    if (path.empty()) return std::nullopt;
    return expand_path(path);
}

PermissionResult ReadTool::check_permissions(const boost::json::object& input,
                                             ToolUseContext& context) const {
    const auto path = get_path(input);
    if (!path.has_value()) return {};

    const auto match = permissions::match_content_rules(context.permission_context, *this, input,
                                                        permissions::rule_roots(context));
    return path_permission_result(match, *path, input, /*reading=*/true);
}

std::string ReadTool::prompt() const {
    return read_tool_prompt(/*include_max_size=*/false, /*max_size_bytes=*/0,
                            /*targeted_offset=*/false, pdf_supported_);
}

ToolResult ReadTool::call(const boost::json::object& input, ToolUseContext& context) {
    const std::string requested = jget_string(input, "file_path");
    const std::string path = resolve(expand_path(requested), context.options.cwd);

    if (is_blocked_device(path)) {
        throw ToolError("Cannot read '" + path +
                            "': this device file would block or produce infinite output.",
                        "Read");
    }

    if (!file_exists(path)) {
        throw ToolError(file_not_found_message(path, context.options.cwd), "Read");
    }

    if (is_directory(path)) {
        throw ToolError("Cannot read '" + path + "': this is a directory.", "Read");
    }

    // The dedup key uses the offsets Claude Code uses: the offset is 1 when
    // the model did not pass one, and the limit is absent in that case too.
    const int64_t dedup_offset = jget_int(input, "offset", 1);
    const std::optional<int64_t> dedup_limit = optional_int(input, "limit");

    // A repeat read of the same range is answered with a stub: the earlier
    // tool_result is still in the conversation, and sending the content twice
    // only wastes cache tokens.
    if (tracker_->range_unchanged_since_read(path, dedup_offset, dedup_limit)) {
        ToolResult result;
        result.content = kFileUnchangedStub;
        result.tool_use_result =
            boost::json::value(read_payload(path, "file_unchanged", 0, count_lines_of(path)));
        return result;
    }

    const auto contents = read_file(path);
    if (!contents) {
        throw ToolError("Cannot read '" + path + "'.", "Read");
    }

    if (looks_binary(*contents)) {
        const std::string file_extension = extension(path);
        throw ToolError("This tool cannot read binary files. The file appears to be a binary " +
                            (file_extension.empty() ? std::string("file")
                                                   : file_extension + " file") +
                            ". Please use appropriate tools for binary file analysis.",
                        "Read");
    }

    const int64_t offset = jget_int(input, "offset", 0);
    const int64_t limit = jget_int(input, "limit", 0);

    const auto lines = split_lines(*contents);
    const size_t total_lines = lines.size();

    // `offset` is 1-based; 0 means the very beginning.
    const size_t start_index = offset <= 0 ? 0 : static_cast<size_t>(offset - 1);
    const size_t start_line = start_index + 1;

    if (start_index > total_lines) {
        ToolResult result;
        result.content =
            system_reminder("Warning: the file exists but is shorter than the provided offset (" +
                            std::to_string(start_line) + "). The file has " +
                            std::to_string(total_lines) + " lines.");
        result.tool_use_result = boost::json::value(read_payload(path, "text", 0, total_lines));
        return result;
    }

    size_t end_index = total_lines;
    if (limit > 0) {
        end_index = std::min(total_lines, start_index + static_cast<size_t>(limit));
    } else {
        end_index = std::min(total_lines, start_index + kMaxLinesToRead);
    }

    if (start_index >= end_index) {
        ToolResult result;
        result.content = system_reminder("Warning: the file exists but the contents are empty.");
        result.tool_use_result = boost::json::value(read_payload(path, "text", 0, total_lines));
        return result;
    }

    std::string selected;
    for (size_t i = start_index; i < end_index; ++i) {
        if (i > start_index) selected.push_back('\n');
        selected += lines[i];
    }

    tracker_->record_read(path, dedup_offset, dedup_limit);

    std::string content = add_line_numbers(selected, start_line, true);
    content += kCyberRiskMitigationReminder;

    ToolResult result;
    result.content = content;
    result.tool_use_result =
        boost::json::value(read_payload(path, "text", end_index - start_index, total_lines));
    return result;
}

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------

WriteTool::WriteTool(std::shared_ptr<FileStateTracker> tracker) : tracker_(std::move(tracker)) {}

boost::json::object WriteTool::input_schema() const {
    return schema_object(
        {
            {"file_path",
             schema_string("The absolute path to the file to write (must be absolute, not "
                           "relative)")},
            {"content", schema_string("The content to write to the file")},
        },
        {"file_path", "content"});
}

std::optional<std::string> WriteTool::get_path(const boost::json::object& input) const {
    const std::string path = jget_string(input, "file_path");
    if (path.empty()) return std::nullopt;
    return expand_path(path);
}

PermissionResult WriteTool::check_permissions(const boost::json::object& input,
                                              ToolUseContext& context) const {
    const auto path = get_path(input);
    if (!path.has_value()) return {};

    const auto match = permissions::match_content_rules(context.permission_context, *this, input,
                                                        permissions::rule_roots(context));
    return path_permission_result(match, *path, input, /*reading=*/false);
}

std::string WriteTool::prompt() const { return write_tool_prompt(); }

ToolResult WriteTool::call(const boost::json::object& input, ToolUseContext& context) {
    const std::string requested = jget_string(input, "file_path");
    const std::string path = resolve(expand_path(requested), context.options.cwd);
    const std::string content = jget_string(input, "content");

    const bool existed = file_exists(path);

    // A read that covered only part of the file still allows writing it.
    if (existed && !tracker_->was_read(path)) {
        throw ToolError("File has not been read yet. Read it first before writing to it.",
                        "Write");
    }
    if (existed && !tracker_->unchanged_since_read(path)) {
        throw ToolError(
            "File has been modified since read, either by the user or by a linter. Read it again "
            "before attempting to write it.",
            "Write");
    }

    // The content the file held before the write, for the diff the UI shows.
    const std::string original_content =
        existed ? read_file(path).value_or(std::string()) : std::string();

    // The file is copied aside before it is changed, so that the change can
    // be undone.
    if (context.on_file_edit) context.on_file_edit(path);

    std::string error;
    if (!write_file(path, content, &error)) {
        throw ToolError("Error writing file: " + error, "Write");
    }

    tracker_->record_write(path);

    ToolResult result;
    result.content = existed ? "The file " + path + " has been updated successfully."
                              : "File created successfully at: " + path;

    boost::json::object payload;
    payload["type"] = existed ? "update" : "create";
    payload["filePath"] = path;
    payload["content"] = content;
    if (existed) payload["originalFile"] = original_content;
    result.tool_use_result = boost::json::value(payload);
    return result;
}

// ---------------------------------------------------------------------------
// Edit
// ---------------------------------------------------------------------------

EditTool::EditTool(std::shared_ptr<FileStateTracker> tracker) : tracker_(std::move(tracker)) {}

boost::json::object EditTool::input_schema() const {
    return schema_object(
        {
            {"file_path", schema_string("The absolute path to the file to modify")},
            {"old_string", schema_string("The text to replace")},
            {"new_string",
             schema_string("The text to replace it with (must be different from old_string)")},
            {"replace_all",
             schema_boolean("Replace all occurrences of old_string (default false)")},
        },
        {"file_path", "old_string", "new_string"});
}

std::optional<std::string> EditTool::get_path(const boost::json::object& input) const {
    const std::string path = jget_string(input, "file_path");
    if (path.empty()) return std::nullopt;
    return expand_path(path);
}

PermissionResult EditTool::check_permissions(const boost::json::object& input,
                                             ToolUseContext& context) const {
    const auto path = get_path(input);
    if (!path.has_value()) return {};

    const auto match = permissions::match_content_rules(context.permission_context, *this, input,
                                                        permissions::rule_roots(context));
    return path_permission_result(match, *path, input, /*reading=*/false);
}

std::string EditTool::prompt() const { return edit_tool_prompt(/*compact_line_prefix=*/true); }

ToolResult EditTool::call(const boost::json::object& input, ToolUseContext& context) {
    const std::string requested = jget_string(input, "file_path");
    const std::string path = resolve(expand_path(requested), context.options.cwd);
    const std::string old_string = jget_string(input, "old_string");
    const std::string new_string = jget_string(input, "new_string");
    const bool replace_everywhere = jget_bool(input, "replace_all", false);

    if (old_string == new_string) {
        throw ToolError("No changes to make: old_string and new_string are exactly the same.",
                        "Edit");
    }

    const bool existed = file_exists(path);
    if (!existed) {
        if (!old_string.empty()) {
            throw ToolError(file_not_found_message(path, context.options.cwd), "Edit");
        }
        // An empty old_string on a missing file creates the file.
        std::string error;
        if (context.on_file_edit) context.on_file_edit(path);

        if (!write_file(path, new_string, &error)) {
            throw ToolError("Error writing file: " + error, "Edit");
        }
        tracker_->record_write(path);
        ToolResult result;
        result.content = "The file " + path + " has been updated successfully.";
        return result;
    }

    if (extension(path) == "ipynb") {
        throw ToolError("File is a Jupyter Notebook. Use the NotebookEdit to edit this file.",
                        "Edit");
    }

    // A read that covered only part of the file still allows editing it.
    if (!tracker_->was_read(path)) {
        throw ToolError("File has not been read yet. Read it first before writing to it.", "Edit");
    }
    if (!tracker_->unchanged_since_read(path)) {
        throw ToolError(
            "File has been modified since read, either by the user or by a linter. Read it again "
            "before attempting to write it.",
            "Edit");
    }

    const FileState state = file_state(path);
    if (state.size > kMaxEditFileSize) {
        throw ToolError("File is too large to edit (" + format_file_size(state.size) +
                            "). Maximum editable file size is " + format_file_size(kMaxEditFileSize) +
                            ".",
                        "Edit");
    }

    const auto contents = read_file(path);
    if (!contents) {
        throw ToolError("Cannot read '" + path + "'.", "Edit");
    }

    // Match against LF-normalised content, then restore the original endings.
    const bool had_crlf = contents->find("\r\n") != std::string::npos;
    std::string text = replace_all(*contents, "\r\n", "\n");

    if (old_string.empty()) {
        throw ToolError("Cannot create new file - file already exists.", "Edit");
    }

    // The text the file really holds, which may differ from `old_string` in
    // its quote style.
    const auto actual_old_string = find_actual_string(text, old_string);
    if (!actual_old_string) {
        throw ToolError("String to replace not found in file.\nString: " + old_string, "Edit");
    }

    // The file keeps the quote style it uses.
    const std::string replacement = preserve_quote_style(old_string, *actual_old_string, new_string);

    size_t occurrences = 0;
    size_t position = 0;
    while ((position = text.find(*actual_old_string, position)) != std::string::npos) {
        ++occurrences;
        position += actual_old_string->size();
    }

    if (occurrences > 1 && !replace_everywhere) {
        throw ToolError("Found " + std::to_string(occurrences) +
                            " matches of the string to replace, but replace_all is false. To "
                            "replace all occurrences, set replace_all to true. To replace only one "
                            "occurrence, please provide more context to uniquely identify the "
                            "instance.\nString: " +
                            old_string,
                        "Edit");
    }

    std::string updated;
    if (replace_everywhere) {
        size_t at = 0;
        size_t found = text.find(*actual_old_string, at);
        while (found != std::string::npos) {
            updated += text.substr(at, found - at);
            updated += replacement;
            at = found + actual_old_string->size();
            found = text.find(*actual_old_string, at);
        }
        updated += text.substr(at);
    } else {
        const size_t at = text.find(*actual_old_string);
        updated = text.substr(0, at) + replacement + text.substr(at + actual_old_string->size());
    }

    if (had_crlf) {
        updated = replace_all(updated, "\n", "\r\n");
    }

    std::string error;
    if (context.on_file_edit) context.on_file_edit(path);

    if (!write_file(path, updated, &error)) {
        throw ToolError("Error writing file: " + error, "Edit");
    }

    tracker_->record_write(path);

    ToolResult result;
    result.content = replace_everywhere
                         ? "The file " + path +
                               " has been updated. All occurrences were successfully replaced."
                         : "The file " + path + " has been updated successfully.";

    boost::json::object payload;
    payload["oldContent"] = text;
    payload["newContent"] = updated;
    result.tool_use_result = boost::json::value(payload);
    return result;
}

} // namespace cc::tools
