#pragma once

#include "types/permissions.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cc::permissions {

/// Escapes backslashes and parentheses so the content can be stored inside a
/// `Tool(content)` rule.
std::string escape_rule_content(std::string_view content);

/// Reverses `escape_rule_content`.
std::string unescape_rule_content(std::string_view content);

/// Maps legacy tool names onto their current canonical names.
std::string normalize_legacy_tool_name(std::string_view name);

/// The legacy names that resolve to `canonical_name`.
std::vector<std::string> legacy_tool_names(std::string_view canonical_name);

/// Parses `"Tool"` or `"Tool(content)"` into its parts.
///
/// Malformed input is treated as a bare tool name, matching the TypeScript
/// implementation: an unmatched parenthesis, trailing content after the
/// closing parenthesis, an empty tool name, empty content, or a bare `*`
/// all degrade to a tool-wide rule.
PermissionRuleValue permission_rule_value_from_string(std::string_view rule_string);

/// Renders a rule value back to its canonical string form.
std::string permission_rule_value_to_string(const PermissionRuleValue& value);

/// Parses a rule string into a rule with the given source and behavior.
PermissionRule permission_rule_from_string(std::string_view rule_string,
                                           PermissionRuleSource source,
                                           PermissionBehavior behavior);

} // namespace cc::permissions
