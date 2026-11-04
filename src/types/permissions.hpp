#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc {

/// The permission mode a session runs under.
enum class PermissionMode {
    Default,
    Plan,
    AcceptEdits,
    BypassPermissions,
    DontAsk,
    Auto,
    Bubble,
};

/// The modes a user may select from the CLI or settings file.
bool is_external_permission_mode(PermissionMode mode);

/// The modes accepted by `settings.json` `defaultMode` and `--permission-mode`.
bool is_internal_permission_mode(PermissionMode mode);

/// All user-addressable modes, in the order they are presented.
const std::vector<PermissionMode>& permission_modes();

std::string_view to_string(PermissionMode mode);
std::string_view to_string_external(PermissionMode mode);

/// Parses a mode name. Unknown names fall back to `Default`.
PermissionMode permission_mode_from_string(std::string_view s);

/// Human readable title, e.g. "Accept edits".
std::string_view permission_mode_title(PermissionMode mode);

/// Compact title used in the status line, e.g. "Accept".
std::string_view permission_mode_short_title(PermissionMode mode);

/// The symbol shown next to the mode in the UI.
std::string_view permission_mode_symbol(PermissionMode mode);

/// Whether the mode is `default` (or unset).
bool is_default_mode(PermissionMode mode);

/// What a rule does when it matches.
enum class PermissionBehavior { Allow, Deny, Ask };

std::string_view to_string(PermissionBehavior behavior);
PermissionBehavior permission_behavior_from_string(std::string_view s);

/// Where a permission rule came from.
enum class PermissionRuleSource {
    UserSettings,
    ProjectSettings,
    LocalSettings,
    FlagSettings,
    PolicySettings,
    CliArg,
    Command,
    Session,
};

/// All rule sources, in the order rules are evaluated.
const std::vector<PermissionRuleSource>& permission_rule_sources();

std::string_view to_string(PermissionRuleSource source);

/// Lowercase display name for the source, e.g. "user settings".
std::string_view setting_source_display_name(PermissionRuleSource source);

/// The tool a rule applies to, plus optional content (the text inside the
/// parentheses).
struct PermissionRuleValue {
    std::string tool_name;
    std::optional<std::string> rule_content;

    bool operator==(const PermissionRuleValue& other) const {
        return tool_name == other.tool_name && rule_content == other.rule_content;
    }
};

/// A rule with its origin and behavior.
struct PermissionRule {
    PermissionRuleSource source = PermissionRuleSource::UserSettings;
    PermissionBehavior rule_behavior = PermissionBehavior::Allow;
    PermissionRuleValue rule_value;
};

/// Where a permission update is written to.
enum class PermissionUpdateDestination {
    UserSettings,
    ProjectSettings,
    LocalSettings,
    Session,
    CliArg,
};

std::string_view to_string(PermissionUpdateDestination destination);
std::optional<PermissionUpdateDestination> permission_update_destination_from_string(
    std::string_view s);

/// The rule source a permission update is written to.
///
/// Rules are stored per source; an update destination names the source the
/// rules end up in.
PermissionRuleSource permission_rule_source_for_destination(PermissionUpdateDestination d);

/// The kinds of mutation that can be applied to the permission configuration.
enum class PermissionUpdateType {
    AddRules,
    ReplaceRules,
    RemoveRules,
    SetMode,
    AddDirectories,
    RemoveDirectories,
};

std::string_view to_string(PermissionUpdateType type);

/// A single mutation of the permission configuration.
struct PermissionUpdate {
    PermissionUpdateType type = PermissionUpdateType::AddRules;
    PermissionUpdateDestination destination = PermissionUpdateDestination::UserSettings;
    std::vector<PermissionRuleValue> rules;
    PermissionBehavior behavior = PermissionBehavior::Allow;
    std::optional<PermissionMode> mode;
    std::vector<std::string> directories;
};

/// An extra working directory the user granted access to.
struct AdditionalWorkingDirectory {
    std::string path;
    std::optional<std::string> date_added;
};

/// Rule strings keyed by the source they came from.
using PermissionRulesBySource = std::map<PermissionRuleSource, std::vector<std::string>>;

/// The complete permission configuration for a session.
struct ToolPermissionContext {
    PermissionMode mode = PermissionMode::Default;
    std::map<std::string, AdditionalWorkingDirectory> additional_working_directories;
    PermissionRulesBySource always_allow_rules;
    PermissionRulesBySource always_deny_rules;
    PermissionRulesBySource always_ask_rules;
    /// True when the user started the session in `bypassPermissions` mode; plan
    /// mode then also bypasses permissions.
    bool is_bypass_permissions_mode_available = false;
    /// True when permission prompts cannot be shown (background agents), so
    /// asking degenerates into denying.
    bool should_avoid_permission_prompts = false;
    /// The mode to restore when leaving plan mode.
    std::optional<PermissionMode> pre_plan_mode;
};

/// An empty context, equivalent to `getEmptyToolPermissionContext`.
ToolPermissionContext empty_tool_permission_context();

} // namespace cc
