#include "types/permissions.hpp"

#include <algorithm>

namespace cc {
namespace {

struct ModeConfig {
    PermissionMode mode;
    std::string_view title;
    std::string_view short_title;
    std::string_view symbol;
    PermissionMode external;
};

constexpr ModeConfig kModeConfigs[] = {
    {PermissionMode::Default, "Default", "Default", "", PermissionMode::Default},
    {PermissionMode::Plan, "Plan Mode", "Plan", "\xE2\x8F\xB8", PermissionMode::Plan},
    {PermissionMode::AcceptEdits, "Accept edits", "Accept", "\xE2\x8F\xB5\xE2\x8F\xB5",
     PermissionMode::AcceptEdits},
    {PermissionMode::BypassPermissions, "Bypass Permissions", "Bypass", "\xE2\x8F\xB5\xE2\x8F\xB5",
     PermissionMode::BypassPermissions},
    {PermissionMode::DontAsk, "Don't Ask", "DontAsk", "\xE2\x8F\xB5\xE2\x8F\xB5",
     PermissionMode::DontAsk},
    // `auto` and `bubble` are internal; they surface as `default` externally.
    {PermissionMode::Auto, "Auto mode", "Auto", "\xE2\x8F\xB5\xE2\x8F\xB5", PermissionMode::Default},
    {PermissionMode::Bubble, "Bubble mode", "Bubble", "\xE2\x8F\xB5\xE2\x8F\xB5",
     PermissionMode::Default},
};

const ModeConfig& mode_config(PermissionMode mode) {
    for (const auto& cfg : kModeConfigs) {
        if (cfg.mode == mode) {
            return cfg;
        }
    }
    return kModeConfigs[0];
}

} // namespace

bool is_external_permission_mode(PermissionMode mode) {
    return mode != PermissionMode::Auto && mode != PermissionMode::Bubble;
}

bool is_internal_permission_mode(PermissionMode mode) {
    return is_external_permission_mode(mode);
}

const std::vector<PermissionMode>& permission_modes() {
    static const std::vector<PermissionMode> kModes = {
        PermissionMode::Default,       PermissionMode::Plan,
        PermissionMode::AcceptEdits,   PermissionMode::BypassPermissions,
        PermissionMode::DontAsk,       PermissionMode::Auto,
    };
    return kModes;
}

std::string_view to_string(PermissionMode mode) {
    switch (mode) {
    case PermissionMode::Default:
        return "default";
    case PermissionMode::Plan:
        return "plan";
    case PermissionMode::AcceptEdits:
        return "acceptEdits";
    case PermissionMode::BypassPermissions:
        return "bypassPermissions";
    case PermissionMode::DontAsk:
        return "dontAsk";
    case PermissionMode::Auto:
        return "auto";
    case PermissionMode::Bubble:
        return "bubble";
    }
    return "default";
}

std::string_view to_string_external(PermissionMode mode) {
    return to_string(mode_config(mode).external);
}

PermissionMode permission_mode_from_string(std::string_view s) {
    for (PermissionMode mode : permission_modes()) {
        if (to_string(mode) == s) {
            return mode;
        }
    }
    return PermissionMode::Default;
}

std::string_view permission_mode_title(PermissionMode mode) {
    return mode_config(mode).title;
}

std::string_view permission_mode_short_title(PermissionMode mode) {
    return mode_config(mode).short_title;
}

std::string_view permission_mode_symbol(PermissionMode mode) {
    return mode_config(mode).symbol;
}

bool is_default_mode(PermissionMode mode) {
    return mode == PermissionMode::Default;
}

std::string_view to_string(PermissionBehavior behavior) {
    switch (behavior) {
    case PermissionBehavior::Allow:
        return "allow";
    case PermissionBehavior::Deny:
        return "deny";
    case PermissionBehavior::Ask:
        return "ask";
    }
    return "allow";
}

PermissionBehavior permission_behavior_from_string(std::string_view s) {
    if (s == "deny") {
        return PermissionBehavior::Deny;
    }
    if (s == "ask") {
        return PermissionBehavior::Ask;
    }
    return PermissionBehavior::Allow;
}

const std::vector<PermissionRuleSource>& permission_rule_sources() {
    static const std::vector<PermissionRuleSource> kSources = {
        PermissionRuleSource::UserSettings, PermissionRuleSource::ProjectSettings,
        PermissionRuleSource::LocalSettings,  PermissionRuleSource::FlagSettings,
        PermissionRuleSource::PolicySettings, PermissionRuleSource::CliArg,
        PermissionRuleSource::Command,        PermissionRuleSource::Session,
    };
    return kSources;
}

std::string_view to_string(PermissionRuleSource source) {
    switch (source) {
    case PermissionRuleSource::UserSettings:
        return "userSettings";
    case PermissionRuleSource::ProjectSettings:
        return "projectSettings";
    case PermissionRuleSource::LocalSettings:
        return "localSettings";
    case PermissionRuleSource::FlagSettings:
        return "flagSettings";
    case PermissionRuleSource::PolicySettings:
        return "policySettings";
    case PermissionRuleSource::CliArg:
        return "cliArg";
    case PermissionRuleSource::Command:
        return "command";
    case PermissionRuleSource::Session:
        return "session";
    }
    return "userSettings";
}

std::string_view setting_source_display_name(PermissionRuleSource source) {
    switch (source) {
    case PermissionRuleSource::UserSettings:
        return "user settings";
    case PermissionRuleSource::ProjectSettings:
        return "project settings";
    case PermissionRuleSource::LocalSettings:
        return "local settings";
    case PermissionRuleSource::FlagSettings:
        return "flag settings";
    case PermissionRuleSource::PolicySettings:
        return "policy settings";
    case PermissionRuleSource::CliArg:
        return "cli argument";
    case PermissionRuleSource::Command:
        return "command";
    case PermissionRuleSource::Session:
        return "session";
    }
    return "user settings";
}

std::string_view to_string(PermissionUpdateDestination destination) {
    switch (destination) {
    case PermissionUpdateDestination::UserSettings:
        return "userSettings";
    case PermissionUpdateDestination::ProjectSettings:
        return "projectSettings";
    case PermissionUpdateDestination::LocalSettings:
        return "localSettings";
    case PermissionUpdateDestination::Session:
        return "session";
    case PermissionUpdateDestination::CliArg:
        return "cliArg";
    }
    return "userSettings";
}

PermissionRuleSource permission_rule_source_for_destination(PermissionUpdateDestination d) {
    switch (d) {
    case PermissionUpdateDestination::UserSettings:
        return PermissionRuleSource::UserSettings;
    case PermissionUpdateDestination::ProjectSettings:
        return PermissionRuleSource::ProjectSettings;
    case PermissionUpdateDestination::LocalSettings:
        return PermissionRuleSource::LocalSettings;
    case PermissionUpdateDestination::Session:
        return PermissionRuleSource::Session;
    case PermissionUpdateDestination::CliArg:
        return PermissionRuleSource::CliArg;
    }
    return PermissionRuleSource::UserSettings;
}

std::optional<PermissionUpdateDestination> permission_update_destination_from_string(
    std::string_view s) {
    for (auto destination : {PermissionUpdateDestination::UserSettings,
                             PermissionUpdateDestination::ProjectSettings,
                             PermissionUpdateDestination::LocalSettings,
                             PermissionUpdateDestination::Session,
                             PermissionUpdateDestination::CliArg}) {
        if (to_string(destination) == s) {
            return destination;
        }
    }
    return std::nullopt;
}

std::string_view to_string(PermissionUpdateType type) {
    switch (type) {
    case PermissionUpdateType::AddRules:
        return "addRules";
    case PermissionUpdateType::ReplaceRules:
        return "replaceRules";
    case PermissionUpdateType::RemoveRules:
        return "removeRules";
    case PermissionUpdateType::SetMode:
        return "setMode";
    case PermissionUpdateType::AddDirectories:
        return "addDirectories";
    case PermissionUpdateType::RemoveDirectories:
        return "removeDirectories";
    }
    return "addRules";
}

ToolPermissionContext empty_tool_permission_context() {
    return ToolPermissionContext{};
}

} // namespace cc
