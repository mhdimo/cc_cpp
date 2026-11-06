#pragma once

#include <boost/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc::hooks {

/// The events a hook can be registered for.
enum class Event {
    PreToolUse,
    PostToolUse,
    UserPromptSubmit,
    Notification,
    Stop,
    SubagentStop,
    PreCompact,
    SessionStart,
    SessionEnd,
};

/// The name of an event, as it appears in the settings.
std::string_view to_string(Event event);

/// Parses an event name. Returns false when it is not known.
bool parse_event(std::string_view name, Event& out);

/// One registered hook.
struct Hook {
    /// The event the hook runs for.
    Event event = Event::PreToolUse;
    /// The tool selector.
    ///
    /// An empty matcher, and `*`, select every tool. A matcher made only of
    /// letters, digits, underscores and pipes is a list of exact tool names,
    /// so `Write` selects the tool named `Write` and nothing else. Anything
    /// else is a pattern in the grammar of JavaScript searched for the tool
    /// name. The comparison is case-sensitive.
    std::string matcher;
    /// The shell command the hook runs.
    std::string command;
    /// How long the hook may run, in seconds.
    int timeout_seconds = 60;
};

/// The hooks of a session.
class Registry {
public:
    /// Reads the hooks from a settings object.
    void configure(const boost::json::object& settings);

    /// Adds a hook to the registry.
    ///
    /// The plugins bring their own hooks, which are added after the ones of
    /// the settings have been read.
    void add(Hook hook);

    /// The hooks registered for `event`.
    std::vector<Hook> for_event(Event event) const;

    /// The hooks of `event` whose matcher matches `tool_name`.
    ///
    /// The matcher follows the rules described on `Hook::matcher`.
    std::vector<Hook> matching(Event event, const std::string& tool_name) const;

    /// True when no hook is registered for `event`.
    bool empty(Event event) const;

private:
    std::vector<Hook> hooks_;
};

/// The outcome of running a hook.
struct Result {
    /// The hook's standard output.
    std::string output;
    /// The hook's standard error.
    std::string error_output;
    /// The hook's exit code.
    int exit_code = 0;
    /// True when the hook asked the tool not to run.
    bool blocked = false;
    /// The message shown when the hook blocked the tool.
    std::string block_reason;
    /// Extra context to add to the conversation.
    std::string additional_context;
};

/// Runs the hooks of `event` for `tool_name`.
///
/// The payload is written to the hook's standard input as JSON, the way the
/// reference does. A hook that exits with code 2 blocks the tool; the
/// reference reads the reason from its standard output.
Result run(const std::vector<Hook>& hooks, const std::string& tool_name,
           const boost::json::object& payload, const std::string& cwd);

} // namespace cc::hooks
