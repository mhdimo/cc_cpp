#pragma once

#include "types/permissions.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cc::cli {

/// The arguments the CLI was started with.
struct Args {
    /// The prompt to run, when one was given on the command line.
    std::optional<std::string> prompt;

    /// Print the response and exit, without the interactive prompt.
    bool print = false;

    /// The format `--print` writes in: "text" or "json".
    std::string output_format = "text";

    /// The model to use.
    std::optional<std::string> model;

    /// The fallback model, used when the primary one is unavailable.
    std::optional<std::string> fallback_model;

    /// The permission mode the session runs in.
    PermissionMode permission_mode = PermissionMode::Default;

    /// True when `--dangerously-skip-permissions` was given.
    bool dangerously_skip_permissions = false;

    /// Tools that are allowed without asking.
    std::vector<std::string> allowed_tools;

    /// Tools that are never allowed.
    std::vector<std::string> disallowed_tools;

    /// Extra directories the session may work in.
    std::vector<std::string> additional_directories;

    /// Resume the most recent session.
    bool continue_session = false;

    /// Resume a specific session.
    std::optional<std::string> resume_session;

    /// Pick the conversation that is resumed from a list.
    ///
    /// The reference opens a picker of the conversations of the folder when
    /// `--resume` is given without the id of a session.
    bool resume_picker = false;

    /// The session id to use for a new session.
    std::optional<std::string> session_id;

    /// A replacement for the system prompt.
    std::optional<std::string> system_prompt;

    /// A file whose contents replace the system prompt.
    std::optional<std::string> system_prompt_file;

    /// Text appended to the system prompt.
    std::optional<std::string> append_system_prompt;

    /// A file whose contents are appended to the system prompt.
    std::optional<std::string> append_system_prompt_file;

    /// Settings overrides, as a JSON object.
    std::optional<std::string> settings;

    /// The setting sources to load.
    std::vector<std::string> setting_sources;

    /// Print debug information.
    bool debug = false;

    /// The file debug output goes to.
    std::optional<std::string> debug_file;

    /// Print verbose output.
    bool verbose = false;

    /// Print the version and exit.
    bool show_version = false;

    /// Print the help and exit.
    bool show_help = false;

    /// Emit the partial messages as they stream.
    bool include_partial_messages = false;

    /// The format the input is read in: "text" or "stream-json".
    std::string input_format = "text";

    /// The maximum number of model round-trips a turn may take.
    int max_turns = 50;

    /// The number of output tokens per request.
    int max_output_tokens = 32000;

    /// Enable extended thinking.
    bool thinking = false;

    /// The effort level: "low", "medium", "high" or "max".
    std::optional<std::string> effort;

    /// The arguments that were not understood.
    std::vector<std::string> unknown;
};

/// Parses the command line, excluding the program name.
///
/// Throws `cc::UsageError` when an argument is malformed.
Args parse_args(const std::vector<std::string>& argv);

/// The help text the CLI prints for `--help`.
std::string help_text();

/// The version string the CLI prints for `--version`.
std::string version_text();

} // namespace cc::cli
