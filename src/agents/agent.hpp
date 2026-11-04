#pragma once

#include <boost/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc::agents {

/// The path reported for the definitions that ship with the CLI.
inline constexpr std::string_view kBuiltInPath = "built-in";

/// One agent definition, loaded from a markdown file with frontmatter.
struct Definition {
    std::string name;
    std::string description;
    /// The system prompt from the body of the file.
    std::string prompt;
    /// The tools the agent may use; empty means all of them.
    std::vector<std::string> tools;
    /// The model the agent runs on; empty means it inherits the parent's.
    std::string model;
    /// The path the definition was read from.
    std::string path;
};

/// The agent definitions available to a session.
///
/// The list starts with the built-in general-purpose agent and continues
/// with the definitions found in `~/.claude/agents` and
/// `<cwd>/.claude/agents`. A definition read from the project directory
/// replaces one with the same name read from the user's directory.
std::vector<Definition> load_definitions(const std::string& cwd, const std::string& home);

/// The agents that ship with the CLI.
///
/// The list holds the agents that explore a codebase, the ones that plan an
/// implementation and the one that sets the status line up.
std::vector<Definition> builtin_definitions();

/// The definition with `name`, or null.
std::optional<Definition> find(const std::vector<Definition>& definitions, const std::string& name);

/// The frontmatter of a markdown document, parsed into a JSON object.
boost::json::object parse_frontmatter(const std::string& contents);

} // namespace cc::agents
