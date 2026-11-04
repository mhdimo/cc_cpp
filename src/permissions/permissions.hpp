#pragma once

#include "permissions/decision.hpp"
#include "permissions/rule_parser.hpp"
#include "tools/tool.hpp"
#include "tools/tool_use_context.hpp"
#include "types/permissions.hpp"

#include <optional>
#include <string>
#include <vector>

namespace cc::permissions {

/// All allow rules across every source, in source order.
std::vector<PermissionRule> get_allow_rules(const ToolPermissionContext& context);

/// All deny rules across every source, in source order.
std::vector<PermissionRule> get_deny_rules(const ToolPermissionContext& context);

/// All ask rules across every source, in source order.
std::vector<PermissionRule> get_ask_rules(const ToolPermissionContext& context);

/// The rule that allows the entire tool, or null.
std::optional<PermissionRule> tool_always_allowed_rule(const ToolPermissionContext& context,
                                                        const Tool& tool);

/// The rule that denies the entire tool, or null.
std::optional<PermissionRule> get_deny_rule_for_tool(const ToolPermissionContext& context,
                                                      const Tool& tool);

/// The rule that asks for the entire tool, or null.
std::optional<PermissionRule> get_ask_rule_for_tool(const ToolPermissionContext& context,
                                                     const Tool& tool);

/// The deny rule that blocks a specific agent type, e.g. `Agent(Explore)`.
std::optional<PermissionRule> get_deny_rule_for_agent(const ToolPermissionContext& context,
                                                       const std::string& agent_tool_name,
                                                       const std::string& agent_type);

/// Removes agents denied through `Agent(type)` rules.
template <typename AgentT>
std::vector<AgentT> filter_denied_agents(const std::vector<AgentT>& agents,
                                          const ToolPermissionContext& context,
                                          const std::string& agent_tool_name) {
    std::vector<std::string> denied;
    for (const auto& rule : get_deny_rules(context)) {
        if (rule.rule_value.tool_name == agent_tool_name &&
            rule.rule_value.rule_content.has_value()) {
            denied.push_back(*rule.rule_value.rule_content);
        }
    }
    std::vector<AgentT> result;
    for (const auto& agent : agents) {
        bool blocked = false;
        for (const auto& type : denied) {
            if (agent.agent_type == type) {
                blocked = true;
                break;
            }
        }
        if (!blocked) {
            result.push_back(agent);
        }
    }
    return result;
}

/// The rule contents that apply to `tool_name` for the given behavior,
/// keyed by their content.
std::map<std::string, PermissionRule>
get_rule_by_contents_for_tool_name(const ToolPermissionContext& context,
                                   const std::string& tool_name,
                                   PermissionBehavior behavior);

/// The rule contents that apply to `tool` for the given behavior.
std::map<std::string, PermissionRule>
get_rule_by_contents_for_tool(const ToolPermissionContext& context, const Tool& tool,
                              PermissionBehavior behavior);

/// A rule that can never be reached.
struct UnreachableRule {
    /// The rule that cannot be reached.
    PermissionRule rule;

    /// Why the rule cannot be reached.
    std::string reason;

    /// The rule that hides it.
    PermissionRule shadowed_by;

    /// Whether the rule that hides it denies the tool or asks for it.
    PermissionBehavior shadow_type = PermissionBehavior::Deny;

    /// What the user may do about it.
    std::string fix;
};

/// The rules that can never be reached.
///
/// A rule that names the content of a tool — `Bash(ls:*)` — is hidden by a
/// rule that denies or that asks for the whole tool: the tool-wide rule is
/// read first, so the rule of the content never comes to it.
std::vector<UnreachableRule> detect_unreachable_rules(const ToolPermissionContext& context);

/// Applies a single update to the permission context.
ToolPermissionContext apply_permission_update(const ToolPermissionContext& context,
                                               const PermissionUpdate& update);

/// Applies a list of updates in order.
ToolPermissionContext apply_permission_updates(const ToolPermissionContext& context,
                                                const std::vector<PermissionUpdate>& updates);

/// Adds rules to the context (used for the initial setup).
ToolPermissionContext
apply_permission_rules_to_permission_context(const ToolPermissionContext& context,
                                              const std::vector<PermissionRule>& rules);

/// Replaces the on-disk rules in the context (used when settings change).
ToolPermissionContext
sync_permission_rules_from_disk(const ToolPermissionContext& context,
                                 const std::vector<PermissionRule>& rules);

/// The name a tool is matched by in permission rules.
///
/// MCP tools are matched by their fully qualified `mcp__server__tool` name so
/// that a rule targeting a builtin never matches its MCP replacement.
std::string get_tool_name_for_permission_check(const Tool& tool);

/// Runs the full permission pipeline for a tool call.
///
/// The order of the checks is significant and mirrors the TypeScript
/// implementation:
///
///  1. A deny rule for the whole tool.
///  2. An ask rule for the whole tool.
///  3. The tool's own `check_permissions`.
///  4. `bypassPermissions` (or plan mode started in bypass mode) allows.
///  5. An allow rule for the whole tool.
///  6. Anything still unresolved becomes an ask.
///
/// `dontAsk` mode converts the final ask into a deny, and contexts that cannot
/// show prompts deny outright.
PermissionResult has_permissions_to_use_tool(const Tool& tool,
                                              const boost::json::object& input,
                                              ToolUseContext& context);

/// Runs only the rule-based part of the pipeline: the subset that
/// `bypassPermissions` mode still respects.
///
/// Returns an empty optional when no rule objects.
std::optional<PermissionResult> check_rule_based_permissions(const Tool& tool,
                                                              const boost::json::object& input,
                                                              ToolUseContext& context);

} // namespace cc::permissions
