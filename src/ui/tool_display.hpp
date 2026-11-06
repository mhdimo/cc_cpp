#pragma once

#include "tools/tool.hpp"
#include "ui/ansi.hpp"
#include "ui/theme.hpp"

#include <boost/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace cc::ui {

/// The state a tool call is in, which decides how its dot is drawn.
enum class ToolCallState {
    Queued,   ///< Not started yet.
    Running,  ///< Currently executing.
    Succeeded,
    Failed,
};

/// Shows a file path the way Claude Code does.
///
/// A path inside the working directory is shown relative to it, a path in
/// the home directory with a tilde, and anything else in full.
std::string relative_path(const std::string& path, const std::string& cwd);

/// The name Claude Code shows for a tool call.
///
/// `Read` shows as `Read`, `Edit` as `Update` (or `Create` when the file did
/// not exist), and so on.
std::string user_facing_tool_name(const Tool& tool, const boost::json::object& input);

/// The arguments Claude Code shows in parentheses after the tool name.
///
/// Paths are shown relative to the working directory.
std::string user_facing_tool_args(const Tool& tool, const boost::json::object& input,
                                  const std::string& cwd);

/// Renders the line that announces a tool call, e.g. `⏺ Read(src/main.cpp)`.
///
/// The dot is dim while the call is queued or running, green once it
/// succeeds and red once it fails.
///
/// `marker_visible` says whether the dot is drawn at all: Claude Code
/// blinks it while a tool runs.
std::string render_tool_use_line(const Tool& tool, const boost::json::object& input,
                                 const std::string& cwd, const Theme& theme, ToolCallState state,
                                 bool marker_visible = true);

/// Renders the diff a file edit produced, the way Claude Code shows it.
///
/// The first line is the summary, e.g. `Added 3 lines, removed 1 line`; the
/// rest is the diff itself.
std::vector<std::string> render_diff_result(const std::string& before, const std::string& after,
                                            const Theme& theme, int width);

/// Renders the lines that show a tool's result.
///
/// The first line carries the `  ⎿  ` gutter; continuation lines are
/// indented to line up underneath it. The output is cut short, and the
/// number of the lines that are left is said; the transcript asks for the
/// whole of it, because it is the view the "ctrl+o to expand" refers to.
std::vector<std::string> render_tool_result_lines(const std::string& content,
                                                  const Theme& theme, int width, bool is_error,
                                                  bool truncate = true);

/// Renders the lines that show a tool's result, the way Claude Code does.
///
/// Claude Code shows a one-line summary for most tools — `Read 42 lines`,
/// `Found 3 files`, `Wrote 12 lines to src/main.cpp` — rather than the raw
/// result, which would fill the transcript with the file's contents. The
/// summary comes from `structured`, the payload the tool attached to its
/// result; a tool that did not attach one falls back to the raw content.
std::vector<std::string> render_tool_result_lines(std::string_view tool_name,
                                                  const std::string& content,
                                                  const boost::json::value& structured,
                                                  const std::string& cwd, const Theme& theme,
                                                  int width, bool is_error, bool verbose = false);

/// Renders the lines that show a tool's error.
///
/// The reference shows the first ten lines of the error and says how many
/// are left.
std::vector<std::string> render_tool_error_lines(const std::string& content, const Theme& theme,
                                                  int width, bool verbose = false);

} // namespace cc::ui
