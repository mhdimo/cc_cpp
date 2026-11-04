#include "permissions/rule_parser.hpp"

#include "tools/tool_names.hpp"

#include <utility>

namespace cc::permissions {
namespace {

/// A legacy tool name and the canonical name it resolves to.
struct Alias {
    std::string_view legacy;
    std::string_view canonical;
};

constexpr Alias kLegacyAliases[] = {
    {tools::LEGACY_AGENT, tools::AGENT},
    {"KillShell", tools::TASK_STOP},
    {"AgentOutputTool", tools::TASK_OUTPUT},
    {"BashOutputTool", tools::TASK_OUTPUT},
    {tools::LEGACY_BRIEF, tools::BRIEF},
};

/// Finds the first occurrence of `ch` that is not preceded by an odd number
/// of backslashes. Returns `std::string::npos` when there is none.
size_t find_first_unescaped(std::string_view s, char ch) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != ch) {
            continue;
        }
        size_t backslashes = 0;
        size_t j = i;
        while (j > 0 && s[j - 1] == '\\') {
            ++backslashes;
            --j;
        }
        if (backslashes % 2 == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

/// Finds the last occurrence of `ch` that is not preceded by an odd number of
/// backslashes. Returns `std::string::npos` when there is none.
size_t find_last_unescaped(std::string_view s, char ch) {
    for (size_t i = s.size(); i-- > 0;) {
        if (s[i] != ch) {
            continue;
        }
        size_t backslashes = 0;
        size_t j = i;
        while (j > 0 && s[j - 1] == '\\') {
            ++backslashes;
            --j;
        }
        if (backslashes % 2 == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

} // namespace

std::string escape_rule_content(std::string_view content) {
    std::string out;
    out.reserve(content.size());
    for (char c : content) {
        if (c == '\\' || c == '(' || c == ')') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string unescape_rule_content(std::string_view content) {
    // Unescape parentheses first, then backslashes — the reverse of the
    // escaping order.
    std::string out;
    out.reserve(content.size());
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\\' && i + 1 < content.size() &&
            (content[i + 1] == '(' || content[i + 1] == ')')) {
            out.push_back(content[i + 1]);
            ++i;
            continue;
        }
        if (content[i] == '\\' && i + 1 < content.size() && content[i + 1] == '\\') {
            out.push_back('\\');
            ++i;
            continue;
        }
        out.push_back(content[i]);
    }
    return out;
}

std::string normalize_legacy_tool_name(std::string_view name) {
    for (const auto& alias : kLegacyAliases) {
        if (alias.legacy == name) {
            return std::string(alias.canonical);
        }
    }
    return std::string(name);
}

std::vector<std::string> legacy_tool_names(std::string_view canonical_name) {
    std::vector<std::string> result;
    for (const auto& alias : kLegacyAliases) {
        if (alias.canonical == canonical_name) {
            result.emplace_back(alias.legacy);
        }
    }
    return result;
}

PermissionRuleValue permission_rule_value_from_string(std::string_view rule_string) {
    const size_t open = find_first_unescaped(rule_string, '(');
    if (open == std::string_view::npos) {
        return PermissionRuleValue{normalize_legacy_tool_name(rule_string), std::nullopt};
    }

    const size_t close = find_last_unescaped(rule_string, ')');
    if (close == std::string_view::npos || close <= open) {
        return PermissionRuleValue{normalize_legacy_tool_name(rule_string), std::nullopt};
    }

    // The closing parenthesis must be the last character.
    if (close != rule_string.size() - 1) {
        return PermissionRuleValue{normalize_legacy_tool_name(rule_string), std::nullopt};
    }

    std::string_view tool_name = rule_string.substr(0, open);
    std::string_view raw_content = rule_string.substr(open + 1, close - open - 1);

    if (tool_name.empty()) {
        return PermissionRuleValue{normalize_legacy_tool_name(rule_string), std::nullopt};
    }

    // Empty content or a bare wildcard means a tool-wide rule.
    if (raw_content.empty() || raw_content == "*") {
        return PermissionRuleValue{normalize_legacy_tool_name(tool_name), std::nullopt};
    }

    return PermissionRuleValue{normalize_legacy_tool_name(tool_name),
                                unescape_rule_content(raw_content)};
}

std::string permission_rule_value_to_string(const PermissionRuleValue& value) {
    if (!value.rule_content.has_value()) {
        return value.tool_name;
    }
    return value.tool_name + "(" + escape_rule_content(*value.rule_content) + ")";
}

PermissionRule permission_rule_from_string(std::string_view rule_string,
                                           PermissionRuleSource source,
                                           PermissionBehavior behavior) {
    PermissionRule rule;
    rule.source = source;
    rule.rule_behavior = behavior;
    rule.rule_value = permission_rule_value_from_string(rule_string);
    return rule;
}

} // namespace cc::permissions
