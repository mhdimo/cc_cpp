#pragma once

#include <string>
#include <vector>

namespace cc::mcp {

/// A command an MCP server offers as a prompt.
///
/// The prompts of a server are slash commands: the name of the command is
/// `mcp__<server>__<prompt>` and the user sees `<server>:<prompt> (MCP)`.
struct McpCommand {
    /// The name of the command: `mcp__<server>__<prompt>`.
    std::string name;

    /// The name the user sees: `<server>:<prompt> (MCP)`.
    std::string user_facing_name;

    /// The description of the prompt.
    std::string description;

    /// The names of the arguments the prompt takes.
    std::vector<std::string> argument_names;

    /// The name of the server the prompt comes from.
    std::string server_name;

    /// The name of the prompt on the server.
    std::string prompt_name;
};

/// True when two commands are the same.
///
/// The commands of the servers are compared on every draw of the prompt box,
/// to know whether the list of the commands has changed.
inline bool operator==(const McpCommand& a, const McpCommand& b) {
    return a.name == b.name && a.user_facing_name == b.user_facing_name &&
           a.description == b.description && a.argument_names == b.argument_names &&
           a.server_name == b.server_name && a.prompt_name == b.prompt_name;
}

} // namespace cc::mcp
