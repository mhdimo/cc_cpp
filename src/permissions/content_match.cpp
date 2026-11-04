#include "permissions/content_match.hpp"

#include "permissions/gitignore.hpp"
#include "permissions/permissions.hpp"
#include "permissions/shell_rules.hpp"
#include "tools/tool.hpp"
#include "tools/tool_use_context.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <map>
#include <set>
#include <utility>
#include <vector>

namespace cc::permissions {
namespace {

/// The name of the tool that runs the commands.
constexpr std::string_view kBashTool = "Bash";

/// The name of the tool that reads the files.
constexpr std::string_view kReadTool = "Read";

/// The name of the tool that changes the files.
constexpr std::string_view kEditTool = "Edit";

/// The name of the tool that fetches the pages.
constexpr std::string_view kWebFetchTool = "WebFetch";

/// The verdict of one command against the rules.
struct ShellVerdict {
    /// The rule that denies the command.
    std::optional<PermissionRule> deny;

    /// The rule that asks the user.
    std::optional<PermissionRule> ask;

    /// The rule that allows the command.
    std::optional<PermissionRule> allow;
};

/// The commands a rule is matched against.
///
/// The command is tried as it is and without its output redirections; the
/// wrappers and the variables that are safe to strip are taken off as well.
///
/// The rules that deny or that ask strip every variable of the environment,
/// so that a denied command stays denied behind one of them. The rules that
/// allow strip only the variables that cannot execute code, the way the
/// reference strips them.
std::vector<std::string> shell_candidates(const std::string& command, ShellMatchMode mode,
                                           bool strip_all_env_vars) {
    const std::string without_redirections = strip_output_redirections(command);

    std::vector<std::string> base;
    if (mode == ShellMatchMode::Exact) {
        // The command as it is, to keep the quotes of it, and the command
        // without its redirections.
        base.push_back(command);
        base.push_back(without_redirections);
    } else {
        base.push_back(without_redirections);
    }

    std::vector<std::string> candidates;
    for (const std::string& cmd : base) {
        const std::string stripped = strip_safe_wrappers(cmd);
        if (stripped != cmd) candidates.push_back(cmd);
        candidates.push_back(stripped);
    }

    if (!strip_all_env_vars) return candidates;

    // The variables and the wrappers are stripped again and again, until
    // nothing new comes out, so that a command that hides behind them is
    // matched too.
    std::set<std::string> seen(candidates.begin(), candidates.end());
    size_t index = 0;
    while (index < candidates.size()) {
        const size_t end = candidates.size();
        for (size_t i = index; i < end; ++i) {
            const std::string cmd = candidates[i];

            const std::string without_env = strip_all_leading_env_vars(cmd);
            if (seen.insert(without_env).second) candidates.push_back(without_env);

            const std::string without_wrapper = strip_safe_wrappers(cmd);
            if (seen.insert(without_wrapper).second) candidates.push_back(without_wrapper);
        }
        index = end;
    }

    return candidates;
}

/// True when the rule matches one of the candidates.
bool shell_rule_matches(const ShellRule& rule, const std::vector<std::string>& candidates,
                        ShellMatchMode mode) {
    for (const std::string& candidate : candidates) {
        switch (rule.type) {
        case ShellRuleType::Exact:
            if (rule.text == candidate) return true;
            break;

        case ShellRuleType::Prefix: {
            if (mode == ShellMatchMode::Exact) {
                // The command has to be the prefix itself.
                if (rule.text == candidate) return true;
                break;
            }
            // The prefix has to end at a word: the rule of `ls` does not
            // match `lsof` nor `lsattr`.
            if (candidate == rule.text) return true;
            if (cc::starts_with(candidate, rule.text + " ")) return true;
            // "xargs grep" runs grep, so the rule of grep matches it too.
            const std::string xargs = "xargs " + rule.text;
            if (candidate == xargs) return true;
            if (cc::starts_with(candidate, xargs + " ")) return true;
            break;
        }

        case ShellRuleType::Wildcard:
            // A wildcard is never matched against the whole command: the
            // `.*` of the pattern would swallow the operators of the
            // command. The command is split into its parts first.
            if (mode == ShellMatchMode::Exact) break;
            if (matches_wildcard(rule.text, candidate)) return true;
            break;
        }
    }
    return false;
}

/// The rule of the behaviour that matches the command, if there is one.
std::optional<PermissionRule> first_matching_shell_rule(const ToolPermissionContext& context,
                                                         const Tool& tool,
                                                         const std::string& command,
                                                         ShellMatchMode mode,
                                                         PermissionBehavior behavior) {
    const auto rules = get_rule_by_contents_for_tool(context, tool, behavior);
    if (rules.empty()) return std::nullopt;

    const bool strip_all = behavior != PermissionBehavior::Allow;
    std::vector<std::string> candidates = shell_candidates(command, mode, strip_all);

    if (behavior == PermissionBehavior::Allow && mode == ShellMatchMode::Prefix) {
        // A rule that allows may not match a compound command: the parts of
        // the command are matched one by one.
        std::vector<std::string> kept;
        for (const std::string& candidate : candidates) {
            if (split_command(candidate).size() > 1) continue;
            kept.push_back(candidate);
        }
        candidates = std::move(kept);
    }

    for (const auto& [content, rule] : rules) {
        const ShellRule parsed = parse_shell_rule(content);
        if (shell_rule_matches(parsed, candidates, mode)) return rule;
    }

    return std::nullopt;
}

/// The verdict of one command against the rules.
///
/// The command is matched exactly first and then as a prefix; a rule that
/// denies the command beats a rule that asks the user.
ShellVerdict judge_shell_command(const ToolPermissionContext& context, const Tool& tool,
                                 const std::string& command) {
    ShellVerdict verdict;

    if (auto rule = first_matching_shell_rule(context, tool, command, ShellMatchMode::Exact,
                                               PermissionBehavior::Deny)) {
        verdict.deny = rule;
        return verdict;
    }
    if (auto rule = first_matching_shell_rule(context, tool, command, ShellMatchMode::Exact,
                                               PermissionBehavior::Ask)) {
        verdict.ask = rule;
        return verdict;
    }
    if (auto rule = first_matching_shell_rule(context, tool, command, ShellMatchMode::Prefix,
                                               PermissionBehavior::Deny)) {
        verdict.deny = rule;
        return verdict;
    }
    if (auto rule = first_matching_shell_rule(context, tool, command, ShellMatchMode::Prefix,
                                               PermissionBehavior::Ask)) {
        verdict.ask = rule;
        return verdict;
    }
    if (auto rule = first_matching_shell_rule(context, tool, command, ShellMatchMode::Exact,
                                               PermissionBehavior::Allow)) {
        verdict.allow = rule;
        return verdict;
    }
    if (auto rule = first_matching_shell_rule(context, tool, command, ShellMatchMode::Prefix,
                                               PermissionBehavior::Allow)) {
        verdict.allow = rule;
        return verdict;
    }

    return verdict;
}

/// The root the rules of the given source are resolved against.
///
/// The rules of the user are resolved against the folder of the settings of
/// the user; the rules of the other sources are resolved against the folder
/// the session started in.
std::string root_path_for_source(PermissionRuleSource source, const std::string& original_cwd) {
    if (source == PermissionRuleSource::UserSettings) {
        return claude_config_dir();
    }
    if (original_cwd.empty()) return cc::cwd();
    return original_cwd;
}

/// A pattern of a path, with the root it is resolved against.
struct PathPattern {
    /// The pattern, relative to the root.
    std::string pattern;

    /// The root the pattern is resolved against; empty when the pattern
    /// matches at every level.
    std::optional<std::string> root;
};

/// The pattern of a rule of a path, with the root it is resolved against.
///
/// A pattern that begins with two separators is fixed to the root of the
/// filesystem, a pattern that begins with a tilde is fixed to the home
/// directory, and a pattern that begins with a separator is fixed to the
/// folder of the settings it comes from. A pattern that names no folder
/// matches at every level.
PathPattern path_pattern_with_root(std::string_view content, PermissionRuleSource source,
                                    const std::string& original_cwd) {
    if (content.size() >= 2 && content[0] == '/' && content[1] == '/') {
        return PathPattern{std::string(content.substr(1)), std::string("/")};
    }
    if (content.size() >= 2 && content[0] == '~' && content[1] == '/') {
        return PathPattern{std::string(content.substr(1)), home_dir()};
    }
    if (!content.empty() && content[0] == '/') {
        return PathPattern{std::string(content), root_path_for_source(source, original_cwd)};
    }

    // The pattern "./.env" is the pattern ".env".
    std::string pattern(content);
    if (pattern.size() >= 2 && pattern[0] == '.' && pattern[1] == '/') pattern.erase(0, 2);
    return PathPattern{std::move(pattern), std::nullopt};
}

/// The rules of the paths of one root.
struct PathGroup {
    /// The root the patterns are resolved against; empty when the patterns
    /// match at every level.
    std::optional<std::string> root;

    /// The patterns of the root, and the rule of each of them.
    std::map<std::string, PermissionRule> patterns;
};

/// The rule of the behaviour whose pattern matches the path, if there is one.
std::optional<PermissionRule> matching_path_rule(const std::map<std::string, PermissionRule>& rules,
                                                  const std::string& path, const RuleRoots& roots) {
    if (rules.empty()) return std::nullopt;

    // The rules are grouped by the root they are resolved against.
    std::vector<PathGroup> groups;
    for (const auto& [content, rule] : rules) {
        const PathPattern pattern =
            path_pattern_with_root(content, rule.source, roots.original_cwd);

        auto it = std::find_if(groups.begin(), groups.end(), [&](const PathGroup& group) {
            return group.root == pattern.root;
        });
        if (it == groups.end()) {
            it = groups.emplace(groups.end(), PathGroup{pattern.root, {}});
        }
        it->patterns[pattern.pattern] = rule;
    }

    const std::string absolute = cc::resolve(cc::trim(path), roots.cwd);

    for (const PathGroup& group : groups) {
        // The "/**" at the end of a pattern is taken off: a pattern that
        // names a folder already takes everything inside it.
        std::vector<std::string> adjusted;
        std::map<std::string, std::string> originals;
        for (const auto& [pattern, rule] : group.patterns) {
            std::string shortened = pattern;
            if (cc::ends_with(shortened, "/**")) shortened.erase(shortened.size() - 3);
            adjusted.push_back(shortened);
            originals.emplace(std::move(shortened), pattern);
        }

        const std::string base = group.root.value_or(roots.cwd);
        const std::string relative = cc::relative_from(base, absolute);
        if (relative == ".." || cc::starts_with(relative, "../")) continue;
        if (relative.empty()) continue;

        const auto matched = gitignore_match(adjusted, relative);
        if (!matched.has_value()) continue;

        // The rule of the pattern that matched.
        const auto original = originals.find(*matched);
        if (original == originals.end()) continue;

        const auto with_wildcard = group.patterns.find(original->second + "/**");
        if (with_wildcard != group.patterns.end()) return with_wildcard->second;

        const auto rule = group.patterns.find(original->second);
        if (rule != group.patterns.end()) return rule->second;
    }

    return std::nullopt;
}

} // namespace

ContentFamily content_family_for_tool(std::string_view tool_name) {
    if (tool_name == kBashTool) return ContentFamily::Shell;
    if (tool_name == kReadTool) return ContentFamily::ReadPath;
    if (tool_name == kEditTool || tool_name == "Write" || tool_name == "NotebookEdit" ||
        tool_name == "MultiEdit") {
        return ContentFamily::EditPath;
    }
    if (tool_name == kWebFetchTool) return ContentFamily::Domain;
    return ContentFamily::None;
}

RuleRoots rule_roots(const ToolUseContext& context) {
    RuleRoots roots;
    roots.original_cwd = context.original_cwd.empty() ? context.options.cwd : context.original_cwd;
    roots.cwd = context.options.cwd.empty() ? cc::cwd() : context.options.cwd;
    return roots;
}

ContentRuleMatch match_shell_rules(const ToolPermissionContext& context, const Tool& tool,
                                   const std::string& command) {
    ContentRuleMatch match;

    const std::string trimmed = cc::trim(command);
    if (trimmed.empty()) return match;

    // The command as a whole, matched exactly: the rule of the shape
    // `Bash(git status)` denies the command itself.
    if (auto rule = first_matching_shell_rule(context, tool, trimmed, ShellMatchMode::Exact,
                                               PermissionBehavior::Deny)) {
        match.deny = rule;
        return match;
    }
    if (auto rule = first_matching_shell_rule(context, tool, trimmed, ShellMatchMode::Exact,
                                               PermissionBehavior::Ask)) {
        match.ask = rule;
        return match;
    }
    const auto exact_allow = first_matching_shell_rule(context, tool, trimmed, ShellMatchMode::Exact,
                                                        PermissionBehavior::Allow);

    // The parts of the command. A command that is made of several parts is
    // matched part by part, so that a denied command cannot hide behind
    // another one.
    std::optional<PermissionRule> deny;
    std::optional<PermissionRule> ask;
    std::optional<PermissionRule> allow;
    bool all_allowed = true;

    for (const std::string& part : split_command(trimmed)) {
        const std::string piece = cc::trim(part);
        if (piece.empty()) continue;

        const ShellVerdict verdict = judge_shell_command(context, tool, piece);
        if (verdict.deny.has_value() && !deny.has_value()) deny = verdict.deny;
        if (verdict.ask.has_value() && !ask.has_value()) ask = verdict.ask;
        if (verdict.allow.has_value() && !allow.has_value()) allow = verdict.allow;
        if (!verdict.allow.has_value()) all_allowed = false;
    }

    // A deny beats an ask, and an ask beats an allow.
    if (deny.has_value()) {
        match.deny = deny;
        return match;
    }
    if (ask.has_value()) {
        match.ask = ask;
        return match;
    }
    if (exact_allow.has_value()) {
        match.allow = exact_allow;
        return match;
    }
    if (all_allowed && allow.has_value()) {
        match.allow = allow;
    }

    return match;
}

ContentRuleMatch match_path_rules(const ToolPermissionContext& context, const Tool& tool,
                                   const std::string& path, const RuleRoots& roots) {
    ContentRuleMatch match;

    const std::string tool_name(tool.name());

    // The rules of the tool itself.
    const auto own_deny = get_rule_by_contents_for_tool(context, tool, PermissionBehavior::Deny);
    const auto own_ask = get_rule_by_contents_for_tool(context, tool, PermissionBehavior::Ask);
    const auto own_allow = get_rule_by_contents_for_tool(context, tool, PermissionBehavior::Allow);

    // The rules of the tool that changes the files: they are the rules of
    // every tool that changes a file.
    const auto edit_deny = get_rule_by_contents_for_tool_name(context, std::string(kEditTool),
                                                              PermissionBehavior::Deny);
    const auto edit_ask = get_rule_by_contents_for_tool_name(context, std::string(kEditTool),
                                                             PermissionBehavior::Ask);
    const auto edit_allow = get_rule_by_contents_for_tool_name(context, std::string(kEditTool),
                                                               PermissionBehavior::Allow);

    if (content_family_for_tool(tool_name) == ContentFamily::ReadPath) {
        // The rules of the tool that reads the file.
        match.deny = matching_path_rule(own_deny, path, roots);
        if (match.deny.has_value()) return match;

        match.ask = matching_path_rule(own_ask, path, roots);
        if (match.ask.has_value()) return match;

        // The access to a file that may be changed is the access to read it
        // too; the reference honours the rules of the changes for a read
        // when they allow it.
        match.allow = matching_path_rule(edit_allow, path, roots);
        if (match.allow.has_value()) return match;

        match.allow = matching_path_rule(own_allow, path, roots);
        return match;
    }

    // The rules of the tool that changes the file, and the rules of the
    // tool that changes the files in general.
    match.deny = matching_path_rule(own_deny, path, roots);
    if (!match.deny.has_value() && tool_name != kEditTool) {
        match.deny = matching_path_rule(edit_deny, path, roots);
    }
    if (match.deny.has_value()) return match;

    match.ask = matching_path_rule(own_ask, path, roots);
    if (!match.ask.has_value() && tool_name != kEditTool) {
        match.ask = matching_path_rule(edit_ask, path, roots);
    }
    if (match.ask.has_value()) return match;

    match.allow = matching_path_rule(edit_allow, path, roots);
    if (!match.allow.has_value() && tool_name != kEditTool) {
        match.allow = matching_path_rule(own_allow, path, roots);
    }

    return match;
}

std::string domain_rule_content(std::string_view hostname) {
    return "domain:" + std::string(hostname);
}

std::string hostname_of_url(std::string_view url) {
    // The scheme comes first, then the host, then the path.
    const size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) return {};

    size_t start = scheme + 3;
    size_t end = url.size();
    for (size_t i = start; i < url.size(); ++i) {
        const char c = url[i];
        if (c == '/' || c == '?' || c == '#') {
            end = i;
            break;
        }
    }

    std::string_view authority = url.substr(start, end - start);

    // The name of the user comes before the host.
    const size_t at = authority.rfind('@');
    if (at != std::string_view::npos) authority.remove_prefix(at + 1);

    // The port comes after the host.
    const size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        authority = authority.substr(0, colon);
    }

    return std::string(authority);
}

ContentRuleMatch match_domain_rules(const ToolPermissionContext& context, const Tool& tool,
                                    const std::string& hostname) {
    ContentRuleMatch match;

    const std::string content = domain_rule_content(hostname);

    const auto deny_rules = get_rule_by_contents_for_tool(context, tool, PermissionBehavior::Deny);
    const auto deny = deny_rules.find(content);
    if (deny != deny_rules.end()) match.deny = deny->second;

    const auto ask_rules = get_rule_by_contents_for_tool(context, tool, PermissionBehavior::Ask);
    const auto ask = ask_rules.find(content);
    if (ask != ask_rules.end()) match.ask = ask->second;

    const auto allow_rules = get_rule_by_contents_for_tool(context, tool, PermissionBehavior::Allow);
    const auto allow = allow_rules.find(content);
    if (allow != allow_rules.end()) match.allow = allow->second;

    return match;
}

ContentRuleMatch match_content_rules(const ToolPermissionContext& context, const Tool& tool,
                                      const boost::json::object& input, const RuleRoots& roots) {
    ContentRuleMatch match;

    const std::optional<std::string> content = tool.permission_content(input);
    if (!content.has_value()) return match;

    switch (content_family_for_tool(tool.name())) {
    case ContentFamily::Shell:
        return match_shell_rules(context, tool, *content);
    case ContentFamily::ReadPath:
    case ContentFamily::EditPath:
        return match_path_rules(context, tool, *content, roots);
    case ContentFamily::Domain:
        return match_domain_rules(context, tool, *content);
    case ContentFamily::None:
        break;
    }

    return match;
}

DecisionReason rule_decision_reason(const PermissionRule& rule) {
    DecisionReason reason;
    reason.type = DecisionReasonType::Rule;
    reason.rule = rule;
    return reason;
}

} // namespace cc::permissions
