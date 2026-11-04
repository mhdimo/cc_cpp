#pragma once

#include <string>
#include <string_view>

namespace cc::messages {

/// Shared guidance appended to permission denials, telling the model how to
/// proceed without working around the denial in malicious ways.
inline constexpr std::string_view kDenialWorkaroundGuidance =
    "IMPORTANT: You *may* attempt to accomplish this action using other tools that might "
    "naturally be used to accomplish this goal, e.g. using head instead of cat. But you "
    "*should not* attempt to work around this denial in malicious ways, e.g. do not use your "
    "ability to run tests to execute non-test actions. You should only try to work around this "
    "restriction in reasonable ways that do not attempt to bypass the intent behind this denial. "
    "If you believe this capability is essential to complete the user's request, STOP and explain "
    "to the user what you were trying to do and why you need this permission. Let the user decide "
    "how to proceed.";

/// The message shown when a tool is denied because prompts are unavailable.
std::string auto_reject_message(const std::string& tool_name);

/// The message shown when a tool is denied because the session runs in
/// `don't ask` mode.
std::string dont_ask_reject_message(const std::string& tool_name);

/// The message shown when the auto-mode classifier blocks an action.
std::string build_yolo_rejection_message(const std::string& reason);

/// The message shown when the auto-mode classifier could not be reached.
std::string build_classifier_unavailable_message(const std::string& tool_name,
                                                  const std::string& classifier_model);

/// The prefix that marks a classifier denial, used by the UI to render a
/// condensed summary.
inline constexpr std::string_view kAutoModeRejectionPrefix =
    "Permission for this action has been denied. Reason: ";

/// True when a tool result is a classifier denial.
bool is_classifier_denial(const std::string& content);

} // namespace cc::messages
