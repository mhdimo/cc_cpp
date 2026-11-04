#pragma once

#include "config/claude_md.hpp"
#include "config/settings.hpp"
#include "types/permissions.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cc::query {

/// The options that shape the system prompt.
struct SystemPromptOptions {
    /// The working directory the session runs in.
    std::string cwd;

    /// The model the session uses.
    std::string model;

    /// The Claude Code version.
    std::string version;

    /// The permission mode the session runs in.
    PermissionMode permission_mode = PermissionMode::Default;

    /// The file the plan of the session is written to.
    ///
    /// The plan mode names the file in the prompt, the way the reference
    /// does; it is empty when the session does not run in the plan mode.
    std::string plan_file_path;

    /// The tool names that are available.
    std::vector<std::string> tool_names;

    /// The memory files that contribute to the prompt.
    std::vector<config::MemoryFile> memory_files;

    /// The output style to apply, if any.
    std::optional<std::string> output_style;

    /// Extra text appended to the prompt.
    std::optional<std::string> append_prompt;

    /// A replacement for the whole prompt.
    std::optional<std::string> custom_prompt;
};

/// Builds the system prompt for a session.
///
/// The prompt lists the available tools, states the permission mode, points
/// at the current date and working directory, and includes the contents of
/// the memory files under a `claudeMd` heading.
std::string build_system_prompt(const SystemPromptOptions& options);

/// The date string Claude Code puts in the prompt, e.g. `2026-11-03`.
std::string current_date();

} // namespace cc::query
