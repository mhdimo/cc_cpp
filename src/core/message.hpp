#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cc {

/// The role an API message carries.
enum class Role { User, Assistant };

std::string_view to_string(Role role);

/// A block of text.
struct TextBlock {
    std::string text;
    bool cache_control = false;
};

/// An extended-thinking block. `signature` must be echoed back verbatim on
/// the next turn or the API rejects the request.
struct ThinkingBlock {
    std::string thinking;
    std::string signature;
};

/// A request from the model to run a tool.
struct ToolUseBlock {
    std::string id;
    std::string name;
    boost::json::value input = boost::json::object{};
};

/// The result of a tool invocation, returned to the model.
struct ToolResultBlock {
    std::string tool_use_id;
    std::string content;
    bool is_error = false;
    /// When the tool produced structured content (MCP tools), it is preserved
    /// here so it can be replayed to the model verbatim.
    boost::json::value structured_content = boost::json::object{};
};

/// An inline image, base64-encoded, for models with vision support.
struct ImageBlock {
    std::string media_type; ///< e.g. "image/png"
    std::string data;       ///< base64 payload, no data: prefix
};

using ContentBlock = std::variant<TextBlock, ThinkingBlock, ToolUseBlock, ToolResultBlock, ImageBlock>;

/// Convenience accessors over the variant.
bool is_text(const ContentBlock& block);
bool is_thinking(const ContentBlock& block);
bool is_tool_use(const ContentBlock& block);
bool is_tool_result(const ContentBlock& block);
bool is_image(const ContentBlock& block);

const TextBlock* as_text(const ContentBlock& block);
const ThinkingBlock* as_thinking(const ContentBlock& block);
const ToolUseBlock* as_tool_use(const ContentBlock& block);
const ToolResultBlock* as_tool_result(const ContentBlock& block);
const ImageBlock* as_image(const ContentBlock& block);

/// Token accounting for one API turn.
struct Usage {
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t cache_creation_input_tokens = 0;
    int64_t cache_read_input_tokens = 0;

    Usage& operator+=(const Usage& other);
    boost::json::value to_json() const;
    static Usage from_json(const boost::json::value& v);
};

/// A message in the conversation, as persisted in the session transcript.
///
/// `is_meta` marks messages the model can see but the user cannot (command
/// output injected as context, hook results, and similar).
/// `is_sidechain` marks messages produced inside a sub-agent.
struct Message {
    Role role = Role::User;
    std::string uuid;
    std::string session_id;
    std::string parent_tool_use_id;
    std::string timestamp; ///< ISO-8601
    std::string cwd;
    std::string version;
    std::string git_branch;
    std::string model;

    std::vector<ContentBlock> content;

    bool is_meta = false;
    bool is_sidechain = false;
    bool is_compact_summary = false;
    bool is_api_error_message = false;

    std::string stop_reason;
    Usage usage;

    /// The raw result a tool produced, kept for the transcript so the UI can
    /// re-render a tool card without re-running it.
    boost::json::value tool_use_result = boost::json::value{nullptr};

    /// All text blocks joined with newlines.
    std::string text() const;

    /// The first tool-use block, or nullptr.
    const ToolUseBlock* first_tool_use() const;

    /// The first tool-result block, or nullptr.
    const ToolResultBlock* first_tool_result() const;

    bool has_tool_use() const;
    bool has_tool_result() const;

    boost::json::value to_json() const;
    static Message from_json(const boost::json::value& v);

    /// Converts to the Anthropic API content-block shape.
    boost::json::array to_api_content() const;
};

/// Serializes a list of messages to the API content-block form, dropping
/// sidechain-only metadata that the API does not accept.
boost::json::array to_api_content(const std::vector<Message>& messages);

} // namespace cc
