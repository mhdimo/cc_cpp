#include "hooks/hooks.hpp"

#include "permissions/rule_parser.hpp"
#include "utils/js_regex.hpp"
#include "utils/json_util.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cc::hooks {
namespace {

/// The text used when a blocking hook printed nothing at all.
constexpr std::string_view kNoOutput = "No stderr output";

/// The number of seconds a hook may run when it does not say otherwise.
constexpr int kDefaultTimeoutSeconds = 60;

/// Appends `text` to `out`, on a line of its own.
void append_output(std::string& out, const std::string& text) {
    if (text.empty()) return;
    if (!out.empty() && !ends_with(out, "\n")) out += '\n';
    out += text;
}

/// True when `matcher` holds nothing but the characters a plain tool name can
/// have: letters, digits, underscores, and the pipe that separates the names
/// of a list.
///
/// The reference treats such a matcher as a list of exact tool names; every
/// other matcher is a regular expression.
bool is_plain_tool_name_list(std::string_view matcher) {
    if (matcher.empty()) return false;
    for (const char c : matcher) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '|';
        if (!plain) return false;
    }
    return true;
}

/// True when `matcher` selects `tool_name`.
///
/// An empty matcher, and the `*` matcher, select every tool. A matcher made
/// only of letters, digits, underscores and pipes is a list of exact tool
/// names, so `Write` selects the tool named `Write` and nothing else — not
/// `NotebookWrite`. Any other matcher is compiled as a pattern in the grammar
/// of JavaScript and searched for the tool name. An invalid pattern selects
/// nothing and is reported through `error`, when one is given.
///
/// Matching is case-sensitive throughout, as it is in the reference.
bool matcher_matches(const std::string& matcher, const std::string& tool_name,
                     std::string* error) {
    if (matcher.empty() || matcher == "*") return true;

    if (is_plain_tool_name_list(matcher)) {
        // A pipe-separated list of exact tool names.
        if (matcher.find('|') != std::string::npos) {
            for (const std::string& name : cc::split(matcher, '|')) {
                if (permissions::normalize_legacy_tool_name(name) == tool_name) return true;
            }
            return false;
        }
        // A single exact tool name.
        return permissions::normalize_legacy_tool_name(matcher) == tool_name;
    }

    // An invalid pattern must not throw; it selects nothing and is reported
    // instead.
    std::string failure;
    const std::optional<JsRegex> pattern = JsRegex::compile(matcher, &failure);
    if (!pattern) {
        if (error != nullptr) {
            *error = "Invalid hook matcher '" + matcher + "': " + failure;
        }
        return false;
    }

    if (pattern->search(tool_name)) return true;
    // The legacy names of the tool are searched as well, so that a
    // pattern like `^Task$` still selects the tool now called `Agent`.
    for (const std::string& legacy : permissions::legacy_tool_names(tool_name)) {
        if (pattern->search(legacy)) return true;
    }
    return false;
}

/// The JSON a hook printed on its standard output, if it printed any.
boost::json::value parse_hook_output(const std::string& stdout) {
    const std::string trimmed = trim(stdout);
    if (!starts_with(trimmed, "{")) return boost::json::value(nullptr);
    return json_parse(trimmed);
}

/// True when the hook's JSON output asks for the tool to be blocked.
///
/// The reason is written to `reason`, when the hook gave one.
bool json_blocks(const boost::json::value& parsed, std::string& reason) {
    if (!parsed.is_object()) return false;
    const boost::json::object object = parsed.as_object();

    if (jget_string(object, "decision") == "block") {
        reason = jget_string(object, "reason", "Blocked by hook");
        return true;
    }

    const boost::json::object specific = jget_object(object, "hookSpecificOutput");
    if (jget_string(specific, "hookEventName") == "PreToolUse" &&
        jget_string(specific, "permissionDecision") == "deny") {
        reason = jget_string(specific, "permissionDecisionReason");
        if (reason.empty()) reason = jget_string(object, "reason", "Blocked by hook");
        return true;
    }
    return false;
}

/// The extra context the hook's JSON output asks to add to the conversation.
std::string json_additional_context(const boost::json::value& parsed) {
    if (!parsed.is_object()) return "";
    const boost::json::object specific = jget_object(parsed.as_object(), "hookSpecificOutput");
    return jget_string(specific, "additionalContext");
}

} // namespace

std::string_view to_string(Event event) {
    switch (event) {
    case Event::PreToolUse: return "PreToolUse";
    case Event::PostToolUse: return "PostToolUse";
    case Event::UserPromptSubmit: return "UserPromptSubmit";
    case Event::Notification: return "Notification";
    case Event::Stop: return "Stop";
    case Event::SubagentStop: return "SubagentStop";
    case Event::PreCompact: return "PreCompact";
    case Event::SessionStart: return "SessionStart";
    case Event::SessionEnd: return "SessionEnd";
    }
    return "PreToolUse";
}

bool parse_event(std::string_view name, Event& out) {
    if (name == "PreToolUse") {
        out = Event::PreToolUse;
        return true;
    }
    if (name == "PostToolUse") {
        out = Event::PostToolUse;
        return true;
    }
    if (name == "UserPromptSubmit") {
        out = Event::UserPromptSubmit;
        return true;
    }
    if (name == "Notification") {
        out = Event::Notification;
        return true;
    }
    if (name == "Stop") {
        out = Event::Stop;
        return true;
    }
    if (name == "SubagentStop") {
        out = Event::SubagentStop;
        return true;
    }
    if (name == "PreCompact") {
        out = Event::PreCompact;
        return true;
    }
    if (name == "SessionStart") {
        out = Event::SessionStart;
        return true;
    }
    if (name == "SessionEnd") {
        out = Event::SessionEnd;
        return true;
    }
    return false;
}

void Registry::configure(const boost::json::object& settings) {
    hooks_.clear();

    const boost::json::object groups = jget_object(settings, "hooks");
    for (const auto& [name, value] : groups) {
        Event event = Event::PreToolUse;
        if (!parse_event(name, event)) continue;
        if (!value.is_array()) continue;

        for (const auto& entry : value.as_array()) {
            if (!entry.is_object()) continue;
            const boost::json::object group = entry.as_object();
            const std::string matcher = jget_string(group, "matcher");

            for (const auto& item : jget_array(group, "hooks")) {
                if (!item.is_object()) continue;
                const boost::json::object object = item.as_object();
                if (jget_string(object, "type") != "command") continue;

                const std::string command = jget_string(object, "command");
                if (command.empty()) continue;

                Hook hook;
                hook.event = event;
                hook.matcher = matcher;
                hook.command = command;
                hook.timeout_seconds = static_cast<int>(
                    jget_int(object, "timeout", kDefaultTimeoutSeconds));
                if (hook.timeout_seconds <= 0) hook.timeout_seconds = kDefaultTimeoutSeconds;
                hooks_.push_back(std::move(hook));
            }
        }
    }
}

void Registry::add(Hook hook) {
    hooks_.push_back(std::move(hook));
}

std::vector<Hook> Registry::for_event(Event event) const {
    std::vector<Hook> hooks;
    for (const auto& hook : hooks_) {
        if (hook.event == event) hooks.push_back(hook);
    }
    return hooks;
}

std::vector<Hook> Registry::matching(Event event, const std::string& tool_name) const {
    std::vector<Hook> hooks;
    for (const auto& hook : for_event(event)) {
        if (matcher_matches(hook.matcher, tool_name, nullptr)) hooks.push_back(hook);
    }
    return hooks;
}

bool Registry::empty(Event event) const {
    for (const auto& hook : hooks_) {
        if (hook.event == event) return false;
    }
    return true;
}

Result run(const std::vector<Hook>& hooks, const std::string& tool_name,
           const boost::json::object& payload, const std::string& cwd) {
    Result result;

    for (const auto& hook : hooks) {
        std::string matcher_error;
        if (!matcher_matches(hook.matcher, tool_name, &matcher_error)) {
            if (!matcher_error.empty()) append_output(result.error_output, matcher_error);
            continue;
        }
        if (hook.command.empty()) continue;

        // The hook reads the event payload from its standard input.
        boost::json::object input = payload;
        if (input.find("tool_name") == input.end()) {
            input["tool_name"] = boost::json::value(std::string_view(tool_name));
        }
        if (input.find("hook_event_name") == input.end()) {
            input["hook_event_name"] = boost::json::value(std::string_view(to_string(hook.event)));
        }
        const std::string stdin_text = json_dump(boost::json::value(input)) + "\n";

        const ProcessOutcome outcome =
            run_process(hook.command, cwd, stdin_text, hook.timeout_seconds);

        append_output(result.output, outcome.out);
        append_output(result.error_output, outcome.err);
        if (outcome.timed_out) {
            append_output(result.error_output, "hook timed out after " +
                                                  std::to_string(hook.timeout_seconds) + " seconds");
        }
        if (outcome.failed) {
            append_output(result.error_output, "hook could not be run: " + hook.command);
        }

        const boost::json::value parsed = parse_hook_output(outcome.out);

        std::string reason;
        const bool json_blocked = json_blocks(parsed, reason);
        if (reason.empty()) {
            reason = trim(outcome.out);
            if (reason.empty()) reason = trim(outcome.err);
            if (reason.empty()) reason = std::string(kNoOutput);
        }

        const bool blocked = outcome.exit_code == 2 || json_blocked;
        if (blocked) {
            if (!result.blocked) {
                result.blocked = true;
                result.block_reason = reason;
                result.exit_code = outcome.exit_code;
            }
        } else if (!result.blocked) {
            result.exit_code = outcome.exit_code;
        }

        const std::string context = json_additional_context(parsed);
        if (!context.empty()) append_output(result.additional_context, context);
    }

    return result;
}

} // namespace cc::hooks
