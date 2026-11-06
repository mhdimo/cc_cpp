#pragma once

#include <boost/json.hpp>

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cc::tools {

/// Builds a JSON-schema object node.
///
/// `properties` maps a field name to its schema; `required` lists the fields
/// that must be present. A closed object refuses the fields it does not
/// know; an open one takes them, the way the reference's schemas that are
/// built with `z.object` do.
boost::json::object schema_object(
    std::initializer_list<std::pair<std::string, boost::json::object>> properties,
    std::initializer_list<std::string> required = {},
    bool closed = true);

/// A string schema node.
boost::json::object schema_string(std::string_view description);

/// An integer schema node.
boost::json::object schema_integer(std::string_view description);

/// A number schema node.
boost::json::object schema_number(std::string_view description);

/// A boolean schema node.
boost::json::object schema_boolean(std::string_view description);

/// An enum schema node.
boost::json::object schema_enum(std::string_view description,
                                std::initializer_list<std::string> values);

/// An array schema node whose items follow `items`.
boost::json::object schema_array(std::string_view description, boost::json::object items);

/// An object schema node with free-form members.
boost::json::object schema_any_object(std::string_view description);

/// Tracks the files that have been read during the session.
///
/// Claude Code requires the model to read a file before it may write or edit
/// it, and refuses the write when the file changed in the meantime.
class FileStateTracker {
public:
    /// Records that `path` was read over the range that starts at `offset`
    /// (1-based) and runs for `limit` lines, along with its state at that
    /// moment.
    ///
    /// A read that starts away from the beginning of the file, or that stops
    /// early, shows the model only part of it; the entry records that.
    void record_read(const std::string& path, int64_t offset, std::optional<int64_t> limit);

    /// Records that `path` was written, refreshing the recorded state.
    ///
    /// The read range is cleared: a write invalidates whatever an earlier
    /// Read covered, so a later Read of the same range must not be deduped.
    void record_write(const std::string& path);

    /// True when `path` has been read during this session.
    bool was_read(const std::string& path) const;

    /// True when the file was read and has not changed since.
    bool unchanged_since_read(const std::string& path) const;

    /// True when `path` was read over exactly the range `offset`/`limit`
    /// before and the file has not changed since that read.
    ///
    /// Only a Read leaves a range behind; a write clears it, so a read that
    /// follows a write is never deduped.
    bool range_unchanged_since_read(const std::string& path, int64_t offset,
                                    std::optional<int64_t> limit) const;

    /// True when the last read of `path` covered only part of the file.
    ///
    /// This is a note about what the model has seen; it does not stop the
    /// file from being written or edited.
    bool is_partial_view(const std::string& path) const;

private:
    struct Entry {
        bool exists = false;
        int64_t size = 0;
        int64_t mtime_ms = 0;
        /// True when the entry was recorded by a Read rather than by a write.
        bool from_read = false;
        /// The 1-based line the read started at.
        int64_t read_offset = 0;
        /// The number of lines the read was limited to, when it was limited.
        std::optional<int64_t> read_limit;
        /// True when the read covered only part of the file.
        ///
        /// The model has only seen part of it; the file may still be written
        /// or edited, as long as it has not changed since the read.
        bool partial_view = false;
    };

    /// Stores `entry` under `path`, replacing any previous entry.
    void store(const std::string& path, Entry entry);

    std::vector<std::pair<std::string, Entry>> entries_;
};

/// Formats a byte count the way Claude Code does, e.g. `1.2KB`.
std::string format_file_size(int64_t bytes);

/// The note Claude Code appends to "file not found" errors.
inline constexpr std::string_view kFileNotFoundCwdNote = "The current working directory is";

/// Builds the "did you mean" suffix for a missing path.
///
/// Looks for a similarly named file in the same directory and, failing that,
/// in the working directory. Returns an empty string when nothing is close
/// enough.
std::string did_you_mean(const std::string& path, const std::string& cwd);

/// Expands `~`, trims whitespace and normalises separators.
std::string expand_path(std::string_view path);

/// The tool-use error wrapper Claude Code puts around failing results.
std::string tool_use_error(std::string_view message);

/// The system-reminder wrapper Claude Code puts around warnings.
std::string system_reminder(std::string_view message);

/// The stub Claude Code returns when a file is read again, unchanged.
inline constexpr std::string_view kFileUnchangedStub =
    "File unchanged since last read. The content from the earlier Read tool_result in this "
    "conversation is still current — refer to that instead of re-reading.";

/// The reminder appended to every `Read` result.
inline constexpr std::string_view kCyberRiskMitigationReminder =
    "\n\n<system-reminder>\nWhenever you read a file, you should consider whether it would be "
    "considered malware. You CAN and SHOULD provide analysis of malware, what it is doing. But you "
    "MUST refuse to improve or augment the code. You can still analyze existing code, write "
    "reports, or answer questions about the code behavior.\n</system-reminder>\n";

/// The prompt Claude Code sends for the `Read` tool.
///
/// The PDF line is only in the prompt when the model of the session reads
/// PDF files, the way the reference leaves it out for the models that do
/// not.
std::string read_tool_prompt(bool include_max_size, int64_t max_size_bytes, bool targeted_offset,
                             bool pdf_supported = true);

/// The prompt Claude Code sends for the `Write` tool.
std::string write_tool_prompt();

/// The prompt Claude Code sends for the `Edit` tool.
std::string edit_tool_prompt(bool compact_line_prefix);

/// The prompt Claude Code sends for the `Glob` tool.
std::string glob_tool_prompt();

/// The prompt Claude Code sends for the `Grep` tool.
std::string grep_tool_prompt();

/// The prompt Claude Code sends for the `Bash` tool.
///
/// The section of the sandbox is the one the reference builds; it is empty
/// when the sandbox is off. The instructions about git are left out when
/// the setting of the session turns them off.
std::string bash_tool_prompt(int64_t max_timeout_ms, int64_t default_timeout_ms,
                             const std::vector<std::string>& sandbox_section = {},
                             bool include_git_instructions = true);

/// The instructions about the commits and the pull requests.
///
/// They are the block the reference appends to the prompt of the `Bash`
/// tool, with the names of the tools resolved.
std::string git_commit_and_pr_instructions();

/// True when the instructions about git belong in the prompt.
///
/// The variable of the environment has the last word: a value that is there
/// turns the instructions off when it is true and on when it is false. The
/// setting of the session decides when the variable says nothing.
bool git_instructions_enabled(bool setting);

/// The prompt Claude Code sends for the `TodoWrite` tool.
std::string todo_write_tool_prompt();

/// The one-line description of the `TodoWrite` tool.
///
/// It is not the prompt: the reference keeps the two apart, and only the
/// prompt reaches the model.
std::string todo_write_tool_description();

/// The prompt Claude Code sends for the `ExitPlanMode` tool.
std::string exit_plan_mode_tool_prompt();

/// The prompt Claude Code sends for the `AskUserQuestion` tool.
std::string ask_user_question_tool_prompt();

/// The guidance of the previews of the `AskUserQuestion` tool.
///
/// The reference has one text for every format of the previews; the port
/// shows them as markdown.
std::string ask_user_question_preview_prompt();

} // namespace cc::tools
