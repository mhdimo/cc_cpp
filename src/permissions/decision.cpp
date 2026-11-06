#include "permissions/decision.hpp"

#include "permissions/rule_parser.hpp"
#include "utils/string.hpp"

#include <utility>

namespace cc {

std::string_view to_string(PermissionBehaviorKind kind) {
    switch (kind) {
    case PermissionBehaviorKind::Allow:
        return "allow";
    case PermissionBehaviorKind::Deny:
        return "deny";
    case PermissionBehaviorKind::Ask:
        return "ask";
    case PermissionBehaviorKind::Passthrough:
        return "passthrough";
    }
    return "passthrough";
}

std::string_view rule_behavior_description(PermissionBehaviorKind kind) {
    switch (kind) {
    case PermissionBehaviorKind::Allow:
        return "allowed";
    case PermissionBehaviorKind::Deny:
        return "denied";
    default:
        return "asked for confirmation for";
    }
}

std::string create_permission_request_message(const std::string& tool_name,
                                              const std::optional<DecisionReason>& reason) {
    if (reason.has_value()) {
        switch (reason->type) {
        case DecisionReasonType::Classifier:
            return "Classifier '" + reason->classifier + "' requires approval for this " + tool_name +
                   " command: " + reason->reason;
        case DecisionReasonType::Hook: {
            if (reason->reason.empty()) {
                return "Hook '" + reason->hook_name + "' requires approval for this " + tool_name +
                       " command";
            }
            return "Hook '" + reason->hook_name + "' blocked this action: " + reason->reason;
        }
        case DecisionReasonType::Rule: {
            const std::string rule_string =
                permissions::permission_rule_value_to_string(reason->rule.rule_value);
            const std::string source_string =
                std::string(setting_source_display_name(reason->rule.source));
            return "Permission rule '" + rule_string + "' from " + source_string +
                   " requires approval for this " + tool_name + " command";
        }
        case DecisionReasonType::SubcommandResults: {
            std::vector<std::string> needs_approval;
            for (const auto& [cmd, result] : reason->reasons) {
                if (result->behavior == PermissionBehaviorKind::Ask ||
                    result->behavior == PermissionBehaviorKind::Passthrough) {
                    needs_approval.push_back(cmd);
                }
            }
            if (!needs_approval.empty()) {
                const size_t n = needs_approval.size();
                const std::string part = n == 1 ? "part" : "parts";
                const std::string verb = n == 1 ? "requires" : "require";
                return "This " + tool_name + " command contains multiple operations. The following " +
                       std::to_string(n) + " " + part + " " + verb + " approval: " +
                       join(needs_approval, ", ");
            }
            return "This " + tool_name +
                   " command contains multiple operations that require approval";
        }
        case DecisionReasonType::PermissionPromptTool:
            return "Tool '" + reason->permission_prompt_tool_name + "' requires approval for this " +
                   tool_name + " command";
        case DecisionReasonType::SandboxOverride:
            return "Run outside of the sandbox";
        case DecisionReasonType::WorkingDir:
        case DecisionReasonType::SafetyCheck:
        case DecisionReasonType::Other:
            return reason->reason;
        case DecisionReasonType::Mode:
            return "Current permission mode (" +
                   std::string(permission_mode_title(reason->mode)) +
                   ") requires approval for this " + tool_name + " command";
        case DecisionReasonType::AsyncAgent:
            return reason->reason;
        }
    }

    return "Claude requested permissions to use " + tool_name + ", but you haven't granted it yet.";
}

} // namespace cc
