#pragma once

#include "permissions/decision.hpp"
#include "types/permissions.hpp"

#include <boost/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace cc {

class Tool;
struct ToolUseContext;

namespace permissions {

/// The kind of content the rules of a tool carry.
enum class ContentFamily {
    /// The tool has no content; only the rules of the whole tool apply.
    None,
    /// The command of a shell command.
    Shell,
    /// The path of a file that is read.
    ReadPath,
    /// The path of a file that is changed.
    EditPath,
    /// The host of a URL.
    Domain,
};

/// The family of the rules of the tool of the name.
ContentFamily content_family_for_tool(std::string_view tool_name);

/// The rule that decides a call.
///
/// The rules are read in the order deny, ask and allow: a deny beats an
/// ask, and an ask beats an allow.
struct ContentRuleMatch {
    /// The rule that denies the call.
    std::optional<PermissionRule> deny;

    /// The rule that asks the user.
    std::optional<PermissionRule> ask;

    /// The rule that allows the call.
    std::optional<PermissionRule> allow;
};

/// The folders the rules of the paths are resolved against.
struct RuleRoots {
    /// The folder the session started in; the rules of the sources that
    /// name a folder are resolved against it.
    std::string original_cwd;

    /// The folder of the session; the rules that name no folder are
    /// resolved against it.
    std::string cwd;
};

/// The folders the rules of the paths of the call are resolved against.
RuleRoots rule_roots(const ToolUseContext& context);

/// Matches the rules of `tool` against the call described by `input`.
///
/// The rules of the whole tool are not read here; the pipeline reads them.
ContentRuleMatch match_content_rules(const ToolPermissionContext& context, const Tool& tool,
                                      const boost::json::object& input, const RuleRoots& roots);

/// Matches the rules of the shell commands against a command.
///
/// The command is split into its parts, and every part is matched on its
/// own, so that a denied command cannot hide behind another one.
ContentRuleMatch match_shell_rules(const ToolPermissionContext& context, const Tool& tool,
                                   const std::string& command);

/// Matches the rules of the paths against a path.
ContentRuleMatch match_path_rules(const ToolPermissionContext& context, const Tool& tool,
                                   const std::string& path, const RuleRoots& roots);

/// Matches the rules of the hosts against a host.
///
/// The host is looked up as it is written: a rule that holds a wildcard
/// never matches.
ContentRuleMatch match_domain_rules(const ToolPermissionContext& context, const Tool& tool,
                                    const std::string& hostname);

/// The content of the rule of a host, e.g. `domain:example.com`.
std::string domain_rule_content(std::string_view hostname);

/// The host of a URL, the way the reference reads it.
///
/// The empty string is returned when the URL names no host.
std::string hostname_of_url(std::string_view url);

/// The explanation of a decision that comes from a rule.
DecisionReason rule_decision_reason(const PermissionRule& rule);

} // namespace permissions
} // namespace cc
