#include "config/settings.hpp"

#include "config/global_config.hpp"
#include "config/managed_settings.hpp"
#include "config/project_key.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <set>

namespace cc::config {
namespace {

/// A settings file location: its path and the source its rules belong to.
struct SettingsLocation {
    /// The path of the file.
    std::string path;
    /// The source the permission rules in the file belong to.
    PermissionRuleSource source;
};

/// The strings in `array`, ignoring the entries that are not strings.
std::vector<std::string> strings_from_array(const boost::json::array& array) {
    std::vector<std::string> out;
    for (const auto& item : array) {
        if (item.is_string()) out.emplace_back(item.as_string().c_str());
    }
    return out;
}

/// Appends the entries of `array` to `out`, skipping the ones already there.
void append_unique(std::vector<std::string>& out, const boost::json::array& array) {
    for (auto& value : strings_from_array(array)) {
        if (std::find(out.begin(), out.end(), value) == out.end()) {
            out.push_back(std::move(value));
        }
    }
}

/// Reads a string member that the settings may hold as a string or as a
/// number.
std::optional<std::string> jget_optional_scalar(const boost::json::object& o,
                                                 std::string_view key) {
    const auto entry = o.find(key);
    if (entry == o.end()) return std::nullopt;
    const boost::json::value& value = entry->value();
    if (value.is_string()) return std::string(value.as_string().c_str());
    if (value.is_int64()) return std::to_string(value.as_int64());
    if (value.is_double()) return std::to_string(value.as_double());
    return std::nullopt;
}

/// Merges the object of the sandbox of one scope into the one that is
/// already there.
///
/// The reference merges the settings of the scopes key by key: the more
/// specific scope wins for the key it names, and the arrays of the scopes
/// are put together.
void merge_sandbox_object(boost::json::object& into, const boost::json::object& from) {
    for (const auto& [key, value] : from) {
        // The objects of the network and of the filesystem are merged key
        // by key as well.
        if (key == "network" || key == "filesystem") {
            if (!value.is_object()) {
                into[key] = value;
                continue;
            }
            boost::json::object nested;
            const auto existing = into.find(key);
            if (existing != into.end() && existing->value().is_object()) {
                nested = existing->value().as_object();
            }
            merge_sandbox_object(nested, value.as_object());
            into[key] = boost::json::value(std::move(nested));
            continue;
        }

        // The arrays of the scopes are put together, without a repetition.
        if (value.is_array()) {
            boost::json::array merged;
            const auto existing = into.find(key);
            if (existing != into.end() && existing->value().is_array()) {
                for (const auto& entry : existing->value().as_array()) {
                    merged.emplace_back(entry);
                }
            }
            for (const auto& entry : value.as_array()) {
                bool known = false;
                for (const auto& item : merged) {
                    if (item == entry) known = true;
                }
                if (!known) merged.emplace_back(entry);
            }
            into[key] = boost::json::value(std::move(merged));
            continue;
        }

        into[key] = value;
    }
}

/// Merges the contents of one settings file into `settings`.
///
/// The permission rules are kept per source; every other setting is
/// overwritten, so that the more specific file wins.
void merge_settings_file(Settings& settings, const boost::json::object& object,
                         PermissionRuleSource source) {
    const auto permissions = jget_object(object, "permissions");

    const boost::json::array allow = jget_array(permissions, "allow");
    if (!allow.empty()) append_unique(settings.allow_rules[source], allow);
    const boost::json::array deny = jget_array(permissions, "deny");
    if (!deny.empty()) append_unique(settings.deny_rules[source], deny);
    const boost::json::array ask = jget_array(permissions, "ask");
    if (!ask.empty()) append_unique(settings.ask_rules[source], ask);

    if (const auto mode = jget_optional_string(permissions, "defaultMode")) {
        settings.default_mode = permission_mode_from_string(*mode);
    }

    // The reference removes the mode that skips the questions from the list
    // of the modes the user may pick when the settings say so.
    if (jget_string(permissions, "disableBypassPermissionsMode") == "disable") {
        settings.disable_bypass_permissions_mode = true;
    }
    if (jget_string(permissions, "disableAutoMode") == "disable") {
        settings.disable_auto_mode = true;
    }
    settings.allow_managed_permission_rules_only =
        jget_bool(object, "allowManagedPermissionRulesOnly", settings.allow_managed_permission_rules_only);
    settings.allow_managed_mcp_servers_only =
        jget_bool(object, "allowManagedMcpServersOnly", settings.allow_managed_mcp_servers_only);

    append_unique(settings.additional_directories,
                  jget_array(permissions, "additionalDirectories"));

    for (const auto& [key, value] : jget_object(object, "env")) {
        if (!value.is_string()) continue;
        const std::string entry(value.as_string().c_str());
        settings.env[key] = entry;
        // The variables of the user are kept apart from the ones of the
        // project: the first are applied before the folder is trusted, the
        // second wait for the trust. The managed settings are trusted as
        // well — the administration wrote them.
        if (source == PermissionRuleSource::UserSettings ||
            source == PermissionRuleSource::PolicySettings ||
            source == PermissionRuleSource::FlagSettings) {
            settings.user_env[key] = entry;
        } else {
            settings.project_env[key] = entry;
        }
    }

    if (const auto model = jget_optional_string(object, "model")) {
        settings.model = model;
    }

    if (const auto theme = jget_optional_string(object, "theme")) {
        ui::ThemeName name = ui::ThemeName::Dark;
        if (*theme == "auto") {
            // "auto" follows the system colour scheme, so no theme is pinned.
            settings.theme.reset();
        } else if (ui::parse_theme_name(*theme, name)) {
            settings.theme = name;
        }
    }

    if (const auto output_style = jget_optional_string(object, "outputStyle")) {
        settings.output_style = output_style;
    }

    settings.include_co_authored_by =
        jget_bool(object, "includeCoAuthoredBy", settings.include_co_authored_by);
    settings.include_git_instructions =
        jget_bool(object, "includeGitInstructions", settings.include_git_instructions);

    settings.prefers_reduced_motion =
        jget_bool(object, "prefersReducedMotion", settings.prefers_reduced_motion);

    settings.cleanup_period_days = static_cast<int>(
        jget_int(object, "cleanupPeriodDays", settings.cleanup_period_days));

    const auto spinner = jget_object(object, "spinnerVerbs");
    if (!spinner.empty()) {
        settings.spinner_verbs_replace = jget_string(spinner, "mode", "append") == "replace";
        settings.spinner_verbs = strings_from_array(jget_array(spinner, "verbs"));
    }
    settings.spinner_tips_enabled =
        jget_bool(object, "spinnerTipsEnabled", settings.spinner_tips_enabled);
    if (const auto tips = object.find("spinnerTipsOverride");
        tips != object.end() && !tips->value().is_null()) {
        settings.spinner_tips_override = tips->value();
    }

    const auto status_line = jget_object(object, "statusLine");
    if (!status_line.empty()) {
        if (const auto command = jget_optional_string(status_line, "command")) {
            settings.status_line_command = command;
        }
        settings.status_line_padding = static_cast<int>(jget_int(status_line, "padding", 0));
    }

    // The servers of the `.mcp.json` file of the project.
    settings.enable_all_project_mcp_servers =
        jget_bool(object, "enableAllProjectMcpServers", settings.enable_all_project_mcp_servers);
    settings.enabled_mcpjson_servers =
        strings_from_array(jget_array(object, "enabledMcpjsonServers"));
    settings.disabled_mcpjson_servers =
        strings_from_array(jget_array(object, "disabledMcpjsonServers"));
    settings.allowed_mcp_servers = jget_array(object, "allowedMcpServers");
    settings.denied_mcp_servers = jget_array(object, "deniedMcpServers");

    // The hooks of every file are gathered: the reference adds the hooks of
    // one file to the hooks of the next, event by event.
    for (const auto& [event, value] : jget_object(object, "hooks")) {
        if (!value.is_array()) continue;
        boost::json::value& target = settings.hooks[event];
        if (!target.is_array()) target = boost::json::array();
        for (const auto& entry : value.as_array()) {
            target.as_array().emplace_back(entry);
        }
    }
    settings.disable_all_hooks = jget_bool(object, "disableAllHooks", settings.disable_all_hooks);
    settings.allow_managed_hooks_only =
        jget_bool(object, "allowManagedHooksOnly", settings.allow_managed_hooks_only);
    settings.allowed_http_hook_urls = strings_from_array(jget_array(object, "allowedHttpHookUrls"));
    settings.http_hook_allowed_env_vars =
        strings_from_array(jget_array(object, "httpHookAllowedEnvVars"));

    // The helpers that write the credentials.
    if (const auto helper = jget_optional_string(object, "apiKeyHelper")) {
        settings.api_key_helper = helper;
    }
    if (const auto helper = jget_optional_string(object, "awsCredentialExport")) {
        settings.aws_credential_export = helper;
    }
    if (const auto helper = jget_optional_string(object, "awsAuthRefresh")) {
        settings.aws_auth_refresh = helper;
    }
    if (const auto helper = jget_optional_string(object, "gcpAuthRefresh")) {
        settings.gcp_auth_refresh = helper;
    }
    if (const auto helper = jget_optional_string(object, "otelHeadersHelper")) {
        settings.otel_headers_helper = helper;
    }

    // The command that suggests the files.
    const auto file_suggestion = jget_object(object, "fileSuggestion");
    if (const auto command = jget_optional_string(file_suggestion, "command")) {
        settings.file_suggestion_command = command;
    }

    settings.respect_gitignore = jget_bool(object, "respectGitignore", settings.respect_gitignore);

    // The text the commits and the pull requests carry.
    const auto attribution = jget_object(object, "attribution");
    if (const auto text = jget_optional_string(attribution, "commit")) {
        settings.attribution_commit = text;
    }
    if (const auto text = jget_optional_string(attribution, "pr")) {
        settings.attribution_pr = text;
    }

    // The models.
    settings.available_models = strings_from_array(jget_array(object, "availableModels"));
    for (const auto& [name, id] : jget_object(object, "modelOverrides")) {
        if (!id.is_string()) continue;
        settings.model_overrides[name] = std::string(id.as_string().c_str());
    }

    // The worktrees.
    const auto worktree = jget_object(object, "worktree");
    settings.worktree_symlink_directories =
        strings_from_array(jget_array(worktree, "symlinkDirectories"));
    settings.worktree_sparse_paths = strings_from_array(jget_array(worktree, "sparsePaths"));

    // The plugins and the marketplaces.
    settings.enabled_plugins = jget_object(object, "enabledPlugins");
    settings.extra_known_marketplaces = jget_object(object, "extraKnownMarketplaces");
    settings.strict_known_marketplaces = jget_array(object, "strictKnownMarketplaces");
    settings.blocked_marketplaces = jget_array(object, "blockedMarketplaces");
    settings.plugin_configs = jget_object(object, "pluginConfigs");
    settings.plugin_trust_message = jget_optional_string(object, "pluginTrustMessage");

    // The sign in.
    settings.force_login_method = jget_optional_string(object, "forceLoginMethod");
    settings.force_login_org_uuid = jget_optional_string(object, "forceLoginOrgUUID");

    // The language of the answers.
    settings.language = jget_optional_string(object, "language");

    // The shell of the commands the user writes with `!`.
    settings.default_shell = jget_optional_string(object, "defaultShell");

    settings.skip_web_fetch_preflight =
        jget_bool(object, "skipWebFetchPreflight", settings.skip_web_fetch_preflight);

    // The survey of the session.
    if (const auto rate = object.find("feedbackSurveyRate");
        rate != object.end() && rate->value().is_double()) {
        settings.feedback_survey_rate = rate->value().as_double();
    }

    settings.syntax_highlighting_disabled =
        jget_bool(object, "syntaxHighlightingDisabled", settings.syntax_highlighting_disabled);
    settings.terminal_title_from_rename =
        jget_bool(object, "terminalTitleFromRename", settings.terminal_title_from_rename);
    settings.always_thinking_enabled =
        jget_bool(object, "alwaysThinkingEnabled", settings.always_thinking_enabled);
    settings.effort_level = jget_optional_scalar(object, "effortLevel");
    settings.advisor_model = jget_optional_string(object, "advisorModel");
    settings.fast_mode = jget_bool(object, "fastMode", settings.fast_mode);
    settings.fast_mode_per_session_opt_in =
        jget_bool(object, "fastModePerSessionOptIn", settings.fast_mode_per_session_opt_in);
    settings.prompt_suggestion_enabled =
        jget_bool(object, "promptSuggestionEnabled", settings.prompt_suggestion_enabled);
    settings.show_clear_context_on_plan_accept =
        jget_bool(object, "showClearContextOnPlanAccept", settings.show_clear_context_on_plan_accept);
    settings.agent = jget_optional_string(object, "agent");
    settings.company_announcements = strings_from_array(jget_array(object, "companyAnnouncements"));

    // The remote sessions.
    const auto remote = jget_object(object, "remote");
    settings.remote_default_environment_id =
        jget_optional_string(remote, "defaultEnvironmentId");

    settings.auto_updates_channel = jget_optional_string(object, "autoUpdatesChannel");
    settings.minimum_version = jget_optional_string(object, "minimumVersion");
    settings.plans_directory = jget_optional_string(object, "plansDirectory");
    settings.default_view = jget_optional_string(object, "defaultView");

    // The memory of the project.
    if (const auto enabled = object.find("autoMemoryEnabled");
        enabled != object.end() && enabled->value().is_bool()) {
        settings.auto_memory_enabled = enabled->value().as_bool();
    }
    settings.auto_memory_directory = jget_optional_string(object, "autoMemoryDirectory");
    if (const auto enabled = object.find("autoDreamEnabled");
        enabled != object.end() && enabled->value().is_bool()) {
        settings.auto_dream_enabled = enabled->value().as_bool();
    }

    settings.show_thinking_summaries =
        jget_bool(object, "showThinkingSummaries", settings.show_thinking_summaries);
    settings.skip_dangerous_mode_permission_prompt = jget_bool(
        object, "skipDangerousModePermissionPrompt", settings.skip_dangerous_mode_permission_prompt);
    settings.skip_auto_permission_prompt =
        jget_bool(object, "skipAutoPermissionPrompt", settings.skip_auto_permission_prompt);
    settings.use_auto_mode_during_plan =
        jget_bool(object, "useAutoModeDuringPlan", settings.use_auto_mode_during_plan);
    if (const auto mode = object.find("autoMode");
        mode != object.end() && !mode->value().is_null()) {
        settings.auto_mode = mode->value();
    }
    if (jget_string(object, "disableAutoMode") == "disable") {
        settings.disable_auto_mode = true;
    }

    settings.ssh_configs = jget_array(object, "sshConfigs");
    settings.claude_md_excludes = strings_from_array(jget_array(object, "claudeMdExcludes"));

    if (const auto value = object.find("strictPluginOnlyCustomization");
        value != object.end() && !value->value().is_null()) {
        settings.strict_plugin_only_customization = value->value();
    }

    settings.schema = jget_optional_string(object, "$schema");
}

/// The rules of `rules`, flattened across the sources in source order.
boost::json::array flatten_rules(const PermissionRulesBySource& rules) {
    boost::json::array out;
    for (const auto& [source, entries] : rules) {
        for (const auto& entry : entries) out.emplace_back(entry);
    }
    return out;
}

/// The MCP servers of the user, from the global configuration file.
void merge_user_mcp_servers(Settings& settings, const boost::json::object& global) {
    for (const auto& [name, server] : jget_object(global, "mcpServers")) {
        if (!server.is_object()) continue;
        settings.mcp_servers[name] = McpServer{server.as_object(), "user"};
    }
}

/// The MCP servers of the project, from the `.mcp.json` file of the folder.
void merge_project_mcp_servers(Settings& settings, const std::string& cwd) {
    const auto contents = read_file(path_join(cwd, ".mcp.json"));
    if (!contents) return;

    const boost::json::value parsed = cc::json_parse(*contents, boost::json::value(nullptr));
    if (!parsed.is_object()) return;

    for (const auto& [name, server] : jget_object(parsed.as_object(), "mcpServers")) {
        if (!server.is_object()) continue;
        settings.mcp_servers[name] = McpServer{server.as_object(), "project"};
    }
}

/// The MCP servers of the session, from the global configuration file.
///
/// The user keeps the servers of one session in `projects[<folder>]`.
void merge_local_mcp_servers(Settings& settings, const boost::json::object& global,
                             const std::string& cwd) {
    // The reference keys the projects on the root of the repository, so
    // that a folder inside a repository and the repository itself share
    // one bucket.
    const std::string key = project_config_key(cwd);

    const auto projects = jget_object(global, "projects");
    for (const auto& [folder, entry] : projects) {
        if (folder != key) continue;
        if (!entry.is_object()) continue;

        for (const auto& [name, server] : jget_object(entry.as_object(), "mcpServers")) {
            if (!server.is_object()) continue;
            settings.mcp_servers[name] = McpServer{server.as_object(), "local"};
        }
    }
}

/// The name of a server of an MCP client, the way the reference writes it.
///
/// Every character that is not a letter, a digit, an underline or a dash
/// becomes an underline, so that the names of the servers of the settings
/// and the names of the servers of the `.mcp.json` file can be compared.
std::string normalize_mcp_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
        out.push_back(keep ? c : '_');
    }
    return out;
}

} // namespace

boost::json::value Settings::to_json() const {
    boost::json::object permissions;

    const boost::json::array allow = flatten_rules(allow_rules);
    if (!allow.empty()) permissions["allow"] = allow;
    const boost::json::array deny = flatten_rules(deny_rules);
    if (!deny.empty()) permissions["deny"] = deny;
    const boost::json::array ask = flatten_rules(ask_rules);
    if (!ask.empty()) permissions["ask"] = ask;

    permissions["defaultMode"] = std::string(to_string(default_mode));

    if (!additional_directories.empty()) {
        boost::json::array directories;
        for (const auto& directory : additional_directories) directories.emplace_back(directory);
        permissions["additionalDirectories"] = std::move(directories);
    }

    if (disable_bypass_permissions_mode) permissions["disableBypassPermissionsMode"] = "disable";
    if (disable_auto_mode) permissions["disableAutoMode"] = "disable";

    boost::json::object root;
    root["permissions"] = std::move(permissions);

    if (!env.empty()) {
        boost::json::object variables;
        for (const auto& [key, value] : env) variables[key] = value;
        root["env"] = std::move(variables);
    }

    if (model) root["model"] = *model;
    if (theme) root["theme"] = std::string(ui::to_string(*theme));
    if (output_style) root["outputStyle"] = *output_style;
    if (!include_co_authored_by) root["includeCoAuthoredBy"] = false;
    if (cleanup_period_days != 30) {
        root["cleanupPeriodDays"] = static_cast<int64_t>(cleanup_period_days);
    }

    if (!spinner_verbs.empty()) {
        boost::json::object spinner;
        spinner["mode"] = std::string(spinner_verbs_replace ? "replace" : "append");
        boost::json::array verbs;
        for (const auto& verb : spinner_verbs) verbs.emplace_back(verb);
        spinner["verbs"] = std::move(verbs);
        root["spinnerVerbs"] = std::move(spinner);
    }

    if (status_line_command) {
        boost::json::object status_line;
        status_line["type"] = std::string("command");
        status_line["command"] = *status_line_command;
        if (status_line_padding != 0) status_line["padding"] = status_line_padding;
        root["statusLine"] = std::move(status_line);
    }

    if (!mcp_servers.empty()) {
        boost::json::object servers;
        for (const auto& [name, server] : mcp_servers) {
            servers[name] = boost::json::value(server.config);
        }
        root["mcpServers"] = std::move(servers);
    }

    if (!hooks.empty()) root["hooks"] = boost::json::value(hooks);

    return boost::json::value(std::move(root));
}

std::string user_settings_path() {
    return cc::path_join(cc::claude_config_dir(), "settings.json");
}

std::string project_settings_path(const std::string& cwd) {
    return cc::path_join(cc::path_join(cc::resolve(cwd), ".claude"), "settings.json");
}

std::string local_settings_path(const std::string& cwd) {
    return cc::path_join(cc::path_join(cc::resolve(cwd), ".claude"), "settings.local.json");
}

Settings load_settings(const std::string& cwd, bool include_project_servers) {
    Settings settings;

    // The global configuration is read once: the theme of the user lives in
    // it, and so do the environment variables and the servers.
    const boost::json::object global = load_global_config();

    // The theme of the user lives in the global configuration file; the
    // settings file may name one as well, and then it wins.
    settings.theme = global_theme(global);
    settings.global_env = global_config_env(global);

    const std::vector<SettingsLocation> locations = {
        {user_settings_path(), PermissionRuleSource::UserSettings},
        {project_settings_path(cwd), PermissionRuleSource::ProjectSettings},
        {local_settings_path(cwd), PermissionRuleSource::LocalSettings},
    };

    // The same file can be reached through more than one location, for
    // instance when the session runs in the home directory. It is only read
    // once, and its rules belong to the first source that reached it.
    std::set<std::string> seen;

    for (const auto& location : locations) {
        SettingsFile file;
        file.path = location.path;
        file.contents = boost::json::value(nullptr);
        file.exists = cc::file_exists(location.path);

        if (file.exists && seen.insert(cc::normalize(location.path)).second) {
            if (const auto text = cc::read_file(location.path)) {
                const boost::json::value parsed =
                    cc::json_parse(*text, boost::json::value(nullptr));
                if (parsed.is_object()) {
                    file.contents = parsed;
                    merge_settings_file(settings, parsed.as_object(), location.source);

                    // The settings of the sandbox are merged scope by
                    // scope, the way the reference merges them: the more
                    // specific scope wins for the key it names, and the keys
                    // it does not name are kept.
                    const boost::json::object& object = parsed.as_object();
                    const auto sandbox = object.find("sandbox");
                    if (sandbox != object.end() && sandbox->value().is_object()) {
                        boost::json::object merged;
                        if (settings.sandbox.is_object()) merged = settings.sandbox.as_object();
                        merge_sandbox_object(merged, sandbox->value().as_object());
                        settings.sandbox = boost::json::value(std::move(merged));
                    }
                }
            }
        }

        settings.files.push_back(std::move(file));
    }

    // The managed settings of the administration win over the settings of
    // the files, the way the reference merges them.
    {
        const boost::json::object managed = load_managed_settings();
        if (!managed.empty()) {
            SettingsFile file;
            file.path = managed_settings_path();
            file.contents = boost::json::value(managed);
            file.exists = true;
            merge_settings_file(settings, managed, PermissionRuleSource::PolicySettings);
            settings.files.push_back(std::move(file));
        }
    }

    // The servers of the MCP clients, in the order of their scope: the more
    // specific scope wins.
    merge_user_mcp_servers(settings, global);
    if (include_project_servers) merge_project_mcp_servers(settings, cwd);
    merge_local_mcp_servers(settings, global, cwd);

    return settings;
}

McpServerApproval mcpjson_server_approval(const Settings& settings, const std::string& name) {
    const std::string normalized = normalize_mcp_name(name);

    for (const auto& refused : settings.disabled_mcpjson_servers) {
        if (normalize_mcp_name(refused) == normalized) return McpServerApproval::Rejected;
    }

    for (const auto& allowed : settings.enabled_mcpjson_servers) {
        if (normalize_mcp_name(allowed) == normalized) return McpServerApproval::Approved;
    }
    if (settings.enable_all_project_mcp_servers) return McpServerApproval::Approved;

    return McpServerApproval::Pending;
}

bool write_settings(const std::string& path, const boost::json::value& contents,
                    std::string* error) {
    std::string text = cc::json_dump_pretty(contents);
    text += "\n";
    return cc::write_file(path, text, error);
}

} // namespace cc::config
