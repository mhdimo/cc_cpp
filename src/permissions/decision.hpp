#pragma once

#include "types/permissions.hpp"

#include <boost/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cc {

/// Why a permission decision was reached.
enum class DecisionReasonType {
    Rule,
    Mode,
    SubcommandResults,
    PermissionPromptTool,
    Hook,
    AsyncAgent,
    SandboxOverride,
    Classifier,
    WorkingDir,
    SafetyCheck,
    Other,
};

/// The outcome of a permission check. Defined below; `DecisionReason`
/// refers to it, so it is forward declared here.
struct PermissionResult;

/// The explanation attached to a permission decision.
struct DecisionReason {
    DecisionReasonType type = DecisionReasonType::Other;

    // type == Rule
    PermissionRule rule;

    // type == Mode
    PermissionMode mode = PermissionMode::Default;

    // type == SubcommandResults
    std::map<std::string, std::shared_ptr<PermissionResult>> reasons;

    // type == PermissionPromptTool
    std::string permission_prompt_tool_name;

    // type == Hook
    std::string hook_name;
    std::optional<std::string> hook_source;

    // type == AsyncAgent / WorkingDir / Classifier / SafetyCheck / Other
    std::string reason;

    // type == Classifier
    std::string classifier;

    // type == SandboxOverride
    std::string sandbox_override;

    // type == SafetyCheck
    bool classifier_approvable = false;
};

/// The behavior a permission check produced.
enum class PermissionBehaviorKind { Allow, Deny, Ask, Passthrough };

std::string_view to_string(PermissionBehaviorKind kind);

/// The outcome of a permission check.
///
/// A `Passthrough` result means the tool had no opinion and the surrounding
/// pipeline decides; `Ask` means the user must approve.
struct PermissionResult {
    PermissionBehaviorKind behavior = PermissionBehaviorKind::Passthrough;
    std::string message;
    std::optional<DecisionReason> decision_reason;
    std::optional<boost::json::object> updated_input;
    std::vector<PermissionUpdate> suggestions;
    std::optional<std::string> blocked_path;
    /// True when the user edited the input before approving.
    bool user_modified = false;
    /// The instructions the user gave while they approved the call.
    ///
    /// The reference hands them to the model with the result of the tool.
    std::string accept_feedback;

    bool is_allow() const { return behavior == PermissionBehaviorKind::Allow; }
    bool is_deny() const { return behavior == PermissionBehaviorKind::Deny; }
    bool is_ask() const { return behavior == PermissionBehaviorKind::Ask; }
    bool is_passthrough() const { return behavior == PermissionBehaviorKind::Passthrough; }
};

/// Prose describing what a permission behavior means, e.g. "allowed".
std::string_view rule_behavior_description(PermissionBehaviorKind kind);

/// The message shown when the model asks for a tool the user has not granted.
///
/// When `reason` is supplied the message explains the specific cause.
std::string create_permission_request_message(const std::string& tool_name,
                                              const std::optional<DecisionReason>& reason = std::nullopt);

} // namespace cc
