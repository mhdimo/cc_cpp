#include "commands/registry.hpp"

#include "auth/oauth.hpp"
#include "auth/storage.hpp"

#include "chrome.hpp"
#include "config/global_config.hpp"
#include "config/output_styles.hpp"

#include "core/ids.hpp"
#include "desktop.hpp"
#include "ide.hpp"
#include "plugins.hpp"
#include "release_notes.hpp"
#include "core/version.hpp"
#include "session/compact.hpp"
#include "session/transcript.hpp"
#include "tools/impl/bash_tool.hpp"
#include "ui/figures.hpp"
#include "ui/qr.hpp"
#include "ui/theme.hpp"
#include "utils/fs.hpp"
#include "utils/git.hpp"
#include "utils/json_util.hpp"
#include "utils/model.hpp"
#include "utils/path.hpp"
#include "utils/process.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <utility>
#include <vector>

namespace cc::commands {
namespace {

/// True when the value of an environment variable says that it is on.
///
/// The reference counts `1`, `true`, `yes` and `on` as on.
bool env_truthy(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) return false;
    const std::string lower = cc::to_lower(cc::trim(value));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

/// True when the session talks to Anthropic itself.
///
/// The reference keeps the customers of another provider and the users of a
/// gateway out of it: the address of the API tells which one it is.
bool first_party_api() {
    if (env_truthy("CLAUDE_CODE_USE_BEDROCK") || env_truthy("CLAUDE_CODE_USE_VERTEX") ||
        env_truthy("CLAUDE_CODE_USE_FOUNDRY")) {
        return false;
    }

    const char* url = std::getenv("ANTHROPIC_BASE_URL");
    if (url == nullptr || *url == '\0') return true;

    // The host of the address is what tells, so the scheme and the path are
    // left out.
    std::string host(url);
    const size_t scheme = host.find("://");
    if (scheme != std::string::npos) host = host.substr(scheme + 3);
    const size_t slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);
    const size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);

    return host == "api.anthropic.com";
}

/// True when the `/install-github-app` command may be offered.
///
/// The reference offers it to the subscribers of Claude and to the
/// customers of the API that talk to Anthropic themselves.
bool install_github_app_available() {
    // The kill switch of the reference.
    if (env_truthy("DISABLE_INSTALL_GITHUB_APP_COMMAND")) return false;

    // A subscriber of Claude.
    const auto tokens = auth::current_tokens();
    if (tokens && !tokens->subscription_type.empty()) return true;

    // A customer of the API.
    return first_party_api();
}

/// The commands of the reference, in the order it registers them in
/// `src/commands.ts`.
///
/// The description and the argument hint are the reference's own words. A few
/// of them are computed at runtime there (the model the session runs on, the
/// state of the sandbox); the static part of those is used here.
std::vector<Command> build_commands() {
    return {
        {.name = "add-dir",
         .description = "Add a new working directory",
         .argument_hint = "<path>",
         .type = "local-jsx"},
        {.name = "advisor",
         .description = "Configure the advisor model",
         .argument_hint = "[<model>|off]",
         .type = "local"},
        {.name = "agents", .description = "Manage agent configurations", .type = "local-jsx"},
        {.name = "branch",
         .aliases = {"fork"},
         .description = "Create a branch of the current conversation at this point",
         .argument_hint = "[name]",
         .type = "local-jsx"},
        {.name = "btw",
         .description = "Ask a quick side question without interrupting the main conversation",
         .argument_hint = "<question>",
         .type = "local-jsx"},
        {.name = "chrome", .description = "Claude in Chrome (Beta) settings", .type = "local-jsx"},
        {.name = "clear",
         .aliases = {"reset", "new"},
         .description = "Clear conversation history and free up context",
         .type = "local"},
        {.name = "color",
         .description = "Set the prompt bar color for this session",
         .argument_hint = "<color|default>",
         .type = "local-jsx"},
        {.name = "compact",
         .description =
             "Clear conversation history but keep a summary in context. Optional: /compact "
             "[instructions for summarization]",
         .argument_hint = "<optional custom summarization instructions>",
         .type = "local"},
        {.name = "config",
         .aliases = {"settings"},
         .description = "Open config panel",
         .type = "local-jsx"},
        {.name = "commit",
         .description = "Create a git commit",
         .type = "prompt"},
        {.name = "commit-push-pr",
         .description = "Commit, push, and open a PR",
         .type = "prompt"},
        {.name = "copy",
         .description = "Copy Claude's last response to clipboard (or /copy N for the Nth-latest)",
         .type = "local-jsx"},
        {.name = "desktop",
         .aliases = {"app"},
         .description = "Continue the current session in Claude Desktop",
         .type = "local-jsx"},
        {.name = "context",
         .description = "Visualize current context usage as a colored grid",
         .type = "local-jsx"},
        {.name = "cost",
         .description = "Show the total cost and duration of the current session",
         .type = "local"},
        {.name = "diff",
         .description = "View uncommitted changes and per-turn diffs",
         .type = "local-jsx"},
        {.name = "doctor",
         .description = "Diagnose and verify your Claude Code installation and settings",
         .type = "local-jsx"},
        {.name = "effort",
         .description = "Set effort level for model usage",
         .argument_hint = "[low|medium|high|max|auto]",
         .type = "local-jsx"},
        {.name = "exit", .aliases = {"quit"}, .description = "Exit the REPL", .type = "local-jsx"},
        {.name = "fast",
         .description = "Toggle fast mode (Experimental)",
         .argument_hint = "[on|off]",
         .type = "local-jsx"},
        {.name = "files",
         .description = "List all files currently in context",
         .type = "local"},
        {.name = "goal",
         .description = "Set a goal Claude checks before stopping",
         .argument_hint = "[<condition> | clear]",
         .type = "local"},
        {.name = "heapdump",
         .description = "Dump the JS heap to ~/Desktop",
         .type = "local",
         .hidden = true},
        {.name = "help", .description = "Show help and available commands", .type = "local-jsx"},
        {.name = "ide",
         .description = "Manage IDE integrations and show status",
         .argument_hint = "[open]",
         .type = "local-jsx"},
        {.name = "init",
         .description = "Initialize a new CLAUDE.md file with codebase documentation",
         .type = "prompt"},
        {.name = "keybindings",
         .description = "Open or create your keybindings configuration file",
         .type = "local"},
        {.name = "install-github-app",
         .description = "Set up Claude GitHub Actions for a repository",
         .type = "local-jsx",
         .enabled = [] { return install_github_app_available(); }},
        {.name = "install-slack-app",
         .description = "Install the Claude Slack app",
         .type = "local"},
        {.name = "mcp",
         .description = "Manage MCP servers",
         .argument_hint = "[enable|disable [server-name]]",
         .type = "local-jsx"},
        {.name = "memory", .description = "Edit Claude memory files", .type = "local-jsx"},
        {.name = "mobile",
         .aliases = {"ios", "android"},
         .description = "Show QR code to download the Claude mobile app",
         .type = "local-jsx"},
        {.name = "model",
         .description = "Set the AI model for Claude Code",
         .argument_hint = "[model]",
         .type = "local-jsx"},
        {.name = "output-style",
         .description = "Deprecated: use /config to change output style",
         .type = "local-jsx",
         .hidden = true},
        {.name = "remote-env",
         .description = "Configure the default remote environment for teleport sessions",
         .type = "local-jsx",
         .hidden = true},
        {.name = "plugin",
         .aliases = {"plugins", "marketplace"},
         .description = "Manage Claude Code plugins",
         .type = "local-jsx"},
        {.name = "pr-comments",
         .description = "Get comments from a GitHub pull request",
         .type = "prompt"},
        {.name = "release-notes", .description = "View release notes", .type = "local"},
        {.name = "reload-plugins",
         .description = "Activate pending plugin changes in the current session",
         .type = "local"},
        {.name = "rename",
         .description = "Rename the current conversation",
         .argument_hint = "[name]",
         .type = "local-jsx"},
        {.name = "resume",
         .aliases = {"continue"},
         .description = "Resume a previous conversation",
         .argument_hint = "[conversation id or search term]",
         .type = "local-jsx"},
        {.name = "session",
         .aliases = {"remote"},
         .description = "Show remote session URL and QR code",
         .type = "local-jsx",
         .hidden = true},
        {.name = "skills", .description = "List available skills", .type = "local-jsx"},
        {.name = "stats",
         .description = "Show your Claude Code usage statistics and activity",
         .type = "local-jsx"},
        {.name = "status",
         .description =
             "Show Claude Code status including version, model, account, API connectivity, and tool "
             "statuses",
         .type = "local-jsx"},
        {.name = "statusline",
         .description = "Set up Claude Code's status line UI",
         .type = "prompt"},
        {.name = "stickers", .description = "Order Claude Code stickers", .type = "local"},
        {.name = "tag",
         .description = "Toggle a searchable tag on the current session",
         .argument_hint = "<tag-name>",
         .type = "local-jsx",
         .hidden = true},
        {.name = "theme", .description = "Change the theme", .type = "local-jsx"},
        {.name = "feedback",
         .aliases = {"bug"},
         .description = "Submit feedback about Claude Code",
         .argument_hint = "[report]",
         .type = "local-jsx"},
        {.name = "review", .description = "Review a pull request", .type = "prompt"},
        {.name = "ultrareview",
         .description =
             "~10–20 min · Finds and verifies bugs in your branch. Runs in Claude Code on the web. "
             "See https://code.claude.com/docs/en/claude-code-on-the-web",
         .type = "local-jsx",
         .hidden = true},
        {.name = "rewind",
         .aliases = {"checkpoint"},
         .description = "Restore the code and/or conversation to a previous point",
         .type = "local"},
        {.name = "security-review",
         .description = "Complete a security review of the pending changes on the current branch",
         .type = "prompt"},
        {.name = "terminal-setup",
         .description = "Install Shift+Enter key binding for newlines",
         .type = "local-jsx"},
        {.name = "upgrade",
         .description = "Upgrade to Max for higher rate limits and more Opus",
         .type = "local-jsx"},
        {.name = "extra-usage",
         .description = "Configure extra usage to keep working when limits are hit",
         .type = "local-jsx",
         .enabled = [] { return auth::overage_provisioning_allowed(); }},
        {.name = "rate-limit-options",
         .description = "Show options when rate limit is reached",
         .type = "local-jsx",
         .hidden = true},
        {.name = "usage", .description = "Show plan usage limits", .type = "local-jsx"},
        {.name = "insights",
         .description = "Generate a report analyzing your Claude Code sessions",
         .type = "prompt"},
        {.name = "vim",
         .description = "Toggle between Vim and Normal editing modes",
         .type = "local"},
        {.name = "brief",
         .description = "Toggle brief-only mode",
         .type = "local-jsx",
         .hidden = true},
        {.name = "ultraplan",
         .description =
             "~10–30 min · Claude Code on the web drafts an advanced plan you can edit and approve. "
             "See https://code.claude.com/docs/en/claude-code-on-the-web",
         .argument_hint = "<prompt>",
         .type = "local-jsx",
         .hidden = true},
        {.name = "web-setup",
         .description = "Setup Claude Code on the web (requires connecting your GitHub account)",
         .type = "local-jsx",
         .hidden = true},
        {.name = "remote-control",
         .aliases = {"rc"},
         .description = "Connect this terminal for remote-control sessions",
         .argument_hint = "[name]",
         .type = "local-jsx",
         .hidden = true},
        {.name = "voice",
         .description = "Toggle voice mode",
         .type = "local",
         .hidden = true},
        {.name = "think-back",
         .description = "Your 2025 Claude Code Year in Review",
         .type = "local-jsx",
         .hidden = true},
        {.name = "thinkback-play",
         .description = "Play the thinkback animation",
         .type = "local",
         .hidden = true},
        {.name = "permissions",
         .aliases = {"allowed-tools"},
         .description = "Manage allow & deny tool permission rules",
         .type = "local-jsx"},
        {.name = "plan",
         .description = "Enable plan mode or view the current session plan",
         .argument_hint = "[open|<description>]",
         .type = "local-jsx"},
        {.name = "privacy-settings",
         .description = "View and update your privacy settings",
         .type = "local-jsx"},
        {.name = "hooks",
         .description = "View hook configurations for tool events",
         .type = "local-jsx"},
        {.name = "export",
         .description = "Export the current conversation to a file or clipboard",
         .argument_hint = "[filename]",
         .type = "local-jsx"},
        {.name = "sandbox",
         .description = "◯ sandbox disabled (⏎ to configure)",
         .argument_hint = "exclude \"command pattern\"",
         .type = "local-jsx"},
        {.name = "logout", .description = "Sign out from your Anthropic account", .type = "local-jsx"},
        {.name = "login",
         .description = "Sign in with your Anthropic account",
         .type = "local-jsx"},
        {.name = "passes",
         .description = "Share a free week of Claude Code with friends",
         .type = "local-jsx",
         .hidden = true},
        {.name = "tasks",
         .aliases = {"bashes"},
         .description = "List and manage background tasks",
         .type = "local-jsx"},
        {.name = "version",
         .description = "Print the version this session is running (not what autoupdate downloaded)",
         .type = "local",
         .hidden = true},
    };
}

/// The name of the terminal the CLI runs in.
std::string terminal_name() {
    if (const char* program = std::getenv("TERM_PROGRAM")) {
        if (*program) return program;
    }
    if (const char* term = std::getenv("TERM")) {
        if (*term) return term;
    }
    return "unknown";
}

/// True when an Anthropic API key is set in the environment.
bool api_key_set() {
    const char* key = std::getenv("ANTHROPIC_API_KEY");
    return key != nullptr && *key != '\0';
}

/// The label of the source a settings file belongs to.
std::string settings_source_label(const std::string& path, const std::string& cwd) {
    if (cc::normalize(path) == cc::normalize(config::user_settings_path())) return "user settings";
    if (cc::normalize(path) == cc::normalize(config::project_settings_path(cwd))) {
        return "project settings";
    }
    if (cc::normalize(path) == cc::normalize(config::local_settings_path(cwd))) {
        return "local settings";
    }
    return "settings";
}

/// The settings files of a session, with whether they exist.
std::vector<std::pair<std::string, bool>> settings_files(const CommandContext& context) {
    std::vector<std::pair<std::string, bool>> files;
    if (context.settings) {
        for (const auto& file : context.settings->files) {
            files.emplace_back(file.path, file.exists);
        }
        return files;
    }
    files.emplace_back(config::user_settings_path(),
                       cc::file_exists(config::user_settings_path()));
    files.emplace_back(config::project_settings_path(context.cwd),
                       cc::file_exists(config::project_settings_path(context.cwd)));
    files.emplace_back(config::local_settings_path(context.cwd),
                       cc::file_exists(config::local_settings_path(context.cwd)));
    return files;
}

/// One MCP server, as it is configured in a settings file.
struct McpServer {
    /// The name of the server.
    std::string name;
    /// The command the server is started with, when it is a local server.
    std::string command;
    /// The URL of the server, when it is a remote server.
    std::string url;
    /// The settings file the server is configured in.
    std::string source;
};

/// The MCP servers configured for the session.
std::vector<McpServer> mcp_servers(const CommandContext& context) {
    std::vector<McpServer> servers;
    if (!context.settings) return servers;

    for (const auto& [name, server] : context.settings->mcp_servers) {
        McpServer entry;
        entry.name = name;
        entry.source = server.scope;
        entry.command = jget_string(server.config, "command");
        entry.url = jget_string(server.config, "url");
        servers.push_back(std::move(entry));
    }
    return servers;
}

/// One hook, as it is configured in a settings file.
struct Hook {
    /// The event the hook runs on, e.g. `PreToolUse`.
    std::string event;
    /// The tool pattern the hook matches, empty when it matches everything.
    std::string matcher;
    /// The kind of the hook, e.g. `command`.
    std::string type;
    /// The command the hook runs, for a command hook.
    std::string command;
    /// The settings file the hook is configured in.
    std::string source;
};

/// The hooks configured in the settings.
std::vector<Hook> hooks(const CommandContext& context) {
    std::vector<Hook> configured;
    if (!context.settings) return configured;

    for (const auto& file : context.settings->files) {
        if (!file.exists || !file.contents.is_object()) continue;
        const auto hooks_object = jget_object(file.contents.as_object(), "hooks");
        if (hooks_object.empty()) continue;

        const std::string source = settings_source_label(file.path, context.cwd);
        for (const auto& event : hooks_object) {
            if (!event.value().is_array()) continue;
            for (const auto& matcher : event.value().as_array()) {
                if (!matcher.is_object()) continue;
                const auto& matcher_object = matcher.as_object();
                for (const auto& hook : jget_array(matcher_object, "hooks")) {
                    if (!hook.is_object()) continue;
                    const auto& hook_object = hook.as_object();
                    Hook entry;
                    entry.event = std::string(event.key());
                    entry.matcher = jget_string(matcher_object, "matcher");
                    entry.type = jget_string(hook_object, "type");
                    entry.command = jget_string(hook_object, "command");
                    entry.source = source;
                    configured.push_back(std::move(entry));
                }
            }
        }
    }
    return configured;
}

/// One agent definition, read from a markdown file.
struct AgentDefinition {
    /// The name of the agent.
    std::string name;
    /// What the agent does.
    std::string description;
    /// Where the definition was found.
    std::string source;
};

/// The `name` and the `description` of the frontmatter of a markdown file.
std::pair<std::string, std::string> parse_frontmatter(const std::string& text) {
    std::string name;
    std::string description;

    const auto lines = cc::split_lines(text);
    if (lines.empty() || cc::trim(lines.front()) != "---") return {name, description};

    for (size_t i = 1; i < lines.size(); ++i) {
        const std::string line = cc::trim(lines[i]);
        if (line == "---") break;

        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        const std::string key = cc::trim(line.substr(0, colon));
        std::string value = cc::trim(line.substr(colon + 1));
        if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                  (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (key == "name") {
            name = value;
        } else if (key == "description") {
            description = value;
        }
    }
    return {name, description};
}

/// The agent definitions found in the `.claude/agents` directories.
std::vector<AgentDefinition> agent_definitions(const std::string& cwd) {
    std::vector<AgentDefinition> agents;

    const std::vector<std::pair<std::string, std::string>> directories = {
        {cc::path_join(cc::claude_config_dir(), "agents"), "user settings"},
        {cc::path_join(cc::path_join(cwd, ".claude"), "agents"), "project settings"},
    };

    for (const auto& [directory, source] : directories) {
        if (!cc::is_directory(directory)) continue;
        for (const auto& entry : cc::list_dir(directory)) {
            if (!cc::ends_with(entry, ".md")) continue;
            const std::string path = cc::path_join(directory, entry);
            if (!cc::is_file(path)) continue;

            const auto text = cc::read_file(path);
            if (!text) continue;

            const auto [name, description] = parse_frontmatter(*text);
            AgentDefinition agent;
            agent.name = name.empty() ? cc::stem(entry) : name;
            agent.description = description;
            agent.source = source;
            agents.push_back(std::move(agent));
        }
    }
    return agents;
}

/// A digest of the conversation, built from the conversation's own text.
///
/// The reference asks the model to write the summary. The C++ port has no
/// model to call here, so the summary is made from the messages themselves.
std::string summarize(const std::vector<Message>& messages) {
    std::string out;

    size_t counted = 0;
    for (const auto& message : messages) {
        if (message.is_meta) continue;
        if (cc::trim(message.text()).empty()) continue;
        ++counted;
    }
    out += "Messages in the conversation: " + std::to_string(counted) + "\n";

    for (const auto& message : messages) {
        if (message.is_meta) continue;
        const std::string text = cc::trim(message.text());
        if (text.empty()) continue;

        const char* role = message.role == Role::User ? "User" : "Assistant";
        out += std::string("  ") + role + ": " + cc::truncate(text, 200) + "\n";
    }

    return out;
}

/// The text the `/init` command writes into `CLAUDE.md`.
std::string claude_md_stub() {
    return "# CLAUDE.md\n"
           "\n"
           "This file provides guidance to Claude Code (claude.ai/code) when working with code in "
           "this repository.\n"
           "\n"
           "## Commands\n"
           "\n"
           "<!-- How to build, lint and test this repository. -->\n"
           "\n"
           "## Architecture\n"
           "\n"
           "<!-- The high-level structure of the codebase. -->\n";
}

/// The text the `/doctor` command prints.
std::string doctor_text(const CommandContext& context) {
    std::string out;
    out += "Claude Code doctor\n";
    out += "\n";
    out += "  Version: " + (context.version.empty() ? std::string(version()) : context.version) +
           "\n";
    out += "  Working directory: " + context.cwd + "\n";
    out += "  API key: " + std::string(api_key_set() ? "set" : "not set") + "\n";
    out += "  Settings files:\n";
    for (const auto& [path, exists] : settings_files(context)) {
        out += "    " + std::string(exists ? "found   " : "missing ") +
               cc::tilde_path(path) + "\n";
    }
    return out;
}

/// The text the `/mcp` command prints.
std::string mcp_text(const CommandContext& context) {
    const auto servers = mcp_servers(context);
    if (servers.empty()) return "No MCP servers configured.\n";

    std::vector<McpServerState> states;
    if (context.mcp_states) states = context.mcp_states();

    std::string out;
    out += "MCP servers:\n";
    for (const auto& server : servers) {
        out += "  " + server.name + " [" + server.source + "]\n";
        if (!server.command.empty()) {
            out += "    command: " + server.command + "\n";
        } else if (!server.url.empty()) {
            out += "    url: " + server.url + "\n";
        }

        const McpServerState* state = nullptr;
        for (const auto& entry : states) {
            if (entry.name == server.name) state = &entry;
        }
        if (state == nullptr) continue;

        if (state->connected) {
            out += "    " + std::string(ui::figures::kTick) + " connected\n";
            if (state->tool_count > 0) {
                out += "      " + std::to_string(state->tool_count) + " tools\n";
            }
            if (state->resource_count > 0) {
                out += "      " + std::to_string(state->resource_count) + " resources\n";
            }
            if (state->prompt_count > 0) {
                out += "      " + std::to_string(state->prompt_count) + " prompts\n";
            }
            continue;
        }

        // A server that is still on its way is not a failure.
        if (state->connecting) {
            out += "    " + std::string(ui::figures::kRadioOff) + " connecting…\n";
            continue;
        }

        out += "    " + std::string(ui::figures::kCross) + " failed\n";
        if (!state->error.empty()) out += "      " + state->error + "\n";
    }
    return out;
}

/// The text the `/hooks` command prints.
std::string hooks_text(const CommandContext& context) {
    const auto configured = hooks(context);
    if (configured.empty()) return "No hooks configured.\n";

    std::string out;
    out += "Hooks:\n";
    for (const auto& hook : configured) {
        out += "  " + hook.event;
        if (!hook.matcher.empty()) out += " (matcher: " + hook.matcher + ")";
        out += " [" + hook.source + "]\n";
        if (!hook.command.empty()) out += "    command: " + hook.command + "\n";
        if (!hook.type.empty() && hook.type != "command") {
            out += "    type: " + hook.type + "\n";
        }
    }
    return out;
}

/// The text the `/agents` command prints.
std::string agents_text(const CommandContext& context) {
    const auto agents = agent_definitions(context.cwd);
    if (agents.empty()) return "No agent definitions found.\n";

    std::string out;
    out += "Agents:\n";
    for (const auto& agent : agents) {
        out += "  " + agent.name + " [" + agent.source + "]\n";
        if (!agent.description.empty()) out += "    " + agent.description + "\n";
    }
    return out;
}

/// The text the `/permissions` command prints.
std::string permissions_text(const CommandContext& context) {
    std::string out;

    const auto section = [&out](const char* title, const PermissionRulesBySource& rules) {
        out += title;
        out += "\n";
        bool any = false;
        for (const auto& [source, entries] : rules) {
            for (const auto& entry : entries) {
                any = true;
                out += "  " + entry + " [" + std::string(setting_source_display_name(source)) + "]\n";
            }
        }
        if (!any) out += "  (none)\n";
    };

    section("Allow rules:", context.settings ? context.settings->allow_rules
                                              : PermissionRulesBySource{});
    section("Deny rules:", context.settings ? context.settings->deny_rules
                                             : PermissionRulesBySource{});
    return out;
}

/// The text the `/memory` command prints.
std::string memory_text(const CommandContext& context) {
    const std::string path = cc::path_join(cc::claude_config_dir(), "CLAUDE.md");

    std::string out;
    out += "Memory file: " + cc::tilde_path(path) + "\n";
    out += "  " + std::string(cc::file_exists(path) ? "exists" : "does not exist yet") + "\n";
    out += "  " + std::string(cc::file_exists(cc::path_join(context.cwd, "CLAUDE.md"))
                                  ? "Project memory: CLAUDE.md\n"
                                  : "Project memory: none\n");
    return out;
}

/// The text the `/resume` command prints.
///
/// The argument is the conversation id or a search term; when it is given,
/// only the sessions that match it are listed.
std::string resume_text(const CommandContext& context, const std::string& argument) {
    if (!context.sessions || context.sessions->empty()) {
        return "No previous sessions to resume.\n";
    }

    std::vector<std::string> matching;
    for (const auto& id : *context.sessions) {
        if (argument.empty() || cc::contains(id, argument)) matching.push_back(id);
    }

    if (matching.empty()) {
        return "No session matches \"" + argument + "\".\n";
    }

    std::string out;
    out += "Sessions that can be resumed:\n";
    for (const auto& id : matching) {
        out += "  " + id + "\n";
    }
    return out;
}

/// The text the `/theme` command prints.
std::string theme_text() {
    static const ui::ThemeName kThemes[] = {
        ui::ThemeName::Dark,
        ui::ThemeName::Light,
        ui::ThemeName::LightDaltonized,
        ui::ThemeName::DarkDaltonized,
        ui::ThemeName::LightAnsi,
        ui::ThemeName::DarkAnsi,
    };

    std::string out;
    out += "Available themes:\n";
    for (const auto theme : kThemes) {
        out += "  " + std::string(ui::to_string(theme)) + "\n";
    }
    return out;
}

/// The text the `/model` command prints.
std::string model_text(const CommandContext& context) {
    if (context.model.empty()) return "No model is set.\n";

    const std::string display = model_display_name(context.model);
    std::string out = "Current model: " + display;
    if (display != context.model) out += " (" + context.model + ")";
    out += "\n";
    return out;
}

/// The text the `/version` command prints.
std::string version_text(const CommandContext& context) {
    return (context.version.empty() ? std::string(version()) : context.version) + "\n";
}

/// The text the `/release-notes` command prints.
///
/// The reference fetches the changelog over the network. The port brings a
/// changelog of its own, and it is the one that is printed; when the port
/// brings none, the changelog of the reference is named the way the
/// reference names it when the fetch fails.
std::string release_notes_text() {
    const std::string bundled = cc::bundled_changelog();
    if (!bundled.empty()) return bundled;

    return "See the full changelog at: "
           "https://github.com/anthropics/claude-code/blob/main/CHANGELOG.md\n";
}

/// The text the `/init` command prints, after writing the file.
std::string init_text(const CommandContext& context, const std::string& path, bool written,
                      const std::string& error) {
    if (!written) {
        return "Could not write " + path + (error.empty() ? "" : ": " + error) + "\n";
    }
    std::string out;
    out += "Created " + cc::relative(path, context.cwd) + "\n";
    out += "  " + path + "\n";
    out += "Edit it to describe the build, test and lint commands of this repository and the "
           "architecture of the codebase.\n";
    return out;
}

/// The message shown for a command the C++ port does not implement.
std::string unavailable_text(const std::string& name) {
    return "The /" + name + " command is not available in the C++ port.\n";
}

/// The name of the file the conversation is exported to.
///
/// The name is made of the day and the time, the way the reference makes it,
/// and of the first thing the user said.
std::string export_file_name(const std::vector<Message>& messages) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm parts{};
    localtime_r(&seconds, &parts);

    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", &parts);
    const std::string timestamp = stamp;

    // The first thing the user said, on its first line and cut short.
    std::string first;
    for (const Message& message : messages) {
        if (message.role != Role::User) continue;
        first = message.text();
        break;
    }
    const size_t newline = first.find('\n');
    if (newline != std::string::npos) first = first.substr(0, newline);
    first = cc::trim(first);
    if (first.size() > 50) first = first.substr(0, 49) + "\xE2\x80\xA6";

    // The name of the file holds only the characters a file may hold.
    std::string sanitized;
    for (const char c : cc::to_lower(first)) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-';
        sanitized += plain ? c : '-';
    }
    while (!sanitized.empty() && sanitized.front() == '-') sanitized.erase(0, 1);
    while (!sanitized.empty() && sanitized.back() == '-') sanitized.pop_back();

    if (sanitized.empty()) return "conversation-" + timestamp + ".txt";
    return timestamp + "-" + sanitized + ".txt";
}

/// The conversation as plain text.
///
/// The words of the user and the answers of the model are written out, the
/// way the reference writes them out.
std::string export_text(const std::vector<Message>& messages) {
    std::string out;
    for (const Message& message : messages) {
        if (message.is_meta) continue;

        if (message.role == Role::User) {
            const std::string text = cc::trim(message.text());
            if (text.empty()) continue;
            out += "> " + text + "\n\n";
            continue;
        }

        const std::string text = message.text();
        if (!text.empty()) out += text + "\n\n";
    }
    return out;
}

/// The text the `/export` command prints.
///
/// The conversation is written to a file in the folder of the session.
std::string export_text_command(const CommandContext& context, const std::string& argument) {
    if (!context.messages) return "There is no conversation to export.\n";

    const std::string content = export_text(*context.messages);
    if (content.empty()) return "There is no conversation to export.\n";

    // The name of the file is the one the user typed, or one made of the
    // day, the time and the first thing they said.
    std::string filename = cc::trim(argument);
    if (filename.empty()) {
        filename = export_file_name(*context.messages);
    } else if (!cc::ends_with(filename, ".txt")) {
        const size_t dot = filename.find_last_of('.');
        if (dot != std::string::npos) filename.erase(dot);
        filename += ".txt";
    }

    const std::string path = cc::path_join(context.cwd, filename);
    std::string error;
    if (!cc::write_file(path, content, &error)) {
        return "Failed to export conversation: " + error + "\n";
    }

    return "Conversation exported to: " + path + "\n";
}

/// The text the `/feedback` command prints.
///
/// The command sends what the user wrote to the API of Anthropic and offers
/// to open the issue of the report in the browser.
std::string feedback_text(const CommandContext& context, const std::string& argument) {
    if (!context.send_feedback) {
        return "The feedback could not be sent.\n";
    }
    return context.send_feedback(argument);
}

/// The text the `/vim` command prints.
///
/// The command turns the keys of the prompt box into the keys of vim, or
/// back into the usual ones. The way the keys are read is remembered, the
/// way the reference remembers it.
std::string vim_text(const CommandContext& context) {
    const std::string current = config::global_editor_mode();
    const bool vim = current == "vim";

    const std::string mode = vim ? "normal" : "vim";
    config::set_global_editor_mode(mode);

    if (context.set_editor_mode) context.set_editor_mode(!vim);

    if (vim) {
        return "Editor mode set to normal. Using standard (readline) keyboard bindings.\n";
    }
    return "Editor mode set to vim. Use Escape key to toggle between INSERT and NORMAL modes.\n";
}

/// The text the `/desktop` command prints.
///
/// The command carries the session to the Claude Desktop application. When
/// the application is not there, the user is asked whether it should be
/// downloaded.
std::string desktop_text(const CommandContext& context) {
    const desktop::Status status = desktop::install_status();

    if (status.status == desktop::InstallStatus::Ready) {
        if (!context.open_session) {
            return "Failed to open Claude Desktop. Please try opening it manually.\n";
        }
        if (!context.open_session(context.session_id, context.cwd)) {
            return "Failed to open Claude Desktop. Please try opening it manually.\n";
        }
        return "Opening the current session in Claude Desktop...\n";
    }

    // The application is not there, or it is too old: the user is asked
    // whether it should be downloaded.
    std::string message;
    if (status.status == desktop::InstallStatus::VersionTooOld) {
        message = "Claude Desktop needs to be updated (found v" + status.version + ", need v" +
                  std::string(desktop::kMinimumVersion) + "+).";
    } else {
        message = "Claude Desktop is not installed.";
    }

    if (!context.confirm) return message + "\n";

    const bool download = context.confirm(message + " Download it?");
    if (!download) {
        return "The desktop app is required for /desktop. Learn more at " +
               std::string(desktop::kDocsUrl) + "\n";
    }

    if (context.open_browser) context.open_browser(desktop::download_url());
    return "Starting download. Re-run /desktop once you've installed the app.\nLearn more at " +
           std::string(desktop::kDocsUrl) + "\n";
}

/// The text the `/stickers` command prints.
///
/// The command opens the page of the stickers in the browser.
std::string stickers_text(const CommandContext& context) {
    const std::string url = "https://www.stickermule.com/claudecode";
    if (context.open_browser) context.open_browser(url);
    return "Opening sticker page in browser\xE2\x80\xA6\n";
}

/// The text the `/plan` command prints.
///
/// The command turns the plan mode on; when the session is already in it,
/// the plan of the session is shown.
std::string plan_text(const CommandContext& context, const std::string& argument) {
    const std::string trimmed = cc::trim(argument);

    // The plan mode is turned on when it is not on already.
    if (!context.is_plan_mode || !context.is_plan_mode()) {
        if (context.enable_plan_mode) context.enable_plan_mode(trimmed);
        return "";
    }

    // The session is already in the plan mode: the plan is shown.
    if (context.current_plan) {
        const std::string plan = context.current_plan();
        if (!plan.empty()) {
            // The plan is opened in the editor when the user asks for it.
            if (trimmed == "open" && context.open_in_editor && context.plan_path) {
                context.open_in_editor(context.plan_path());
                return "Opened plan in editor: " + context.plan_path() + "\n";
            }
            return plan;
        }
    }

    return "Already in plan mode. No plan written yet.\n";
}

/// The text the `/install-slack-app` command prints.
///
/// The command opens the page of the application in the browser.
std::string install_slack_app_text(const CommandContext& context) {
    const std::string url = "https://slack.com/marketplace/A08SF47R6P4-claude";
    if (context.open_browser) context.open_browser(url);
    return "Opening Slack app installation page in browser…\n";
}

/// The repository of the working tree on GitHub, as `owner/name`.
///
/// The name is empty when the folder is not in a repository of GitHub.
std::string github_repository(const std::string& cwd) {
    const cc::ProcessOutcome outcome =
        cc::run_process("git remote get-url origin", cwd, "", 10);
    if (outcome.failed || outcome.exit_code != 0) return {};

    const std::string url = cc::trim(outcome.out);
    std::string host;
    std::string path;

    if (cc::starts_with(url, "git@")) {
        // The address of ssh: git@github.com:owner/name.git
        const std::string rest = url.substr(4);
        const size_t colon = rest.find(':');
        if (colon == std::string::npos) return {};
        host = rest.substr(0, colon);
        path = rest.substr(colon + 1);
    } else {
        // The address of https: https://github.com/owner/name.git
        std::string rest = url;
        const size_t scheme = rest.find("://");
        if (scheme != std::string::npos) rest = rest.substr(scheme + 3);
        const size_t slash = rest.find('/');
        if (slash == std::string::npos) return {};
        host = rest.substr(0, slash);
        path = rest.substr(slash + 1);
    }

    if (host != "github.com" && !cc::ends_with(host, ".github.com")) return {};
    if (cc::ends_with(path, ".git")) path = path.substr(0, path.size() - 4);
    return path;
}

/// The text the `/install-github-app` command prints.
///
/// The reference walks the user through a wizard that installs the GitHub
/// application, writes the workflow of Claude and puts the key of the API
/// in the secrets of the repository. The port opens the page the
/// application is installed from and names the repository it works on; the
/// wizard itself is left out.
std::string install_github_app_text(const CommandContext& context) {
    const std::string repository = github_repository(context.cwd);
    if (repository.empty()) {
        return "This folder is not in a repository of GitHub.\n"
               "The command needs a repository that GitHub holds, with a remote called origin.\n";
    }

    std::string out;

    // The GitHub command line is what writes the workflow and the secret.
    const cc::ProcessOutcome gh = cc::run_process("gh --version", context.cwd, "", 10);
    if (gh.failed || gh.exit_code != 0) {
        out += "The GitHub CLI (gh) was not found.\n";
        out += "Install it from https://cli.github.com/ and sign in with `gh auth login`.\n";
    }

    out += "Opening browser to install the Claude GitHub App…\n";
    out += "If your browser doesn't open automatically, visit:\n";
    out += "https://github.com/apps/claude\n";
    out += "Please install the app for repository: " + repository + "\n";
    out += "Important: Make sure to grant access to this specific repository\n";

    const std::string url = "https://github.com/apps/claude";
    bool opened = false;
    if (context.open_browser) opened = context.open_browser(url);
    if (!opened) {
        out += "The browser could not be opened.\n";
    }

    out += "Having trouble? See manual setup instructions at: "
           "https://github.com/anthropics/claude-code-action/blob/main/docs/setup.md\n";
    return out;
}

/// The text the `/privacy-settings` command prints.
///
/// The settings of the account are read over the API of the account; the
/// address of the page is shown when the account cannot be asked.
std::string privacy_settings_text() {
    return "Review and manage your privacy settings at "
           "https://claude.ai/settings/data-privacy-controls\n";
}

// ------------------------------------------------------------------------------------------------
// The commands the reference opens a picker, a panel or the editor for.
// ------------------------------------------------------------------------------------------------

/// Runs `command` through the shell in `cwd` and returns what it printed.
std::string shell_output(const std::string& command, const std::string& cwd,
                         int* exit_code = nullptr) {
    const auto result = cc::tools::run_shell_command(command, cwd, 15000);
    if (exit_code) *exit_code = result.exit_code;
    return result.output;
}

/// Writes `value` under `key` in the user's settings file, keeping the rest of
/// the file as it was.
///
/// The reference writes the settings a user changes through a panel to
/// `~/.claude/settings.json`; the port does the same.
bool write_user_setting(const std::string& key, const boost::json::value& value,
                        std::string* error) {
    const std::string path = config::user_settings_path();

    boost::json::object root;
    if (const auto text = cc::read_file(path)) {
        const auto parsed = cc::json_parse(*text, boost::json::value(nullptr));
        if (parsed.is_object()) root = parsed.as_object();
    }

    root[key] = value;
    return config::write_settings(path, boost::json::value(std::move(root)), error);
}

/// Removes `key` from the user's settings file.
bool remove_user_setting(const std::string& key, std::string* error) {
    const std::string path = config::user_settings_path();

    boost::json::object root;
    if (const auto text = cc::read_file(path)) {
        const auto parsed = cc::json_parse(*text, boost::json::value(nullptr));
        if (parsed.is_object()) root = parsed.as_object();
    }

    root.erase(key);
    return config::write_settings(path, boost::json::value(std::move(root)), error);
}

/// The note the port adds when a model was chosen but the session keeps the
/// model it started with.
std::string model_kept_note(const CommandContext& context) {
    if (context.model.empty()) return {};
    return "The C++ port cannot switch the model mid-session; this session keeps " +
           model_display_name(context.model) + ".\n";
}

/// Sends `prompt` to the model and returns its answer.
///
/// The reference's `type: 'prompt'` commands send their text to the model and
/// show what it answered; the port does the same through the `ask_model` hook.
std::string ask_model(const CommandContext& context, const std::string& prompt,
                      const std::vector<std::string>& allowed_tools = {}) {
    if (!context.ask_model) {
        return "The command needs a model to run; it is not available in a non-interactive session.\n";
    }
    return context.ask_model(prompt, allowed_tools);
}

/// The body of a command of the CLI, with the metadata of the command in
/// front of it.
///
/// The reference tells the model which command was run before it sends the
/// body of the command, the way it does for the commands of the user.
std::string prompt_with_metadata(const std::string& name, const std::string& args,
                                  const std::string& body) {
    return command_metadata(name, args) + "\n" + body;
}

/// The text the `/config` command prints.
///
/// The reference opens a tabbed panel; the port has no panel, so it shows the
/// merged settings as text, under the labels the panel uses.
std::string config_text(const CommandContext& context) {
    std::string out;
    out += "Settings:\n";

    if (!context.settings) {
        out += "  No settings were loaded.\n";
    } else {
        const auto& settings = *context.settings;
        const std::string model =
            settings.model ? *settings.model : model_display_name(context.model);
        out += "  Model: " + (model.empty() ? std::string("(default)") : model) + "\n";
        out += "  Theme: " +
               std::string(settings.theme ? std::string(ui::to_string(*settings.theme)) : "auto") +
               "\n";
        out += "  Output style: " +
               std::string(settings.output_style ? *settings.output_style : "default") + "\n";
        out += "  Default permission mode: " + std::string(to_string(settings.default_mode)) + "\n";
        out += "  Cleanup period: " + std::to_string(settings.cleanup_period_days) + " days\n";
        out += "  Include Co-Authored-By: " +
               std::string(settings.include_co_authored_by ? "yes" : "no") + "\n";

        if (!settings.env.empty()) {
            out += "  Env:\n";
            for (const auto& [key, value] : settings.env) {
                out += "    " + key + "=" + value + "\n";
            }
        }

        const auto rules = [&out](const char* title, const PermissionRulesBySource& rules) {
            out += title;
            out += "\n";
            bool any = false;
            for (const auto& [source, entries] : rules) {
                for (const auto& entry : entries) {
                    any = true;
                    out += "  " + entry + " [" + std::string(setting_source_display_name(source)) +
                           "]\n";
                }
            }
            if (!any) out += "  (none)\n";
        };

        rules("Allow rules:", settings.allow_rules);
        rules("Deny rules:", settings.deny_rules);
    }

    out += "\n";
    out += "Settings files:\n";
    for (const auto& [path, exists] : settings_files(context)) {
        out += "  " + std::string(exists ? "found   " : "missing ") + cc::tilde_path(path) + "\n";
    }
    return out;
}

/// The models the reference's model picker offers.
///
/// The reference builds the list from the account the session runs under; the
/// port shows the first-party options, with the wording the reference uses for
/// each of them.

/// True when `name` names a model the reference knows.
bool known_model(const std::string& name) {
    static const char* kAliases[] = {"default", "opus", "sonnet", "haiku", "opusplan",
                                     "opus[1m]", "sonnet[1m]"};
    for (const char* alias : kAliases) {
        if (name == alias) return true;
    }
    return cc::starts_with(name, "claude-");
}

/// The label the reference shows for a model setting.
std::string model_setting_label(const std::string& setting) {
    if (setting == "default") return "Default (recommended)";
    if (setting == "opus") return "Opus";
    if (setting == "opus[1m]") return "Opus (1M context)";
    if (setting == "sonnet") return "Sonnet";
    if (setting == "sonnet[1m]") return "Sonnet (1M context)";
    if (setting == "haiku") return "Haiku";
    if (setting == "opusplan") return "Opus Plan Mode";
    return model_display_name(setting);
}

/// The themes the reference's theme picker offers.

/// The effort levels the reference offers, with their descriptions.

/// The description the reference shows for an effort level.
std::string effort_description(const std::string& level) {
    for (const auto& option : effort_options()) {
        if (option.value == level) return option.description;
    }
    return "Balanced approach with standard implementation and testing";
}

/// The advisor model the session is configured with.
std::string advisor_model(const CommandContext& context) {
    if (!context.settings) return {};
    for (const auto& file : context.settings->files) {
        if (!file.exists || !file.contents.is_object()) continue;
        const auto value = jget_optional_string(file.contents.as_object(), "advisorModel");
        if (value) return *value;
    }
    return {};
}

/// The text the `/advisor` command prints.
std::string advisor_text(const CommandContext& context, const std::string& argument) {
    const std::string arg = cc::to_lower(cc::trim(argument));
    const std::string current = advisor_model(context);

    if (arg.empty()) {
        if (current.empty()) {
            return "Advisor: not set\nUse \"/advisor <model>\" to enable (e.g. \"/advisor opus\").\n";
        }
        return "Advisor: " + current +
               "\nUse \"/advisor unset\" to disable or \"/advisor <model>\" to change.\n";
    }

    if (arg == "unset" || arg == "off") {
        if (current.empty()) return "Advisor already unset.\n";

        std::string error;
        if (!remove_user_setting("advisorModel", &error)) {
            return "Failed to set advisor model: " +
                   (error.empty() ? "Unknown error" : error) + "\n";
        }
        return "Advisor disabled (was " + current + ").\n";
    }

    if (!known_model(arg)) {
        return "Unknown model: " + arg + " (" + arg + ")\n";
    }

    std::string error;
    if (!write_user_setting("advisorModel", boost::json::value(arg), &error)) {
        return "Failed to set advisor model: " + (error.empty() ? "Unknown error" : error) + "\n";
    }
    return "Advisor set to " + arg + ".\n";
}

/// The text the `/theme` command prints after a theme was chosen.
std::string theme_set_text(const std::string& setting) {
    return "Theme set to " + setting + "\n";
}

/// The text the `/effort` command prints for the effort level in force.
std::string effort_current_text(const CommandContext& context) {
    std::string level;
    if (context.settings) {
        // The port keeps the effort level in the user settings, the way the
        // reference does.
        for (const auto& file : context.settings->files) {
            if (!file.exists || !file.contents.is_object()) continue;
            const auto value = jget_optional_string(file.contents.as_object(), "effortLevel");
            if (value) level = *value;
        }
    }

    if (level.empty()) {
        return "Effort level: auto (currently high)\n";
    }
    return "Current effort level: " + level + " (" + effort_description(level) + ")\n";
}

/// The text the `/effort` command prints after an effort level was set.
std::string effort_set_text(const std::string& level) {
    // `max` is session-only for everyone but Anthropic's own builds, so the
    // reference does not save it.
    const bool session_only = level == "max";
    std::string out = "Set effort level to " + level;
    if (session_only) out += " (this session only)";
    out += ": " + effort_description(level) + "\n";
    return out;
}

/// The effort level the environment pins the session to, if it does.
std::string effort_env_override() {
    const char* value = std::getenv("CLAUDE_CODE_EFFORT_LEVEL");
    if (!value || !*value) return {};
    const std::string lower = cc::to_lower(value);
    if (lower == "unset" || lower == "auto") return {};
    return value;
}

/// The text the `/effort` command prints after the effort level was cleared.
std::string effort_unset() {
    const std::string env = effort_env_override();
    if (!env.empty()) {
        return "Cleared effort from settings, but CLAUDE_CODE_EFFORT_LEVEL=" + env +
               " still controls this session\n";
    }

    std::string error;
    if (!remove_user_setting("effortLevel", &error)) {
        return "Failed to set effort level: " + (error.empty() ? "Unknown error" : error) + "\n";
    }
    return "Effort level set to auto\n";
}

/// The text the `/effort` command prints after an effort level was set, and
/// the setting that goes with it.
std::string effort_applied(const std::string& level) {
    // `auto` clears the setting rather than writing one.
    if (level == "auto") return effort_unset();

    // `max` is session-only for everyone but Anthropic's own builds, so the
    // reference does not save it.
    const bool session_only = level == "max";

    const std::string env = effort_env_override();
    if (!env.empty() && cc::to_lower(env) != level) {
        if (session_only) {
            return "Not applied: CLAUDE_CODE_EFFORT_LEVEL=" + env +
                   " overrides effort this session, and " + level + " is session-only (nothing saved)\n";
        }
        return "CLAUDE_CODE_EFFORT_LEVEL=" + env + " overrides this session — clear it and " + level +
               " takes over\n";
    }

    if (!session_only) {
        std::string error;
        if (!write_user_setting("effortLevel", boost::json::value(level), &error)) {
            return "Failed to set effort level: " + (error.empty() ? "Unknown error" : error) + "\n";
        }
    }

    return effort_set_text(level);
}

/// The text the `/fast` command prints after fast mode was switched.
std::string fast_set_text(bool enabled, const CommandContext& context) {
    if (!enabled) return "Fast mode OFF\n";

    std::string out = "↯ Fast mode ON · $30/$150 per Mtok\n";
    out += "The C++ port has no fast mode; the session keeps running on " +
           model_display_name(context.model) + ".\n";
    return out;
}

/// The output styles the reference's output-style picker offers.

/// The keybindings file the reference writes when it does not exist yet.
///
/// The file holds the default bindings of the port, so that the user can
/// change the ones they want to change.
std::string keybindings_template() {
    const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> kDefaults = {
        {"Global",
         {{"ctrl+c", "app:interrupt"},
          {"ctrl+d", "app:exit"},
          {"ctrl+l", "app:redraw"},
          {"ctrl+t", "app:toggleTodos"},
          {"ctrl+o", "app:toggleTranscript"},
          {"ctrl+r", "history:search"}}},
        {"Chat",
         {{"escape", "chat:cancel"},
          {"shift+tab", "chat:cycleMode"},
          {"meta+p", "chat:modelPicker"},
          {"enter", "chat:submit"},
          {"up", "history:previous"},
          {"down", "history:next"},
          {"ctrl+_", "chat:undo"},
          {"ctrl+g", "chat:externalEditor"},
          {"ctrl+s", "chat:stash"},
          {"ctrl+v", "chat:imagePaste"},
          {"shift+up", "chat:messageActions"}}},
        {"Autocomplete",
         {{"tab", "autocomplete:accept"},
          {"escape", "autocomplete:dismiss"},
          {"up", "autocomplete:previous"},
          {"down", "autocomplete:next"}}},
    };

    boost::json::array bindings;
    for (const auto& [context, keys] : kDefaults) {
        boost::json::object object;
        object["context"] = context;
        boost::json::object entries;
        for (const auto& [key, action] : keys) entries[key] = action;
        object["bindings"] = std::move(entries);
        bindings.emplace_back(std::move(object));
    }

    boost::json::object root;
    root["$schema"] = "https://www.schemastore.org/claude-code-keybindings.json";
    root["$docs"] = "https://code.claude.com/docs/en/keybindings";
    root["bindings"] = std::move(bindings);
    return cc::json_dump_pretty(boost::json::value(std::move(root))) + "\n";
}


/// One file of the working tree that git reports as changed.
struct ChangedFile {
    /// The two-character status git gave the file, e.g. `M ` or `??`.
    std::string status;
    /// The path of the file, relative to the working directory.
    std::string path;
    /// The number of lines the file gained.
    int added = 0;
    /// The number of lines the file lost.
    int removed = 0;
};

/// The text the `/diff` command prints.
///
/// The reference opens a dialog with the uncommitted changes; the port runs
/// `git` and lists the files that changed.
std::string diff_text(const CommandContext& context) {
    int exit_code = 0;
    const std::string status =
        shell_output("git status --porcelain 2>/dev/null", context.cwd, &exit_code);
    if (exit_code != 0) return "No changed files\n";

    std::vector<ChangedFile> files;
    for (const auto& line : cc::split_lines(status)) {
        if (line.size() < 4) continue;
        ChangedFile file;
        file.status = cc::trim(line.substr(0, 2));
        file.path = cc::trim(line.substr(3));
        files.push_back(std::move(file));
    }

    if (files.empty()) return "No changed files\n";

    // The line counts come from the diff against HEAD, the way the reference
    // asks git for them.
    const std::string numstat = shell_output("git diff --numstat HEAD 2>/dev/null", context.cwd);
    for (const auto& line : cc::split_lines(numstat)) {
        const auto parts = cc::split_any(line, "\t");
        if (parts.size() < 3) continue;
        for (auto& file : files) {
            if (file.path != parts[2]) continue;
            file.added = static_cast<int>(cc::parse_int(parts[0]).value_or(0));
            file.removed = static_cast<int>(cc::parse_int(parts[1]).value_or(0));
        }
    }

    int added = 0;
    int removed = 0;
    std::string out;
    out += "Changed files:\n";
    for (const auto& file : files) {
        out += "  " + file.status + " " + file.path;
        if (file.added || file.removed) {
            out += "  +" + std::to_string(file.added) + " -" + std::to_string(file.removed);
        }
        out += "\n";
        added += file.added;
        removed += file.removed;
    }
    out += "\n" + std::to_string(files.size()) + " files changed, " +
           std::to_string(added) + " insertions(+), " + std::to_string(removed) + " deletions(-)\n";
    return out;
}

/// The text the `/ide` command prints.
///
/// The IDEs that are running are found by the lock files they leave in
/// `~/.claude/ide`; the ones whose folders do not hold the folder of the
/// session are named apart, the way the reference names them.
std::string ide_text(const CommandContext& context, const std::string& argument) {
    const std::vector<DetectedIde> ides = detect_ides(context.cwd);

    std::vector<const DetectedIde*> available;
    std::vector<const DetectedIde*> unavailable;
    for (const auto& ide : ides) {
        if (ide.matches_cwd && ide.is_running) {
            available.push_back(&ide);
        } else {
            unavailable.push_back(&ide);
        }
    }

    if (cc::trim(argument) == "open") {
        if (available.empty()) return "No IDEs with Claude Code extension detected.\n";
        std::string out;
        for (const auto* ide : available) {
            out += ide->name + " (port " + std::to_string(ide->port) + ")\n";
        }
        return out;
    }

    if (available.empty()) {
        std::string out =
            "No available IDEs detected. Make sure your IDE has the Claude Code extension or plugin "
            "installed and is running.\n";
        if (!unavailable.empty()) {
            out += "\nFound " + std::to_string(unavailable.size()) +
                   " other running IDE(s). However, their workspace/project directories do not match "
                   "the current cwd.\n";
            for (const auto* ide : unavailable) {
                out += "  " + ide->name + "\n";
            }
        }
        return out;
    }

    // The IDEs of the folder of the session are offered in a picker.
    std::vector<CommandOption> options;
    for (const auto* ide : available) {
        options.push_back({ide->name, std::to_string(ide->port), "Port " + std::to_string(ide->port)});
    }
    options.push_back({"None", "", "Do not connect to an IDE"});

    if (!context.select_option) {
        std::string out;
        for (const auto& option : options) {
            out += option.label + "\n";
        }
        return out;
    }

    const std::string chosen = context.select_option("Select an IDE to connect to:", options);
    if (chosen.empty()) {
        return "No IDE connected.\n";
    }
    return "Connected to the IDE on port " + chosen + ".\n";
}

/// The text the `/plugin` command prints.
///
/// The command manages the plugins of the user and the marketplaces they come
/// from.
std::string plugin_text(const CommandContext& context, const std::string& argument) {
    const std::string trimmed = cc::trim(argument);
    std::vector<std::string> words = cc::split(trimmed, ' ');
    std::vector<std::string> parts;
    for (const std::string& word : words) {
        if (!word.empty()) parts.push_back(word);
    }

    const std::string command = parts.empty() ? "" : parts[0];
    const std::string target = parts.size() > 1 ? parts[1] : "";

    if (command == "help" || command == "--help" || command == "-h") {
        return "Usage: /plugin [install|uninstall|enable|disable|validate|marketplace] [name]\n"
               "\n"
               "  install <plugin>          Install a plugin of a marketplace\n"
               "  uninstall <plugin>        Remove a plugin\n"
               "  enable <plugin>           Turn a plugin on\n"
               "  disable <plugin>          Turn a plugin off\n"
               "  marketplace list          List the marketplaces that are known\n"
               "  marketplace add <source>  Add a marketplace\n"
               "  marketplace remove <name> Remove a marketplace\n"
               "  marketplace refresh <name> Fetch a marketplace again\n";
    }

    if (command == "marketplace") {
        const std::string action = target;

        if (action == "add") {
            const std::string source = parts.size() > 2 ? parts[2] : "";
            if (source.empty()) return "Usage: /plugin marketplace add <source>\n";
            std::string error;
            if (!plugins::add_marketplace("", source, &error)) return "Error: " + error + "\n";
            return "Added the marketplace " +
                   plugins::known_marketplaces().back().name + ".\n";
        }

        if (action == "remove") {
            if (target.empty()) return "Usage: /plugin marketplace remove <name>\n";
            std::string error;
            if (!plugins::remove_marketplace(target, &error)) return "Error: " + error + "\n";
            return "Removed the marketplace " + target + ".\n";
        }

        if (action == "refresh") {
            if (target.empty()) return "Usage: /plugin marketplace refresh <name>\n";
            if (!context.refresh_marketplace) {
                return "The marketplaces cannot be fetched in this session.\n";
            }
            const std::string failure = context.refresh_marketplace(target);
            if (!failure.empty()) return "Error: " + failure + "\n";
            return "Fetched the marketplace " + target + ".\n";
        }

        // The list of the marketplaces.
        const std::vector<plugins::Marketplace> marketplaces = plugins::known_marketplaces();
        if (marketplaces.empty()) return "No marketplace is known.\n";

        std::string out;
        for (const plugins::Marketplace& marketplace : marketplaces) {
            out += marketplace.name;
            if (!marketplace.source.empty()) out += " (" + marketplace.source + ")";
            out += "\n";
        }
        return out;
    }

    if (command == "enable" || command == "disable") {
        if (target.empty()) return "Usage: /plugin " + command + " <plugin>\n";
        std::string error;
        if (!plugins::set_plugin_enabled(target, command == "enable", &error)) {
            return "Error: " + error + "\n";
        }
        return std::string("The plugin ") + target + (command == "enable" ? " is on.\n"
                                                                          : " is off.\n");
    }

    if (command == "uninstall") {
        if (target.empty()) return "Usage: /plugin uninstall <plugin>\n";
        std::string error;
        if (!plugins::uninstall_plugin(target, &error)) return "Error: " + error + "\n";
        return "Removed the plugin " + target + ".\n";
    }

    if (command == "install") {
        if (target.empty()) return "Usage: /plugin install <plugin>[@marketplace]\n";

        // The plugin is named after the marketplace it comes from, the way
        // the reference names it.
        std::string name = target;
        std::string marketplace;
        const size_t at = name.find('@');
        if (at != std::string::npos) {
            marketplace = name.substr(at + 1);
            name = name.substr(0, at);
        } else {
            const std::vector<plugins::Marketplace> known = plugins::known_marketplaces();
            for (const plugins::Marketplace& candidate : known) {
                const auto offered = plugins::marketplace_plugins(candidate.name);
                for (const plugins::MarketplacePlugin& plugin : offered) {
                    if (plugin.name == name) {
                        marketplace = candidate.name;
                        break;
                    }
                }
                if (!marketplace.empty()) break;
            }
        }
        if (marketplace.empty()) {
            return "No marketplace offers the plugin " + name + ".\n";
        }

        std::string error;
        if (!plugins::install_plugin(marketplace, name, &error)) return "Error: " + error + "\n";
        return "Installed the plugin " + name + " from the marketplace " + marketplace + ".\n";
    }

    // The menu: the plugins that are installed and the marketplaces.
    const std::vector<plugins::Plugin> installed = plugins::installed_plugins();
    const std::vector<plugins::Marketplace> marketplaces = plugins::known_marketplaces();

    // The screen of the plugins is opened when the command is used without
    // an argument.
    if (command.empty() && context.plugin_screen) return context.plugin_screen();

    std::string out;
    out += "Installed plugins:\n";
    if (installed.empty()) {
        out += "  No plugin is installed.\n";
    }
    for (const plugins::Plugin& plugin : installed) {
        out += "  " + plugin.name;
        if (!plugin.marketplace.empty()) out += " (" + plugin.marketplace + ")";
        out += plugin.enabled ? " [on]\n" : " [off]\n";
    }

    out += "\nMarketplaces:\n";
    if (marketplaces.empty()) {
        out += "  No marketplace is known.\n";
    }
    for (const plugins::Marketplace& marketplace : marketplaces) {
        out += "  " + marketplace.name;
        if (!marketplace.source.empty()) out += " (" + marketplace.source + ")";
        out += "\n";

        // The plugins the marketplace offers.
        for (const plugins::MarketplacePlugin& plugin : plugins::marketplace_plugins(marketplace.name)) {
            const bool known = std::any_of(
                installed.begin(), installed.end(), [&](const plugins::Plugin& candidate) {
                    return candidate.name == plugin.name && candidate.marketplace == marketplace.name;
                });
            out += "    " + plugin.name;
            if (!plugin.description.empty()) out += " — " + plugin.description;
            out += known ? " [installed]\n" : "\n";
        }
    }

    return out;
}

/// The text the `/chrome` command prints.
///
/// The command shows the state of the browser of Claude, and lets the user
/// install the extension, reconnect it, or turn it on by default.
std::string chrome_text(const CommandContext& context) {
    const bool installed = chrome::extension_installed();
    const std::string browser = chrome::extension_browser();
    bool enabled = config::global_claude_in_chrome_default_enabled();

    // The state of the server of the browser.
    bool connected = false;
    if (context.mcp_states) {
        for (const McpServerState& state : context.mcp_states()) {
            if (state.name == chrome::kServerName) connected = state.connected;
        }
    }

    std::string hint;
    while (true) {
        std::vector<CommandOption> options;
        if (!installed) {
            options.push_back({"Install Chrome extension", "install-extension", ""});
        }
        options.push_back({"Manage permissions" + std::string(installed ? "" : " (requires extension)"),
                           "manage-permissions", ""});
        options.push_back({"Reconnect extension" + std::string(installed ? "" : " (requires extension)"),
                           "reconnect", ""});
        options.push_back({std::string("Enabled by default: ") + (enabled ? "Yes" : "No"), "toggle-default",
                           ""});

        if (!context.select_option) {
            // Without a picker the options are listed as text.
            std::string out;
            for (const auto& option : options) out += option.label + "\n";
            return out;
        }

        const std::string chosen = context.select_option("Claude in Chrome (Beta)", options);
        if (chosen.empty()) break;

        if (chosen == "install-extension") {
            if (context.open_browser) context.open_browser(std::string(chrome::kExtensionUrl));
            hint = "Once installed, select \"Reconnect extension\" to connect.";
            continue;
        }
        if (chosen == "reconnect") {
            if (context.open_browser) context.open_browser(std::string(chrome::kReconnectUrl));
            hint.clear();
            continue;
        }
        if (chosen == "manage-permissions") {
            if (context.open_browser) context.open_browser(std::string(chrome::kPermissionsUrl));
            hint.clear();
            continue;
        }
        if (chosen == "toggle-default") {
            enabled = !enabled;
            config::set_claude_in_chrome_default_enabled(enabled);
            hint.clear();
            continue;
        }
        break;
    }

    std::string out;
    out +=
        "Claude in Chrome works with the Chrome extension to let you control your browser directly "
        "from Claude Code. Navigate websites, fill forms, capture screenshots, record GIFs, and "
        "debug with console logs and network requests.\n\n";
    out += std::string("Status: ") + (connected ? "Enabled" : "Disabled") + "\n";
    out += std::string("Extension: ") + (installed ? "Installed" : "Not detected");
    if (installed && !browser.empty()) out += " (" + browser + ")";
    out += "\n";

    if (!hint.empty()) out += "\n" + hint + "\n";

    out += "\nUsage: cc++ --chrome or cc++ --no-chrome\n";
    out +=
        "Site-level permissions are inherited from the Chrome extension. Manage permissions in the "
        "Chrome extension settings to control which sites Claude can browse, click, and type on.\n";
    out += "Learn more: https://code.claude.com/docs/en/chrome\n";
    return out;
}

/// The text the `/mobile` command prints when there is no view to show.
///
/// The code of the address of the application of the iPhone, the way the
/// reference draws it.
std::string mobile_text() {
    const std::string url = "https://apps.apple.com/app/claude-by-anthropic/id6473753684";
    const std::optional<ui::QrCode> code = ui::QrCode::encode(url);
    if (!code) return "The address of the application is too long for a code.\n";

    std::string out;
    for (const auto& line : code->lines()) out += line + "\n";
    out += "\n";
    out += "iOS / Android\n";
    out += url + "\n";
    return out;
}

/// One skill found in a skills directory.
struct Skill {
    /// The name of the skill, the name of its directory.
    std::string name;
    /// The description from the frontmatter of its `SKILL.md`.
    std::string description;
    /// The settings file the skills directory belongs to.
    std::string source;
    /// The path of the `SKILL.md` file.
    std::string path;
};

/// The skills found in the skills directories of the user and the project.
std::vector<Skill> skills(const CommandContext& context) {
    std::vector<Skill> found;

    // The reference lists the project skills before the user ones.
    const std::vector<std::pair<std::string, std::string>> directories = {
        {cc::path_join(cc::path_join(context.cwd, ".claude"), "skills"), "project settings"},
        {cc::path_join(cc::claude_config_dir(), "skills"), "user settings"},
    };

    for (const auto& [directory, source] : directories) {
        if (!cc::is_directory(directory)) continue;
        for (const auto& entry : cc::list_dir(directory)) {
            const std::string path = cc::path_join(directory, entry);
            if (!cc::is_directory(path)) continue;

            const std::string file = cc::path_join(path, "SKILL.md");
            const auto text = cc::read_file(file);
            if (!text) continue;

            const auto [name, description] = parse_frontmatter(*text);
            Skill skill;
            skill.name = name.empty() ? entry : name;
            skill.description = description;
            skill.source = source;
            skill.path = file;
            found.push_back(std::move(skill));
        }
    }

    return found;
}

/// The text the `/skills` command prints.
std::string skills_text(const CommandContext& context) {
    const auto found = skills(context);
    if (found.empty()) {
        return "No skills found\nCreate skills in .claude/skills/ or ~/.claude/skills/\n";
    }

    std::string out;
    out += std::to_string(found.size()) + (found.size() == 1 ? " skill\n" : " skills\n");
    out += "\n";

    std::string source;
    for (const auto& skill : found) {
        if (skill.source != source) {
            source = skill.source;
            out += source + " skills\n";
        }
        out += "  /" + skill.name;
        if (!skill.description.empty()) out += " · " + skill.description;
        out += "\n";
    }
    return out;
}

/// The text the `/statusline` command prints.
std::string statusline_text(const CommandContext& context, bool opened) {
    std::string out;
    const std::string path = config::user_settings_path();

    if (opened) {
        out += "Opened " + cc::tilde_path(path) + " in your editor.\n";
    } else {
        out += "Set the status line in " + cc::tilde_path(path) + ".\n";
    }

    if (context.settings && context.settings->status_line_command) {
        out += "Status line: " + *context.settings->status_line_command + "\n";
    } else {
        out += "No status line is configured.\n";
    }
    out += "Add a \"statusLine\" object with a \"command\" to the settings file to set one.\n";
    return out;
}

/// The text the `/keybindings` command prints.
std::string keybindings_text(const std::string& path, bool created, bool opened) {
    if (opened) {
        return created ? "Created " + path + " with template. Opened in your editor.\n"
                       : "Opened " + path + " in your editor.\n";
    }
    return created ? "Created " + path + " with template.\n" : "Opened " + path + ".\n";
}

/// The keybindings file of the user.
std::string keybindings_path() { return cc::path_join(cc::claude_config_dir(), "keybindings.json"); }

/// The prompt the `/review` command sends to the model.
std::string review_prompt(const std::string& args) {
    return "\n"
           "      You are an expert code reviewer. Follow these steps:\n"
           "\n"
           "      1. If no PR number is provided in the args, run `gh pr list` to show open PRs\n"
           "      2. If a PR number is provided, run `gh pr view <number>` to get PR details\n"
           "      3. Run `gh pr diff <number>` to get the diff\n"
           "      4. Analyze the changes and provide a thorough code review that includes:\n"
           "         - Overview of what the PR does\n"
           "         - Analysis of code quality and style\n"
           "         - Specific suggestions for improvements\n"
           "         - Any potential issues or risks\n"
           "\n"
           "      Keep your review concise but thorough. Focus on:\n"
           "      - Code correctness\n"
           "      - Following project conventions\n"
           "      - Performance implications\n"
           "      - Test coverage\n"
           "      - Security considerations\n"
           "\n"
           "      Format your review with clear sections and bullet points.\n"
           "\n"
           "      PR number: " +
           args + "\n"
           "    ";
}

/// The prompt the `/pr-comments` command sends to the model.
std::string pr_comments_prompt(const std::string& args) {
    return "You are an AI assistant integrated into a git-based version control system. Your task is "
           "to fetch and display comments from a GitHub pull request.\n"
           "\n"
           "Follow these steps:\n"
           "\n"
           "1. Use `gh pr view --json number,headRepository` to get the PR number and repository info\n"
           "2. Use `gh api /repos/{owner}/{repo}/issues/{number}/comments` to get PR-level comments\n"
           "3. Use `gh api /repos/{owner}/{repo}/pulls/{number}/comments` to get review comments. Pay "
           "particular attention to the following fields: `body`, `diff_hunk`, `path`, `line`, etc. If "
           "the comment references some code, consider fetching it using eg `gh api "
           "/repos/{owner}/{repo}/contents/{path}?ref={branch} | jq .content -r | base64 -d`\n"
           "4. Parse and format all comments in a readable way\n"
           "5. Return ONLY the formatted comments, with no additional text\n"
           "\n"
           "Format the comments as:\n"
           "\n"
           "## Comments\n"
           "\n"
           "[For each comment thread:]\n"
           "- @author file.ts#line:\n"
           "  ```diff\n"
           "  [diff_hunk from the API response]\n"
           "  ```\n"
           "  > quoted comment text\n"
           "\n"
           "  [any replies indented]\n"
           "\n"
           "If there are no comments, return \"No comments found.\"\n"
           "\n"
           "Remember:\n"
           "1. Only show the actual comments, no explanatory text\n"
           "2. Include both PR-level and code review comments\n"
           "3. Preserve the threading/nesting of comment replies\n"
           "4. Show the file and line number context for code review comments\n"
           "5. Use jq to parse the JSON responses from the GitHub API\n"
           "\n" +
           (args.empty() ? std::string() : "Additional user input: " + args + "\n");
}

/// The lines the attribution of the commits and of the pull requests holds.
struct Attribution {
    /// The line that is put at the end of the message of a commit.
    std::string commit;
    /// The line that is put at the end of the description of a pull request.
    std::string pr;
};

/// The attribution the reference adds to the commits and to the pull
/// requests.
///
/// The settings of the attribution win over the old setting of the
/// co-author; when the co-author is turned off, there is no attribution at
/// all.
Attribution attribution_texts(const CommandContext& context) {
    Attribution out;

    const std::string model = cc::model_public_name(context.model);
    const std::string default_commit = "Co-Authored-By: " + model + " <noreply@anthropic.com>";
    const std::string default_pr =
        "🤖 Generated with [Claude Code](https://claude.com/claude-code)";

    if (context.settings != nullptr) {
        if (context.settings->attribution_commit.has_value()) {
            out.commit = *context.settings->attribution_commit;
        } else if (context.settings->attribution_pr.has_value()) {
            out.pr = *context.settings->attribution_pr;
        } else if (!context.settings->include_co_authored_by) {
            return out;
        } else {
            out.commit = default_commit;
            out.pr = default_pr;
        }
        return out;
    }

    out.commit = default_commit;
    out.pr = default_pr;
    return out;
}

/// The commands the model may run while it makes a commit.
///
/// The reference names them the same way, as the rules of the tools.
const std::vector<std::string> kCommitTools = {
    "Bash(git add:*)",
    "Bash(git status:*)",
    "Bash(git commit:*)",
};

/// The commands the model may run while it makes a commit and opens a pull
/// request.
const std::vector<std::string> kCommitPushPrTools = {
    "Bash(git checkout --branch:*)",
    "Bash(git checkout -b:*)",
    "Bash(git add:*)",
    "Bash(git status:*)",
    "Bash(git push:*)",
    "Bash(git commit:*)",
    "Bash(gh pr create:*)",
    "Bash(gh pr edit:*)",
    "Bash(gh pr view:*)",
    "Bash(gh pr merge:*)",
};

/// The prompt the `/commit` command sends to the model.
///
/// The context of the repository is read into the prompt, the way the
/// reference reads it.
std::string commit_prompt(const CommandContext& context) {
    const Attribution attribution = attribution_texts(context);

    std::string out =
        "## Context\n"
        "\n"
        "- Current git status: !`git status`\n"
        "- Current git diff (staged and unstaged changes): !`git diff HEAD`\n"
        "- Current branch: !`git branch --show-current`\n"
        "- Recent commits: !`git log --oneline -10`\n"
        "\n"
        "## Git Safety Protocol\n"
        "\n"
        "- NEVER update the git config\n"
        "- NEVER skip hooks (--no-verify, --no-gpg-sign, etc) unless the user explicitly requests "
        "it\n"
        "- CRITICAL: ALWAYS create NEW commits. NEVER use git commit --amend, unless the user "
        "explicitly requests it\n"
        "- Do not commit files that likely contain secrets (.env, credentials.json, etc). Warn the "
        "user if they specifically request to commit those files\n"
        "- If there are no changes to commit (i.e., no untracked files and no modifications), do not "
        "create an empty commit\n"
        "- Never use git commands with the -i flag (like git rebase -i or git add -i) since they "
        "require interactive input which is not supported\n"
        "\n"
        "## Your task\n"
        "\n"
        "Based on the above changes, create a single git commit:\n"
        "\n"
        "1. Analyze all staged changes and draft a commit message:\n"
        "   - Look at the recent commits above to follow this repository's commit message style\n"
        "   - Summarize the nature of the changes (new feature, enhancement, bug fix, refactoring, "
        "test, docs, etc.)\n"
        "   - Ensure the message accurately reflects the changes and their purpose (i.e. \"add\" "
        "means a wholly new feature, \"update\" means an enhancement to an existing feature, "
        "\"fix\" means a bug fix, etc.)\n"
        "   - Draft a concise (1-2 sentences) commit message that focuses on the \"why\" rather "
        "than the \"what\"\n"
        "\n"
        "2. Stage relevant files and create the commit using HEREDOC syntax:\n"
        "```\n"
        "git commit -m \"$(cat <<'EOF'\n"
        "Commit message here.";

    if (!attribution.commit.empty()) {
        out += "\n\n" + attribution.commit;
        out +=
            "\nEOF\n"
            "\"\n"
            "```";
    } else {
        out +=
            "\nEOF\n"
            "\"\n"
            "```";
    }

    out +=
        "\n"
        "\n"
        "You have the capability to call multiple tools in a single response. Stage and create the "
        "commit using a single message. Do not use any other tools or do anything else. Do not "
        "send any other text or messages besides these tool calls.";

    return out;
}

/// The prompt the `/commit-push-pr` command sends to the model.
std::string commit_push_pr_prompt(const CommandContext& context) {
    const Attribution attribution = attribution_texts(context);

    const std::string branch =
        git::default_branch(context.cwd).empty() ? std::string("main")
                                                 : git::default_branch(context.cwd);

    std::string out =
        "## Context\n"
        "\n"
        "- `SAFEUSER`: " +
        std::string(std::getenv("SAFEUSER") != nullptr ? std::getenv("SAFEUSER") : "") +
        "\n"
        "- `whoami`: " +
        std::string(std::getenv("USER") != nullptr ? std::getenv("USER") : "") +
        "\n"
        "- `git status`: !`git status`\n"
        "- `git diff HEAD`: !`git diff HEAD`\n"
        "- `git branch --show-current`: !`git branch --show-current`\n"
        "- `git diff " +
        branch +
        "...HEAD`: !`git diff " + branch +
        "...HEAD`\n"
        "- `gh pr view --json number 2>/dev/null || true`: !`gh pr view --json number 2>/dev/null "
        "|| true`\n"
        "\n"
        "## Git Safety Protocol\n"
        "\n"
        "- NEVER update the git config\n"
        "- NEVER run destructive/irreversible git commands (like push --force, hard reset, etc) "
        "unless the user explicitly requests them\n"
        "- NEVER skip hooks (--no-verify, --no-gpg-sign, etc) unless the user explicitly requests "
        "it\n"
        "- NEVER run force push to main/master, warn the user if they request it\n"
        "- Do not commit files that likely contain secrets (.env, credentials.json, etc)\n"
        "- Never use git commands with the -i flag (like git rebase -i or git add -i) since they "
        "require interactive input which is not supported\n"
        "\n"
        "## Your task\n"
        "\n"
        "Analyze all changes that will be included in the pull request, making sure to look at all "
        "relevant commits (NOT just the latest commit, but ALL commits that will be included in the "
        "pull request from the git diff " +
        branch +
        "...HEAD output above).\n"
        "\n"
        "Based on the above changes:\n"
        "1. Create a new branch if on " +
        branch +
        " (use SAFEUSER from context above for the branch name prefix, falling back to whoami if "
        "SAFEUSER is empty, e.g., `username/feature-name`)\n"
        "2. Create a single commit with an appropriate message using heredoc syntax";

    if (!attribution.commit.empty()) {
        out += ", ending with the attribution text shown in the example below";
    }

    out +=
        ":\n"
        "```\n"
        "git commit -m \"$(cat <<'EOF'\n"
        "Commit message here.";

    if (!attribution.commit.empty()) out += "\n\n" + attribution.commit;

    out +=
        "\nEOF\n"
        "\"\n"
        "```\n"
        "3. Push the branch to origin\n"
        "4. If a PR already exists for this branch (check the gh pr view output above), update the "
        "PR title and body using `gh pr edit` to reflect the current diff (and add `--add-reviewer "
        "anthropics/claude-code`). Otherwise, create a pull request using `gh pr create` with "
        "heredoc syntax for the body and `--reviewer anthropics/claude-code`.\n"
        "   - IMPORTANT: Keep PR titles short (under 70 characters). Use the body for details.\n"
        "```\n"
        "gh pr create --title \"Short, descriptive title\" --body \"$(cat <<'EOF'\n"
        "## Summary\n"
        "<1-3 bullet points>\n"
        "\n"
        "## Test plan\n"
        "[Bulleted markdown checklist of TODOs for testing the pull request...]\n"
        "\n"
        "## Changelog\n"
        "<!-- CHANGELOG:START -->\n"
        "[If this PR contains user-facing changes, add a changelog entry here. Otherwise, remove "
        "this section.]\n"
        "<!-- CHANGELOG:END -->";

    if (!attribution.pr.empty()) out += "\n\n" + attribution.pr;

    out +=
        "\nEOF\n"
        "\"\n"
        "```\n"
        "\n"
        "You have the capability to call multiple tools in a single response. You MUST do all of "
        "the above in a single message.\n"
        "\n"
        "Return the PR URL when you're done, so the user can see it.";

    return out;
}

/// The prompt the `/security-review` command sends to the model.
///
/// This is the body of the reference's `security-review` markdown, without
/// its frontmatter.
std::string security_review_prompt() {
    return R"(You are a senior security engineer conducting a focused security review of the changes on this branch.

GIT STATUS:

```
!`git status`
```

FILES MODIFIED:

```
!`git diff --name-only origin/HEAD...`
```

COMMITS:

```
!`git log --no-decorate origin/HEAD...`
```

DIFF CONTENT:

```
!`git diff origin/HEAD...`
```

Review the complete diff above. This contains all code changes in the PR.


OBJECTIVE:
Perform a security-focused code review to identify HIGH-CONFIDENCE security vulnerabilities that could have real exploitation potential. This is not a general code review - focus ONLY on security implications newly added by this PR. Do not comment on existing security concerns.

CRITICAL INSTRUCTIONS:
1. MINIMIZE FALSE POSITIVES: Only flag issues where you're >80% confident of actual exploitability
2. AVOID NOISE: Skip theoretical issues, style concerns, or low-impact findings
3. FOCUS ON IMPACT: Prioritize vulnerabilities that could lead to unauthorized access, data breaches, or system compromise
4. EXCLUSIONS: Do NOT report the following issue types:
   - Denial of Service (DOS) vulnerabilities, even if they allow service disruption
   - Secrets or sensitive data stored on disk (these are handled by other processes)
   - Rate limiting or resource exhaustion issues

SECURITY CATEGORIES TO EXAMINE:

**Input Validation Vulnerabilities:**
- SQL injection via unsanitized user input
- Command injection in system calls or subprocesses
- XXE injection in XML parsing
- Template injection in templing engines
- NoSQL injection in database queries
- Path traversal in file operations

**Authentication & Authorization Issues:**
- Authentication bypass logic
- Privilege escalation paths
- Session management flaws
- JWT token vulnerabilities
- Authorization logic bypasses

**Crypto & Secrets Management:**
- Hardcoded API keys, passwords, or tokens
- Weak cryptographic algorithms or implementations
- Improper key storage or management
- Cryptographic randomness issues
- Certificate validation bypasses

**Injection & Code Execution:**
- Remote code execution via deseralization
- Pickle injection in Python
- YAML deserialization vulnerabilities
- Eval injection in dynamic code execution
- XSS vulnerabilities in web applications (reflected, stored, DOM-based)

**Data Exposure:**
- Sensitive data logging or storage
- PII handling violations
- API endpoint data leakage
- Debug information exposure

Additional notes:
- Even if something is only exploitable from the local network, it can still be a HIGH severity issue

ANALYSIS METHODOLOGY:

Phase 1 - Repository Context Research (Use file search tools):
- Identify existing security frameworks and libraries in use
- Look for established secure coding patterns in the codebase
- Examine existing sanitization and validation patterns
- Understand the project's security model and threat model

Phase 2 - Comparative Analysis:
- Compare new code changes against existing security patterns
- Identify deviations from established secure practices
- Look for inconsistent security implementations
- Flag code that introduces new attack surfaces

Phase 3 - Vulnerability Assessment:
- Examine each modified file for security implications
- Trace data flow from user inputs to sensitive operations
- Identify privilege boundaries being crossed unsafely
- Identify injection points and unsafe deserialization

REQUIRED OUTPUT FORMAT:

You MUST output your findings in markdown. The markdown output should contain the file, line number, severity, category (e.g. `sql_injection` or `xss`), description, exploit scenario, and fix recommendation.

For example:

# Vuln 1: XSS: `foo.py:42`

* Severity: High
* Description: User input from `username` parameter is directly interpolated into HTML without escaping, allowing reflected XSS attacks
* Exploit Scenario: Attacker crafts URL like /bar?q=<script>alert(document.cookie)</script> to execute JavaScript in victim's browser, enabling session hijacking or data theft
* Recommendation: Use Flask's escape() function or Jinja2 templates with auto-escaping enabled for all user inputs rendered in HTML

SEVERITY GUIDELINES:
- **HIGH**: Directly exploitable vulnerabilities leading to RCE, data breach, or authentication bypass
- **MEDIUM**: Vulnerabilities requiring specific conditions but with significant impact
- **LOW**: Defense-in-depth issues or lower-impact vulnerabilities

CONFIDENCE SCORING:
- 0.9-1.0: Certain exploit path identified, tested if possible
- 0.8-0.9: Clear vulnerability pattern with known exploitation methods
- 0.7-0.8: Suspicious pattern requiring specific conditions to exploit
- Below 0.7: Don't report (too speculative)

FINAL REMINDER:
Focus on HIGH and MEDIUM findings only. Better to miss some theoretical issues than flood the report with false positives. Each finding should be something a security engineer would confidently raise in a PR review.

FALSE POSITIVE FILTERING:

> You do not need to run commands to reproduce the vulnerability, just read the code to determine if it is a real vulnerability. Do not use the bash tool or write to any files.
>
> HARD EXCLUSIONS - Automatically exclude findings matching these patterns:
> 1. Denial of Service (DOS) vulnerabilities or resource exhaustion attacks.
> 2. Secrets or credentials stored on disk if they are otherwise secured.
> 3. Rate limiting concerns or service overload scenarios.
> 4. Memory consumption or CPU exhaustion issues.
> 5. Lack of input validation on non-security-critical fields without proven security impact.
> 6. Input sanitization concerns for GitHub Action workflows unless they are clearly triggerable via untrusted input.
> 7. A lack of hardening measures. Code is not expected to implement all security best practices, only flag concrete vulnerabilities.
> 8. Race conditions or timing attacks that are theoretical rather than practical issues. Only report a race condition if it is concretely problematic.
> 9. Vulnerabilities related to outdated third-party libraries. These are managed separately and should not be reported here.
> 10. Memory safety issues such as buffer overflows or use-after-free-vulnerabilities are impossible in rust. Do not report memory safety issues in rust or any other memory safe languages.
> 11. Files that are only unit tests or only used as part of running tests.
> 12. Log spoofing concerns. Outputting un-sanitized user input to logs is not a vulnerability.
> 13. SSRF vulnerabilities that only control the path. SSRF is only a concern if it can control the host or protocol.
> 14. Including user-controlled content in AI system prompts is not a vulnerability.
> 15. Regex injection. Injecting untrusted content into a regex is not a vulnerability.
> 16. Regex DOS concerns.
> 16. Insecure documentation. Do not report any findings in documentation files such as markdown files.
> 17. A lack of audit logs is not a vulnerability.
>
> PRECEDENTS -
> 1. Logging high value secrets in plaintext is a vulnerability. Logging URLs is assumed to be safe.
> 2. UUIDs can be assumed to be unguessable and do not need to be validated.
> 3. Environment variables and CLI flags are trusted values. Attackers are generally not able to modify them in a secure environment. Any attack that relies on controlling an environment variable is invalid.
> 4. Resource management issues such as memory or file descriptor leaks are not valid.
> 5. Subtle or low impact web vulnerabilities such as tabnabbing, XS-Leaks, prototype pollution, and open redirects should not be reported unless they are extremely high confidence.
> 6. React and Angular are generally secure against XSS. These frameworks do not need to sanitize or escape user input unless it is using dangerouslySetInnerHTML, bypassSecurityTrustHtml, or similar methods. Do not report XSS vulnerabilities in React or Angular components or tsx files unless they are using unsafe methods.
> 7. Most vulnerabilities in github action workflows are not exploitable in practice. Before validating a github action workflow vulnerability ensure it is concrete and has a very specific attack path for untrusted input.
> 8. A lack of permission checking or authentication in client-side JS/TS code is not a vulnerability. Client-side code is not trusted and does not need to implement these checks, they are handled on the server-side. The same applies to all flows that send untrusted data to the backend, the backend is responsible for validating and sanitizing all inputs.
> 9. Only include MEDIUM findings if they are obvious and concrete issues.
> 10. Most vulnerabilities in ipython notebooks (.ipynb files) are not exploitable in practice. Before validating a ipython notebook vulnerability ensure it is concrete and has a very specific attack path for untrusted input.
> 11. Logging non-PII data is not a vulnerability even if the data may be sensitive. Only report logging vulnerabilities if they expose sensitive information such as secrets, passwords, or personally identifiable information (PII).
> 12. Command injection vulnerabilities in shell scripts are generally not exploitable in practice since shell scripts generally do not run with untrusted user input. Only report command injection vulnerabilities in shell scripts if they are concrete and have a very specific attack path for untrusted input.
>
> SIGNAL QUALITY CRITERIA - For remaining findings, assess:
> 1. Is there a concrete, exploitable vulnerability with a clear attack path?
> 2. Does this represent a real security risk vs theoretical best practice?
> 3. Are there specific code locations and reproduction steps?
> 4. Would this finding be actionable for a security team?
>
> For each finding, assign a confidence score from 1-10:
> - 1-3: Low confidence, likely false positive or noise
> - 4-6: Medium confidence, needs investigation
> - 7-10: High confidence, likely true vulnerability

START ANALYSIS:

Begin your analysis now. Do this in 3 steps:

1. Use a sub-task to identify vulnerabilities. Use the repository exploration tools to understand the codebase context, then analyze the PR changes for security implications. In the prompt for this sub-task, include all of the above.
2. Then for each vulnerability identified by the above sub-task, create a new sub-task to filter out false-positives. Launch these sub-tasks as parallel sub-tasks. In the prompt for these sub-tasks, include everything in the "FALSE POSITIVE FILTERING" instructions.
3. Filter out any vulnerabilities where the sub-task reported a confidence less than 8.

Your final reply must contain the markdown report and nothing else.)";
}

/// Replaces the `!`command`` markers of a prompt with what the command printed.
///
/// The reference runs the commands of a prompt before it sends the prompt to
/// the model; the port does the same.
std::string expand_prompt_commands(const std::string& text, const std::string& cwd) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const size_t start = text.find("!`", i);
        if (start == std::string::npos) {
            out += text.substr(i);
            break;
        }
        const size_t end = text.find('`', start + 2);
        if (end == std::string::npos) {
            out += text.substr(i);
            break;
        }
        out += text.substr(i, start - i);
        const std::string command = text.substr(start + 2, end - start - 2);
        out += shell_output(command, cwd);
        i = end + 1;
    }
    return out;
}

/// The text the `/copy` command prints.
///
/// The port has no clipboard, so it writes the answer to a file the way the
/// reference does as its fallback and says that the clipboard is out of reach.
std::string copy_text(const CommandContext& context, const std::string& argument) {
    static const std::vector<Message> kNoMessages;
    const auto& messages = context.messages ? *context.messages : kNoMessages;

    // The reference walks the messages newest first and keeps the answers
    // that actually said something.
    std::vector<std::string> texts;
    for (auto it = messages.rbegin(); it != messages.rend() && texts.size() < 20; ++it) {
        const auto& message = *it;
        if (message.role != Role::Assistant) continue;
        if (message.is_api_error_message) continue;
        const std::string text = cc::trim(message.text());
        if (!text.empty()) texts.push_back(text);
    }

    if (texts.empty()) return "No assistant message to copy\n";

    size_t age = 0;
    const std::string arg = cc::trim(argument);
    if (!arg.empty()) {
        const auto number = cc::parse_int(arg);
        if (!number || *number < 1) {
            return "Usage: /copy [N] where N is 1 (latest), 2, 3, … Got: " + arg + "\n";
        }
        if (static_cast<size_t>(*number) > texts.size()) {
            return "Only " + std::to_string(texts.size()) + " assistant " +
                   (texts.size() == 1 ? "message" : "messages") + " available to copy\n";
        }
        age = static_cast<size_t>(*number) - 1;
    }

    const std::string& text = texts[age];
    // The reference counts the newlines of the answer and adds one.
    size_t lines = 1;
    for (const char c : text) {
        if (c == '\n') ++lines;
    }

    // The reference writes the answer to a file in the temporary directory,
    // the way it does when the clipboard is out of reach.
    const char* tmpdir = std::getenv("TMPDIR");
    const std::string directory =
        cc::path_join((tmpdir && *tmpdir) ? std::string(tmpdir) : std::string("/tmp"), "claude");
    const std::string path = cc::path_join(directory, "response.md");
    std::string error;
    if (!cc::write_file(path, text, &error)) {
        return "Could not write " + path + "\n";
    }

    std::string out;
    out += "Also written to " + path + " (" + std::to_string(text.size()) + " characters, " +
           std::to_string(lines) + " lines)\n";
    out += "Copying to the clipboard is not available in the C++ port.\n";
    return out;
}

/// The colours the reference's `/color` command accepts.
const std::vector<std::string>& agent_colors() {
    static const std::vector<std::string> colors = {"red",   "blue",  "green", "yellow",
                                                    "purple", "orange", "pink", "cyan"};
    return colors;
}

/// The text the `/color` command prints.
std::string color_text(const std::string& argument) {
    const std::string colors = cc::join(agent_colors(), ", ");

    if (cc::trim(argument).empty()) {
        return "Please provide a color. Available colors: " + colors + ", default\n";
    }

    const std::string arg = cc::to_lower(cc::trim(argument));

    static const std::vector<std::string> kResetAliases = {"default", "reset", "none", "gray", "grey"};
    if (std::find(kResetAliases.begin(), kResetAliases.end(), arg) != kResetAliases.end()) {
        return "Session color reset to default\n";
    }

    const auto& colors_list = agent_colors();
    if (std::find(colors_list.begin(), colors_list.end(), arg) == colors_list.end()) {
        return "Invalid color \"" + arg + "\". Available colors: " + colors + ", default\n";
    }

    return "Session color set to: " + arg + "\n";
}

/// The text the `/rename` command prints.
std::string rename_text(const std::string& name) { return "Session renamed to: " + name + "\n"; }

/// The text the `/stats` command prints.
///
/// The reference aggregates the transcripts of every session; the port counts
/// the sessions of the project and the messages of the current one.
std::string stats_text(const CommandContext& context) {
    const auto sessions = session::Transcript::list_sessions(context.cwd);

    size_t messages = 0;
    if (context.messages) {
        for (const auto& message : *context.messages) {
            if (!message.is_meta) ++messages;
        }
    }

    std::string out;
    out += "Usage statistics\n\n";
    out += "  Sessions: " + std::to_string(sessions.size()) + "\n";
    out += "  Messages: " + std::to_string(messages) + "\n";

    if (context.usage) {
        const int64_t total = context.usage->input_tokens + context.usage->output_tokens;
        out += "  Total tokens: " + std::to_string(total) + "\n";
    }
    if (!context.model.empty()) {
        out += "  Favorite model: " + model_display_name(context.model) + "\n";
    }
    return out;
}

/// The text the `/session` command prints.
///
/// The port has no remote mode, so it reports what the reference reports when
/// the session did not start with `--remote`.
std::string session_text() {
    return "Not in remote mode. Start with `claude --remote` to use this command.\n";
}

/// The text the `/branch` command prints.
///
/// The port cannot switch to the branch it creates, so it prints the message
/// the reference prints when it cannot resume into the fork either.
std::string branch_text(const std::string& title, const std::string& session_id) {
    const std::string title_info = title.empty() ? "" : " \"" + title + "\"";
    return "Branched conversation" + title_info + ". Resume with: /resume " + session_id + "\n";
}

/// The id of the session the command runs in.
///
/// The context carries it when the app knows it. Otherwise the transcript
/// that holds the last message of the conversation is the session.
std::string current_session_id(const CommandContext& context) {
    if (!context.session_id.empty()) return context.session_id;

    std::string wanted;
    if (context.messages && !context.messages->empty()) {
        wanted = context.messages->back().uuid;
    }
    if (wanted.empty()) return {};

    const std::string directory = session::Transcript::project_dir(context.cwd);
    for (const auto& info : session::Transcript::list_sessions(context.cwd)) {
        const auto messages = session::Transcript::read(cc::path_join(directory, info.id + ".jsonl"));
        if (messages.empty()) continue;
        if (messages.back().uuid == wanted) return info.id;
    }
    return {};
}

/// The title the reference gives a branch: the first thing the user asked,
/// on a single line and at most a hundred characters.
std::string branch_base_name(const CommandContext& context) {
    static const std::vector<Message> kNoMessages;
    const auto& messages = context.messages ? *context.messages : kNoMessages;

    for (const auto& message : messages) {
        if (message.role != Role::User || message.is_meta) continue;
        const std::string text = cc::trim(message.text());
        if (text.empty()) continue;

        std::string collapsed;
        bool space = false;
        for (const char c : text) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                space = true;
                continue;
            }
            if (space && !collapsed.empty()) collapsed.push_back(' ');
            space = false;
            collapsed.push_back(c);
        }
        if (collapsed.size() > 100) collapsed.resize(100);
        if (!collapsed.empty()) return collapsed;
    }
    return "Branched conversation";
}

/// The prompt the `/rename` command sends to the model when it has to make up
/// a name for the session.
std::string rename_prompt(const CommandContext& context) {
    static const std::vector<Message> kNoMessages;
    const auto& messages = context.messages ? *context.messages : kNoMessages;

    std::string conversation;
    for (const auto& message : messages) {
        if (message.is_meta) continue;
        const std::string text = cc::trim(message.text());
        if (text.empty()) continue;
        conversation += (message.role == Role::User ? "User: " : "Assistant: ") + text + "\n";
    }

    return "Generate a short kebab-case name (2-4 words) that captures the main topic of this "
           "conversation. Use lowercase words separated by hyphens. Examples: \"fix-login-bug\", "
           "\"add-auth-feature\", \"refactor-api-client\", \"debug-test-failures\". Return JSON with a "
           "\"name\" field.\n\n" +
           conversation;
}

/// The name the model made up for the session, or an empty string.
std::string generated_session_name(const std::string& answer) {
    const auto parsed = cc::json_parse(answer, boost::json::value(nullptr));
    if (!parsed.is_object()) return {};
    const auto name = jget_optional_string(parsed.as_object(), "name");
    if (!name) return {};
    return cc::trim(*name);
}

/// The text the `/add-dir` command prints.
std::string add_dir_text(const CommandContext& context, const std::string& argument) {
    const std::string path = cc::trim(argument);
    if (path.empty()) return "Please provide a directory path.\n";

    const std::string absolute = cc::resolve(path);
    if (!cc::is_directory(absolute)) {
        if (cc::file_exists(absolute)) {
            return path + " is not a directory. Did you mean to add the parent directory " +
                   cc::dirname(absolute) + "?\n";
        }
        return "Path " + absolute + " was not found.\n";
    }

    if (context.settings) {
        for (const auto& directory : context.settings->additional_directories) {
            if (cc::normalize(directory) == cc::normalize(absolute)) {
                return path + " is already accessible within the existing working directory " +
                       directory + ".\n";
            }
        }
    }

    // The port cannot add the directory to the running session, so it saves
    // it to the local settings, the way the reference does when the user asks
    // for the directory to be remembered.
    const std::string settings_path = config::local_settings_path(context.cwd);
    boost::json::object root;
    if (const auto text = cc::read_file(settings_path)) {
        const auto parsed = cc::json_parse(*text, boost::json::value(nullptr));
        if (parsed.is_object()) root = parsed.as_object();
    }

    auto permissions = jget_object(root, "permissions");
    auto directories = jget_array(permissions, "additionalDirectories");
    bool known = false;
    for (const auto& directory : directories) {
        if (!directory.is_string()) continue;
        if (cc::normalize(std::string(directory.as_string().c_str())) == cc::normalize(absolute)) {
            known = true;
        }
    }
    if (!known) directories.emplace_back(absolute);
    permissions["additionalDirectories"] = std::move(directories);
    root["permissions"] = std::move(permissions);

    std::string error;
    if (!config::write_settings(settings_path, boost::json::value(std::move(root)), &error)) {
        return "Added " + absolute + " as a working directory. Failed to save to local settings: " +
               (error.empty() ? "Unknown error" : error) + "\n";
    }

    return "Added " + absolute + " as a working directory and saved to local settings · /permissions to "
           "manage\n";
}

/// The terminals that support the CSI u keyboard protocol natively, and the
/// name the reference shows for each of them.
const std::vector<std::pair<std::string, std::string>>& native_csiu_terminals() {
    static const std::vector<std::pair<std::string, std::string>> terminals = {
        {"ghostty", "Ghostty"},
        {"kitty", "Kitty"},
        {"iTerm.app", "iTerm2"},
        {"WezTerm", "WezTerm"},
        {"WarpTerminal", "Warp"},
    };
    return terminals;
}

/// The name of the terminal the CLI runs in, the way the reference names it.
std::string terminal_kind() {
    if (const char* program = std::getenv("TERM_PROGRAM")) {
        if (*program) return program;
    }
    if (const char* program = std::getenv("TERM_PROGRAM_SHORT")) {
        if (*program) return program;
    }
    return "";
}

/// The text the `/terminal-setup` command prints for Terminal.app.
///
/// The reference enables "Use Option as Meta key" and switches to the visual
/// bell in the default and the startup profile.
std::string setup_apple_terminal() {
    const std::string backup = cc::path_join("/tmp", "claude-terminal-preferences.plist");
    const std::string plist =
        cc::path_join(cc::home_dir(), "Library/Preferences/com.apple.Terminal.plist");
    const std::string script =
        "cp '" + plist + "' '" + backup + "' 2>/dev/null; " +
        "default_profile=$(defaults read com.apple.Terminal 'Default Window Settings' 2>/dev/null); " +
        "startup_profile=$(defaults read com.apple.Terminal 'Startup Window Settings' 2>/dev/null); " +
        "for profile in \"$default_profile\" \"$startup_profile\"; do " +
        "[ -z \"$profile\" ] && continue; " +
        "/usr/libexec/PlistBuddy -c \"Add :'Window Settings':'$profile':useOptionAsMetaKey bool true\" "
        "'" + plist + "' 2>/dev/null || " +
        "/usr/libexec/PlistBuddy -c \"Set :'Window Settings':'$profile':useOptionAsMetaKey true\" '" +
        plist + "' 2>/dev/null; " +
        "/usr/libexec/PlistBuddy -c \"Add :'Window Settings':'$profile':Bell bool false\" '" + plist +
        "' 2>/dev/null || " +
        "/usr/libexec/PlistBuddy -c \"Set :'Window Settings':'$profile':Bell false\" '" + plist +
        "' 2>/dev/null; " +
        "done; killall cfprefsd 2>/dev/null; exit 0";

    int exit_code = 0;
    shell_output(script, "", &exit_code);
    if (exit_code != 0) {
        return "Failed to enable Option as Meta key for Terminal.app.\n";
    }

    std::string out;
    out += "Configured Terminal.app settings:\n";
    out += "- Enabled \"Use Option as Meta key\"\n";
    out += "- Switched to visual bell\n";
    out += "Option+Enter will now enter a newline.\n";
    out += "You must restart Terminal.app for changes to take effect.\n";
    return out;
}

/// Adds `item` to the JSON array in `text`, keeping the rest of the file as it
/// was.
std::string add_to_json_array(const std::string& text, const std::string& item) {
    const size_t open = text.find('[');
    const size_t close = text.find_last_of(']');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return "[\n  " + item + "\n]\n";
    }

    const std::string body = text.substr(open + 1, close - open - 1);
    bool empty = true;
    for (const char c : body) {
        if (!std::isspace(static_cast<unsigned char>(c)) && c != ',') {
            empty = false;
            break;
        }
    }

    std::string out = text.substr(0, close);
    if (!out.empty() && !std::isspace(static_cast<unsigned char>(out.back()))) out += "\n";
    if (!empty) out += ",";
    out += "\n  " + item + "\n";
    out += text.substr(close);
    return out;
}

/// The text the `/terminal-setup` command prints for VS Code and the editors
/// that share its key bindings.
std::string setup_vscode_terminal(const std::string& editor, const std::string& directory) {
    const std::string user_dir = cc::path_join(
        cc::path_join(cc::home_dir(), "Library/Application Support"), cc::path_join(directory, "User"));
    const std::string path = cc::path_join(user_dir, "keybindings.json");

    std::string content = "[]";
    if (const auto existing = cc::read_file(path)) {
        content = *existing;
        if (cc::contains(content, "shift+enter") &&
            cc::contains(content, "workbench.action.terminal.sendSequence")) {
            return "Found existing " + editor +
                   " terminal Shift+Enter key binding. Remove it to continue.\nSee " + path + "\n";
        }
    }

    std::string error;
    if (!cc::mkdirp(user_dir, &error)) {
        return "Failed to install " + editor + " terminal Shift+Enter key binding\n";
    }

    // The file is backed up before it is changed, the way the reference does.
    if (cc::file_exists(path)) {
        cc::write_file(path + ".bak", content, &error);
    }

    const std::string binding =
        "{\n"
        "  \"key\": \"shift+enter\",\n"
        "  \"command\": \"workbench.action.terminal.sendSequence\",\n"
        "  \"args\": { \"text\": \"\\u001b\\r\" },\n"
        "  \"when\": \"terminalFocus\"\n"
        "}";

    if (!cc::write_file(path, add_to_json_array(content, binding), &error)) {
        return "Failed to install " + editor + " terminal Shift+Enter key binding\n";
    }

    return "Installed " + editor + " terminal Shift+Enter key binding\nSee " + path + "\n";
}

/// The text the `/terminal-setup` command prints for Alacritty.
std::string setup_alacritty() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const std::string base = (xdg && *xdg) ? std::string(xdg) : cc::path_join(cc::home_dir(), ".config");
    const std::string path = cc::path_join(cc::path_join(base, "alacritty"), "alacritty.toml");

    std::string content;
    if (const auto existing = cc::read_file(path)) {
        content = *existing;
        if (cc::contains(content, "mods = \"Shift\"") && cc::contains(content, "key = \"Return\"")) {
            return "Found existing Alacritty Shift+Enter key binding. Remove it to continue.\nSee " +
                   path + "\n";
        }
    }

    std::string error;
    if (!cc::mkdirp(cc::dirname(path), &error)) {
        return "Failed to install Alacritty Shift+Enter key binding\n";
    }
    if (cc::file_exists(path)) {
        cc::write_file(path + ".bak", content, &error);
    }

    if (!content.empty() && content.back() != '\n') content += "\n";
    content +=
        "\n[[keyboard.bindings]]\n"
        "key = \"Return\"\n"
        "mods = \"Shift\"\n"
        "chars = \"\\u001B\\r\"\n";

    if (!cc::write_file(path, content, &error)) {
        return "Failed to install Alacritty Shift+Enter key binding\n";
    }

    return "Installed Alacritty Shift+Enter key binding\n"
           "You may need to restart Alacritty for changes to take effect\nSee " +
           path + "\n";
}

/// The text the `/terminal-setup` command prints for Zed.
std::string setup_zed() {
    const std::string directory = cc::path_join(cc::home_dir(), ".config/zed");
    const std::string path = cc::path_join(directory, "keymap.json");

    std::string content = "[]";
    if (const auto existing = cc::read_file(path)) {
        content = *existing;
        if (cc::contains(content, "shift-enter")) {
            return "Found existing Zed Shift+Enter key binding. Remove it to continue.\nSee " + path +
                   "\n";
        }
    }

    std::string error;
    if (!cc::mkdirp(directory, &error)) {
        return "Failed to install Zed Shift+Enter key binding\n";
    }
    if (cc::file_exists(path)) {
        cc::write_file(path + ".bak", content, &error);
    }

    boost::json::array keymap;
    const auto parsed = cc::json_parse(content, boost::json::value(nullptr));
    if (parsed.is_array()) {
        for (const auto& item : parsed.as_array()) {
            if (item.is_object()) keymap.push_back(item);
        }
    }

    boost::json::object binding;
    binding["context"] = "Terminal";
    boost::json::object bindings;
    boost::json::array sequence;
    sequence.emplace_back("terminal::SendText");
    sequence.emplace_back("\x1b\r");
    bindings["shift-enter"] = std::move(sequence);
    binding["bindings"] = std::move(bindings);
    keymap.emplace_back(std::move(binding));

    if (!cc::write_file(path, cc::json_dump_pretty(boost::json::value(std::move(keymap))) + "\n", &error)) {
        return "Failed to install Zed Shift+Enter key binding\n";
    }

    return "Installed Zed Shift+Enter key binding\nSee " + path + "\n";
}

/// The text the `/terminal-setup` command prints.
///
/// The reference installs the key bindings of the terminal it runs in; the
/// port installs the ones of the terminals the reference supports and reports
/// what the reference reports for the terminals it cannot touch.
std::string terminal_setup_text() {
    const std::string terminal = terminal_kind();

    for (const auto& [kind, name] : native_csiu_terminals()) {
        if (kind != terminal) continue;
        return "Shift+Enter is natively supported in " + name +
               ".\n\nNo configuration needed. Just use Shift+Enter to add newlines.\n";
    }

    if (terminal == "Apple_Terminal") return setup_apple_terminal();
    if (terminal == "vscode") return setup_vscode_terminal("VSCode", "Code");
    if (terminal == "cursor") return setup_vscode_terminal("Cursor", "Cursor");
    if (terminal == "windsurf") return setup_vscode_terminal("Windsurf", "Windsurf");
    if (terminal == "alacritty") return setup_alacritty();
    if (terminal == "zed") return setup_zed();

    const std::string name = terminal.empty() ? "your current terminal" : terminal;
    std::string out;
    out += "Terminal setup cannot be run from " + name + ".\n";
    out += "\n";
    out += "This command configures a convenient Shift+Enter shortcut for multi-line prompts.\n";
    out += "Note: You can already use backslash (\\) + return to add newlines.\n";
    out += "\n";
    out += "To set up the shortcut (optional):\n";
    out += "1. Exit tmux/screen temporarily\n";
    out += "2. Run /terminal-setup directly in one of these terminals:\n";
    out += "   • macOS: Apple Terminal\n";
    out += "   • IDE: VSCode, Cursor, Windsurf, Zed\n";
    out += "   • Other: Alacritty\n";
    out += "3. Return to tmux/screen - settings will persist\n";
    out += "\n";
    out += "Note: iTerm2, WezTerm, Ghostty, Kitty, and Warp support Shift+Enter natively.\n";
    return out;
}

} // namespace

std::vector<CommandOption> model_options(const CommandContext& context) {
    std::vector<CommandOption> options;
    options.push_back({"Default (recommended)", "default",
                       "Use the default model (currently " +
                           model_display_name(context.model.empty() ? "claude-sonnet-4-6"
                                                                    : context.model) +
                           ")"});
    options.push_back({"Sonnet", "sonnet", "Sonnet 4.6 · Best for everyday tasks · $3/$15 per Mtok"});
    options.push_back({"Sonnet (1M context)", "sonnet[1m]",
                       "Sonnet 4.6 for long sessions · $3/$15 per Mtok"});
    options.push_back({"Opus", "opus", "Opus 4.6 · Most capable for complex work · $5/$25 per Mtok"});
    options.push_back({"Opus (1M context)", "opus[1m]",
                       "Opus 4.6 for long sessions · $5/$25 per Mtok"});
    options.push_back({"Haiku", "haiku", "Haiku 4.5 · Fastest for quick answers · $1/$5 per Mtok"});

    // The reference marks the model the session runs on with the description
    // "Current model", and adds it to the list when the picker does not
    // offer it.
    const std::string current = context.model;
    if (!current.empty()) {
        bool found = false;
        for (auto& option : options) {
            if (option.value == current) {
                option.description = "Current model";
                found = true;
            }
        }
        if (!found) {
            options.push_back({model_display_name(current), current, "Current model"});
        }
    }

    return options;
}

std::vector<CommandOption> theme_options() {
    return {
        {"Auto (match terminal)", "auto", "Follow the colour scheme of your terminal"},
        {"Dark mode", "dark", ""},
        {"Light mode", "light", ""},
        {"Dark mode (colorblind-friendly)", "dark-daltonized", ""},
        {"Light mode (colorblind-friendly)", "light-daltonized", ""},
        {"Dark mode (ANSI colors only)", "dark-ansi", ""},
        {"Light mode (ANSI colors only)", "light-ansi", ""},
    };
}

std::vector<CommandOption> effort_options() {
    return {
        {"low", "low", "Quick, straightforward implementation with minimal overhead"},
        {"medium", "medium", "Balanced approach with standard implementation and testing"},
        {"high", "high",
         "Comprehensive implementation with extensive testing and documentation"},
        {"max", "max", "Maximum capability with deepest reasoning"},
        {"auto", "auto", "Use the default effort level for your model"},
    };
}

std::vector<CommandOption> output_style_options(const std::string& cwd) {
    std::vector<CommandOption> options = {
        {"Default", "default",
         "Claude completes coding tasks efficiently and provides concise responses"},
        {"Explanatory", "Explanatory",
         "Claude explains its implementation choices and codebase patterns"},
        {"Learning", "Learning",
         "Claude pauses and asks you to write small pieces of code for hands-on practice"},
    };

    // The styles the user wrote themselves, and the styles of the folder.
    for (const config::OutputStyle& style : config::load_output_styles(cwd)) {
        options.push_back({style.name, style.name, style.description});
    }

    return options;
}


/// The description of the `/sandbox` command.
///
/// The reference writes the state of the sandbox in the description of the
/// command: the mark of the state, the state itself and the way it is
/// changed.
std::string sandbox_description(const SandboxState& state) {
    std::string icon = "◯";
    if (!state.has_dependencies) {
        icon = "⚠";
    } else if (state.enabled) {
        // The reference draws the mark with the tick of the `figures`
        // package, which is the heavy one.
        icon = std::string(ui::figures::kTick);
    }

    std::string status = "sandbox disabled";
    if (state.enabled) {
        status = state.auto_allow ? "sandbox enabled (auto-allow)" : "sandbox enabled";
        if (state.allow_unsandboxed) status += ", fallback allowed";
    }
    if (state.locked) status += " (managed)";

    return icon + " " + status + " (⏎ to configure)";
}

/// The commands of the CLI.
///
/// The list is built once and kept, the way the reference keeps its list; the
/// description of the `/sandbox` command is written into it when the state of
/// the sandbox changes.
std::vector<Command>& commands_storage() {
    static std::vector<Command> commands = build_commands();
    return commands;
}

const std::vector<Command>& all() {
    return commands_storage();
}

std::string render_model_name(const std::string& model) {
    // The tag of the window of a million tokens is shown with the name.
    std::string name = model;
    bool one_million = false;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, "[1m]") == 0) {
        one_million = true;
        name = name.substr(0, name.size() - 4);
    }

    // The public names of the models the reference knows.
    static const std::vector<std::pair<std::string, std::string>> kPublicNames = {
        {"claude-opus-4-6", "Opus 4.6"},
        {"claude-opus-4-5", "Opus 4.5"},
        {"claude-opus-4-1", "Opus 4.1"},
        {"claude-sonnet-4-6", "Sonnet 4.6"},
        {"claude-sonnet-4-5", "Sonnet 4.5"},
        {"claude-haiku-4-5", "Haiku 4.5"},
        {"claude-3-7-sonnet", "Sonnet 3.7"},
        {"claude-3-5-sonnet", "Sonnet 3.5"},
    };
    for (const auto& [id, public_name] : kPublicNames) {
        if (name != id) continue;
        name = public_name;
        break;
    }

    if (one_million) name += " (1M context)";
    return name;
}

void set_model(const std::string& model) {
    for (Command& command : commands_storage()) {
        if (command.name != "model") continue;
        command.description = "Set the AI model for Claude Code (currently " +
                              render_model_name(model) + ")";
        return;
    }
}

void set_sandbox_state(const SandboxState& state) {
    for (Command& command : commands_storage()) {
        if (command.name != "sandbox") continue;
        command.description = sandbox_description(state);
        return;
    }
}

bool is_available(const Command& command) {
    if (command.hidden) return false;
    if (command.enabled && !command.enabled()) return false;
    return true;
}

const Command* find_command(const std::string& name) {
    for (const auto& command : all()) {
        if (command.name == name) return &command;
        for (const auto& alias : command.aliases) {
            if (alias == name) return &command;
        }
    }
    return nullptr;
}

std::string command_metadata(const std::string& name, const std::string& args) {
    std::string out;
    out += "<command-message>" + name + "</command-message>\n";
    out += "<command-name>/" + name + "</command-name>";
    if (!args.empty()) out += "\n<command-args>" + args + "</command-args>";
    return out;
}

std::string help_text() {
    std::string out;
    out += "Commands:\n";
    for (const auto& command : all()) {
        if (command.hidden) continue;
        std::string line = "  /" + command.name;
        if (!command.argument_hint.empty()) line += " " + command.argument_hint;
        if (line.size() < 34) {
            while (line.size() < 34) line.push_back(' ');
        } else {
            // The hint already fills the column, so the description is put
            // two spaces further along.
            line += "  ";
        }
        line += command.description;
        out += line + "\n";
    }
    out += "\nShortcuts:\n";
    out += "  ! for bash mode\n";
    out += "  / for commands\n";
    out += "  @ for file paths\n";
    out += "  & for background\n";
    out += "\n  double tap esc to clear input\n";
    out += "  shift+tab to auto-accept edits\n";
    out += "  ctrl+o for verbose output\n";
    out += "  ctrl+t to toggle tasks\n";
    out += "  backslash (\\) + return (⏎) for newline\n";
    return out;
}

std::string status_text(const std::string& cwd) { return status_text(cwd, ""); }

std::string status_text(const std::string& cwd, const std::string& model) {
    std::string out;
    out += "  Version: " + std::string(version()) + "\n";
    out += "  cwd: " + cwd + "\n";
    if (!model.empty()) {
        out += "  Model: " + model_display_name(model) + "\n";
    }
    out += "  OS: " + std::string("macOS") + "\n";
    out += "  Terminal: " + terminal_name() + "\n";
    return out;
}

std::string cost_text(const Usage& usage) {
    std::string out;
    out += "Total tokens: " + std::to_string(usage.input_tokens + usage.output_tokens) + "\n";
    out += "  Input: " + std::to_string(usage.input_tokens) + "\n";
    out += "  Output: " + std::to_string(usage.output_tokens) + "\n";
    if (usage.cache_read_input_tokens) {
        out += "  Cache read: " + std::to_string(usage.cache_read_input_tokens) + "\n";
    }
    if (usage.cache_creation_input_tokens) {
        out += "  Cache write: " + std::to_string(usage.cache_creation_input_tokens) + "\n";
    }
    return out;
}

std::string context_text(size_t message_count) {
    std::string out;
    out += "Context: " + std::to_string(message_count) + " messages in the conversation\n";
    return out;
}

std::string files_text(const std::vector<Message>& messages) {
    std::vector<std::string> files;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            const auto* tool_use = as_tool_use(block);
            if (!tool_use) continue;
            if (!tool_use->input.is_object()) continue;
            const auto& object = tool_use->input.as_object();
            const auto path = object.find("file_path");
            if (path == object.end()) continue;
            if (!path->value().is_string()) continue;
            const std::string name = std::string(path->value().as_string().c_str());
            if (std::find(files.begin(), files.end(), name) == files.end()) {
                files.push_back(name);
            }
        }
    }

    if (files.empty()) return "No files in context.\n";

    std::string out;
    out += "Files in context:\n";
    for (const auto& file : files) {
        out += "  " + file + "\n";
    }
    return out;
}

CommandResult run_command(const std::string& name, const std::string& argument,
                           const CommandContext& context) {
    const Command* command = find_command(name);
    if (!command) return {};

    CommandResult result;
    result.handled = true;

    const std::string& command_name = command->name;

    if (command_name == "help") {
        if (context.show_help) {
            context.show_help();
        } else {
            result.output = help_text();
        }
    } else if (command_name == "exit") {
        if (context.exit_repl) context.exit_repl();
    } else if (command_name == "clear") {
        if (context.clear_conversation) context.clear_conversation();
    } else if (command_name == "version") {
        result.output = version_text(context);
    } else if (command_name == "config") {
        // The reference opens the tabbed panel of the settings with the tab
        // of the configuration chosen; the port opens the same panel, and
        // falls back to the text when there is nobody to press a key.
        if (context.show_settings) {
            context.show_settings("Config");
        } else {
            result.output = config_text(context);
        }
    } else if (command_name == "model") {
        const std::string arg = cc::trim(argument);
        if (!arg.empty()) {
            if (!known_model(arg)) {
                result.output = "Model '" + arg + "' not found\n";
            } else {
                result.output = "Set model to " + model_setting_label(arg) + "\n";
                result.output += model_kept_note(context);
            }
        } else if (context.select_option) {
            const auto options = model_options(context);
            const std::string chosen = context.select_option("Select model", options);
            if (chosen.empty()) {
                result.output = "Kept model as " + model_display_name(context.model) + "\n";
            } else {
                std::string label = chosen;
                for (const auto& option : options) {
                    if (option.value == chosen) {
                        label = option.label;
                        break;
                    }
                }
                result.output = "Set model to " + label + "\n";
                result.output += model_kept_note(context);
            }
        } else {
            result.output = model_text(context);
        }
    } else if (command_name == "theme") {
        if (context.pick_theme) {
            // The reference opens a picker that shows what each theme looks
            // like, so that the theme is seen before it is chosen.
            const std::string chosen = context.pick_theme();
            if (chosen.empty()) {
                result.output = "Theme picker dismissed\n";
            } else {
                std::string error;
                if (!write_user_setting("theme", boost::json::value(chosen), &error)) {
                    result.output = "Failed to set theme: " +
                                    (error.empty() ? "Unknown error" : error) + "\n";
                } else {
                    result.output = theme_set_text(chosen);
                    result.output += "The theme applies from the next session.\n";
                }
            }
        } else if (context.select_option) {
            const auto options = theme_options();
            const std::string chosen = context.select_option("Theme", options);
            if (chosen.empty()) {
                result.output = "Theme picker dismissed\n";
            } else {
                std::string error;
                if (!write_user_setting("theme", boost::json::value(chosen), &error)) {
                    result.output = "Failed to set theme: " +
                                    (error.empty() ? "Unknown error" : error) + "\n";
                } else {
                    result.output = theme_set_text(chosen);
                    result.output += "The theme applies from the next session.\n";
                }
            }
        } else {
            result.output = theme_text();
        }
    } else if (command_name == "output-style") {
        result.output =
            "/output-style has been deprecated. Use /config to change your output style, or set it in "
            "your settings file. Changes take effect on the next session.\n";
        if (context.select_option) {
            const auto options = output_style_options(context.cwd);
            const std::string chosen = context.select_option("Preferred output style", options);
            if (chosen.empty()) {
                result.output += "Output style picker dismissed\n";
            } else {
                std::string error;
                if (!write_user_setting("outputStyle", boost::json::value(chosen), &error)) {
                    result.output += "Failed to set output style: " +
                                     (error.empty() ? "Unknown error" : error) + "\n";
                } else {
                    result.output += "Output style set to " + chosen + "\n";
                }
            }
        }
    } else if (command_name == "effort") {
        const std::string arg = cc::to_lower(cc::trim(argument));
        if (arg == "help" || arg == "-h" || arg == "--help") {
            result.output =
                "Usage: /effort [low|medium|high|max|auto]\n"
                "\n"
                "Effort levels:\n"
                "- low: Quick, straightforward implementation\n"
                "- medium: Balanced approach with standard testing\n"
                "- high: Comprehensive implementation with extensive testing\n"
                "- max: Maximum capability with deepest reasoning\n"
                "- auto: Use the default effort level for your model\n";
        } else if (arg.empty() || arg == "current" || arg == "status") {
            if (!context.select_option) {
                result.output = effort_current_text(context);
            } else {
                const auto options = effort_options();
                const std::string chosen = context.select_option("Set effort level", options);
                if (chosen.empty()) {
                    result.output = effort_current_text(context);
                } else {
                    result.output = effort_applied(chosen);
                }
            }
        } else if (arg == "auto" || arg == "unset") {
            result.output = effort_unset();
        } else {
            bool known = false;
            for (const auto& option : effort_options()) {
                if (option.value == arg) known = true;
            }
            if (!known) {
                result.output = "Invalid argument: " + argument +
                                ". Valid options are: low, medium, high, max, auto\n";
            } else {
                result.output = effort_applied(arg);
            }
        }
    } else if (command_name == "fast") {
        const std::string arg = cc::to_lower(cc::trim(argument));
        if (arg == "on" || arg == "off") {
            result.output = fast_set_text(arg == "on", context);
        } else if (context.select_option) {
            const std::vector<CommandOption> options = {
                {"Fast mode ON", "on", "$30/$150 per Mtok"},
                {"Fast mode OFF", "off", "$5/$25 per Mtok"},
            };
            const std::string chosen =
                context.select_option("Fast mode (research preview)", options);
            if (chosen.empty()) {
                result.output = "Kept Fast mode OFF\n";
            } else {
                result.output = fast_set_text(chosen == "on", context);
            }
        } else {
            result.output = fast_set_text(false, context);
        }
    } else if (command_name == "diff") {
        if (context.show_diff) {
            // The changes are shown in the diff view, the way Claude Code
            // shows them.
            context.show_diff();
        } else {
            result.output = diff_text(context);
        }
    } else if (command_name == "ide") {
        result.output = ide_text(context, argument);
    } else if (command_name == "plugin") {
        // The reference opens the panel of the plugins; the commands that
        // take arguments are the text of the port.
        if (context.show_plugin && cc::trim(argument).empty()) {
            context.show_plugin();
        } else {
            result.output = plugin_text(context, argument);
        }
    } else if (command_name == "reload-plugins") {
        result.output = context.reload_plugins ? context.reload_plugins()
                                               : "The plugins could not be read again.\n";
    } else if (command_name == "btw") {
        if (context.show_btw) {
            context.show_btw(argument);
        } else {
            result.output = "The side question could not be asked.\n";
        }
    } else if (command_name == "plan") {
        result.output = plan_text(context, argument);
    } else if (command_name == "export") {
        result.output = export_text_command(context, argument);
    } else if (command_name == "stickers") {
        result.output = stickers_text(context);
    } else if (command_name == "desktop" || command_name == "app") {
        result.output = desktop_text(context);
    } else if (command_name == "vim") {
        result.output = vim_text(context);
    } else if (command_name == "feedback" || command_name == "bug") {
        result.output = feedback_text(context, argument);
    } else if (command_name == "install-slack-app") {
        result.output = install_slack_app_text(context);
    } else if (command_name == "install-github-app") {
        result.output = install_github_app_text(context);
    } else if (command_name == "privacy-settings") {
        result.output = privacy_settings_text();
    } else if (command_name == "rewind" || command_name == "checkpoint") {
        if (context.show_message_selector) {
            context.show_message_selector();
        } else {
            result.output = "The rewind is not available in this session.\n";
        }
    } else if (command_name == "chrome") {
        result.output = chrome_text(context);
    } else if (command_name == "usage") {
        // The reference opens the panel of the settings with the tab of the
        // limits of the plan chosen.
        if (context.show_settings) {
            context.show_settings("Usage");
        } else {
            result.output = context.usage_limits ? context.usage_limits()
                                                 : "Log in with a Claude account to see the limits of "
                                                   "the plan: /login\n";
        }
    } else if (command_name == "extra-usage") {
        // The reference sends the user to the page of the account where the
        // extra usage is managed; for a team the page of the admin is the
        // one that is opened.
        const auto tokens = auth::current_tokens();
        const bool team = tokens && (tokens->subscription_type == "team" ||
                                     tokens->subscription_type == "enterprise");
        const std::string url = team ? "https://claude.ai/admin-settings/usage"
                                     : "https://claude.ai/settings/usage";

        bool opened = false;
        if (context.open_browser) opened = context.open_browser(url);

        result.output = opened ? "Browser opened to manage extra usage. If it didn't open, visit: " +
                                      url
                                : "Please visit " + url + " to manage extra usage.";
    } else if (command_name == "mobile") {
        if (context.show_mobile) {
            // The code of the address is shown in the view, the way Claude
            // Code shows it.
            context.show_mobile();
        } else {
            result.output = mobile_text();
        }
    } else if (command_name == "skills") {
        result.output = skills_text(context);
    } else if (command_name == "statusline") {
        const std::string path = config::user_settings_path();
        bool opened = false;
        if (context.open_in_editor) {
            context.open_in_editor(path);
            opened = true;
        }
        result.output = statusline_text(context, opened);
    } else if (command_name == "keybindings") {
        const std::string path = keybindings_path();
        const bool created = !cc::file_exists(path);
        if (created) {
            std::string error;
            cc::write_file(path, keybindings_template(), &error);
        }
        bool opened = false;
        if (context.open_in_editor) {
            context.open_in_editor(path);
            opened = true;
        }
        result.output = keybindings_text(path, created, opened);
    } else if (command_name == "commit") {
        // The context of the repository is read into the prompt, the way
        // the reference reads it. The model gets the git commands only.
        const std::string body = expand_prompt_commands(commit_prompt(context), context.cwd);
        const std::string prompt = prompt_with_metadata(command_name, argument, body);
        result.output = ask_model(context, prompt, kCommitTools);
    } else if (command_name == "commit-push-pr") {
        const std::string body =
            expand_prompt_commands(commit_push_pr_prompt(context), context.cwd);
        const std::string prompt = prompt_with_metadata(command_name, argument, body);
        result.output = ask_model(context, prompt, kCommitPushPrTools);
    } else if (command_name == "upgrade") {
        // The reference sends the user to the page of the plan; the login
        // that follows is left out, the way the port leaves out the wizards
        // that need an account.
        const std::string url = "https://claude.ai/upgrade/max";
        bool opened = false;
        if (context.open_browser) opened = context.open_browser(url);
        result.output = opened ? "Browser opened to upgrade. If it didn't open, visit: " + url
                                : "Please visit " + url + " to upgrade.";
    } else if (command_name == "review") {
        const std::string body = review_prompt(cc::trim(argument));
        result.output = ask_model(context, prompt_with_metadata(command_name, argument, body));
    } else if (command_name == "security-review") {
        const std::string body = expand_prompt_commands(security_review_prompt(), context.cwd);
        result.output = ask_model(context, prompt_with_metadata(command_name, argument, body));
    } else if (command_name == "pr-comments") {
        const std::string body = pr_comments_prompt(cc::trim(argument));
        result.output = ask_model(context, prompt_with_metadata(command_name, argument, body));
    } else if (command_name == "copy") {
        result.output = copy_text(context, argument);
    } else if (command_name == "color") {
        result.output = color_text(argument);
        if (cc::starts_with(result.output, "Session color set")) {
            result.output += "The C++ port does not colour the prompt bar.\n";
        }
    } else if (command_name == "rename") {
        std::string name = cc::trim(argument);
        if (name.empty()) {
            if (context.ask_model) {
                name = generated_session_name(context.ask_model(rename_prompt(context), {}));
            }
            if (name.empty()) {
                result.output =
                    "Could not generate a name: no conversation context yet. Usage: /rename <name>\n";
            }
        }
        if (!name.empty()) {
            const std::string id = current_session_id(context);
            if (!id.empty()) {
                session::Transcript::rename(context.cwd, id, name);
            }
            result.output = rename_text(name);
            if (id.empty()) {
                result.output += "The C++ port could not find the transcript of this session.\n";
            }
        }
    } else if (command_name == "sandbox") {
        if (context.show_sandbox) {
            result.output = context.show_sandbox(argument);
        } else {
            result.output = "The /sandbox command is not available in this session.\n";
        }
    } else if (command_name == "stats") {
        // The reference opens a panel with the statistics of the sessions.
        if (context.show_stats) {
            context.show_stats();
        } else {
            result.output = stats_text(context);
        }
    } else if (command_name == "session") {
        result.output = session_text();
    } else if (command_name == "branch") {
        static const std::vector<Message> kNoMessages;
        const auto& messages = context.messages ? *context.messages : kNoMessages;
        if (messages.empty()) {
            result.output = "Failed to branch conversation: No messages to branch\n";
        } else {
            const std::string branch_id = cc::uuid_v4();
            auto branch = session::Transcript::open(context.cwd, branch_id);
            for (const auto& message : messages) {
                branch.append(message);
            }
            const std::string title = cc::trim(argument);
            const std::string base = title.empty() ? branch_base_name(context) : title;
            session::Transcript::rename(context.cwd, branch_id, base + " (Branch)");
            result.output = branch_text(title, branch_id);
        }
    } else if (command_name == "add-dir") {
        result.output = add_dir_text(context, argument);
    } else if (command_name == "terminal-setup") {
        result.output = terminal_setup_text();
    } else if (command_name == "advisor") {
        result.output = advisor_text(context, argument);
    } else if (command_name == "status") {
        // The reference opens the panel of the settings with the tab of the
        // state of the session chosen.
        if (context.show_settings) {
            context.show_settings("Status");
        } else {
            result.output = status_text(context.cwd, context.model);
        }
    } else if (command_name == "cost") {
        static const Usage kNoUsage;
        result.output = cost_text(context.usage ? *context.usage : kNoUsage);
    } else if (command_name == "goal") {
        // The goal of the session is kept by the app; the command only
        // hands the argument over to it.
        if (context.set_goal) {
            result.output = context.set_goal(argument);
        } else {
            result.output = "No goal set. Usage: `/goal <condition>`\n";
        }
    } else if (command_name == "context") {
        const size_t count = context.messages ? context.messages->size() : 0;
        result.output = context_text(count);
    } else if (command_name == "files") {
        static const std::vector<Message> kNoMessages;
        result.output = files_text(context.messages ? *context.messages : kNoMessages);
    } else if (command_name == "init") {
        const std::string path = cc::path_join(context.cwd, "CLAUDE.md");
        if (cc::file_exists(path)) {
            result.output = path + " already exists; edit it instead.\n";
        } else {
            std::string error;
            const bool written = cc::write_file(path, claude_md_stub(), &error);
            result.output = init_text(context, path, written, error);
        }
    } else if (command_name == "memory") {
        result.output = memory_text(context);
    } else if (command_name == "compact") {
        static const std::vector<Message> kNoMessages;
        const auto& messages = context.messages ? *context.messages : kNoMessages;

        // The model writes the summary of the conversation, the way the
        // reference does it: it is given the conversation and the prompt of
        // the summary. Without a model of its own, the summary is made out
        // of the text of the conversation.
        std::string summary;
        if (context.ask_model) {
            summary = context.ask_model(session::compact_prompt(argument), {});
        } else {
            summary = summarize(messages);
        }

        std::string out;
        out += "Compacted the conversation into a summary.\n";
        if (context.replace_conversation) {
            context.replace_conversation(summary);
        } else if (context.clear_conversation) {
            context.clear_conversation();
        }
        out += "\n";
        out += summary;
        result.output = out;
    } else if (command_name == "permissions") {
        result.output = permissions_text(context);
    } else if (command_name == "doctor") {
        result.output = doctor_text(context);
    } else if (command_name == "mcp") {
        // The reference opens the panel of the servers.
        if (context.show_mcp) {
            context.show_mcp();
        } else {
            result.output = mcp_text(context);
        }
    } else if (command_name == "login") {
        // The login opens the browser and waits for the user, so it is the
        // session that runs it.
        const bool logged_in = context.login ? context.login() : false;
        result.output = logged_in ? "Login successful\n" : "Login interrupted\n";
    } else if (command_name == "logout") {
        const bool gone = context.logout ? context.logout() : false;
        result.output = gone ? "Successfully logged out from your Anthropic account.\n"
                              : "The logout did not finish.\n";
    } else if (command_name == "agents") {
        result.output = agents_text(context);
    } else if (command_name == "tasks") {
        result.output = "No background tasks.\n";
    } else if (command_name == "hooks") {
        result.output = hooks_text(context);
    } else if (command_name == "release-notes") {
        result.output = release_notes_text();
    } else if (command_name == "resume") {
        // The reference opens the picker of the conversations.
        if (context.resume_picker) {
            result.output = context.resume_picker();
        } else {
            result.output = resume_text(context, cc::trim(argument));
        }
    } else {
        result.output = unavailable_text(command_name);
    }

    return result;
}

} // namespace cc::commands
