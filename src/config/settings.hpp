#pragma once

#include "types/permissions.hpp"
#include "ui/theme.hpp"

#include <boost/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cc::config {

/// One settings file on disk.
struct SettingsFile {
    /// The path the settings were read from.
    std::string path;
    /// The raw contents.
    boost::json::value contents;
    /// True when the file exists.
    bool exists = false;
};

/// One MCP server, as it is configured.
struct McpServer {
    /// The configuration of the server.
    boost::json::object config;

    /// The scope the server comes from: `user`, `project` or `local`.
    std::string scope;
};

/// The state of a server of a `.mcp.json` file.
enum class McpServerApproval {
    /// The server may be started without asking.
    Approved,
    /// The user refused the server.
    Rejected,
    /// The user has to be asked about the server.
    Pending,
};

/// The merged settings for a session.
///
/// Claude Code reads settings from four places and merges them, with the
/// more specific file winning: the user file (`~/.claude/settings.json`),
/// the project file (`.claude/settings.json`), the local project file
/// (`.claude/settings.local.json`) and the managed file of the
/// administration, which wins over all of them.
struct Settings {
    /// The permission rules, per source.
    PermissionRulesBySource allow_rules;
    PermissionRulesBySource deny_rules;
    PermissionRulesBySource ask_rules;

    /// The permission mode the session starts in.
    PermissionMode default_mode = PermissionMode::Default;

    /// True when the user may not pick the mode that skips the questions.
    ///
    /// The reference removes `bypassPermissions` from the list of the modes
    /// the user may choose.
    bool disable_bypass_permissions_mode = false;

    /// True when the user may not pick the mode that lets the model ask.
    bool disable_auto_mode = false;

    /// True when only the rules of the managed settings are honoured.
    bool allow_managed_permission_rules_only = false;

    /// True when only the allowlist of the managed settings is honoured.
    bool allow_managed_mcp_servers_only = false;

    /// Extra directories the session may work in.
    std::vector<std::string> additional_directories;

    /// Environment variables to set for every tool call.
    std::map<std::string, std::string> env;

    /// The environment variables of the user, from the user settings file.
    ///
    /// The reference applies them before the folder is trusted, because the
    /// user wrote them; the ones of the project wait for the trust.
    std::map<std::string, std::string> user_env;

    /// The environment variables of the project, from the project settings
    /// file and from the local one.
    std::map<std::string, std::string> project_env;

    /// The environment variables of the global configuration file.
    ///
    /// The reference applies them before the folder is trusted, because the
    /// user wrote them.
    std::map<std::string, std::string> global_env;

    /// The model to use, when the settings name one.
    std::optional<std::string> model;

    /// The theme to use, when the settings name one.
    ///
    /// The reference keeps the theme in the global configuration file; the
    /// settings file may name one as well.
    std::optional<ui::ThemeName> theme;

    /// The output style to use.
    std::optional<std::string> output_style;

    /// Whether the assistant should add the `Co-Authored-By` trailer to
    /// commits.
    ///
    /// The reference adds it by default.
    bool include_co_authored_by = true;

    /// Whether the instructions of the workflow of git are in the prompt.
    bool include_git_instructions = true;

    /// The animations are turned off.
    ///
    /// The setting is `prefersReducedMotion`; the users who asked for it
    /// get a spinner that does not spin.
    bool prefers_reduced_motion = false;

    /// How many days transcripts are kept.
    int cleanup_period_days = 30;

    /// The spinner verbs, and whether they replace or extend the defaults.
    std::vector<std::string> spinner_verbs;
    bool spinner_verbs_replace = false;

    /// Whether the tips of the spinner are shown.
    bool spinner_tips_enabled = true;

    /// The tips of the spinner, and whether they replace the usual ones.
    boost::json::value spinner_tips_override = boost::json::value{nullptr};

    /// The status line command.
    std::optional<std::string> status_line_command;

    /// The number of columns the status line is indented by.
    int status_line_padding = 0;

    /// The MCP servers, by name.
    ///
    /// The servers of the user come from the global configuration file, the
    /// servers of the project from its `.mcp.json` file, and the servers of
    /// the session from the global configuration file as well. The more
    /// specific scope wins.
    std::map<std::string, McpServer> mcp_servers;

    /// Whether every server of the `.mcp.json` file is allowed.
    bool enable_all_project_mcp_servers = false;

    /// The servers of the `.mcp.json` file the user allowed.
    std::vector<std::string> enabled_mcpjson_servers;

    /// The servers of the `.mcp.json` file the user refused.
    std::vector<std::string> disabled_mcpjson_servers;

    /// The servers the administration allows.
    boost::json::array allowed_mcp_servers;

    /// The servers the administration forbids.
    boost::json::array denied_mcp_servers;

    /// The `sandbox` key of the settings, as it is written.
    boost::json::value sandbox = boost::json::value{nullptr};

    /// The `hooks` key of the settings, as it is written.
    ///
    /// The hooks of the settings files are merged the way the reference
    /// merges them — every file adds its own — and they run beside the
    /// hooks of the plugins.
    boost::json::object hooks;

    /// True when no hook and no status line runs.
    bool disable_all_hooks = false;

    /// True when only the hooks of the managed settings run.
    bool allow_managed_hooks_only = false;

    /// The addresses the hooks of the network may call.
    std::vector<std::string> allowed_http_hook_urls;

    /// The variables the hooks of the network may put in their headers.
    std::vector<std::string> http_hook_allowed_env_vars;

    /// The script that writes the key of the API.
    std::optional<std::string> api_key_helper;

    /// The script that exports the credentials of AWS.
    std::optional<std::string> aws_credential_export;

    /// The script that refreshes the authentication of AWS.
    std::optional<std::string> aws_auth_refresh;

    /// The command that refreshes the authentication of GCP.
    std::optional<std::string> gcp_auth_refresh;

    /// The script that writes the headers of OpenTelemetry.
    std::optional<std::string> otel_headers_helper;

    /// The command that suggests the files the user names with `@`.
    std::optional<std::string> file_suggestion_command;

    /// Whether the file picker respects the `.gitignore` files.
    bool respect_gitignore = true;

    /// The text the commits carry, when the settings name one.
    std::optional<std::string> attribution_commit;

    /// The text the pull requests carry, when the settings name one.
    std::optional<std::string> attribution_pr;

    /// The models the user may pick.
    std::vector<std::string> available_models;

    /// The models of the providers, by the name of the model of Anthropic.
    std::map<std::string, std::string> model_overrides;

    /// The directories a worktree shares with the main repository.
    std::vector<std::string> worktree_symlink_directories;

    /// The directories a worktree writes.
    std::vector<std::string> worktree_sparse_paths;

    /// The plugins the user turned on.
    boost::json::object enabled_plugins;

    /// The marketplaces the project adds.
    boost::json::object extra_known_marketplaces;

    /// The marketplaces the administration allows.
    boost::json::array strict_known_marketplaces;

    /// The marketplaces the administration forbids.
    boost::json::array blocked_marketplaces;

    /// The way the user has to sign in.
    std::optional<std::string> force_login_method;

    /// The organization the sign in has to use.
    std::optional<std::string> force_login_org_uuid;

    /// The language the answers are written in.
    std::optional<std::string> language;

    /// The shell that backs the commands the user writes with `!`.
    ///
    /// The reference uses `bash` on every platform.
    std::optional<std::string> default_shell;

    /// True when the check of the addresses of the network is skipped.
    bool skip_web_fetch_preflight = false;

    /// How often the survey of the session appears.
    std::optional<double> feedback_survey_rate;

    /// True when the code of the diffs is not coloured.
    bool syntax_highlighting_disabled = false;

    /// True when `/rename` changes the title of the terminal.
    bool terminal_title_from_rename = true;

    /// True when the model thinks before it answers.
    bool always_thinking_enabled = true;

    /// The effort the model spends on its answers.
    std::optional<std::string> effort_level;

    /// The model that advises the server.
    std::optional<std::string> advisor_model;

    /// True when the session runs in the fast mode.
    bool fast_mode = false;

    /// True when the fast mode is asked for in every session.
    bool fast_mode_per_session_opt_in = false;

    /// True when the prompt box suggests what to write.
    bool prompt_suggestion_enabled = true;

    /// True when the plan offers to clear the context.
    bool show_clear_context_on_plan_accept = false;

    /// The agent the main thread uses.
    std::optional<std::string> agent;

    /// The announcements of the company, shown at the start of the session.
    std::vector<std::string> company_announcements;

    /// The options of the plugins.
    boost::json::object plugin_configs;

    /// The environment the remote sessions run in.
    std::optional<std::string> remote_default_environment_id;

    /// The channel the updates come from.
    std::optional<std::string> auto_updates_channel;

    /// The version the session may not go under.
    std::optional<std::string> minimum_version;

    /// The directory the plans are written in.
    std::optional<std::string> plans_directory;

    /// The view the session starts in.
    std::optional<std::string> default_view;

    /// True when the session reads and writes the memory of the project.
    std::optional<bool> auto_memory_enabled;

    /// The directory the memory of the project is written in.
    std::optional<std::string> auto_memory_directory;

    /// True when the memory of the project is tidied in the background.
    std::optional<bool> auto_dream_enabled;

    /// True when the summaries of the thinking are shown.
    bool show_thinking_summaries = false;

    /// True when the user accepted the dialog of the mode that skips the
    /// questions.
    bool skip_dangerous_mode_permission_prompt = false;

    /// True when the user accepted the dialog of the mode that lets the
    /// model ask.
    bool skip_auto_permission_prompt = false;

    /// True when the plan uses the semantics of the mode that lets the
    /// model ask.
    bool use_auto_mode_during_plan = true;

    /// The rules of the mode that lets the model ask.
    boost::json::value auto_mode = boost::json::value{nullptr};

    /// The remote hosts the sessions may run on.
    boost::json::array ssh_configs;

    /// The files of the memory the session does not read.
    std::vector<std::string> claude_md_excludes;

    /// The text the warning of the trust of a plugin carries.
    std::optional<std::string> plugin_trust_message;

    /// The customisations that only the plugins may bring.
    boost::json::value strict_plugin_only_customization = boost::json::value{nullptr};

    /// The schema the settings file names, when it names one.
    std::optional<std::string> schema;

    /// The individual settings files, in merge order.
    std::vector<SettingsFile> files;

    /// The settings as they would be written back to disk.
    boost::json::value to_json() const;
};

/// The path of the user settings file.
std::string user_settings_path();

/// The path of the project settings file for `cwd`.
std::string project_settings_path(const std::string& cwd);

/// The path of the local project settings file for `cwd`.
std::string local_settings_path(const std::string& cwd);

/// Reads and merges the settings for `cwd`.
///
/// The servers the folder configures in its `.mcp.json` file are only read
/// when `include_project_servers` is set, which the session does once the
/// folder is trusted.
Settings load_settings(const std::string& cwd, bool include_project_servers = false);

/// The state of the server `name` of the `.mcp.json` file.
///
/// The reference keeps the answer of the user in the settings: a server
/// that is named in `disabledMcpjsonServers` is refused, one that is named
/// in `enabledMcpjsonServers` — or every server, when
/// `enableAllProjectMcpServers` is set — is allowed, and every other server
/// has to be asked for.
McpServerApproval mcpjson_server_approval(const Settings& settings, const std::string& name);

/// Writes `settings` back to `path`, creating parent directories.
bool write_settings(const std::string& path, const boost::json::value& contents,
                    std::string* error = nullptr);

} // namespace cc::config
