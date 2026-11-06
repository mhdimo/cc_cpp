#pragma once

#include <optional>
#include <string>

namespace cc {

/// The parts of a slash command the user typed.
struct ParsedSlashCommand {
    /// The name of the command, without the leading slash.
    std::string command_name;

    /// The arguments of the command.
    std::string args;

    /// True when the command comes from an MCP server.
    ///
    /// The name of such a command ends in ` (MCP)`, and the arguments start
    /// after that word.
    bool is_mcp = false;
};

/// Parses the slash command the user typed.
///
/// The command is the first word after the slash. When the second word is
/// `(MCP)`, the command is a prompt of an MCP server: its name is the word
/// followed by ` (MCP)` and its arguments start after that word.
///
/// Returns nothing when the text is not a slash command.
std::optional<ParsedSlashCommand> parse_slash_command(const std::string& input);

} // namespace cc
