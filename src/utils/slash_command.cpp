#include "utils/slash_command.hpp"

#include "utils/string.hpp"

namespace cc {

std::optional<ParsedSlashCommand> parse_slash_command(const std::string& input) {
    const std::string trimmed = trim(input);
    if (!starts_with(trimmed, "/")) return std::nullopt;

    const std::vector<std::string> words = split(trimmed.substr(1), ' ');
    if (words.empty() || words[0].empty()) return std::nullopt;

    ParsedSlashCommand parsed;
    parsed.command_name = words[0];

    size_t first_argument = 1;
    // The prompts of an MCP server are written `<server>:<prompt> (MCP)`, so
    // the marker is the second word and the arguments start after it.
    if (words.size() > 1 && words[1] == "(MCP)") {
        parsed.command_name += " (MCP)";
        parsed.is_mcp = true;
        first_argument = 2;
    }

    for (size_t i = first_argument; i < words.size(); ++i) {
        if (i > first_argument) parsed.args += " ";
        parsed.args += words[i];
    }

    return parsed;
}

} // namespace cc
