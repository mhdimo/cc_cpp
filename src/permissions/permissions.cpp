#include "permissions/permissions.hpp"

#include "utils/messages.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <mutex>

namespace cc::permissions {
namespace {

/// The rules of one behavior across all sources.
std::vector<PermissionRule> collect_rules(const PermissionRulesBySource& by_source,
                                          PermissionBehavior behavior) {
    std::vector<PermissionRule> rules;
    for (PermissionRuleSource source : permission_rule_sources()) {
        auto it = by_source.find(source);
        if (it == by_source.end()) {
            continue;
        }
        for (const auto& rule_string : it->second) {
            rules.push_back(permission_rule_from_string(rule_string, source, behavior));
        }
    }
    return rules;
}

/// Parses `mcp__server__tool` into its parts.
struct McpName {
    std::string server_name;
    std::optional<std::string> tool_name;
};

std::optional<McpName> mcp_info_from_string(const std::string& name) {
    static constexpr std::string_view kPrefix = "mcp__";
    if (!cc::starts_with(name, kPrefix)) {
        return std::nullopt;
    }
    const std::string body = name.substr(kPrefix.size());
    const size_t sep = body.find("__");
    if (sep == std::string::npos) {
        return McpName{body, std::nullopt};
    }
    return McpName{body.substr(0, sep), body.substr(sep + 2)};
}

/// True when the rule applies to the tool as a whole (no content).
bool tool_matches_rule(const std::string& name_for_rule_match, const PermissionRule& rule) {
    if (rule.rule_value.rule_content.has_value()) {
        return false;
    }

    if (rule.rule_value.tool_name == name_for_rule_match) {
        return true;
    }

    // A server-level rule ("mcp__server" or "mcp__server__*") matches every
    // tool that server provides.
    auto rule_info = mcp_info_from_string(rule.rule_value.tool_name);
    auto tool_info = mcp_info_from_string(name_for_rule_match);
    if (!rule_info || !tool_info) {
        return false;
    }
    const bool server_wildcard =
        !rule_info->tool_name.has_value() || *rule_info->tool_name == "*";
    return server_wildcard && rule_info->server_name == tool_info->server_name;
}

} // namespace

std::vector<PermissionRule> get_allow_rules(const ToolPermissionContext& context) {
    return collect_rules(context.always_allow_rules, PermissionBehavior::Allow);
}

std::vector<PermissionRule> get_deny_rules(const ToolPermissionContext& context) {
    return collect_rules(context.always_deny_rules, PermissionBehavior::Deny);
}

std::vector<PermissionRule> get_ask_rules(const ToolPermissionContext& context) {
    return collect_rules(context.always_ask_rules, PermissionBehavior::Ask);
}

std::string get_tool_name_for_permission_check(const Tool& tool) {
    auto info = tool.mcp_info();
    if (info) {
        return "mcp__" + info->server_name + "__" + info->tool_name;
    }
    return std::string(tool.name());
}

std::optional<PermissionRule> tool_always_allowed_rule(const ToolPermissionContext& context,
                                                        const Tool& tool) {
    const std::string name = get_tool_name_for_permission_check(tool);
    for (const auto& rule : get_allow_rules(context)) {
        if (tool_matches_rule(name, rule)) {
            return rule;
        }
    }
    return std::nullopt;
}

std::optional<PermissionRule> get_deny_rule_for_tool(const ToolPermissionContext& context,
                                                      const Tool& tool) {
    const std::string name = get_tool_name_for_permission_check(tool);
    for (const auto& rule : get_deny_rules(context)) {
        if (tool_matches_rule(name, rule)) {
            return rule;
        }
    }
    return std::nullopt;
}

std::optional<PermissionRule> get_ask_rule_for_tool(const ToolPermissionContext& context,
                                                     const Tool& tool) {
    const std::string name = get_tool_name_for_permission_check(tool);
    for (const auto& rule : get_ask_rules(context)) {
        if (tool_matches_rule(name, rule)) {
            return rule;
        }
    }
    return std::nullopt;
}

std::optional<PermissionRule> get_deny_rule_for_agent(const ToolPermissionContext& context,
                                                       const std::string& agent_tool_name,
                                                       const std::string& agent_type) {
    for (const auto& rule : get_deny_rules(context)) {
        if (rule.rule_value.tool_name == agent_tool_name &&
            rule.rule_value.rule_content.has_value() &&
            *rule.rule_value.rule_content == agent_type) {
            return rule;
        }
    }
    return std::nullopt;
}

std::map<std::string, PermissionRule>
get_rule_by_contents_for_tool_name(const ToolPermissionContext& context,
                                   const std::string& tool_name,
                                   PermissionBehavior behavior) {
    std::map<std::string, PermissionRule> result;
    std::vector<PermissionRule> rules;
    switch (behavior) {
    case PermissionBehavior::Allow:
        rules = get_allow_rules(context);
        break;
    case PermissionBehavior::Deny:
        rules = get_deny_rules(context);
        break;
    case PermissionBehavior::Ask:
        rules = get_ask_rules(context);
        break;
    }
    for (const auto& rule : rules) {
        if (rule.rule_value.tool_name == tool_name &&
            rule.rule_value.rule_content.has_value() &&
            rule.rule_behavior == behavior) {
            result[*rule.rule_value.rule_content] = rule;
        }
    }
    return result;
}

std::map<std::string, PermissionRule>
get_rule_by_contents_for_tool(const ToolPermissionContext& context, const Tool& tool,
                              PermissionBehavior behavior) {
    return get_rule_by_contents_for_tool_name(context, get_tool_name_for_permission_check(tool),
                                              behavior);
}

namespace {

/// The wording of the fix of a rule that is hidden by another one.
std::string fix_suggestion(PermissionBehavior shadow_type, const PermissionRule& shadowing_rule,
                           const PermissionRule& shadowed_rule) {
    const std::string shadowing_source(setting_source_display_name(shadowing_rule.source));
    const std::string shadowed_source(setting_source_display_name(shadowed_rule.source));
    const std::string& tool_name = shadowing_rule.rule_value.tool_name;

    if (shadow_type == PermissionBehavior::Deny) {
        return "Remove the \"" + tool_name + "\" deny rule from " + shadowing_source +
               ", or remove the specific allow rule from " + shadowed_source;
    }
    return "Remove the \"" + tool_name + "\" ask rule from " + shadowing_source +
           ", or remove the specific allow rule from " + shadowed_source;
}

/// The rule of the whole tool that hides the rules of its content.
std::optional<PermissionRule> whole_tool_rule(const std::vector<PermissionRule>& rules,
                                               const std::string& tool_name) {
    for (const PermissionRule& rule : rules) {
        if (rule.rule_value.tool_name == tool_name && !rule.rule_value.rule_content.has_value()) {
            return rule;
        }
    }
    return std::nullopt;
}

} // namespace

std::vector<UnreachableRule> detect_unreachable_rules(const ToolPermissionContext& context) {
    std::vector<UnreachableRule> unreachable;

    const std::vector<PermissionRule> allow_rules = get_allow_rules(context);
    const std::vector<PermissionRule> ask_rules = get_ask_rules(context);
    const std::vector<PermissionRule> deny_rules = get_deny_rules(context);

    for (const PermissionRule& allow_rule : allow_rules) {
        // A rule of the whole tool cannot be hidden by another one.
        if (!allow_rule.rule_value.rule_content.has_value()) continue;

        const std::string& tool_name = allow_rule.rule_value.tool_name;

        // A rule that denies the whole tool blocks the rule of the content
        // altogether.
        if (auto shadowing = whole_tool_rule(deny_rules, tool_name)) {
            const std::string source(setting_source_display_name(shadowing->source));
            unreachable.push_back(UnreachableRule{
                allow_rule,
                "Blocked by \"" + tool_name + "\" deny rule (from " + source + ")",
                *shadowing,
                PermissionBehavior::Deny,
                fix_suggestion(PermissionBehavior::Deny, *shadowing, allow_rule),
            });
            continue;
        }

        // A rule that asks for the whole tool makes the rule of the content
        // unreachable: the user is asked first.
        if (auto shadowing = whole_tool_rule(ask_rules, tool_name)) {
            const std::string source(setting_source_display_name(shadowing->source));
            unreachable.push_back(UnreachableRule{
                allow_rule,
                "Shadowed by \"" + tool_name + "\" ask rule (from " + source + ")",
                *shadowing,
                PermissionBehavior::Ask,
                fix_suggestion(PermissionBehavior::Ask, *shadowing, allow_rule),
            });
        }
    }

    return unreachable;
}

ToolPermissionContext apply_permission_update(const ToolPermissionContext& context,
                                               const PermissionUpdate& update) {
    ToolPermissionContext result = context;

    auto* target = &result.always_allow_rules;
    switch (update.behavior) {
    case PermissionBehavior::Allow:
        target = &result.always_allow_rules;
        break;
    case PermissionBehavior::Deny:
        target = &result.always_deny_rules;
        break;
    case PermissionBehavior::Ask:
        target = &result.always_ask_rules;
        break;
    }

    const PermissionRuleSource source =
        permission_rule_source_for_destination(update.destination);

    switch (update.type) {
    case PermissionUpdateType::AddRules: {
        auto& list = (*target)[source];
        for (const auto& rule : update.rules) {
            list.push_back(permission_rule_value_to_string(rule));
        }
        break;
    }
    case PermissionUpdateType::ReplaceRules: {
        auto& list = (*target)[source];
        list.clear();
        for (const auto& rule : update.rules) {
            list.push_back(permission_rule_value_to_string(rule));
        }
        break;
    }
    case PermissionUpdateType::RemoveRules: {
        auto it = target->find(source);
        if (it != target->end()) {
            auto& list = it->second;
            for (const auto& rule : update.rules) {
                const std::string text = permission_rule_value_to_string(rule);
                list.erase(std::remove(list.begin(), list.end(), text), list.end());
            }
        }
        break;
    }
    case PermissionUpdateType::SetMode: {
        if (update.mode.has_value()) {
            result.mode = *update.mode;
        }
        break;
    }
    case PermissionUpdateType::AddDirectories: {
        for (const auto& dir : update.directories) {
            AdditionalWorkingDirectory entry;
            entry.path = dir;
            result.additional_working_directories.emplace(dir, entry);
        }
        break;
    }
    case PermissionUpdateType::RemoveDirectories: {
        for (const auto& dir : update.directories) {
            result.additional_working_directories.erase(dir);
        }
        break;
    }
    }
    return result;
}

ToolPermissionContext apply_permission_updates(const ToolPermissionContext& context,
                                                const std::vector<PermissionUpdate>& updates) {
    ToolPermissionContext result = context;
    for (const auto& update : updates) {
        result = apply_permission_update(result, update);
    }
    return result;
}

namespace {

/// Groups rules by source and behavior so they can be turned into updates.
std::vector<PermissionUpdate> convert_rules_to_updates(const std::vector<PermissionRule>& rules,
                                                        PermissionUpdateType type) {
    std::map<std::pair<PermissionRuleSource, PermissionBehavior>, std::vector<PermissionRuleValue>>
        grouped;
    for (const auto& rule : rules) {
        grouped[{rule.source, rule.rule_behavior}].push_back(rule.rule_value);
    }

    std::vector<PermissionUpdate> updates;
    for (const auto& [key, values] : grouped) {
        PermissionUpdate update;
        update.type = type;
        update.destination = static_cast<PermissionUpdateDestination>(key.first);
        update.behavior = key.second;
        update.rules = values;
        updates.push_back(std::move(update));
    }
    return updates;
}

} // namespace

ToolPermissionContext
apply_permission_rules_to_permission_context(const ToolPermissionContext& context,
                                              const std::vector<PermissionRule>& rules) {
    return apply_permission_updates(context, convert_rules_to_updates(rules,
                                                                      PermissionUpdateType::AddRules));
}

ToolPermissionContext
sync_permission_rules_from_disk(const ToolPermissionContext& context,
                                 const std::vector<PermissionRule>& rules) {
    ToolPermissionContext result = context;

    // Clear the disk-backed sources before applying the new rules. Without
    // this, a rule removed from settings would linger in the context because
    // the grouped updates only cover source/behavior pairs that still have
    // rules.
    for (auto destination : {PermissionUpdateDestination::UserSettings,
                              PermissionUpdateDestination::ProjectSettings,
                              PermissionUpdateDestination::LocalSettings}) {
        for (auto behavior : {PermissionBehavior::Allow, PermissionBehavior::Deny,
                              PermissionBehavior::Ask}) {
            PermissionUpdate update;
            update.type = PermissionUpdateType::ReplaceRules;
            update.destination = destination;
            update.behavior = behavior;
            result = apply_permission_update(result, update);
        }
    }

    return apply_permission_updates(result,
                                    convert_rules_to_updates(rules,
                                                             PermissionUpdateType::ReplaceRules));
}

namespace {

/// The input to use after a permission decision, preferring the value the
/// tool produced over the original.
boost::json::object updated_input_or_fallback(const PermissionResult& result,
                                               const boost::json::object& fallback) {
    if (result.updated_input.has_value()) {
        return *result.updated_input;
    }
    return fallback;
}

PermissionResult deny_result(const std::string& message, DecisionReason reason) {
    PermissionResult result;
    result.behavior = PermissionBehaviorKind::Deny;
    result.message = message;
    result.decision_reason = std::move(reason);
    return result;
}

PermissionResult ask_result(const std::string& message, DecisionReason reason) {
    PermissionResult result;
    result.behavior = PermissionBehaviorKind::Ask;
    result.message = message;
    result.decision_reason = std::move(reason);
    return result;
}

PermissionResult allow_result(boost::json::object updated_input, DecisionReason reason) {
    PermissionResult result;
    result.behavior = PermissionBehaviorKind::Allow;
    result.updated_input = std::move(updated_input);
    result.decision_reason = std::move(reason);
    return result;
}

/// The rule-based part of the pipeline, shared by both entry points.
///
/// The tool's own opinion is returned through `tool_result`, so that the
/// caller does not have to ask the tool a second time; the reference calls
/// `checkPermissions` once per tool call.
std::optional<PermissionResult> run_rule_checks(const Tool& tool,
                                                 const boost::json::object& input,
                                                 ToolUseContext& context,
                                                 bool respect_user_interaction,
                                                 PermissionResult& tool_result) {
    tool_result.behavior = PermissionBehaviorKind::Passthrough;
    tool_result.message = create_permission_request_message(std::string(tool.name()));

    // 1a. The whole tool is denied.
    if (auto deny_rule = get_deny_rule_for_tool(context.permission_context, tool)) {
        DecisionReason reason;
        reason.type = DecisionReasonType::Rule;
        reason.rule = *deny_rule;
        return deny_result("Permission to use " + std::string(tool.name()) + " has been denied.",
                           std::move(reason));
    }

    // 1b. The whole tool needs approval.
    if (auto ask_rule = get_ask_rule_for_tool(context.permission_context, tool)) {
        DecisionReason reason;
        reason.type = DecisionReasonType::Rule;
        reason.rule = *ask_rule;
        return ask_result(create_permission_request_message(std::string(tool.name())), reason);
    }

    // 1c. The tool's own opinion.
    try {
        tool_result = tool.check_permissions(input, context);
    } catch (const std::exception&) {
        // A tool that fails to evaluate its permissions passes through; the
        // surrounding pipeline still applies the mode rules.
    }

    // 1d. The tool denied the call outright.
    if (tool_result.is_deny()) {
        return tool_result;
    }

    // 1e. Some tools always need the user, even in bypass mode.
    if (respect_user_interaction && tool.requires_user_interaction() && tool_result.is_ask()) {
        return tool_result;
    }

    // 1f. A content-specific ask rule beats bypass mode.
    if (tool_result.is_ask() && tool_result.decision_reason.has_value() &&
        tool_result.decision_reason->type == DecisionReasonType::Rule &&
        tool_result.decision_reason->rule.rule_behavior == PermissionBehavior::Ask) {
        return tool_result;
    }

    // 1g. Safety checks are bypass-immune.
    if (tool_result.is_ask() && tool_result.decision_reason.has_value() &&
        tool_result.decision_reason->type == DecisionReasonType::SafetyCheck) {
        return tool_result;
    }

    return std::nullopt;
}

} // namespace

std::optional<PermissionResult> check_rule_based_permissions(const Tool& tool,
                                                              const boost::json::object& input,
                                                              ToolUseContext& context) {
    // Step 1e is not replicated here: the caller must have checked
    // `requires_user_interaction` themselves.
    PermissionResult tool_result;
    return run_rule_checks(tool, input, context, /*respect_user_interaction=*/false, tool_result);
}

PermissionResult has_permissions_to_use_tool(const Tool& tool,
                                              const boost::json::object& input,
                                              ToolUseContext& context) {
    if (context.abort_signal && context.abort_signal->aborted()) {
        PermissionResult result;
        result.behavior = PermissionBehaviorKind::Deny;
        result.message = "Request aborted";
        return result;
    }

    // The tool's own opinion, asked once, the way the reference asks it.
    PermissionResult tool_result;
    auto rule_result =
        run_rule_checks(tool, input, context, /*respect_user_interaction=*/true, tool_result);
    if (rule_result.has_value()) {
        auto& result = *rule_result;
        if (result.is_ask() && context.permission_context.mode == PermissionMode::DontAsk) {
            DecisionReason reason;
            reason.type = DecisionReasonType::Mode;
            reason.mode = PermissionMode::DontAsk;
            return deny_result(messages::dont_ask_reject_message(std::string(tool.name())),
                               std::move(reason));
        }
        if (result.is_ask() && context.permission_context.should_avoid_permission_prompts) {
            DecisionReason reason;
            reason.type = DecisionReasonType::AsyncAgent;
            reason.reason = "Permission prompts are not available in this context";
            return deny_result(messages::auto_reject_message(std::string(tool.name())),
                               std::move(reason));
        }
        return result;
    }

    // The tool had no objection; its own result carries the input the mode
    // rules must see.

    // 2a. Bypass mode allows everything the rules did not object to.
    const bool should_bypass =
        context.permission_context.mode == PermissionMode::BypassPermissions ||
        (context.permission_context.mode == PermissionMode::Plan &&
         context.permission_context.is_bypass_permissions_mode_available);
    if (should_bypass) {
        DecisionReason reason;
        reason.type = DecisionReasonType::Mode;
        reason.mode = context.permission_context.mode;
        return allow_result(updated_input_or_fallback(tool_result, input), std::move(reason));
    }

    // 2b. The whole tool is allowed by a rule.
    if (auto always_allowed = tool_always_allowed_rule(context.permission_context, tool)) {
        DecisionReason reason;
        reason.type = DecisionReasonType::Rule;
        reason.rule = *always_allowed;
        return allow_result(updated_input_or_fallback(tool_result, input), std::move(reason));
    }

    // 3. Turn a passthrough into an ask.
    PermissionResult result = tool_result;
    if (result.is_passthrough()) {
        result.behavior = PermissionBehaviorKind::Ask;
        result.message = create_permission_request_message(std::string(tool.name()),
                                                           tool_result.decision_reason);
    }

    if (result.is_ask() && context.permission_context.mode == PermissionMode::DontAsk) {
        DecisionReason reason;
        reason.type = DecisionReasonType::Mode;
        reason.mode = PermissionMode::DontAsk;
        return deny_result(messages::dont_ask_reject_message(std::string(tool.name())),
                           std::move(reason));
    }

    if (result.is_ask() && context.permission_context.should_avoid_permission_prompts) {
        DecisionReason reason;
        reason.type = DecisionReasonType::AsyncAgent;
        reason.reason = "Permission prompts are not available in this context";
        return deny_result(messages::auto_reject_message(std::string(tool.name())),
                           std::move(reason));
    }

    return result;
}

} // namespace cc::permissions

namespace cc {

PermissionResult Tool::check_permissions(const boost::json::object& /*input*/,
                                         ToolUseContext& /*context*/) const {
    PermissionResult result;
    result.behavior = PermissionBehaviorKind::Passthrough;
    return result;
}

} // namespace cc
