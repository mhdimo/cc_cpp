#pragma once

#include "permissions/decision.hpp"
#include "types/permissions.hpp"

#include <boost/json.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc {

class Tool;
struct ToolUseContext;

/// The payload a tool returns to the model.
struct ToolResult {
    /// The textual result handed back to the model.
    std::string content;

    /// Extra messages injected into the conversation after the tool ran.
    std::vector<boost::json::value> new_messages;

    /// Structured output, used by tools with an output schema.
    boost::json::value structured_content = boost::json::value{nullptr};

    /// MCP pass-through metadata (`_meta`, `structuredContent`).
    boost::json::value mcp_meta = boost::json::value{nullptr};

    /// True when the tool failed in a way the model should see as an error
    /// result rather than a crash.
    bool is_error = false;

    /// The raw result a tool produced, kept for the transcript so the UI can
    /// re-render a tool card without re-running it.
    boost::json::value tool_use_result = boost::json::value{nullptr};
};

/// The result of asking the user to pick an option.
struct ToolChoiceOption {
    std::string label;
    std::string description;
};

/// The base class for every tool the model can call.
///
/// Tools are described by a JSON schema, executed with a JSON object of
/// arguments, and may take part in the permission pipeline by overriding
/// `check_permissions`.
class Tool {
public:
    virtual ~Tool() = default;

    /// The wire name the model sees.
    virtual std::string_view name() const = 0;

    /// The description shown to the model.
    virtual std::string description() const = 0;

    /// The prompt Claude Code sends for the tool.
    ///
    /// The reference sends the prompt of the tool to the model and not its
    /// description; the default keeps the two the same, which is what the
    /// tools of an MCP server need.
    virtual std::string prompt() const { return description(); }

    /// The JSON schema for the tool's arguments.
    virtual boost::json::object input_schema() const = 0;

    /// Runs the tool.
    virtual ToolResult call(const boost::json::object& input, ToolUseContext& context) = 0;

    /// A one-line capability phrase used for keyword search over deferred
    /// tools. 3–10 words, no trailing period.
    virtual std::optional<std::string> search_hint() const { return std::nullopt; }

    /// Whether the tool may run concurrently with other tools.
    virtual bool is_concurrency_safe(const boost::json::object& input) const { return true; }

    /// Whether the tool only reads state.
    virtual bool is_read_only(const boost::json::object& input) const { return false; }

    /// Whether the tool performs an irreversible operation.
    virtual bool is_destructive(const boost::json::object& input) const { return false; }

    /// Whether the tool requires the user to answer, which means it cannot be
    /// auto-approved even in bypass mode.
    virtual bool requires_user_interaction() const { return false; }

    /// Whether the tool reaches the open internet.
    virtual bool is_open_world(const boost::json::object& input) const { return false; }

    /// The path the tool operates on, when it operates on a single path.
    /// Used by the permission engine to check filesystem rules.
    virtual std::optional<std::string> get_path(const boost::json::object& input) const {
        return std::nullopt;
    }

    /// The content of the call, as the rules of the tool are matched
    /// against it.
    ///
    /// The reference matches the content of a rule against the call: the
    /// command of a shell command, the path of a file, the host of a URL. A
    /// tool that has no content returns nothing, and only the rules of the
    /// whole tool apply to it.
    virtual std::optional<std::string> permission_content(const boost::json::object& input) const {
        return get_path(input);
    }

    /// The tool's own opinion on whether the call is permitted.
    ///
    /// The default implementation always passes through, letting the
    /// surrounding permission pipeline decide.
    virtual PermissionResult check_permissions(const boost::json::object& input,
                                               ToolUseContext& context) const;

    /// MCP tools carry the server and tool names as the server reported them.
    struct McpInfo {
        std::string server_name;
        std::string tool_name;
    };
    virtual std::optional<McpInfo> mcp_info() const { return std::nullopt; }

    /// The maximum size of a tool result before it is spilled to disk. Zero
    /// means unlimited.
    virtual size_t max_result_size_chars() const { return 0; }
};

/// A shared handle to a tool.
using ToolPtr = std::shared_ptr<Tool>;

/// The set of tools available to the model.
using Tools = std::vector<ToolPtr>;

/// Finds a tool by name.
Tool* find_tool_by_name(const Tools& tools, std::string_view name);

} // namespace cc
