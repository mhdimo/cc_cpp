#include <catch2/catch_test_macros.hpp>

#include "config/cleanup.hpp"
#include "config/claude_md.hpp"
#include "config/credential_helpers.hpp"
#include "config/global_config.hpp"
#include "config/managed_settings.hpp"
#include "config/output_styles.hpp"
#include "config/project_key.hpp"
#include "config/settings.hpp"
#include "config/trust.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"
#include "utils/string.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cc;

namespace {

/// Joins the parts of a path.
std::string path_of(std::initializer_list<std::string> parts) {
    std::string out;
    for (const auto& part : parts) out = path_join(out, part);
    return out;
}

/// A folder that holds the configuration of the user while the test runs.
///
/// The variable `CLAUDE_CONFIG_DIR` points the configuration of Claude Code
/// at the folder, and `CLAUDE_CODE_MANAGED_SETTINGS_PATH` points the
/// managed settings at it as well — the tests never read the settings of
/// the machine they run on.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const char* base = std::getenv("TMPDIR");
        std::string root = base != nullptr ? base : "/tmp";
        while (!root.empty() && root.back() == '/') root.pop_back();

        path_ = path_join(path_join(root, "cc-config-test-" + std::to_string(::getpid())),
                          "home-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        remove_dir_all(path_);
        mkdirp(path_);

        had_previous_ = std::getenv("CLAUDE_CONFIG_DIR") != nullptr;
        if (had_previous_) previous_ = std::getenv("CLAUDE_CONFIG_DIR");
        ::setenv("CLAUDE_CONFIG_DIR", path_.c_str(), 1);

        had_managed_ = std::getenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH") != nullptr;
        if (had_managed_) managed_previous_ = std::getenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH");
        ::setenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH",
                 path_join(path_, "managed").c_str(), 1);
    }

    ~TemporaryDirectory() {
        if (had_previous_) {
            ::setenv("CLAUDE_CONFIG_DIR", previous_.c_str(), 1);
        } else {
            ::unsetenv("CLAUDE_CONFIG_DIR");
        }
        if (had_managed_) {
            ::setenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH", managed_previous_.c_str(), 1);
        } else {
            ::unsetenv("CLAUDE_CODE_MANAGED_SETTINGS_PATH");
        }
        remove_dir_all(path_);
    }

    /// The folder the configuration lives in.
    std::string path() const { return path_; }

    /// A folder below the one of the configuration.
    std::string sub(const std::string& name) const {
        const std::string out = path_join(path_, name);
        mkdirp(out);
        return out;
    }

private:
    std::string path_;
    std::string previous_{};
    std::string managed_previous_{};
    bool had_previous_ = false;
    bool had_managed_ = false;
};

/// Writes `contents` in `path`, creating the folders it lives in.
void write(const std::string& path, const std::string& contents) {
    std::string error;
    REQUIRE(write_file(path, contents, &error));
}

/// Writes an object as JSON in `path`.
void write_json(const std::string& path, const boost::json::object& object) {
    write(path, json_dump_pretty(object) + "\n");
}

/// The names of the files of `directory` that start with `prefix`.
std::vector<std::string> names_with_prefix(const std::string& directory,
                                           const std::string& prefix) {
    std::vector<std::string> out;
    for (const auto& name : list_dir(directory)) {
        if (starts_with(name, prefix)) out.push_back(name);
    }
    return out;
}

/// True when `haystack` holds `needle`.
bool holds(const std::string& haystack, const std::string& needle) {
    return contains(haystack, needle);
}

} // namespace

TEST_CASE("The global configuration file follows CLAUDE_CONFIG_DIR") {
    TemporaryDirectory home;

    // The variable takes the place of the home directory of the user.
    REQUIRE(config::global_config_path() == path_join(home.path(), ".claude.json"));

    // The legacy file of the reference wins when it is there.
    write(path_join(home.path(), ".config.json"), "{}\n");
    REQUIRE(config::global_config_path() == path_join(home.path(), ".config.json"));
}

TEST_CASE("The configuration keeps a backup of the file it replaces") {
    TemporaryDirectory home;

    boost::json::object first;
    first["theme"] = "light";
    std::string error;
    REQUIRE(config::save_global_config(first, &error));
    REQUIRE(error.empty());

    // The first write has nothing to keep.
    REQUIRE(names_with_prefix(path_join(home.path(), "backups"), ".claude.json.backup.").empty());

    boost::json::object second;
    second["theme"] = "dark";
    REQUIRE(config::save_global_config(second, &error));

    // The second write keeps the file it replaces.
    const std::vector<std::string> backups =
        names_with_prefix(path_join(home.path(), "backups"), ".claude.json.backup.");
    REQUIRE(backups.size() == 1);
    REQUIRE(file_exists(path_join(home.path(), ".claude.json.backup")));

    // The backup holds the old file.
    const auto contents = read_file(path_of({home.path(), "backups", backups.front()}));
    REQUIRE(contents.has_value());
    REQUIRE(holds(*contents, "\"theme\": \"light\""));
}

TEST_CASE("A corrupted configuration file is recovered from the backup") {
    TemporaryDirectory home;

    boost::json::object config;
    config["theme"] = "light";
    std::string error;
    REQUIRE(config::save_global_config(config, &error));
    REQUIRE(config::save_global_config(config, &error));

    // The file cannot be parsed any more.
    write(path_join(home.path(), ".claude.json"), "{ this is not json");

    // The session keeps the settings of the backup.
    const boost::json::object recovered = config::load_global_config();
    REQUIRE(jget_string(recovered, "theme") == "light");

    // The corrupted file is kept, so that the user can look at it.
    const std::vector<std::string> corrupted =
        names_with_prefix(path_join(home.path(), "backups"), ".claude.json.corrupted.");
    REQUIRE(corrupted.size() == 1);
}

TEST_CASE("The environment of the global configuration file is read") {
    TemporaryDirectory home;

    boost::json::object config;
    boost::json::object env;
    env["CC_TEST_GLOBAL_VARIABLE"] = "the value";
    config["env"] = boost::json::value(env);
    std::string error;
    REQUIRE(config::save_global_config(config, &error));

    const auto variables = config::global_config_env();
    REQUIRE(variables.count("CC_TEST_GLOBAL_VARIABLE") == 1);
    REQUIRE(variables.at("CC_TEST_GLOBAL_VARIABLE") == "the value");

    ::unsetenv("CC_TEST_GLOBAL_VARIABLE");
    config::apply_global_config_env();
    const char* value = std::getenv("CC_TEST_GLOBAL_VARIABLE");
    REQUIRE(value != nullptr);
    REQUIRE(std::string(value) == "the value");
    ::unsetenv("CC_TEST_GLOBAL_VARIABLE");
}

TEST_CASE("The theme of the user lives in the global configuration file") {
    TemporaryDirectory home;

    REQUIRE(config::global_theme() == ui::ThemeName::Dark);

    boost::json::object config;
    config["theme"] = "light";
    std::string error;
    REQUIRE(config::save_global_config(config, &error));
    REQUIRE(config::global_theme() == ui::ThemeName::Light);

    // "auto" follows the colour scheme of the system, so no theme is pinned.
    config["theme"] = "auto";
    REQUIRE(config::save_global_config(config, &error));
    REQUIRE_FALSE(config::global_theme().has_value());
}

TEST_CASE("The managed settings are read from the directory of the platform") {
    TemporaryDirectory home;

    const std::string managed = home.sub("managed");
    write_json(path_join(managed, "managed-settings.json"), [] {
        boost::json::object object;
        object["includeCoAuthoredBy"] = false;
        return object;
    }());

    mkdirp(path_join(managed, "managed-settings.d"));
    write_json(path_of({managed, "managed-settings.d", "10-second.json"}), [] {
        boost::json::object object;
        object["cleanupPeriodDays"] = 7;
        return object;
    }());
    write_json(path_of({managed, "managed-settings.d", "20-last.json"}), [] {
        boost::json::object object;
        object["cleanupPeriodDays"] = 9;
        return object;
    }());

    const boost::json::object settings = config::load_managed_settings();
    REQUIRE(jget_bool(settings, "includeCoAuthoredBy", true) == false);
    // The files of the drop-in directory are merged in the order of their
    // names; the last one wins.
    REQUIRE(jget_int(settings, "cleanupPeriodDays", 0) == 9);
}

TEST_CASE("The managed settings win over the settings of the user") {
    TemporaryDirectory home;

    write_json(path_join(home.path(), "settings.json"), [] {
        boost::json::object object;
        object["cleanupPeriodDays"] = 3;
        return object;
    }());

    const std::string managed = home.sub("managed");
    write_json(path_join(managed, "managed-settings.json"), [] {
        boost::json::object object;
        object["cleanupPeriodDays"] = 11;
        return object;
    }());

    const config::Settings settings = config::load_settings(home.path());
    REQUIRE(settings.cleanup_period_days == 11);
}

TEST_CASE("A project is keyed on the root of its repository") {
    TemporaryDirectory home;

    const std::string repository = home.sub("repository");
    mkdirp(path_join(repository, ".git"));
    const std::string inside = path_of({repository, "src", "deep"});
    mkdirp(inside);

    // The folder inside the repository and the repository itself share one
    // bucket.
    REQUIRE(config::project_config_key(inside) == config::project_config_key(repository));
    REQUIRE(config::project_config_key(repository) == normalize_path_for_config_key(repository));

    // A folder that is not in a repository is keyed on itself.
    const std::string alone = home.sub("alone");
    REQUIRE(config::project_config_key(alone) == normalize_path_for_config_key(alone));
}

TEST_CASE("The trust of a project is kept in the bucket of the repository") {
    TemporaryDirectory home;

    const std::string repository = home.sub("repository");
    mkdirp(path_join(repository, ".git"));
    const std::string inside = path_join(repository, "src");
    mkdirp(inside);

    REQUIRE_FALSE(config::trust_accepted(inside));

    config::remember_trust(inside);

    // The acceptance is in the bucket of the root of the repository, the way
    // the reference keeps it.
    const boost::json::object global = config::load_global_config();
    const boost::json::object projects = jget_object(global, "projects");
    REQUIRE(projects.find(normalize_path_for_config_key(repository)) != projects.end());

    // The folder of the repository is trusted as well.
    REQUIRE(config::trust_accepted(inside));
    REQUIRE(config::trust_accepted(repository));
}

TEST_CASE("The settings read the keys the reference declares") {
    TemporaryDirectory home;

    write_json(path_join(home.path(), "settings.json"), [] {
        boost::json::object object;

        boost::json::object permissions;
        permissions["disableBypassPermissionsMode"] = "disable";
        permissions["disableAutoMode"] = "disable";
        object["permissions"] = boost::json::value(permissions);

        object["apiKeyHelper"] = "/usr/local/bin/key";
        object["awsAuthRefresh"] = "/usr/local/bin/aws";
        object["awsCredentialExport"] = "/usr/local/bin/creds";
        object["gcpAuthRefresh"] = "gcloud auth print-access-token";
        object["otelHeadersHelper"] = "/usr/local/bin/otel";
        object["attribution"] = boost::json::value([&] {
            boost::json::object attribution;
            attribution["commit"] = "Co-Authored-By: Claude";
            attribution["pr"] = "Generated with Claude";
            return attribution;
        }());
        object["respectGitignore"] = false;
        object["defaultShell"] = "bash";
        object["language"] = "japanese";
        object["autoMemoryEnabled"] = true;
        object["autoMemoryDirectory"] = "~/memory";
        object["autoDreamEnabled"] = true;
        object["claudeMdExcludes"] = boost::json::array({"**/code/CLAUDE.md"});
        object["plansDirectory"] = "docs/plans";
        object["worktree"] = boost::json::value([&] {
            boost::json::object worktree;
            worktree["symlinkDirectories"] = boost::json::array({"node_modules"});
            worktree["sparsePaths"] = boost::json::array({"src"});
            return worktree;
        }());
        object["alwaysThinkingEnabled"] = false;
        object["effortLevel"] = "high";
        object["availableModels"] = boost::json::array({"opus", "sonnet"});
        object["modelOverrides"] = boost::json::value([&] {
            boost::json::object overrides;
            overrides["claude-opus-4-6"] = "arn:aws:bedrock:us-east-1:1:model/x";
            return overrides;
        }());
        object["fastMode"] = true;
        object["spinnerTipsEnabled"] = false;
        object["spinnerTipsOverride"] = boost::json::value([&] {
            boost::json::object tips;
            tips["excludeDefault"] = true;
            tips["tips"] = boost::json::array({"A tip."});
            return tips;
        }());
        object["syntaxHighlightingDisabled"] = true;
        object["terminalTitleFromRename"] = false;
        object["showThinkingSummaries"] = true;
        object["promptSuggestionEnabled"] = false;
        object["defaultView"] = "transcript";
        object["disableAllHooks"] = true;
        object["allowManagedHooksOnly"] = true;
        object["allowedHttpHookUrls"] = boost::json::array({"https://hooks.example.com/*"});
        object["httpHookAllowedEnvVars"] = boost::json::array({"CC_TOKEN"});
        object["strictKnownMarketplaces"] = boost::json::array({"anthropic"});
        object["blockedMarketplaces"] = boost::json::array({"other"});
        object["strictPluginOnlyCustomization"] = true;
        object["minimumVersion"] = "2.0.0";
        object["autoUpdatesChannel"] = "stable";
        object["remote"] = boost::json::value([&] {
            boost::json::object remote;
            remote["defaultEnvironmentId"] = "env-1";
            return remote;
        }());
        object["$schema"] = "https://json.schemastore.org/claude-code-settings.json";
        object["skipWebFetchPreflight"] = true;
        object["enableAllProjectMcpServers"] = true;
        object["enabledMcpjsonServers"] = boost::json::array({"first"});
        object["disabledMcpjsonServers"] = boost::json::array({"second"});
        object["forceLoginMethod"] = "console";
        object["forceLoginOrgUUID"] = "org-1";

        return object;
    }());

    const config::Settings settings = config::load_settings(home.path());

    REQUIRE(settings.disable_bypass_permissions_mode);
    REQUIRE(settings.disable_auto_mode);
    REQUIRE(settings.api_key_helper == "/usr/local/bin/key");
    REQUIRE(settings.aws_auth_refresh == "/usr/local/bin/aws");
    REQUIRE(settings.aws_credential_export == "/usr/local/bin/creds");
    REQUIRE(settings.gcp_auth_refresh == "gcloud auth print-access-token");
    REQUIRE(settings.otel_headers_helper == "/usr/local/bin/otel");
    REQUIRE(settings.attribution_commit == "Co-Authored-By: Claude");
    REQUIRE(settings.attribution_pr == "Generated with Claude");
    REQUIRE_FALSE(settings.respect_gitignore);
    REQUIRE(settings.default_shell == "bash");
    REQUIRE(settings.language == "japanese");
    REQUIRE(settings.auto_memory_enabled == true);
    REQUIRE(settings.auto_memory_directory == "~/memory");
    REQUIRE(settings.auto_dream_enabled == true);
    REQUIRE(settings.claude_md_excludes.size() == 1);
    REQUIRE(settings.plans_directory == "docs/plans");
    REQUIRE(settings.worktree_symlink_directories.size() == 1);
    REQUIRE(settings.worktree_sparse_paths.size() == 1);
    REQUIRE_FALSE(settings.always_thinking_enabled);
    REQUIRE(settings.effort_level == "high");
    REQUIRE(settings.available_models.size() == 2);
    REQUIRE(settings.model_overrides.size() == 1);
    REQUIRE(settings.fast_mode);
    REQUIRE_FALSE(settings.spinner_tips_enabled);
    REQUIRE(settings.spinner_tips_override.is_object());
    REQUIRE(settings.syntax_highlighting_disabled);
    REQUIRE_FALSE(settings.terminal_title_from_rename);
    REQUIRE(settings.show_thinking_summaries);
    REQUIRE_FALSE(settings.prompt_suggestion_enabled);
    REQUIRE(settings.default_view == "transcript");
    REQUIRE(settings.disable_all_hooks);
    REQUIRE(settings.allow_managed_hooks_only);
    REQUIRE(settings.allowed_http_hook_urls.size() == 1);
    REQUIRE(settings.http_hook_allowed_env_vars.size() == 1);
    REQUIRE(settings.strict_known_marketplaces.size() == 1);
    REQUIRE(settings.blocked_marketplaces.size() == 1);
    REQUIRE(settings.strict_plugin_only_customization.is_bool());
    REQUIRE(settings.minimum_version == "2.0.0");
    REQUIRE(settings.auto_updates_channel == "stable");
    REQUIRE(settings.remote_default_environment_id == "env-1");
    REQUIRE(settings.schema == "https://json.schemastore.org/claude-code-settings.json");
    REQUIRE(settings.skip_web_fetch_preflight);
    REQUIRE(settings.enable_all_project_mcp_servers);
    REQUIRE(settings.enabled_mcpjson_servers.size() == 1);
    REQUIRE(settings.disabled_mcpjson_servers.size() == 1);
    REQUIRE(settings.force_login_method == "console");
    REQUIRE(settings.force_login_org_uuid == "org-1");
}

TEST_CASE("The commits carry the co-author by default") {
    TemporaryDirectory home;

    // The reference adds the trailer by default.
    const config::Settings settings = config::load_settings(home.path());
    REQUIRE(settings.include_co_authored_by);

    write_json(path_join(home.path(), "settings.json"), [] {
        boost::json::object object;
        object["includeCoAuthoredBy"] = false;
        return object;
    }());
    REQUIRE_FALSE(config::load_settings(home.path()).include_co_authored_by);
}

TEST_CASE("The servers of the .mcp.json file are asked for") {
    TemporaryDirectory home;

    write_json(path_join(home.path(), "settings.json"), [] {
        boost::json::object object;
        object["enabledMcpjsonServers"] = boost::json::array({"allowed"});
        object["disabledMcpjsonServers"] = boost::json::array({"refused"});
        return object;
    }());

    const config::Settings settings = config::load_settings(home.path());

    REQUIRE(config::mcpjson_server_approval(settings, "allowed") == config::McpServerApproval::Approved);
    REQUIRE(config::mcpjson_server_approval(settings, "refused") == config::McpServerApproval::Rejected);
    REQUIRE(config::mcpjson_server_approval(settings, "unknown") == config::McpServerApproval::Pending);

    // The names are compared the way the reference compares them.
    REQUIRE(config::mcpjson_server_approval(settings, "al lowed") == config::McpServerApproval::Pending);
}

TEST_CASE("Every server of the .mcp.json file is allowed at once") {
    TemporaryDirectory home;

    write_json(path_join(home.path(), "settings.json"), [] {
        boost::json::object object;
        object["enableAllProjectMcpServers"] = true;
        return object;
    }());

    const config::Settings settings = config::load_settings(home.path());
    REQUIRE(config::mcpjson_server_approval(settings, "anything") == config::McpServerApproval::Approved);
}

TEST_CASE("The rules of the user and of the folder are part of the memory") {
    TemporaryDirectory home;

    write(path_of({home.path(), "rules", "user.md"}), "The rule of the user.\n");
    write(path_join(home.path(), "CLAUDE.md"), "The memory of the user.\n");

    const std::string project = home.sub("project");
    mkdirp(path_of({project, ".claude", "rules"}));
    write(path_of({project, ".claude", "rules", "project.md"}), "The rule of the folder.\n");
    write(path_join(project, "CLAUDE.md"), "The memory of the folder.\n");

    const std::vector<config::MemoryFile> files = config::load_memory_files(project);

    bool user_rule = false;
    bool project_rule = false;
    for (const auto& file : files) {
        if (file.path == path_of({home.path(), "rules", "user.md"})) user_rule = true;
        if (file.path == path_of({project, ".claude", "rules", "project.md"})) project_rule = true;
    }
    REQUIRE(user_rule);
    REQUIRE(project_rule);
}

TEST_CASE("The files the project pulls in from outside its folder are asked for") {
    TemporaryDirectory home;

    // A file that lies outside the folder of the session.
    const std::string outside = home.sub("outside");
    const std::string notes = path_join(outside, "notes.md");
    write(notes, "The notes.\n");

    // The memory file of the project pulls it in.
    const std::string project = home.sub("project");
    write(path_join(project, "CLAUDE.md"), "@" + notes + "\n");

    // The file is one of the files that lie outside the folder.
    const std::vector<std::string> includes = config::external_includes(project);
    REQUIRE(includes.size() == 1);
    REQUIRE(includes.front() == notes);

    // It is only read when the user allows it.
    auto holds = [&](bool allowed) {
        for (const auto& file : config::load_memory_files(project, allowed)) {
            if (file.path == notes) return true;
        }
        return false;
    };

    REQUIRE_FALSE(holds(false));
    REQUIRE(holds(true));
}

TEST_CASE("The files of the user are never external ones") {
    TemporaryDirectory home;

    // The memory of the user may pull a file in from anywhere.
    const std::string outside = home.sub("outside");
    const std::string notes = path_join(outside, "notes.md");
    write(notes, "The notes.\n");
    write(path_join(home.path(), "CLAUDE.md"), "@" + notes + "\n");

    const std::string project = home.sub("project");
    write(path_join(project, "CLAUDE.md"), "The memory of the folder.\n");

    // The file is not one of the files that lie outside the folder: the
    // reference leaves the memory of the user alone.
    REQUIRE(config::external_includes(project).empty());

    // It is read all the same, the way the reference reads it.
    bool found = false;
    for (const auto& file : config::load_memory_files(project, false)) {
        if (file.path == notes) found = true;
    }
    REQUIRE(found);
}

TEST_CASE("The answer about the files outside the folder is remembered") {
    TemporaryDirectory home;
    const std::string project = home.sub("project");

    REQUIRE_FALSE(config::external_includes_approved(project));
    REQUIRE_FALSE(config::external_includes_warning_shown(project));

    config::remember_external_includes(project, true);
    REQUIRE(config::external_includes_approved(project));
    REQUIRE(config::external_includes_warning_shown(project));

    // The reference asks the question once: a refusal is remembered as
    // well.
    config::remember_external_includes(project, false);
    REQUIRE_FALSE(config::external_includes_approved(project));
    REQUIRE(config::external_includes_warning_shown(project));
}

TEST_CASE("The output styles of the user and of the folder are read") {
    TemporaryDirectory home;

    write(path_of({home.path(), "output-styles", "mine.md"}),
          "---\nname: Mine\ndescription: The style of the user\n"
          "keep-coding-instructions: true\n---\nDo it my way.\n");

    const std::string project = home.sub("project");
    mkdirp(path_of({project, ".claude", "output-styles"}));
    write(path_of({project, ".claude", "output-styles", "theirs.md"}),
          "---\nname: Theirs\n---\nDo it their way.\n");

    const std::vector<config::OutputStyle> styles = config::load_output_styles(project);
    REQUIRE(styles.size() == 2);

    bool mine = false;
    bool theirs = false;
    for (const auto& style : styles) {
        if (style.name == "Mine") {
            mine = true;
            REQUIRE(style.description == "The style of the user");
            REQUIRE(style.prompt == "Do it my way.");
            REQUIRE(style.keep_coding_instructions == true);
            REQUIRE(style.source == "userSettings");
        }
        if (style.name == "Theirs") {
            theirs = true;
            // A style that does not name a description takes the first line
            // of its body.
            REQUIRE(style.description == "Do it their way.");
            REQUIRE_FALSE(style.keep_coding_instructions.has_value());
            REQUIRE(style.source == "projectSettings");
        }
    }
    REQUIRE(mine);
    REQUIRE(theirs);
}

TEST_CASE("The files that are too old are pruned") {
    TemporaryDirectory home;

    const std::string project = path_join(home.sub("projects"), "project");
    mkdirp(project);
    write(path_join(project, "old.jsonl"), "{}\n");
    write(path_join(project, "new.jsonl"), "{}\n");
    write(path_of({home.path(), "plans", "old.md"}), "# The plan\n");

    // The old files were written a long time ago.
    std::error_code ec;
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 60);
    std::filesystem::last_write_time(path_join(project, "old.jsonl"), old, ec);
    std::filesystem::last_write_time(path_of({home.path(), "plans", "old.md"}), old, ec);

    const config::CleanupResult result = config::prune_old_files(30);
    REQUIRE(result.messages == 2);
    REQUIRE(result.errors == 0);

    REQUIRE_FALSE(file_exists(path_join(project, "old.jsonl")));
    REQUIRE_FALSE(file_exists(path_of({home.path(), "plans", "old.md"})));
    REQUIRE(file_exists(path_join(project, "new.jsonl")));
}

TEST_CASE("The key of the API comes from the helper of the settings") {
    TemporaryDirectory home;

    std::string error;
    const auto key = config::run_api_key_helper("echo sk-the-key", &error);
    REQUIRE(key.has_value());
    REQUIRE(*key == "sk-the-key");

    // A command that fails is an error.
    const auto failed = config::run_api_key_helper("exit 3", &error);
    REQUIRE_FALSE(failed.has_value());
    REQUIRE_FALSE(error.empty());

    // A command that writes nothing is an error as well.
    const auto empty = config::run_api_key_helper("true", &error);
    REQUIRE_FALSE(empty.has_value());
    REQUIRE(error == "did not return a value");
}
