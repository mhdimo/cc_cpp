#include "ui/status_line.hpp"

#include "utils/json_util.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace cc::ui {
namespace {

/// The number of seconds the status line command may run.
///
/// The reference gives the status line a short timeout, so a slow command
/// cannot hold the prompt back.
constexpr int kStatusLineTimeoutSeconds = 5;

/// The percentage of the context window that is in use, and the share that
/// is left.
///
/// The reference counts the input tokens of the last turn, together with the
/// tokens it read from and wrote to the cache, against the size of the
/// context window.
std::pair<int, int> context_percentages(const StatusLineState& state) {
    if (!state.has_usage) return {0, 100};

    const int64_t total = state.usage_input_tokens + state.usage_cache_creation_input_tokens +
                          state.usage_cache_read_input_tokens;
    const int64_t window = state.context_window_size > 0 ? state.context_window_size : 1;

    const auto used = static_cast<int>(std::llround(static_cast<double>(total) /
                                                    static_cast<double>(window) * 100.0));
    const int clamped = std::min(100, std::max(0, used));
    return {clamped, 100 - clamped};
}

} // namespace

boost::json::object build_status_line_input(const StatusLineState& state) {
    boost::json::object input;

    input["session_id"] = state.session_id;
    input["transcript_path"] = state.transcript_path;
    input["cwd"] = state.cwd;

    boost::json::object model;
    model["id"] = state.model_id;
    model["display_name"] = state.model_display_name;
    input["model"] = std::move(model);

    boost::json::object workspace;
    workspace["current_dir"] = state.cwd;
    workspace["project_dir"] = state.project_dir;
    boost::json::array added;
    for (const auto& dir : state.added_dirs) added.emplace_back(dir);
    workspace["added_dirs"] = std::move(added);
    input["workspace"] = std::move(workspace);

    input["version"] = state.version;

    boost::json::object output_style;
    output_style["name"] = state.output_style;
    input["output_style"] = std::move(output_style);

    boost::json::object cost;
    cost["total_cost_usd"] = state.total_cost_usd;
    cost["total_duration_ms"] = state.total_duration_ms;
    cost["total_api_duration_ms"] = state.total_api_duration_ms;
    cost["total_lines_added"] = state.total_lines_added;
    cost["total_lines_removed"] = state.total_lines_removed;
    input["cost"] = std::move(cost);

    const auto [used, remaining] = context_percentages(state);

    boost::json::object context_window;
    context_window["total_input_tokens"] = state.total_input_tokens;
    context_window["total_output_tokens"] = state.total_output_tokens;
    context_window["context_window_size"] = state.context_window_size;
    if (state.has_usage) {
        boost::json::object usage;
        usage["input_tokens"] = state.usage_input_tokens;
        usage["output_tokens"] = state.usage_output_tokens;
        usage["cache_creation_input_tokens"] = state.usage_cache_creation_input_tokens;
        usage["cache_read_input_tokens"] = state.usage_cache_read_input_tokens;
        context_window["current_usage"] = std::move(usage);
    }
    context_window["used_percentage"] = used;
    context_window["remaining_percentage"] = remaining;
    input["context_window"] = std::move(context_window);

    input["exceeds_200k_tokens"] = state.exceeds_200k_tokens;

    return input;
}

std::string run_status_line(const std::string& command, const boost::json::object& input,
                             const std::string& cwd) {
    if (command.empty()) return {};

    const std::string stdin_text = json_dump(boost::json::value(input)) + "\n";
    const ProcessOutcome outcome = run_process(command, cwd, stdin_text, kStatusLineTimeoutSeconds);
    if (outcome.failed || outcome.timed_out || outcome.exit_code != 0) return {};

    // The reference trims the output, drops the blank lines and joins the
    // rest with newlines.
    std::string text;
    for (const auto& line : cc::split_lines(trim(outcome.out))) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) continue;
        if (!text.empty()) text.push_back('\n');
        text += trimmed;
    }
    return text;
}

} // namespace cc::ui
