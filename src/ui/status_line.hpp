#pragma once

#include <boost/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace cc::ui {

/// The values the status line command is given.
///
/// The shape follows the reference's `StatusLineCommandInput`, so a status
/// line written for Claude Code works here unchanged.
struct StatusLineState {
    /// The id of the session.
    std::string session_id;
    /// The path of the transcript file.
    std::string transcript_path;
    /// The directory the session runs in.
    std::string cwd;
    /// The model the session runs on, and the name it is shown under.
    std::string model_id;
    std::string model_display_name;
    /// The directory the session was started in.
    std::string project_dir;
    /// The directories the user added to the session.
    std::vector<std::string> added_dirs;
    /// The version of the program.
    std::string version;
    /// The name of the output style.
    std::string output_style = "default";

    /// The cost of the session, in dollars.
    double total_cost_usd = 0;
    /// How long the session has been running, and how long the API calls
    /// have taken, in milliseconds.
    int64_t total_duration_ms = 0;
    int64_t total_api_duration_ms = 0;
    /// The lines the session has added and removed.
    int64_t total_lines_added = 0;
    int64_t total_lines_removed = 0;

    /// The tokens the session has used.
    int64_t total_input_tokens = 0;
    int64_t total_output_tokens = 0;
    /// The size of the model's context window.
    int64_t context_window_size = 200000;

    /// The usage of the last turn, when there has been one.
    ///
    /// The reference leaves the usage out until the model has answered for
    /// the first time.
    bool has_usage = false;
    int64_t usage_input_tokens = 0;
    int64_t usage_output_tokens = 0;
    int64_t usage_cache_creation_input_tokens = 0;
    int64_t usage_cache_read_input_tokens = 0;

    /// True when the last answer was longer than the context window.
    bool exceeds_200k_tokens = false;
};

/// Builds the JSON the status line command reads on its standard input.
boost::json::object build_status_line_input(const StatusLineState& state);

/// Runs the status line command and returns the text it wrote.
///
/// The command is given the payload on its standard input, the way the
/// reference does. Its output is trimmed, the blank lines are dropped and
/// the rest is joined with newlines. A command that fails leaves the text
/// empty, and the status line is simply not shown.
std::string run_status_line(const std::string& command, const boost::json::object& input,
                             const std::string& cwd);

} // namespace cc::ui
