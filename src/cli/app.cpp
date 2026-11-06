#include "cli/app.hpp"

#include "auth/environment.hpp"
#include "auth/listener.hpp"
#include "auth/oauth.hpp"
#include "auth/storage.hpp"
#include "commands/registry.hpp"
#include "config/claude_md.hpp"
#include "config/cleanup.hpp"
#include "config/environment.hpp"
#include "config/global_config.hpp"
#include "config/settings.hpp"
#include "config/trust.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/version.hpp"
#include "feedback.hpp"
#include "desktop.hpp"
#include "goal.hpp"
#include "ide_diff.hpp"
#include "plugins.hpp"
#include "permissions/shell_rules.hpp"
#include "query/cancellable_client.hpp"
#include "query/errors.hpp"
#include "query/model.hpp"
#include "query/system_prompt.hpp"
#include "release_notes.hpp"
#include "sandbox/manager.hpp"
#include "session/compact.hpp"
#include "session/history.hpp"
#include "session/plan.hpp"
#include "session/stats.hpp"
#include "session/stats_cache.hpp"
#include "tools/impl/agent_tool.hpp"
#include "tools/impl/bash_tool.hpp"
#include "tools/impl/misc_tools.hpp"
#include "tools/tool_names.hpp"
#include "ui/account.hpp"
#include "ui/feed.hpp"
#include "ui/input_dialog.hpp"
#include "ui/logo.hpp"
#include "ui/markdown.hpp"
#include "ui/message_selector.hpp"
#include "ui/notify.hpp"
#include "ui/onboarding.hpp"
#include "ui/mobile_view.hpp"
#include "ui/mcp_auth.hpp"
#include "ui/mcp_panel.hpp"
#include "ui/plugin_configure.hpp"
#include "ui/plugin_panel.hpp"
#include "ui/permission_prompt.hpp"
#include "ui/progress_bar.hpp"
#include "ui/question_dialog.hpp"
#include "ui/select.hpp"
#include "ui/session_picker.hpp"
#include "ui/terminal.hpp"
#include "ui/theme_picker.hpp"
#include "ui/vim.hpp"
#include "ui/trust_dialog.hpp"
#include "ui/transcript_view.hpp"
#include "ui/tool_display.hpp"
#include "ui/diff_view.hpp"
#include "ui/external_includes_dialog.hpp"
#include "utils/fs.hpp"
#include "utils/git.hpp"
#include "utils/json_util.hpp"
#include "utils/messages.hpp"
#include "utils/path.hpp"
#include "utils/model.hpp"
#include "utils/process.hpp"
#include "utils/slash_command.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <memory>
#include <iostream>
#include <random>

namespace cc::cli {
namespace {

/// The number of times the goal of the session is checked before the
/// session gives up on it.
constexpr int kMaxGoalRounds = 50;

/// The number of days after which a plugin counts as not used recently.
///
/// The reference shows the plugins that were not used for a while in a
/// group of their own.
constexpr int kUnusedDays = 14;

/// The model Claude Code uses when none is given.
constexpr std::string_view kDefaultModel = "claude-opus-5-5";

/// The verbs the message of the end of a turn begins with.
///
/// The reference picks one of them for every turn, the way it does.
const std::vector<std::string> kTurnVerbs = {
    "Baked", "Brewed", "Churned", "Cogitated", "Cooked", "Crunched", "Sautéed", "Worked",
};

/// How long a key has to be pressed twice within to count as a double press.
///
/// Claude Code uses the same window before it exits or clears the input.
constexpr auto kDoublePressTimeout = std::chrono::milliseconds(800);

/// Reads the API key from the environment.
std::optional<std::string> api_key() {
    if (const char* key = std::getenv("ANTHROPIC_API_KEY")) {
        if (*key) return std::string(key);
    }
    return std::nullopt;
}

/// True when the value of an environment variable says that it is on.
///
/// The reference counts `1`, `true`, `yes` and `on` as on.
bool env_truthy(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) return false;

    const std::string normalized = cc::to_lower(cc::trim(value));
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

/// The text inside a tag, when the text carries the tag.
///
/// The command of the shell and its output travel in messages that are
/// wrapped in tags, the way the reference wraps them.
std::string tag_content(const std::string& text, const std::string& tag) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const size_t at = text.find(open);
    if (at == std::string::npos) return {};

    const size_t start = at + open.size();
    const size_t end = text.find(close, start);
    if (end == std::string::npos) return {};

    return text.substr(start, end - start);
}

/// True when the text carries the output of a command.
///
/// The reference leaves these messages out of the list of the prompts:
/// they are the output of a shell command and not something the user
/// wrote.
bool is_command_output(const std::string& text) {    static constexpr std::string_view kTags[] = {
        "bash-stdout",         "bash-stderr",         "local-command-stdout",
        "local-command-stderr", "task-notification",  "tick",
        "teammate-message",
    };

    for (const auto tag : kTags) {
        if (text.find("<" + std::string(tag) + ">") != std::string::npos) return true;
    }
    return false;
}

/// The tool that keeps the todo list of the session, when there is one.
const tools::TodoWriteTool* todo_list(const tools::Registry* registry) {
    if (registry == nullptr) return nullptr;
    const Tool* tool = find_tool_by_name(registry->all(), "TodoWrite");
    return dynamic_cast<const tools::TodoWriteTool*>(tool);
}

/// The code that changed, as the rewind view shows it.
std::optional<ui::RewindDiff> rewind_diff(const std::optional<session::DiffStats>& stats) {
    if (!stats) return std::nullopt;

    ui::RewindDiff diff;
    diff.files_changed = stats->files_changed;
    diff.insertions = stats->insertions;
    diff.deletions = stats->deletions;
    return diff;
}

/// The arguments of a tool call, as an object.
const boost::json::object& tool_input(const ToolUseBlock& use) {
    static const boost::json::object empty;
    if (!use.input.is_object()) return empty;
    return use.input.as_object();
}

/// The line of a group of reads and searches.
///
/// The reference counts the files and not the calls, so that a file that is
/// read twice is counted once.
std::string group_line(const std::vector<const ToolUseBlock*>& calls) {
    size_t searches = 0;
    size_t reads = 0;
    std::vector<std::string> paths;

    for (const auto* call : calls) {
        if (call->name == tools::GREP || call->name == tools::GLOB) {
            ++searches;
            continue;
        }
        ++reads;
        paths.push_back(cc::jget_string(tool_input(*call), "file_path"));
    }

    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (!paths.empty()) reads = paths.size();

    std::string text;
    if (searches > 0) {
        text += "Searched for " + std::to_string(searches) + " " +
                (searches == 1 ? "pattern" : "patterns");
    }
    if (reads > 0) {
        // The first part of the line begins with a capital and the parts that
        // follow it do not, the way the reference writes it.
        const bool first = text.empty();
        if (!first) text += ", ";
        text += std::string(first ? "Read " : "read ") + std::to_string(reads) + " " +
                (reads == 1 ? "file" : "files");
    }
    return text;
}

/// Opens a page of the web browser.
///
/// Returns false when the browser could not be opened, so that the caller
/// can tell the user where to go instead.
bool open_browser(const std::string& url) {
#if defined(__APPLE__)
    const cc::ProcessOutcome outcome = cc::run_process("open \"" + url + "\"", cc::cwd(), "", 10);
    return !outcome.failed && outcome.exit_code == 0;
#elif defined(__linux__)
    const cc::ProcessOutcome outcome = cc::run_process("xdg-open \"" + url + "\"", cc::cwd(), "", 10);
    return !outcome.failed && outcome.exit_code == 0;
#else
    (void)url;
    return false;
#endif
}

/// The notices that follow the startup banner.
///
/// The panel and the condensed banner tell the user about the tmux session
/// they run in; the panel and the compact box tell them that their bash
/// commands are sandboxed. The layout of the banner decides which of the
/// notices are shown.
std::vector<std::string> banner_notices(const ui::Theme& theme, ui::LogoLayout layout,
                                        bool sandboxing) {
    std::vector<std::string> notices;

    ui::Style dim;
    dim.foreground = theme.inactive;

    // The tmux session the CLI runs in. The reference shows the session and
    // the way to leave it in the panel and in the condensed banner, and in
    // neither of them in the compact box.
    const char* tmux_session = std::getenv("CLAUDE_CODE_TMUX_SESSION");
    if (tmux_session != nullptr && *tmux_session != '\0' && layout != ui::LogoLayout::Compact) {
        const char* prefix = std::getenv("CLAUDE_CODE_TMUX_PREFIX");
        const std::string key = prefix != nullptr ? prefix : "";
        const char* conflicts = std::getenv("CLAUDE_CODE_TMUX_PREFIX_CONFLICTS");
        const bool clash = conflicts != nullptr && *conflicts != '\0';

        notices.push_back(ui::styled("  tmux session: " + std::string(tmux_session), dim));
        if (clash) {
            notices.push_back(ui::styled("  Detach: " + key + " " + key +
                                             " d (press prefix twice - Claude uses " + key + ")",
                                         dim));
        } else {
            notices.push_back(ui::styled("  Detach: " + key + " d", dim));
        }
    }

    // The notice that the bash commands are sandboxed. The reference shows
    // it in the panel and in the compact box, and in neither of them in the
    // condensed banner.
    if (sandboxing && layout != ui::LogoLayout::Condensed) {
        const std::string notice =
            "Your bash commands will be sandboxed. Disable with /sandbox.";
        if (layout == ui::LogoLayout::Compact) {
            // The compact box leaves the notice out of the frame, after a
            // row of its own.
            notices.push_back(std::string());
            notices.push_back(ui::styled(notice, ui::Style::fg(theme.warning)));
        } else {
            // The panel holds the notice in the column of two.
            notices.push_back(ui::styled("  " + notice, ui::Style::fg(theme.warning)));
        }
    }

    return notices;
}

/// The list of the commands, and the input it was dismissed for.
///
/// The reference remembers the input when the list is dismissed, so that the
/// list does not come back until the input changes.
struct DismissedList {
    /// The input the list was dismissed for.
    std::string input;

    /// True while the list is dismissed.
    bool dismissed = false;
};

/// The list of the commands that was dismissed.
DismissedList& dismissed_list() {
    static DismissedList list;
    return list;
}

/// The prompt that the user put aside with Ctrl+S.
///
/// The reference keeps it in the state of the session and brings it back
/// after the next message is sent.
std::string& stashed_prompt() {
    static std::string text;
    return text;
}

/// Brings the prompt that was put aside back into the box.
///
/// The reference brings it back after the next message is sent, unless the
/// message is a command: the command would put the prompt out of sight, so
/// it stays aside.
void restore_stashed_prompt(ui::PromptInput* input, const std::string& text) {
    if (stashed_prompt().empty() || text.rfind('/', 0) == 0) {
        input->clear();
        return;
    }
    input->set_text(stashed_prompt());
    stashed_prompt().clear();
}

/// True when the session compacts itself.
///
/// The reference reads the environment and the setting of the user; the
/// answer is kept, because the file of the configuration is read on every
/// frame otherwise.
bool auto_compact_enabled() {
    static const bool enabled = [] {
        // The environment can turn the auto-compact off.
        if (env_truthy("DISABLE_COMPACT")) return false;
        if (env_truthy("DISABLE_AUTO_COMPACT")) return false;

        // The setting of the user can turn it off as well.
        const boost::json::object config = config::load_global_config();
        const auto it = config.find("autoCompactEnabled");
        if (it != config.end() && it->value().is_bool()) return it->value().as_bool();
        return true;
    }();
    return enabled;
}

/// Fills the list of the names of the commands the session knows.
///
/// The prompt box paints a "/command" in the colour of the suggestions when
/// the session knows the command, the way the reference paints it.
void update_known_commands(const std::vector<commands::CustomCommand>& custom,
                            const std::vector<mcp::McpCommand>& mcp) {
    std::vector<std::string> names;
    for (const auto& command : commands::all()) names.push_back(command.name);
    for (const auto& command : custom) names.push_back(command.name);
    for (const auto& command : mcp) names.push_back(command.user_facing_name);
    commands::set_known_command_names(std::move(names));
}

/// The hint about the arguments the command takes.
///
/// The reference looks the command up by the name it shows, and the aliases
/// are not part of it.
std::string argument_hint_of(const std::string& name,
                              const std::vector<commands::CustomCommand>& custom) {
    for (const auto& command : commands::all()) {
        if (command.name == name) return command.argument_hint;
    }
    for (const auto& command : custom) {
        if (command.name == name) return command.argument_hint;
    }
    return {};
}

/// The names of the arguments the command takes.
///
/// The commands of the user declare them in the `arguments` frontmatter of
/// their file, the way the reference reads them.
std::vector<std::string> argument_names_of(const std::string& name,
                                           const std::vector<commands::CustomCommand>& custom,
                                           const std::vector<mcp::McpCommand>& mcp) {
    for (const auto& command : commands::all()) {
        if (command.name == name) return command.argument_names;
    }
    for (const auto& command : custom) {
        if (command.name == name) return commands::argument_names_of(command);
    }
    for (const auto& command : mcp) {
        if (command.user_facing_name == name) return command.argument_names;
    }
    return {};
}

/// The message that the footer raises at its right.
///
/// The reference raises the message about the escape and the warning about
/// the context in the column of the notifications, one after the other.
std::string footer_notification(const std::string& message, const std::string& warning) {
    if (message.empty()) return warning;
    if (warning.empty()) return message;
    return message + " " + warning;
}

/// A command that is being typed in the middle of the input.
///
/// The reference completes the name of a command that is typed in the middle
/// of the input, the way it completes the one at the beginning.
struct MidInputCommand {
    /// The part of the name that was typed.
    std::string partial;

    /// The name of the command that completes it.
    std::string full;

    /// The place of the slash in the input.
    size_t start = 0;

    /// The place the name of the command ends, which may be past the cursor.
    size_t end = 0;
};

/// True when the character may be part of the name of a command.
///
/// The reference reads the name with the letters, the digits and the marks
/// a name may carry.
bool name_character(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) return true;
    return c == '_' || c == ':' || c == '-';
}

/// The name of the command a suggestion holds, without the alias.
std::string suggestion_name(const ui::Suggestion& suggestion) {
    std::string name = suggestion.display;
    if (!name.empty() && name.front() == '/') name = name.substr(1);
    const size_t alias = name.find(" (");
    if (alias != std::string::npos) name = name.substr(0, alias);
    return name;
}

/// The command that the input names in the middle of it, and the name that
/// completes it.
///
/// The reference looks for a slash that stands after a space, takes the name
/// that follows it, and completes it with the first command whose name
/// begins with it.
std::optional<MidInputCommand> mid_input_command(const std::string& input, size_t cursor,
                                                  const std::string& cwd,
                                                  const std::vector<commands::CustomCommand>& custom,
                                                  const std::vector<mcp::McpCommand>& mcp) {
    // A command at the beginning of the input is another thing.
    if (input.empty() || input[0] == '/') return std::nullopt;
    if (cursor > input.size()) cursor = input.size();

    // The slash stands after a space.
    size_t slash = std::string::npos;
    for (size_t i = cursor; i-- > 0;) {
        if (input[i] != '/') continue;
        if (i == 0 || std::isspace(static_cast<unsigned char>(input[i - 1]))) slash = i;
        break;
    }
    if (slash == std::string::npos) return std::nullopt;

    // The name of the command reaches from the slash to the cursor, and it
    // holds nothing but the characters a name may carry.
    const std::string partial = input.substr(slash + 1, cursor - slash - 1);
    for (const char c : partial) {
        if (!name_character(c)) return std::nullopt;
    }

    // The name of the command may reach past the cursor, to the end of the
    // word it stands in.
    size_t end = cursor;
    while (end < input.size() && name_character(input[end])) ++end;

    MidInputCommand command;
    command.partial = partial;
    command.start = slash;
    command.end = end;

    // The first command whose name begins with what was typed completes it.
    const auto items = ui::suggestions_for("/" + partial, cwd, custom, mcp);
    const std::string query = cc::to_lower(partial);
    for (const auto& item : items) {
        const std::string name = suggestion_name(item);
        if (name.size() <= partial.size()) continue;
        if (!cc::starts_with(cc::to_lower(name), query)) continue;
        command.full = name;
        return command;
    }

    return std::nullopt;
}

/// Fetches the changelog of the reference and keeps it.
///
/// The fetch happens in the background: the notes that are shown at the
/// start of the session are the ones that were kept by the last one.
///
/// The reference fetches the changelog only when the session is
/// interactive and the traffic that is not essential is allowed. The port
/// does not fetch it when it brings a changelog of its own — the notes of
/// the port are the ones that are of use to the one who uses it.
void fetch_release_notes(bool interactive) {
    if (!interactive) return;
    if (env_truthy("CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC")) return;
    if (!cc::bundled_changelog().empty()) return;

    std::thread([context = std::make_shared<boost::asio::io_context>()] {
        cc::fetch_changelog(*context);
    }).detach();
}

/// True when the user asked for the scrollback layout.
///
/// The port draws the conversation in the scrollback of the terminal, the way
/// the reference does: the text is written once and stays where it is, the
/// box of the prompt stands under it, and the user scrolls through the
/// conversation with the scrollbar of the terminal.
///
/// The full-screen layout is the other one: the window of the terminal holds
/// the conversation, and the page keys and the mouse scroll through it, the
/// way a program that draws its own screen does. It is asked for with
/// `CLAUDE_CODE_FULLSCREEN` and with `CLAUDE_CODE_NO_FLICKER`, whose name
/// says the opposite of what it does.
bool scrollback_requested() {
    if (env_truthy("CLAUDE_CODE_FULLSCREEN")) return false;
    if (const char* value = std::getenv("CLAUDE_CODE_NO_FLICKER")) {
        const std::string normalized = cc::to_lower(cc::trim(value));
        if (normalized != "0" && normalized != "false" && normalized != "no" && normalized != "off") {
            return false;
        }
    }
    return true;
}

/// The beta header of the command line.
inline constexpr std::string_view kClaudeCodeBeta = "claude-code-20250219";

/// The number of files the diff of the pane reads.
///
/// The reference leaves the details of a diff of hundreds of files alone,
/// and says that there are too many files to show.
constexpr int kMaxFilesForDiffDetails = 500;

/// The beta header of the thinking that comes in between the blocks.
inline constexpr std::string_view kInterleavedThinkingBeta = "interleaved-thinking-2025-05-14";

/// The beta headers the session sends.
///
/// The reference asks for the window of a million tokens with a header of its
/// own when the name of the model carries the tag, and the header of the
/// OAuth login is sent when the session is logged in. The header of the
/// command line is sent with every model but Haiku, and the header of the
/// thinking that comes in between the blocks is sent with the models that
/// take it.
std::optional<std::vector<std::string>> beta_headers(const std::string& model, bool oauth) {
    std::vector<std::string> betas;
    const std::string name = cc::to_lower(model);

    if (name.find("haiku") == std::string::npos) betas.emplace_back(kClaudeCodeBeta);
    if (oauth) betas.emplace_back(auth::OauthConfig{}.beta_header);
    if (query::has_1m_context(model)) betas.emplace_back(query::kContext1mBeta);
    if (name.find("opus-4") != std::string::npos || name.find("sonnet-4") != std::string::npos ||
        name.find("opus-5") != std::string::npos || name.find("sonnet-5") != std::string::npos) {
        betas.emplace_back(kInterleavedThinkingBeta);
    }
    if (betas.empty()) return std::nullopt;
    return betas;
}

/// True when the server waits for an authentication.
///
/// The reference knows that a server waits for one when the server answers
/// with a 401; the servers of the port do not say so — the SDK does not tell
/// the answer of the server — so the session works it out: a server that is
/// reached over the network and that has no token of its own waits for an
/// authentication.
bool waits_for_auth(const std::string& name, const mcp::Registry& registry) {
    const std::shared_ptr<mcp::Server> server = registry.server(name);
    if (!server) return false;
    if (server->config().transport != "http") return false;

    const auto auth = mcp::load_server_auth(mcp::server_key(name, server->config()));
    if (auth && mcp::tokens_usable(auth->tokens)) return false;

    return true;
}

/// The names of the servers an agent brings of its own.
///
/// The reference reads the `mcpServers` of the frontmatter with its reader
/// of YAML; the reader of the port knows the lists of words and not the
/// lists of maps, so the block is read here: the key, and the name of every
/// server of the list that stands under it.
///
/// A server that is only named is left out, the way the reference leaves it
/// out: the name of it is the name of a server of the configuration of the
/// user, and it stands in its own group.
std::vector<std::string> servers_of_the_frontmatter(const std::string& contents) {
    std::vector<std::string> names;

    bool in_frontmatter = false;
    bool begun = false;
    bool in_servers = false;
    size_t servers_indent = 0;

    for (const std::string& raw : cc::split_lines(contents)) {
        const std::string trimmed = cc::trim(raw);

        // The frontmatter is the block between the two "---" lines.
        if (trimmed == "---") {
            if (!begun) {
                begun = true;
                in_frontmatter = true;
                continue;
            }
            break;
        }
        if (!in_frontmatter) continue;

        const size_t indent = raw.find_first_not_of(" \t");
        if (indent == std::string::npos) continue;

        // The key of the list of the servers.
        if (!in_servers) {
            if (trimmed != "mcpServers:") continue;
            in_servers = true;
            servers_indent = indent;
            continue;
        }

        // The list ends when a line that is not more indented than the key
        // shows up.
        if (indent <= servers_indent) break;

        // Every item of the list names a server.
        if (!cc::starts_with(trimmed, "- ")) continue;
        std::string item = cc::trim(trimmed.substr(2));
        if (!item.empty() && item.front() == '{') item = cc::trim(item.substr(1));

        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;

        const std::string name = cc::trim(item.substr(0, colon));
        if (name.empty()) continue;
        names.push_back(name);
    }

    return names;
}

} // namespace

/// The servers the agents of the session use.
///
/// The reference reads the servers of an agent out of the `mcpServers` of
/// the frontmatter of its definition; the port reads the names of them out
/// of the same place, and takes what is known about them from the servers
/// of the session.
///
/// The servers that are only named are left out, the way the reference
/// leaves them out: they are the servers of the configuration of the user,
/// and they stand in their own group.
std::map<std::string, std::vector<std::string>> agent_mcp_servers(
    const std::vector<agents::Definition>& definitions) {
    std::map<std::string, std::vector<std::string>> out;

    for (const agents::Definition& definition : definitions) {
        // The agents that ship with the CLI bring no servers of their own.
        if (definition.path.empty() || definition.path == agents::kBuiltInPath) continue;

        const auto contents = cc::read_file(definition.path);
        if (!contents) continue;

        for (const std::string& name : servers_of_the_frontmatter(*contents)) {
            std::vector<std::string>& users = out[name];
            if (std::find(users.begin(), users.end(), definition.name) == users.end()) {
                users.push_back(definition.name);
            }
        }
    }

    return out;
}

/// The components the plugin of the panel brings.
///
/// The components of a plugin that is installed are read from the cache of
/// the user; the ones of a plugin that a marketplace offers are read from
/// the folder of the marketplace.
plugins::PluginComponents components_of(const ui::PluginPanelEntry& entry) {
    if (entry.installed) return plugins::plugin_components(entry.id);
    return plugins::marketplace_plugin_components(entry.marketplace, entry.name);
}

App::App(Options options) : options_(std::move(options)) {
    theme_ = ui::Theme::dark();
    session_id_ = options_.args.session_id.value_or(session_id());
}

App::~App() {
    // The threads that run in the background look at the flag, so that they
    // do not write to a session that is gone.
    alive_->store(false);
}

void App::post_ui(std::function<void()> work) {
    std::lock_guard<std::mutex> lock(ui_mutex_);
    ui_queue_.push_back(std::move(work));
}

void App::drain_ui() {
    // The queue is emptied by the thread that owns the terminal; a work that
    // asks for more work is left for the next round.
    if (ui_draining_) return;
    ui_draining_ = true;

    while (true) {
        std::function<void()> work;
        {
            std::lock_guard<std::mutex> lock(ui_mutex_);
            if (ui_queue_.empty()) break;
            work = std::move(ui_queue_.front());
            ui_queue_.pop_front();
        }
        work();
    }

    ui_draining_ = false;
}

void App::load_settings(bool include_project_servers) {
    settings_ = config::load_settings(options_.cwd, include_project_servers);

    // The pull request of the branch is asked for in the background, the
    // way the reference asks for it.
    refresh_pr_status();

    // The extension of Claude Code is installed in the IDE of the terminal,
    // in the background, the way the reference installs it.
    install_ide_extension();

    // The number of the installs of the plugins is fetched in the
    // background, so that the panel of the plugins has the numbers when it
    // is opened.
    fetch_install_counts();

    // The settings of the sandbox are read out of the settings of the user.
    sandbox_.set_settings(sandbox::parse_settings(settings_.sandbox));
    update_sandbox_description();

    if (options_.args.dangerously_skip_permissions) {
        settings_.default_mode = PermissionMode::BypassPermissions;
    } else if (options_.args.permission_mode != PermissionMode::Default) {
        settings_.default_mode = options_.args.permission_mode;
    } else if (settings_.default_mode == PermissionMode::Default && !settings_.disable_auto_mode) {
        // The session starts in the auto mode, the way Claude Code starts
        // in it: the mode is named in the footer of the prompt box.
        settings_.default_mode = PermissionMode::Auto;
    }

    if (settings_.theme) {
        theme_ = ui::theme_for(*settings_.theme);
    }

    // The model of the session is known before the banner is drawn, the way
    // the reference knows it.
    resolve_model();
}

void App::resolve_model() {
    // The model of the session: the flag of the command line, then the
    // model of the environment, then the settings of the user. The
    // environment is asked before the settings, the way the reference asks
    // it — a gateway that names its model in the environment of the user
    // is reached that way.
    const std::string environment_model = auth::environment_model();
    model_name_ = options_.args.model.value_or(
        (environment_model.empty() ? settings_.model.value_or(std::string(kDefaultModel))
                                   : environment_model));

    // The description of the `/model` command names the model of the
    // session, the way the reference names it.
    commands::set_model(model_name_);
}

void App::apply_user_env() {
    // The variables of the user are applied whole, because the user wrote
    // them; the ones of the project only when they are safe, so that a
    // folder that is not trusted cannot send the session somewhere else.
    config::apply_env(settings_.user_env);

    std::map<std::string, std::string> safe;
    for (const auto& [key, value] : settings_.env) {
        if (config::is_safe_env_var(key)) safe[key] = value;
    }
    config::apply_env(safe);
}

bool App::setup() {    // The detailed output is a setting of the global configuration file, the
    // way the reference keeps it.
    verbose_ = config::global_verbose();

    // The provider.
    // The options hold a reference to the io_context, so they are built with
    // aggregate initialisation.
    // The options hold a reference to the io_context, so their copy
    // assignment is deleted; they are built in one go instead.
    //
    // The key of the environment wins over the login of the user, the way
    // the reference gives it to the environment first.
    const std::optional<std::string> key = api_key();
    const std::optional<auth::Tokens> tokens = key ? std::nullopt : auth::current_tokens();

    // The address of the API, the token of the environment and the headers
    // the user wrote are read the way the reference reads them.
    const std::vector<std::pair<std::string, std::string>> headers = auth::custom_headers();
    const std::string environment_token = auth::auth_token();

    ai::providers::anthropic::AnthropicOptions provider_options{
        .api_key = key,
        .auth_token = tokens ? std::optional<std::string>(tokens->access_token)
                             : (environment_token.empty()
                                    ? std::nullopt
                                    : std::optional<std::string>(environment_token)),
        .base_url = auth::base_api_url(),
        .anthropic_beta = beta_headers(model_name_, tokens.has_value()),
        .io_context = io_context_,
    };
    if (!headers.empty()) {
        provider_options.extra_headers = [headers] {
            ai::http::Headers extra;
            for (const auto& [name, value] : headers) extra[name] = value;
            return extra;
        };
    }

    // The client of the API is one whose answer can be cut short: the
    // request is aborted when the user interrupts the turn.
    provider_options.http_client = std::make_shared<query::CancellableClient>(io_context_);

    // The factory returns the provider through its base class, so the
    // pointer is downcast back to the concrete type.
    provider_ = std::static_pointer_cast<ai::providers::anthropic::AnthropicProvider>(
        ai::providers::anthropic::create_anthropic(provider_options));

    model_ = provider_->language_model(query::model_for_api(model_name_));
    // The tools.
    registry_ = std::make_unique<tools::Registry>();

    // The shell of the session stays alive for the whole session, so that a
    // `cd` of a command moves the folder the session is in, the way the
    // reference does it.
    shell_session_ = std::make_unique<tools::ShellSession>(options_.cwd);
    {
        // The folders the session may work in, besides the folder of the
        // session: a command may leave the folder of the session for one
        // of them, the way the reference lets it.
        std::vector<std::string> allowed;
        for (const std::string& directory : settings_.additional_directories) {
            allowed.push_back(directory);
        }
        for (const std::string& directory : options_.args.additional_directories) {
            allowed.push_back(directory);
        }
        shell_session_->set_allowed_directories(std::move(allowed));

        // The variable of the reference that keeps the folder of the
        // session whatever the commands do.
        if (const char* maintain = std::getenv("CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR")) {
            shell_session_->set_maintain_project_working_dir(std::string(maintain) == "1" ||
                                                             std::string(maintain) == "true");
        }
        registry_->set_shell_session(shell_session_.get());
    }

    // The sandbox of the session: the commands of the `Bash` tool run
    // inside it, and the prompt of the tool tells the model what it does.
    sandbox_.set_working_directory(options_.cwd);
    {
        // The rules of the permissions of the session name the paths and
        // the hosts the sandbox holds, the way the reference merges them
        // with the rules of the `sandbox` key.
        std::vector<std::string> allow_rules;
        std::vector<std::string> deny_rules;
        for (const auto& [source, rules] : settings_.allow_rules) {
            for (const std::string& rule : rules) allow_rules.push_back(rule);
        }
        for (const auto& [source, rules] : settings_.deny_rules) {
            for (const std::string& rule : rules) deny_rules.push_back(rule);
        }
        sandbox_.set_permission_rules(std::move(allow_rules), std::move(deny_rules));
    }
    registry_->set_sandbox(&sandbox_);

    // The models that do not read the PDF files leave the line about them
    // out of the prompt of the `Read` tool.
    registry_->set_pdf_supported(!cc::contains(cc::to_lower(model_name_), "claude-3-haiku"));

    // The instructions about the commits and the pull requests are on unless
    // the settings of the session turn them off.
    registry_->set_include_git_instructions(include_git_instructions());

    // The sub-agents.
    agent_definitions_ = agents::load_definitions(options_.cwd, home_dir());

    // The plugins that are on bring agents of their own.
    for (agents::Definition& definition : plugins::plugin_agents()) {
        const auto existing = std::find_if(
            agent_definitions_.begin(), agent_definitions_.end(),
            [&](const agents::Definition& candidate) { return candidate.name == definition.name; });
        if (existing != agent_definitions_.end()) {
            *existing = std::move(definition);
        } else {
            agent_definitions_.push_back(std::move(definition));
        }
    }

    // The hooks.
    hooks_.configure(settings_.to_json().as_object());

    // The plugins that are on bring hooks of their own.
    for (hooks::Hook& hook : plugins::plugin_hooks()) hooks_.add(std::move(hook));

    // The sessions that can be resumed.
    for (const auto& session : session::Transcript::list_sessions(options_.cwd)) {
        session_ids_.push_back(session.id);
    }

    // The MCP servers.
    configure_mcp_servers();

    // The permission context.
    ToolPermissionContext permission_context;
    permission_context.mode = settings_.default_mode;
    // The mode that bypasses the questions is offered, unless the settings
    // of the user take it away.
    permission_context.is_bypass_permissions_mode_available =
        !settings_.disable_bypass_permissions_mode;
    permission_context.always_allow_rules[PermissionRuleSource::CliArg] =
        options_.args.allowed_tools;
    permission_context.always_deny_rules[PermissionRuleSource::CliArg] =
        options_.args.disallowed_tools;
    for (const auto& [source, rules] : settings_.allow_rules) {
        auto& list = permission_context.always_allow_rules[source];
        list.insert(list.end(), rules.begin(), rules.end());
    }
    for (const auto& [source, rules] : settings_.deny_rules) {
        auto& list = permission_context.always_deny_rules[source];
        list.insert(list.end(), rules.begin(), rules.end());
    }
    for (const auto& [source, rules] : settings_.ask_rules) {
        auto& list = permission_context.always_ask_rules[source];
        list.insert(list.end(), rules.begin(), rules.end());
    }
    for (const auto& directory : settings_.additional_directories) {
        AdditionalWorkingDirectory entry;
        entry.path = directory;
        permission_context.additional_working_directories[directory] = entry;
    }
    for (const auto& directory : options_.args.additional_directories) {
        AdditionalWorkingDirectory entry;
        entry.path = directory;
        permission_context.additional_working_directories[directory] = entry;
    }

    // The engine.
    query::Engine::Options engine_options;
    engine_options.model = model_;
    engine_options.model_name = model_name_;
    engine_options.cwd = options_.cwd;
    engine_options.session_id = session_id_;
    // The prompt is built again for every request, the way the reference
    // does it, so that a change of the permission mode is picked up.
    engine_options.system_prompt_provider = [this] { return build_session_prompt(); };
    engine_options.tools = registry_->all();

    // The sub-agent tool runs a sub-agent on the session's model.
    engine_options.tools.push_back(std::make_shared<tools::AgentTool>(
        agent_definitions_,
        [this](const std::string& system_prompt, const std::string& task,
               const std::vector<std::string>& allowed, const std::string& model) {
            return run_sub_agent(system_prompt, task, allowed, model);
        }));

    // The model may propose a goal for the session; the user approves it
    // with one keypress.
    engine_options.tools.push_back(std::make_shared<goal::ProposeGoalTool>());

    // The tools of the MCP servers are asked for again for every turn: the
    // servers connect in the background, so one of them may have connected
    // while the session was waiting for the user.
    engine_options.tools_provider = [this, base = engine_options.tools]() {
        Tools tools = base;
        for (auto& tool : mcp_->tools()) {
            tools.push_back(tool);
        }

        // A command that names its tools gives the model only those, the
        // way the reference does.
        if (!turn_tool_names_.empty()) {
            Tools kept;
            for (auto& tool : tools) {
                const std::string name(tool->name());
                if (std::find(turn_tool_names_.begin(), turn_tool_names_.end(), name) !=
                    turn_tool_names_.end()) {
                    kept.push_back(tool);
                }
            }
            return kept;
        }
        return tools;
    };
    engine_options.permission_context = permission_context;
    engine_options.max_turns = options_.args.max_turns;
    engine_options.max_output_tokens = options_.args.max_output_tokens;

    engine_options.hooks.on_text = [this](const std::string& delta) {
        // The work is done by the thread that owns the terminal: the
        // coroutine of the engine is resumed on a thread of the SDK's own.
        post_ui([this, delta] {
            // The thinking comes before the answer, so it is written first.
            flush_thinking();
            // The reads and searches that ran before this text are shown now,
            // so they keep their place in the transcript.
            flush_tool_group();
            answer_ += delta;
            // The spinner watches the answer: when it stops growing, the
            // spinner turns red.
            spinner_->set_mode(ui::SpinnerMode::Responding);
            spinner_->note_response(answer_.size());
            // The model has finished thinking.
            spinner_->note_thinking_end();
            render_live();
        });
    };
    // The files the session changes are copied aside, so that a rewind can
    // put them back.
    engine_options.on_file_edit = [this](const std::string& path) {
        // The copies of the files are kept only when the user asked for the
        // checkpoints of the code.
        if (!config::global_flag("fileCheckpointingEnabled", true)) return;
        file_history_.track_edit(path);
    };

    // The snapshot of the files is taken as soon as the user's message is
    // recorded, before the model changes anything.
    engine_options.hooks.on_user_message = [this](const std::string& uuid) {
        file_history_.make_snapshot(uuid);
    };

    engine_options.hooks.on_thinking = [this](const std::string& delta) {
        post_ui([this, delta] {
            thinking_ += delta;
            // The spinner says that the model is thinking.
            spinner_->set_mode(ui::SpinnerMode::Thinking);
            spinner_->note_thinking_start();
        });
    };
    engine_options.hooks.on_progress = [this](const std::string& chunk) {
        post_ui([this, chunk] {
            tool_output_ += chunk;
            // The number of the lines and of the bytes is kept, so that the
            // status row under the output can name them.
            tool_output_bytes_ += chunk.size();
            tool_output_lines_ += static_cast<size_t>(std::count(chunk.begin(), chunk.end(), '\n'));
            render_live();
        });
    };
    engine_options.hooks.on_tool_start = [this](const query::ToolCallInfo& call) {
        post_ui([this, call] {
            // The tool begins now: its output is counted from here on.
            tool_started_ms_ = cc::now_ms();
            tool_output_lines_ = 0;
            tool_output_bytes_ = 0;

            // The tool that runs is the one the spinner speaks of.
            spinner_->set_mode(ui::SpinnerMode::ToolUse);

            // The tool has begun, so it waits no longer.
            for (auto it = queued_tool_calls_.begin(); it != queued_tool_calls_.end(); ++it) {
                if (it->id == call.id) {
                    queued_tool_calls_.erase(it);
                    break;
                }
            }

            // Consecutive reads and searches are shown as one line, so their
            // own lines are held back until the group ends.
            if (is_collapsible_tool(call.name)) {
                // The answer is complete once the model asks for a tool, so
                // it is committed before the tool's line is written.
                commit_answer();
                ++group_count_;
                if (call.name == tools::GREP || call.name == tools::GLOB) {
                    ++group_searches_;
                    // The reference puts the pattern in quotes.
                    group_hint_ = "\"" + cc::jget_string(call.input, "pattern") + "\"";
                } else {
                    ++group_reads_;
                    const std::string path = cc::jget_string(call.input, "file_path");
                    group_read_paths_.insert(path);
                    // The file is named the way the user sees it.
                    group_hint_ = ui::relative_path(path, options_.cwd);
                }
                for (const auto& line : render_tool_call(call)) {
                    group_lines_.push_back(line);
                }
                tool_output_.clear();
                spinner_->note_tool_active();
                render_live();
                return;
            }

            // The reads and searches that ran before this tool are shown now.
            flush_tool_group();

            // The answer is complete once the model asks for a tool, so it is
            // committed before the tool's line is written.
            commit_answer();

            // The line of the tool stays in the dynamic region while the
            // tool runs, so that its marker can blink.
            active_tool_call_ = call;
            tool_output_.clear();
            spinner_->note_tool_active();
            render_live();
        });
    };
    engine_options.hooks.on_tool_end = [this](const query::ToolCallInfo& call,
                                               const ToolResult& result) {
        post_ui([this, call, result] {
            // The tool's line joins the transcript before its result. Its
            // marker turns green when the tool succeeded and red when it
            // failed, the way the reference turns it.
            const auto state = result.is_error ? ui::ToolCallState::Failed : ui::ToolCallState::Succeeded;
            // The tool is over, so its marker is the filled one; the blink is
            // only for the tool that is still running.
            const auto tool_lines = active_tool_call_ ? render_tool_call(*active_tool_call_, true, state)
                                                       : std::vector<std::string>{};
            active_tool_call_.reset();
            awaiting_permission_ = false;
            tool_output_.clear();
            // The model is asked again, so the request is on its way.
            spinner_->set_mode(ui::SpinnerMode::Requesting);
            render_live();

            for (const auto& line : tool_lines) {
                // Every block of the message of the assistant stands under an
                // empty line of its own, the way the reference draws it.
                ui::Terminal::instance().write_static("\n" + line + "\n");
                // The answer that follows the tool is a block of its own.
                assistant_message_started_ = false;
            }

            // The whole of the output of the tool is shown, the way the
            // reference shows the output of a command of the user.
            const auto lines = render_tool_result(call, result, /*expand=*/true);
            if (is_collapsible_tool(call.name)) {
                // The result belongs to the collapsed group.
                for (const auto& line : lines) {
                    group_lines_.push_back(line);
                }
                return;
            }

            // A tool that has no line of its own leaves the result under an
            // empty line of its own.
            if (tool_lines.empty() && !lines.empty()) {
                ui::Terminal::instance().write_static("\n");
            }

            // The reference leaves a blank line before the mode that the
            // tool switched the session into.
            if (call.name == tools::ENTER_PLAN_MODE && !result.is_error) {
                start_assistant_message();
                refresh_status_line();
            }

            // The shell of the session may have moved with the command:
            // the status line carries the folder the session is in.
            if (call.name == tools::BASH) {
                refresh_status_line();
            }

            // The whole of the result is written in one go, so that the
            // region is taken down and put back once and not once per line.
            std::string text;
            for (const auto& line : lines) {
                text += line;
                text += "\n";
            }
            if (!text.empty()) ui::Terminal::instance().write_static(text);
        });
    };
    engine_options.hooks.on_error = [this](const std::string& message) {
        post_ui([this, message] {
            ui::Terminal::instance().write_static("\n" +
                                                 ui::styled(message, ui::Style::fg(theme_.error)) +
                                                 "\n");
        });
    };
    engine_options.hooks.on_user_interaction = [this](
                                                   const std::vector<UserQuestion>& questions) {
        // The question is asked by the thread that owns the terminal.
        return run_on_ui<UserAnswers>([this, questions] {
            ui::Terminal& terminal = ui::Terminal::instance();
            if (questions.empty()) return UserAnswers{};

            awaiting_permission_ = true;
            render_live();

            // The reference shows every question of the call in one dialog:
            // the bar at the top holds them all, and the arrow keys move
            // from one to the next.
            ui::QuestionDialog dialog(theme_, questions.front());
            dialog.set_questions(questions, 0);

            const UserAnswers answers = dialog.run(terminal);

            awaiting_permission_ = false;

            // No answer means the user declined to answer.
            if (answers.empty()) return UserAnswers{};
            return answers;
        });
    };
    // A goal the model proposes is shown to the user, who approves it with
    // one keypress; the question is asked by the thread that owns the
    // terminal.
    engine_options.hooks.on_goal_proposal = [this](const std::string& condition) {
        return run_on_ui<bool>([this, condition] {
            ui::Terminal& terminal = ui::Terminal::instance();

            UserQuestion question;
            question.header = "Propose goal";
            question.text = condition;
            question.options = {{"Approve", "yes", ""}, {"Refuse", "no", ""}};

            awaiting_permission_ = true;
            render_live();

            ui::QuestionDialog dialog(theme_, question);
            const UserAnswers answers = dialog.run(terminal);

            awaiting_permission_ = false;

            // The answer of the user is the goal of the session when the
            // user approved it.
            const auto it = answers.find(condition);
            if (it == answers.end() || it->second != "yes") return false;

            goal_ = condition;
            goal_rounds_ = 0;

            terminal.write_static(std::string(ui::figures::kBlackCircle) + " " +
                                  ui::styled("Goal active: " + condition,
                                             ui::Style::fg(theme_.success)) +
                                  "\n");
            return true;
        });
    };
    engine_options.hooks.on_message = [this](const Message& message) {
        if (transcript_) transcript_->append(message);

        // The tools the model asks for are known before the first of them
        // runs; the ones that wait for their turn are shown as waiting.
        std::vector<query::ToolCallInfo> calls;
        for (const auto& block : message.content) {
            const auto* use = as_tool_use(block);
            if (!use) continue;
            query::ToolCallInfo call;
            call.id = use->id;
            call.name = use->name;
            if (use->input.is_object()) call.input = use->input.as_object();
            calls.push_back(std::move(call));
        }
        if (calls.empty()) return;
        post_ui([this, calls = std::move(calls)] {
            queued_tool_calls_ = calls;
            // The model has written the input of the tools; the spinner says
            // so until one of them runs.
            spinner_->set_mode(ui::SpinnerMode::ToolInput);
            render_live();
        });
    };
    // A message the user sent while the turn was running joins the
    // conversation at the end of the next tool round, the way the reference
    // drains its queue.
    engine_options.hooks.on_pending_messages = [this]() {
        std::vector<Message> messages;
        if (queued_prompts_.empty()) return messages;

        for (const auto& text : queued_prompts_) {
            // The model is told that the message arrived while it was
            // working, and that it has to answer it.
            Message message;
            message.role = Role::User;
            message.uuid = uuid_v4();
            message.session_id = session_id_;
            message.cwd = options_.cwd;
            message.version = version();
            message.content.push_back(
                TextBlock{"<system-reminder>\nThe user sent a new message while you were working:\n" +
                              text +
                              "\n\nIMPORTANT: After completing your current task, you MUST address the "
                              "user's message above. Do not ignore it.\n</system-reminder>",
                          false});
            messages.push_back(std::move(message));

            // The transcript shows the message the way the user wrote it.
            echo_user_message(text);
        }

        queued_prompts_.clear();
        return messages;
    };
    // The hooks run around every tool call.
    engine_options.hooks.on_before_tool =
        [this](const Tool& tool, const boost::json::object& input,
               const std::function<void(const std::string&, const std::string&)>& run_hook) {
            const auto matching = hooks_.matching(hooks::Event::PreToolUse, std::string(tool.name()));
            if (matching.empty()) return;

            boost::json::object payload;
            payload["tool_name"] = std::string(tool.name());
            payload["tool_input"] = input;
            payload["cwd"] = options_.cwd;

            const auto result = hooks::run(matching, std::string(tool.name()), payload, options_.cwd);
            if (result.blocked) {
                run_hook(result.block_reason, "A PreToolUse hook blocked the call");
            }
        };
    engine_options.hooks.on_after_tool =
        [this](const Tool& tool, const boost::json::object& input, const ToolResult& result) {
            const auto matching = hooks_.matching(hooks::Event::PostToolUse, std::string(tool.name()));
            if (matching.empty()) return;

            boost::json::object payload;
            payload["tool_name"] = std::string(tool.name());
            payload["tool_input"] = input;
            payload["tool_response"] = result.content;
            payload["cwd"] = options_.cwd;

            hooks::run(matching, std::string(tool.name()), payload, options_.cwd);
        };
    engine_options.hooks.on_permission_request =
        [this](const Tool& tool, const boost::json::object& input, const PermissionResult& request) {
            // The question is asked by the thread that owns the terminal.
            boost::json::object copy = input;
            PermissionResult asked = request;
            return run_on_ui<PermissionResult>(
                [this, &tool, copy = std::move(copy), asked = std::move(asked)]() mutable {
                    return request_permission(tool, copy, asked);
                });
        };

    engine_ = std::make_unique<query::Engine>(std::move(engine_options));

    // A resumed session continues the transcript it was given; a new one
    // starts empty. The session that is resumed was found before the
    // session was set up, so that the banner is left out.
    transcript_ = std::make_unique<session::Transcript>(session::Transcript::open(
        resumed_cwd_.empty() ? options_.cwd : resumed_cwd_, session_id_));

    if (!transcript_->messages().empty()) {
        engine_->set_messages(transcript_->messages());
    }

    input_ = std::make_unique<ui::PromptInput>(theme_, options_.cwd);
    spinner_ = std::make_unique<ui::Spinner>(theme_);

    // The users who asked for fewer animations get a spinner that does not
    // spin.
    spinner_->set_reduced_motion(settings_.prefers_reduced_motion);

    // The commands the user wrote in their own folders.
    custom_commands_ = commands::load_custom_commands(options_.cwd);

    // The keys of the prompt box are the keys of vim when the user asked for
    // them, the way the reference reads the setting.
    vim_.set_enabled(config::global_editor_mode() == "vim");
    input_->set_vim_mode(vim_.enabled(), true);

    // The plugins that are on bring commands of their own, named after the
    // plugin they come from.
    for (const plugins::PluginCommand& plugin_command : plugins::plugin_commands()) {
        commands::CustomCommand command;
        command.name = plugin_command.name;
        command.description = plugin_command.description;
        command.argument_hint = plugin_command.argument_hint;
        command.content = plugin_command.content;
        command.path = plugin_command.path;
        command.plugin_root = plugin_command.plugin_root;
        command.plugin_id = plugin_command.plugin + "@" + plugin_command.marketplace;
        custom_commands_.push_back(std::move(command));
    }
    std::sort(custom_commands_.begin(), custom_commands_.end(),
              [](const commands::CustomCommand& a, const commands::CustomCommand& b) {
                  return a.name < b.name;
              });

    // The prompt box paints the commands the session knows.
    update_known_commands(custom_commands_, mcp_commands_);

    // The keys the user has bound to something else.
    keybindings_.load();

    // The copies of the files the session changes.
    file_history_ = session::FileHistory(session_id_, options_.cwd);

    // The history of the prompts is kept by the user, in the file the
    // reference keeps it in, so that the arrow up finds the prompts of the
    // earlier sessions.
    for (const session::HistoryEntry& entry : session::read_history()) {
        if (entry.display.empty()) continue;
        history_.push_back(entry.display);
        if (history_.size() > 100) history_.erase(history_.begin());
    }

    // The reference leaves out the tip about `/btw` once the user has used
    // the command.
    btw_use_count_ = config::global_btw_use_count();

    // The changelog is fetched in the background, the way the reference
    // fetches it: the notes that are shown are the ones that were kept by
    // the last session.
    fetch_release_notes(interactive_);

    // The user can add verbs of their own to the spinner, or replace the
    // ones Claude Code ships with.
    if (!settings_.spinner_verbs.empty()) {
        std::vector<std::string> verbs = ui::Spinner::verbs();
        if (settings_.spinner_verbs_replace) {
            verbs = settings_.spinner_verbs;
        } else {
            verbs.insert(verbs.end(), settings_.spinner_verbs.begin(), settings_.spinner_verbs.end());
        }
        spinner_->set_verbs(std::move(verbs));
    }

    return true;
}

void App::print_banner() {
    logo_info_.version = version();
    logo_info_.model = model_display_name(model_name_);
    logo_info_.billing = billing_label();
    logo_info_.cwd = options_.cwd;
    logo_info_.display_name = ui::account_display_name();
    logo_info_.organization_name = ui::account_organization_name();
    logo_info_.effort = ui::effective_effort_level(
        options_.args.effort ? *options_.args.effort : std::string(),
        settings_.effort_level ? *settings_.effort_level : std::string());

    const int width = ui::Terminal::instance().width();

    // The notes of the release are new to the user when there are some
    // that the user has not seen yet. The reference draws the full logo
    // only then, or when the full logo is asked for; otherwise the banner
    // is the condensed one, which holds no feed at all.
    std::string seen_version;
    const ui::Feed whats_new = ui::whats_new_feed(&seen_version);
    banner_force_full_logo_ = env_truthy("CLAUDE_CODE_FORCE_FULL_LOGO");

    // The banner of the first run of the session is the plain one as well:
    // the frame of the panel and the feeds are left out then.
    banner_shows_release_notes_ =
        ui::banner_shows_release_notes(!whats_new.lines.empty(), first_run_, banner_force_full_logo_);

    // The feeds stand in the column of the panel of the banner: the recent
    // activity of the folder and the notes of the release.
    banner_feeds_.clear();
    banner_feeds_.push_back(ui::recent_activity_feed(options_.cwd));
    banner_feeds_.push_back(whats_new);

    const ui::LogoLayout layout =
        ui::logo_layout_for_session(banner_shows_release_notes_, banner_force_full_logo_, width);

    const ui::Logo logo(theme_, logo_info_);
    const auto lines = logo.render_session(width, banner_feeds_, banner_shows_release_notes_,
                                           banner_force_full_logo_);

    // The notices that follow the banner: the tmux session the user runs
    // in, and the sandbox the bash commands run in.
    const std::vector<std::string> notices =
        banner_notices(theme_, layout, sandbox_.is_sandboxing_enabled());

    // The version of the release the user has seen is remembered, so that
    // the notes are shown once. The notes are only remembered when they
    // have been shown, the way the reference remembers them — the first run
    // of the session is the one exception: the notes are taken as seen
    // there too, so that the panel does not come back at the next run.
    if (!seen_version.empty() && (banner_shows_release_notes_ || first_run_)) {
        config::remember_release_notes_seen(seen_version);
    }

    // The banner goes to the transcript, through the terminal, so that the
    // terminal knows where the cursor is: the region of the prompt box is
    // pinned to the bottom of the screen and would be drawn over the banner
    // if the terminal thought the cursor was still at the top.
    ui::Terminal& terminal = ui::Terminal::instance();
    for (const auto& line : lines) {
        terminal.write_static(line + "\n");
    }
    for (const auto& line : notices) {
        terminal.write_static(line + "\n");
    }
    terminal.write_static("\n");
}

bool App::run_login() {
    ui::Terminal& terminal = ui::Terminal::instance();

    // The way the user signs in.
    const std::string method = select_option(
        "Select login method:",
        {
            {"Claude account with subscription", "claude", "Pro, Max, Team, or Enterprise"},
            {"Anthropic Console account", "console", "API usage billing"},
        });
    if (method.empty()) return false;

    const bool claude_ai = method == "claude";
    const auth::OauthConfig config;

    // The values of the flow: the verifier, its challenge and the state.
    const std::string verifier = auth::random_token();
    const std::string challenge = auth::code_challenge(verifier);
    const std::string state = auth::random_token();

    // The local server that catches the code the browser brings.
    auth::AuthCodeListener listener(io_context_);
    const int port = listener.start();
    if (port == 0) {
        terminal.write_static(ui::styled("The local server of the login could not be started.",
                                         ui::Style::fg(theme_.error)) +
                              "\n");
        return false;
    }

    const std::string url =
        auth::authorization_url(config, challenge, state, port, /*manual=*/false, claude_ai);

    // The browser is opened, and the address is shown in case it is not.
    open_browser(url);
    terminal.write_static("Opening the browser to sign in...\n");
    terminal.write_static(url + "\n");

    // The wait for the code.
    while (!listener.code()) {
        io_context_.run_one_for(std::chrono::milliseconds(50));

        // The user can give the login up.
        while (const auto key = terminal.read_key(0)) {
            if (key->type != ui::Key::Type::Escape) continue;
            terminal.write_static("Login interrupted.\n");
            return false;
        }
    }

    // The state has to be the one that was sent, or the code is not ours.
    if (listener.state() && *listener.state() != state) {
        terminal.write_static(
            ui::styled("The login was not accepted: the state did not match.",
                       ui::Style::fg(theme_.error)) +
            "\n");
        return false;
    }

    // The code is exchanged for the tokens.
    auth::TokenResponse response;
    std::string error;
    if (!auth::exchange_code(io_context_, config, *listener.code(), state, verifier, port,
                              /*manual=*/false, &response, &error)) {
        terminal.write_static(ui::styled("Error: " + error, ui::Style::fg(theme_.error)) + "\n");
        return false;
    }

    // The tokens are kept, so that the next session uses them as well.
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    auth::StoredCredentials credentials;
    credentials.access_token = response.access_token;
    credentials.refresh_token = response.refresh_token;
    credentials.expires_at = now + response.expires_in * 1000;
    credentials.scope = response.scope;

    if (!auth::save_credentials(credentials, &error)) {
        terminal.write_static(ui::styled("Error: " + error, ui::Style::fg(theme_.error)) + "\n");
        return false;
    }

    // The session talks to the account from now on.
    apply_access_token(response.access_token);

    ui::Style success;
    success.foreground = theme_.success;
    terminal.write_static(ui::styled("Login successful", success) + "\n");
    return true;
}

bool App::run_logout() {
    ui::Terminal& terminal = ui::Terminal::instance();

    std::string error;
    if (!auth::clear_credentials(&error)) {
        terminal.write_static(ui::styled("Error: " + error, ui::Style::fg(theme_.error)) + "\n");
        return false;
    }

    // The session goes back to the key of the environment.
    apply_access_token(std::string());

    terminal.write_static("Logged out.\n");
    return true;
}

void App::apply_access_token(const std::string& access_token) {
    const std::optional<std::string> key = api_key();

    const std::vector<std::pair<std::string, std::string>> headers = auth::custom_headers();
    const std::string environment_token = auth::auth_token();

    ai::providers::anthropic::AnthropicOptions provider_options{
        .api_key = key,
        .auth_token = access_token.empty()
                          ? (environment_token.empty()
                                 ? std::nullopt
                                 : std::optional<std::string>(environment_token))
                          : std::optional<std::string>(access_token),
        .base_url = auth::base_api_url(),
        .anthropic_beta = beta_headers(model_name_, !access_token.empty()),
        .io_context = io_context_,
    };
    if (!headers.empty()) {
        provider_options.extra_headers = [headers] {
            ai::http::Headers extra;
            for (const auto& [name, value] : headers) extra[name] = value;
            return extra;
        };
    }

    // The client of the API is one whose answer can be cut short: the
    // request is aborted when the user interrupts the turn.
    provider_options.http_client = std::make_shared<query::CancellableClient>(io_context_);

    provider_ = std::static_pointer_cast<ai::providers::anthropic::AnthropicProvider>(
        ai::providers::anthropic::create_anthropic(provider_options));

    model_ = provider_->language_model(query::model_for_api(model_name_));
    if (engine_) engine_->set_model(model_);
}

/// The frames of the mascot's jump.
///
/// The mascot crouches, springs up with its arms raised, crouches again and
/// springs up once more.
std::vector<App::MascotFrame> App::mascot_jump() {
    std::vector<MascotFrame> frames;

    const auto hold = [&frames](ui::MascotPose pose, int offset, size_t count) {
        for (size_t i = 0; i < count; ++i) frames.push_back({pose, offset});
    };

    hold(ui::MascotPose::Default, 1, 2);
    hold(ui::MascotPose::ArmsUp, 0, 3);
    hold(ui::MascotPose::Default, 0, 1);
    hold(ui::MascotPose::Default, 1, 2);
    hold(ui::MascotPose::ArmsUp, 0, 3);
    hold(ui::MascotPose::Default, 0, 1);
    return frames;
}

/// The frames of the mascot looking around.
std::vector<App::MascotFrame> App::mascot_look() {
    std::vector<MascotFrame> frames;

    const auto hold = [&frames](ui::MascotPose pose, int offset, size_t count) {
        for (size_t i = 0; i < count; ++i) frames.push_back({pose, offset});
    };

    hold(ui::MascotPose::LookRight, 0, 5);
    hold(ui::MascotPose::LookLeft, 0, 5);
    hold(ui::MascotPose::Default, 0, 1);
    return frames;
}

void App::start_mascot_animation() {
    if (mascot_animating_) return;

    // The users who asked for less movement get no animation at all.
    if (settings_.prefers_reduced_motion) return;

    // The reference picks one of the two animations when the mascot is
    // clicked.
    static std::random_device device;
    mascot_frames_ = device() % 2 == 0 ? mascot_jump() : mascot_look();
    mascot_frame_ = 0;
    mascot_animating_ = true;
    mascot_frame_at_ = std::chrono::steady_clock::now();
    draw_mascot_frame();
}

void App::tick_mascot() {
    if (!mascot_animating_) return;

    // Every frame of the animation lasts 60 ms, the way the reference's
    // does.
    constexpr auto kFrameTime = std::chrono::milliseconds(60);

    const auto now = std::chrono::steady_clock::now();
    if (now - mascot_frame_at_ < kFrameTime) return;
    mascot_frame_at_ = now;

    ++mascot_frame_;
    if (mascot_frame_ >= mascot_frames_.size()) {
        mascot_animating_ = false;
        mascot_frames_.clear();
    }
    draw_mascot_frame();
}

void App::draw_mascot_frame() {
    ui::Terminal& terminal = ui::Terminal::instance();
    if (!terminal.fullscreen()) return;

    MascotFrame frame;
    if (mascot_animating_ && mascot_frame_ < mascot_frames_.size()) {
        frame = mascot_frames_[mascot_frame_];
    }

    // The whole banner is drawn again, so that the panel keeps its frame
    // while the mascot moves inside it.
    const ui::Logo logo(theme_, logo_info_);
    const auto lines = logo.render_session(terminal.width(), banner_feeds_, banner_shows_release_notes_,
                                           banner_force_full_logo_, frame.pose, frame.offset);
    for (size_t i = 0; i < lines.size(); ++i) {
        terminal.replace_transcript_line(i, lines[i]);
    }
    terminal.flush();
}

void App::handle_mouse_click(int x, int y) {
    ui::Terminal& terminal = ui::Terminal::instance();
    if (!terminal.fullscreen()) return;

    // The click has to land on the mascot, which stands in the banner.
    const ui::Logo logo(theme_, logo_info_);
    const ui::LogoLayout layout = ui::logo_layout_for_session(
        banner_shows_release_notes_, banner_force_full_logo_, terminal.width());
    const ui::Logo::MascotPlace place = logo.mascot_place(terminal.width(), banner_feeds_, layout);
    // The column of the click counts from one, and the mascot is nine
    // columns wide.
    if (x < place.column + 1 || x > place.column + ui::Mascot::kWidth) return;

    const int row = terminal.transcript_line_row(static_cast<size_t>(place.row));
    if (row == 0) return;
    // The mascot is three rows tall, so a click on any of them counts.
    if (y < row || y > row + 2) return;

    start_mascot_animation();
}

int App::run() {
    ui::Terminal& terminal = ui::Terminal::instance();

    // The session that is resumed is found before the banner is drawn: a
    // conversation that is resumed shows the conversation itself, and not
    // the banner of a new session.
    if (!resolve_resumed_session()) return 1;

    // The settings and the model are known before the banner is drawn, so
    // that the banner names the model of the session.
    load_settings(/*include_project_servers=*/false);

    // The environment of the user is applied before the trust question, so
    // that the session already talks to the address the user wrote; the
    // reference does the same.
    apply_user_env();

    // The model is asked for again, for the environment may name it: a
    // gateway that is written down in the settings of the user names its
    // model there, and the reference reads the model after the environment
    // is applied.
    resolve_model();

    // The environment of the global configuration file is applied before the
    // trust question as well, the way the reference applies it.
    config::apply_global_config_env();

    // The files that are older than the period of the user are pruned, the
    // way the reference prunes them. The pruning is a work of its own: it
    // runs in the background, so that the session is ready at once.
    {
        const int period = settings_.cleanup_period_days;
        std::thread([period]() { config::prune_old_files(period); }).detach();
    }

    // The session is set up after the trust question, so that the servers
    // the folder configures in its `.mcp.json` file are only used when the
    // folder is trusted.
    const bool interactive = terminal.interactive() && !options_.args.print;
    interactive_ = interactive;
    if (interactive) {
        if (!terminal.enter_raw_mode()) {
            // Without raw mode there is no way to read one key at a time, so
            // the session cannot run.
            std::cerr << "claude: the terminal could not be put into raw mode\n";
            return 1;
        }

        // The full-screen layout makes the alternate screen of the terminal
        // the session's own window; the reference switches it on the same
        // way.
        if (!scrollback_requested()) terminal.enter_fullscreen();

        // The questions of the setup are asked before the trust of the
        // folder: the reference asks them first.
        if (needs_onboarding()) run_onboarding();

        // The first time the session runs in a folder, the user is asked
        // whether they trust it. The question comes before the banner: the
        // reference asks it before the session is set up.
        if (!ask_for_trust()) {
            terminal.leave_fullscreen();
            terminal.leave_raw_mode();
            return 0;
        }

        // The files the project pulls in from outside its folder are asked
        // about next: the reference asks about them right after the folder
        // is trusted.
        ask_about_external_includes();

        // The reference opens a picker of the conversations of the folder
        // when `--resume` is given without the id of one.
        if (options_.args.resume_picker) {
            const auto picked = pick_session();
            if (picked) {
                session_id_ = picked->id;
                // The conversation of another folder is read from the
                // transcript of that folder.
                resumed_cwd_ = picked->path.empty() ? options_.cwd : picked->path;
                const auto transcript =
                    session::Transcript::open(resumed_cwd_, session_id_);
                resumed_ = !transcript.messages().empty();
            }
        }

        // The banner is for a session that starts empty: a conversation
        // that is resumed shows the conversation itself.
        if (!resumed_) banner_pending_ = true;
    }

    // The folder is trusted now, so the whole environment of the settings is
    // applied — the variables of the project included; the reference applies
    // them at the same moment.
    config::apply_env(settings_.env);
    // The servers of the folder are only used when the folder is trusted; a
    // session that has nobody to ask takes them, the way the reference does.
    load_settings(trusted_ || !interactive_);

    // The servers of the `.mcp.json` file of the folder are asked for, one
    // by one; the reference does the same.
    approve_project_mcp_servers(interactive);

    // The banner is drawn after the environment of the settings is applied,
    // so that it names the model that the environment asks for — the
    // reference reads the model after the environment is applied as well,
    // and a gateway that is written down in the settings of the user names
    // its model there.
    if (banner_pending_) {
        banner_pending_ = false;
        print_banner();
    }

    if (!setup()) {
        if (interactive) terminal.leave_raw_mode();
        return 1;
    }

    if (options_.args.print) return run_print();

    // The conversation of a session that is resumed is shown again, the
    // way the reference shows it.
    if (interactive && resumed_) render_resumed_conversation();

    return run_repl();
}

int App::run_print() {
    std::string prompt = options_.args.prompt.value_or("");
    if (prompt.empty() && !read_line(prompt)) return 0;

    // In print mode the answer goes to stdout as plain text.
    std::string answer;

    query::Engine::Options no_ui = engine_->options();
    no_ui.hooks.on_permission_request = nullptr;

    // There is nobody to ask, so a tool that is not allowed is denied rather
    // than left waiting for an answer.
    no_ui.permission_context.should_avoid_permission_prompts = true;
    no_ui.hooks.on_text = [&answer](const std::string& delta) { answer += delta; };
    no_ui.hooks.on_tool_start = [](const query::ToolCallInfo&) {};
    no_ui.hooks.on_tool_end = [](const query::ToolCallInfo&, const ToolResult&) {};
    no_ui.hooks.on_error = [](const std::string& message) { std::cerr << message << "\n"; };
    // There is nobody to ask, so a tool that wants to ask reports that it
    // cannot instead of waiting for an answer.
    no_ui.hooks.on_user_interaction = nullptr;

    query::Engine engine(std::move(no_ui));
    auto task = engine.run_turn(prompt);
    task.start();
    while (!task.done()) {
        io_context_.run_one_for(std::chrono::milliseconds(16));
    }
    try {
        task.get();
    } catch (const std::exception& error) {
        std::cerr << query::api_error_message(error) << "\n";
        return 1;
    }

    if (options_.args.output_format == "json") {
        boost::json::object out;
        out["result"] = answer;
        out["session_id"] = session_id_;
        std::cout << boost::json::serialize(out) << "\n";
    } else {
        std::cout << answer << "\n";
    }
    std::cout.flush();
    return 0;
}

bool App::read_line(std::string& out) {
    out.clear();
    std::string line;
    while (std::getline(std::cin, line)) {
        out += line;
        out += "\n";
    }
    return !out.empty();
}

bool App::read_prompt(std::string& out) {
    ui::Terminal& terminal = ui::Terminal::instance();
    input_->clear();

    while (true) {
        // The work the engine asked for is done here, by the thread that
        // owns the terminal.
        drain_ui();

        render_prompt_box(false);

        const auto key = terminal.read_key(50);
        if (!key) {
            // The terminal closed: no key will ever come, so the session
            // ends rather than wait for one.
            if (terminal.closed()) return false;

            // The mascot moves on its own while its animation runs.
            tick_mascot();
            continue;
        }

        // The message actions own the keys while they are on.
        if (message_actions_ && handle_action_key(*key)) continue;

        switch (handle_prompt_key(*key, out, /*loading=*/false)) {
        case PromptAction::Submitted:
            return true;
        case PromptAction::Exit:
            return false;
        case PromptAction::Handled:
            break;
        }
    }
}

/// The line that shows a message of the user.
///
/// The reference draws the message on the background it keeps for the user's
/// own words, with the pointer in front of it.
std::string App::user_message_line(const std::string& text) const {
    ui::Style pointer;
    pointer.foreground = theme_.subtle;
    pointer.background = theme_.user_message_background;

    ui::Style body;
    body.foreground = theme_.text;
    body.background = theme_.user_message_background;

    // The reference leaves a column of the background after the message.
    return ui::styled(std::string(ui::figures::kPointer) + " ", pointer) +
           ui::styled(text + " ", body);
}

/// Writes the prompt the user sent to the transcript.
///
/// The prompt keeps the pointer the reference puts in front of it.
void App::echo_user_message(const std::string& text) {
    // The next message of the assistant stands below an empty line.
    assistant_message_started_ = false;
    ui::Terminal::instance().write_static(user_message_line(text) + "\n");
}

bool App::resolve_resumed_session() {
    // A resumed session continues the transcript it was given; a new one
    // starts empty.
    if (!options_.args.resume_session && !options_.args.continue_session) return true;

    const auto sessions = session::Transcript::list_sessions(options_.cwd);
    if (sessions.empty()) return true;

    // The conversation that is continued is the last one; the conversation
    // that is resumed is the one the user named.
    const std::string wanted =
        options_.args.resume_session ? *options_.args.resume_session : sessions.front().id;

    if (wanted.empty()) return true;
    if (std::find(sessions.begin(), sessions.end(), wanted) == sessions.end()) {
        // The reference says that there is no conversation with that id.
        std::cerr << "No conversation found with session ID: " << wanted << "\n";
        return false;
    }

    session_id_ = wanted;

    // The session holds a conversation only when its transcript has
    // messages in it: a session that was never used is a new one.
    const auto transcript = session::Transcript::open(options_.cwd, wanted);
    resumed_ = !transcript.messages().empty();
    return true;
}

std::optional<ui::SessionPicker::Entry> App::pick_session() {
    const int64_t now = cc::now_ms();

    const auto entry_of = [now](const session::SessionInfo& info) {
        ui::SessionPicker::Entry entry;
        entry.id = info.id;
        entry.title = info.title.empty() ? "(no title)" : info.title;
        entry.when = cc::relative_time_ago(info.modified_ms, now);
        entry.branch = info.git_branch;
        entry.size = cc::format_file_size(static_cast<size_t>(info.size));
        entry.messages = info.message_count;
        entry.path = info.cwd;
        return entry;
    };

    // The conversations of the folder the session runs in.
    std::vector<ui::SessionPicker::Entry> entries;
    for (const auto& info : session::Transcript::list_sessions(options_.cwd)) {
        entries.push_back(entry_of(info));
    }

    // The conversations of every folder of the machine: Ctrl+A shows them.
    std::vector<ui::SessionPicker::Entry> all;
    for (const auto& info : session::Transcript::list_all_sessions()) {
        all.push_back(entry_of(info));
    }

    // The branch of the session: Ctrl+B shows the conversations of that
    // branch only.
    const std::string branch = git::current_branch(options_.cwd);

    ui::SessionPicker picker(theme_, std::move(entries), std::move(all), branch);

    const std::string picked = picker.run(ui::Terminal::instance());
    if (picked.empty()) return std::nullopt;

    // The conversation that was picked is found again in the list, so that
    // the folder it belongs to is known.
    for (const auto& info : session::Transcript::list_all_sessions()) {
        if (info.id != picked) continue;
        ui::SessionPicker::Entry entry = entry_of(info);
        return entry;
    }

    ui::SessionPicker::Entry entry;
    entry.id = picked;
    entry.path = options_.cwd;
    return entry;
}

std::string App::show_theme_picker() {
    // The picker shows what every theme looks like, the way the reference
    // shows it.
    const std::string current =
        settings_.theme ? std::string(ui::to_string(*settings_.theme)) : std::string("dark");

    ui::ThemePicker picker(theme_, current);
    const std::string chosen = picker.run(ui::Terminal::instance());
    if (chosen.empty()) return "";

    // The theme is put in place at once: the session draws itself again
    // with the colours of the theme that was chosen.
    ui::ThemeName name = ui::ThemeName::Dark;
    if (ui::parse_theme_name(chosen, name)) {
        settings_.theme = name;
        theme_ = ui::theme_for(name);
    }

    return chosen;
}

void App::render_resumed_conversation() {
    if (!engine_) return;

    ui::Terminal& terminal = ui::Terminal::instance();
    const int width = terminal.width();

    // The results of the tools, so that a call and its result are drawn
    // together.
    std::map<std::string, const ToolResultBlock*> results;
    std::map<std::string, boost::json::value> payloads;
    for (const auto& message : engine_->messages()) {
        for (const auto& block : message.content) {
            const auto* result = as_tool_result(block);
            if (result == nullptr) continue;
            results[result->tool_use_id] = result;
            // The tool's own output is kept with the message, so that the
            // summary of the result can be shown here.
            if (!message.tool_use_result.is_null()) {
                payloads[result->tool_use_id] = message.tool_use_result;
            }
        }
    }

    for (const auto& message : engine_->messages()) {
        for (const auto& block : message.content) {
            if (const auto* text = as_text(block)) {
                if (text->text.empty()) continue;

                if (message.role == Role::User) {
                    terminal.write_static(user_message_line(text->text) + "\n");
                } else {
                    // The model's own text, as markdown, with the marker in
                    // front of it.
                    for (const auto& line :
                         ui::render_assistant_text(text->text, theme_, width, /*with_marker=*/true)) {
                        terminal.write_static(line + "\n");
                    }
                }
                continue;
            }

            if (const auto* use = as_tool_use(block)) {
                query::ToolCallInfo call;
                call.id = use->id;
                call.name = use->name;
                if (use->input.is_object()) call.input = use->input.as_object();

                for (const auto& line : render_tool_call(call)) {
                    terminal.write_static(line + "\n");
                }

                // The result of the call, drawn the way the conversation
                // draws it.
                const auto found = results.find(use->id);
                if (found != results.end()) {
                    ToolResult result;
                    result.content = found->second->content;
                    result.is_error = found->second->is_error;
                    const auto payload = payloads.find(use->id);
                    if (payload != payloads.end()) result.tool_use_result = payload->second;
                    for (const auto& line : render_tool_result(call, result)) {
                        terminal.write_static(line + "\n");
                    }
                }
                continue;
            }
        }
    }
}

/// The names of the tools that the rules name.
///
/// A rule of the reference reads "Bash(git add:*)": the tool is "Bash" and
/// the rule of the tool is "git add:*".
std::vector<std::string> tool_names_of(const std::vector<std::string>& rules) {
    std::vector<std::string> names;
    for (const std::string& rule : rules) {
        const size_t open = rule.find('(');
        const std::string name = open == std::string::npos ? rule : rule.substr(0, open);
        if (name.empty()) continue;
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
}

/// True when the command runs as soon as its suggestion is taken.
///
/// The reference runs a command of its own straight away — the hint about
/// its arguments does not stop it — and only a command of the user waits
/// for the arguments it takes.
bool App::command_runs_right_away(const std::string& name) const {
    if (const commands::Command* command = commands::find_command(name)) {
        // A command that is a prompt waits for the arguments it names; the
        // commands of the CLI name none, so they run straight away.
        return command->type != "prompt" || command->argument_names.empty();
    }

    for (const auto& command : custom_commands_) {
        if (command.name == name) return command.argument_names.empty();
    }

    for (const auto& command : mcp_commands_) {
        if (command.user_facing_name == name) return command.argument_names.empty();
    }

    return false;
}

/// Handles a key in the prompt box.
App::PromptAction App::handle_prompt_key(const ui::Key& key, std::string& out, bool loading) {
    ui::Terminal& terminal = ui::Terminal::instance();

    // A keybinding the user wrote takes precedence over the default.
    for (const std::string_view context : {"Global", "Chat"}) {
        const std::string action = keybindings_.action_for(key, context);
        if (action.empty()) continue;
        const auto result = handle_keybinding(action, out, loading);
        if (result.has_value()) return *result;
    }

    // The vim mode takes the keys of its own first.
    if (vim_.enabled()) {
        const ui::vim::Outcome outcome = vim_.handle(key, input_->text(), input_->cursor());
        input_->set_vim_mode(true, vim_.mode() == ui::vim::Mode::Insert);
        if (outcome == ui::vim::Outcome::Handled) {
            // The text or the cursor may have changed.
            return PromptAction::Handled;
        }
        if (vim_.mode() == ui::vim::Mode::Normal) {
            // The keys of the normal mode are the keys of vim; the ones
            // that are not are left to the prompt box.
            return PromptAction::Handled;
        }
    }

    switch (key.type) {
    case ui::Key::Type::Enter: {
        // A backslash before the cursor makes the newline part of the
        // prompt instead of sending it, the way Claude Code does.
        if (input_->has_backslash_before_cursor()) {
            input_->backspace();
            input_->insert_newline();
            break;
        }

        // A message the user sends takes the view back to the end of the
        // conversation, the way Claude Code does.
        terminal.scroll_to_bottom();

        // The suggestion that is highlighted is taken when there is one.
        // A command that takes no arguments runs straight away, the way
        // Claude Code runs it.
        std::string text;
        if (!suggestions_.empty() && suggestion_index_ < suggestions_.size()) {
            const ui::Suggestion chosen = suggestions_[suggestion_index_];
            const std::string replaced = apply_suggestion(chosen);
            suggestions_.clear();
            suggestion_index_ = 0;

            bool run_now = false;
            if (chosen.kind == ui::Suggestion::Kind::Command) {
                const std::string name = trim(chosen.insert);
                run_now = command_runs_right_away(name.empty() ? std::string() : name.substr(1));
            }
            if (!run_now) break;
            text = replaced;
        } else {
            text = input_->text();
        }

        history_index_ = -1;
        if (text.empty()) break;
        if (loading) {
            // The message is queued; it joins the conversation at the end
            // of the next tool round.
            queued_prompts_.push_back(text);
            restore_stashed_prompt(input_.get(), text);
            break;
        }
        history_.push_back(text);
        if (history_.size() > 100) history_.erase(history_.begin());
        // The prompt is written to the history of the user, so that the next
        // session — and the reference — find it again.
        {
            session::HistoryEntry entry;
            entry.display = text;
            entry.timestamp = cc::now_ms();
            entry.project = options_.cwd;
            entry.session_id = session_id_;
            session::append_history(entry);
        }
        // The placeholder is shown only before the first message.
        input_->note_submit();
        // The box is replaced by the prompt, which stays in the
        // transcript.
        terminal.clear_dynamic();
        echo_user_message(text);
        // The box is emptied: the message is in the transcript now, and the
        // reference does not keep it in the box while the answer is written.
        //
        // The prompt that was put aside comes back, the way the reference
        // brings it back.
        restore_stashed_prompt(input_.get(), text);
        out = text;
        return PromptAction::Submitted;
    }
    case ui::Key::Type::CtrlC: {
        // The first press clears the input and asks for a second one
        // before the session ends.
        if (press_again(pending_ctrl_c_, "Ctrl-C")) {
            terminal.clear_dynamic();
            return PromptAction::Exit;
        }
        input_->clear();
        history_index_ = -1;
        break;
    }
    case ui::Key::Type::CtrlD: {
        // Ctrl-D only leaves the session when the input is empty.
        if (!input_->text().empty()) break;
        if (press_again(pending_ctrl_d_, "Ctrl-D")) {
            terminal.clear_dynamic();
            return PromptAction::Exit;
        }
        break;
    }
    case ui::Key::Type::Backspace:
        // The option of the keyboard of Apple takes the word away and the
        // command takes the line away, the way they do in the text fields
        // of the system.
        if (key.cmd) {
            input_->kill_to_start();
        } else if (key.alt) {
            input_->delete_word_backwards();
        } else {
            input_->backspace();
        }
        break;
    case ui::Key::Type::Delete:
        if (key.cmd) {
            input_->kill_to_end();
        } else if (key.alt) {
            input_->delete_word_forward();
        } else {
            input_->delete_forward();
        }
        break;
    case ui::Key::Type::Left:
        input_->move_left();
        break;
    case ui::Key::Type::Right:
        input_->move_right();
        break;
    case ui::Key::Type::Home:
        input_->move_home();
        break;
    case ui::Key::Type::End:
        input_->move_end();
        break;
    case ui::Key::Type::Up:
        if (!suggestions_.empty()) {
            // The list goes round: the arrow on the first entry comes back
            // from the last one, the way the reference does.
            if (suggestion_index_ == 0) {
                suggestion_index_ = suggestions_.size() - 1;
            } else {
                --suggestion_index_;
            }
            break;
        }
        if (!input_->move_up()) {
            // The cursor is on the first line, so the up arrow walks back
            // through the history, newest entry first.
            if (history_.empty()) break;
            if (history_index_ < 0) {
                // The draft is put back when the user walks past the
                // newest entry.
                history_index_ = 0;
                draft_ = input_->text();
            } else if (static_cast<size_t>(history_index_) + 1 < history_.size()) {
                ++history_index_;
            }
            input_->set_text(history_[history_.size() - 1 - static_cast<size_t>(history_index_)]);
        }
        break;
    case ui::Key::Type::Down:
        if (!suggestions_.empty()) {
            // The list goes round: the arrow on the last entry comes back
            // from the first one, the way the reference does.
            if (suggestion_index_ + 1 >= suggestions_.size()) {
                suggestion_index_ = 0;
            } else {
                ++suggestion_index_;
            }
            break;
        }
        if (!input_->move_down()) {
            // The down arrow walks forward through the history and then
            // restores the draft that was being typed.
            if (history_index_ < 0) break;
            if (history_index_ == 0) {
                history_index_ = -1;
                input_->set_text(draft_);
            } else {
                --history_index_;
                input_->set_text(history_[history_.size() - 1 - static_cast<size_t>(history_index_)]);
            }
        }
        break;
    case ui::Key::Type::Escape: {
        // The list of the commands is dismissed first, the way the reference
        // dismisses it: the input is left alone, and the list does not come
        // back until the input changes.
        if (!suggestions_.empty()) {
            suggestions_.clear();
            suggestion_index_ = 0;
            dismissed_list().input = input_->text();
            dismissed_list().dismissed = true;
            break;
        }

        // Escape clears the input, but only when it is pressed twice.
        const std::string text = input_->text();
        if (text.empty()) {
            history_index_ = -1;
            // Escape twice on an empty prompt opens the rewind view, the
            // way Claude Code does.
            if (engine_ && !engine_->messages().empty() &&
                press_again(pending_escape_, "Esc")) {
                pending_escape_ = PendingPress{};
                show_message_selector();
            }
            break;
        }
        if (press_again(pending_escape_, "Esc")) {
            // The text that was cleared is kept in the history.
            history_.push_back(text);
            if (history_.size() > 100) history_.erase(history_.begin());
            input_->clear();
            history_index_ = -1;
        }
        break;
    }
    case ui::Key::Type::CtrlU:
        // Kill the text before the cursor.
        input_->kill_to_start();
        break;
    case ui::Key::Type::CtrlK:
        // Kill the text after the cursor.
        input_->kill_to_end();
        break;
    case ui::Key::Type::CtrlW:
        input_->delete_word_backwards();
        break;
    case ui::Key::Type::CtrlA:
        input_->move_home();
        break;
    case ui::Key::Type::CtrlE:
        input_->move_end();
        break;
    case ui::Key::Type::CtrlL:
        // Redraw the screen from scratch.
        if (terminal.fullscreen()) {
            terminal.flush();
        } else {
            terminal.write_static("\x1b[2J\x1b[H");
        }
        break;
    case ui::Key::Type::PageUp:
    case ui::Key::Type::PageDown: {
        // In the full-screen layout the conversation scrolls in place; in
        // the classic one the terminal scrolls its own scrollback.
        if (!terminal.fullscreen()) break;
        const int page = std::max(1, terminal.height() - 2);
        terminal.scroll(key.type == ui::Key::Type::PageUp ? page : -page);
        break;
    }
    case ui::Key::Type::MouseScrollUp:
    case ui::Key::Type::MouseScrollDown: {
        // The wheel of the mouse scrolls the conversation, the way the
        // reference scrolls it.
        if (!terminal.fullscreen()) break;
        const int lines = 3;
        terminal.scroll(key.type == ui::Key::Type::MouseScrollUp ? lines : -lines);
        break;
    }
    case ui::Key::Type::MouseClick:
        // A click on the mascot makes it move.
        handle_mouse_click(key.mouse_x, key.mouse_y);
        break;
    case ui::Key::Type::ShiftTab:
        // Shift+Tab cycles through the permission modes.
        cycle_permission_mode();
        break;
    case ui::Key::Type::ShiftUp:
        // Shift+Up turns the message actions on, the way Claude Code
        // does.
        enter_message_actions();
        break;
    case ui::Key::Type::CtrlO:
        // Ctrl+O opens the detailed transcript.
        show_transcript();
        break;
    case ui::Key::Type::CtrlR:
        // Ctrl+R searches the prompts the user has entered.
        search_history();
        break;
    case ui::Key::Type::CtrlS:
        // Ctrl+S puts the prompt aside, the way the reference does; the
        // prompt comes back after the next message is sent.
        if (input_->text().empty()) {
            // The prompt that was put aside comes back.
            if (!stashed_prompt().empty()) {
                input_->set_text(stashed_prompt());
                stashed_prompt().clear();
            }
        } else {
            stashed_prompt() = input_->text();
            input_->clear();
        }
        break;
    case ui::Key::Type::CtrlT:
        // Ctrl+T toggles the todo list.
        toggle_todos();
        break;
    case ui::Key::Type::CtrlN:
        // Ctrl+N moves to the next entry of the list of the commands, the
        // way the reference does.
        if (!suggestions_.empty()) {
            if (suggestion_index_ + 1 >= suggestions_.size()) {
                suggestion_index_ = 0;
            } else {
                ++suggestion_index_;
            }
        }
        break;
    case ui::Key::Type::CtrlP:
        // Ctrl+P moves to the entry before the one that is chosen, the way
        // the reference does.
        if (!suggestions_.empty()) {
            if (suggestion_index_ == 0) {
                suggestion_index_ = suggestions_.size() - 1;
            } else {
                --suggestion_index_;
            }
        }
        break;
    case ui::Key::Type::Paste:
        // A pasted block lands in the input as it is; its newlines do
        // not commit the prompt. The footer names the paste while it is
        // going on, the way the reference names it.
        input_->set_pasting(true);
        paste_at_ = std::chrono::steady_clock::now();
        input_->insert(key.text);
        break;
    case ui::Key::Type::Tab:
        // The command that is typed in the middle of the input is completed
        // first, the way the reference completes it.
        if (const auto command = mid_input_command(input_->text(), input_->cursor(), options_.cwd,
                                                   custom_commands_, mcp_commands_)) {
            // The name that was typed is replaced with the whole name, and a
            // space is put after it.
            const std::string text = input_->text();
            const std::string before = text.substr(0, command->start);
            const std::string after = text.substr(command->end);
            input_->set_text(before + "/" + command->full + " " + after);
            // The cursor stands after the name of the command, the way the
            // reference leaves it.
            input_->cursor() = before.size() + 1 + command->full.size() + 1;
            suggestions_.clear();
            suggestion_index_ = 0;
            break;
        }
        // Tab takes the highlighted suggestion.
        if (!suggestions_.empty()) {
            const auto& chosen = suggestions_[suggestion_index_];
            apply_suggestion(chosen);
            suggestions_.clear();
            suggestion_index_ = 0;
        }
        break;
    case ui::Key::Type::Character:
        // Alt+P opens the model picker, the way Claude Code does.
        if (key.alt && key.text == "p") {
            bool handled = false;
            handle_command("/model", handled);
            break;
        }
        // The shortcuts menu opens on `?` while the input is empty.
        if (key.text == "?" && input_->text().empty()) {
            // The reference keeps the input focused while the menu is up,
            // so a character typed then still lands in the prompt.
            const auto closing = show_shortcuts_menu();
            // The "?" that closes the menu is swallowed, the way the
            // reference swallows it: it only opens and closes the menu.
            if (closing && !closing->text.empty() && closing->text != "?") {
                input_->insert(closing->text);
                suggestion_index_ = 0;
            }
            break;
        }
        input_->insert(key.text);
        suggestion_index_ = 0;
        break;
    default:
        break;
    }

    return PromptAction::Handled;
}

void App::cycle_permission_mode() {
    if (!engine_) return;

    ToolPermissionContext& context = engine_->context().permission_context;

    // The order Claude Code cycles through: default, accept edits, plan,
    // bypass permissions and auto mode.
    PermissionMode next = PermissionMode::Default;
    switch (context.mode) {
    case PermissionMode::Default:
        next = PermissionMode::AcceptEdits;
        break;
    case PermissionMode::AcceptEdits:
        next = PermissionMode::Plan;
        break;
    case PermissionMode::Plan:
        next = context.is_bypass_permissions_mode_available ? PermissionMode::BypassPermissions
                                                             : PermissionMode::Auto;
        break;
    case PermissionMode::BypassPermissions:
        next = PermissionMode::Auto;
        break;
    case PermissionMode::Auto:
        next = PermissionMode::Default;
        break;
    default:
        next = PermissionMode::Default;
        break;
    }

    context.mode = next;

    // The status line carries the state of the session, so it is refreshed
    // when the mode changes.
    refresh_status_line();
}

void App::flush_thinking() {
    if (thinking_.empty()) return;

    const std::string text = thinking_;
    thinking_.clear();

    ui::Terminal& terminal = ui::Terminal::instance();

    ui::Style dim;
    dim.foreground = theme_.inactive;
    dim.italic = true;

    if (verbose_) {
        // The expanded form: the thinking itself, as dim markdown. The
        // reference leaves a blank line under the title, and indents the
        // thinking by two columns.
        terminal.write_static(styled("⇔ Thinking…", dim) + "\n\n");
        const int text_width = std::max(1, terminal.width() - 2);
        std::string line = "  ";
        for (const auto& span : ui::markdown_spans(text, text_width)) {
            if (span.text == "\n") {
                terminal.write_static(line + "\n");
                line = "  ";
                continue;
            }
            ui::Style style = span.style;
            if (!style.foreground && !style.basic_foreground) style.foreground = theme_.inactive;
            line += ui::styled(span.text, style);
        }
        terminal.write_static(line + "\n");
        return;
    }

    terminal.write_static(styled("⇔ Thinking", dim) + " " +
                          styled("(ctrl+o to expand)", dim) + "\n");
}

void App::toggle_todos() {
    show_todos_ = !show_todos_;
}

std::vector<std::string> App::render_todos() const {
    if (!show_todos_ || !registry_) return {};

    const Tool* tool = find_tool_by_name(registry_->all(), "TodoWrite");
    if (!tool) return {};

    const auto* todo_tool = dynamic_cast<const tools::TodoWriteTool*>(tool);
    if (!todo_tool) return {};

    const auto& todos = todo_tool->todos();
    if (todos.empty()) return {};

    size_t done = 0;
    size_t in_progress = 0;
    size_t pending = 0;
    for (const auto& todo : todos) {
        if (todo.status == "completed") ++done;
        else if (todo.status == "in_progress") ++in_progress;
        else ++pending;
    }

    std::vector<std::string> lines;

    ui::Style dim;
    dim.foreground = theme_.inactive;

    // The reference dims the whole line and makes the counts bold within it:
    // the styles of the nested texts are merged, so the counts are dim *and*
    // bold. The line is therefore built out of segments that each carry the
    // whole style, rather than out of a styled() inside a styled().
    ui::Style count;
    count.bold = true;
    count.foreground = theme_.inactive;

    std::string summary = styled("  ", dim);
    summary += styled(std::to_string(todos.size()), count);
    summary += styled(" tasks (", dim);
    summary += styled(std::to_string(done), count);
    summary += styled(" done", dim);
    if (in_progress > 0) {
        summary += styled(", ", dim);
        summary += styled(std::to_string(in_progress), count);
        summary += styled(" in progress", dim);
    }
    summary += styled(", ", dim);
    summary += styled(std::to_string(pending), count);
    summary += styled(" open)", dim);
    lines.push_back(summary);

    for (const auto& todo : todos) {
        // The reference draws the small squares of the `figures` package.
        std::string icon = "▫";
        ui::Rgb icon_colour = theme_.text;
        if (todo.status == "completed") {
            icon = "✔";
            icon_colour = theme_.success;
        } else if (todo.status == "in_progress") {
            icon = "▪";
            icon_colour = theme_.claude;
        }

        const std::string text = todo.status == "in_progress" ? todo.active_form : todo.content;

        ui::Style item;
        item.bold = todo.status == "in_progress";
        item.strikethrough = todo.status == "completed";
        if (todo.status == "completed") item.foreground = theme_.inactive;

        lines.push_back("  " + styled(icon + " ", ui::Style::fg(icon_colour)) + styled(text, item));
    }

    return lines;
}

void App::search_history() {
    ui::Terminal& terminal = ui::Terminal::instance();

    std::string filter;
    size_t selected = 0;

    while (true) {
        // The prompts that match the filter, newest first.
        std::vector<ui::SelectOption> options;
        for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
            if (!filter.empty() && it->find(filter) == std::string::npos) continue;
            options.push_back({*it, ""});
        }
        if (selected >= options.size()) selected = options.empty() ? 0 : options.size() - 1;

        std::vector<std::string> lines;
        lines.push_back(ui::styled("  Filter history…", ui::Style::fg(theme_.inactive)));
        lines.push_back("  " + filter);

        if (options.empty()) {
            lines.push_back("  " + ui::styled("No matching prompts", ui::Style::fg(theme_.inactive)));
        } else {
            ui::Select select(theme_, options);
            select.set_selected(selected);
            for (const auto& line : select.render(terminal.width())) {
                lines.push_back(line);
            }
        }
        lines.push_back("  " + ui::styled("Enter to recall · Esc to cancel",
                                             ui::Style::fg(theme_.inactive)));

        terminal.render_dynamic(lines);

        const auto key = terminal.read_key(100);
        if (!key) {
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return;
            }
            continue;
        }

        switch (key->type) {
        case ui::Key::Type::Escape:
        case ui::Key::Type::CtrlC:
            terminal.clear_dynamic();
            return;
        case ui::Key::Type::Enter: {
            if (selected < options.size()) {
                input_->set_text(options[selected].label);
                terminal.clear_dynamic();
                return;
            }
            break;
        }
        case ui::Key::Type::Backspace:
            if (!filter.empty()) filter.pop_back();
            selected = 0;
            break;
        case ui::Key::Type::Up:
            if (selected > 0) --selected;
            break;
        case ui::Key::Type::Down:
            if (selected + 1 < options.size()) ++selected;
            break;
        case ui::Key::Type::Character:
            filter += key->text;
            selected = 0;
            break;
        default:
            break;
        }
    }
}

bool App::press_again(PendingPress& press, const std::string& key) {
    const auto now = std::chrono::steady_clock::now();
    const bool repeat = press.key == key && now - press.at <= kDoublePressTimeout;
    if (repeat) {
        // The second press has taken effect, so the prompt is cleared.
        press.key.clear();
        return true;
    }
    press.key = key;
    press.at = now;
    return false;
}

std::string App::prompt_hint() const {
    const auto now = std::chrono::steady_clock::now();
    if (pending_ctrl_c_.key == "Ctrl-C" && now - pending_ctrl_c_.at <= kDoublePressTimeout) {
        return "Press Ctrl-C again to exit";
    }
    if (pending_ctrl_d_.key == "Ctrl-D" && now - pending_ctrl_d_.at <= kDoublePressTimeout) {
        return "Press Ctrl-D again to exit";
    }
    return {};
}

/// The message the footer raises at its right.
///
/// The reference raises it as a notification of its own column: the hint of
/// the input stays where it is, and the message about the escape is shown
/// at the right of the footer for a second. The warning about the context
/// stands in the same column.
std::string App::prompt_notification() const {
    const auto now = std::chrono::steady_clock::now();
    if (pending_escape_.key == "Esc" && now - pending_escape_.at <= kDoublePressTimeout &&
        !input_->text().empty()) {
        return "Esc again to clear";
    }

    // The warning about the context is raised at the right of the footer
    // row, the way the reference raises it.
    const std::string warning = context_warning();
    if (warning.empty()) return {};

    ui::Style dim;
    dim.foreground = theme_.inactive;
    return ui::styled(warning, dim);
}

/// The hint about the arguments of the command that is being typed.
///
/// The reference shows the hint of the command after the first space, and
/// the hint it builds itself from the names of the arguments whenever the
/// input ends with a space; the hint of the command is drawn dim after the
/// text.
std::string App::argument_hint() const {
    if (!input_ || input_->mode() != ui::InputMode::Prompt) return {};

    const std::string value = input_->text();
    if (value.empty() || value.front() != '/') return {};

    // The name of the command and the arguments that follow it.
    const size_t space = value.find(' ');
    if (space == std::string::npos) return {};

    const std::string name = value.substr(1, space - 1);

    // The hint of the command itself is shown after the first space.
    if (space + 1 == value.size()) {
        const std::string hint = argument_hint_of(name, custom_commands_);
        if (!hint.empty()) return hint;
    }

    // The hint the reference builds itself names the arguments that are
    // still to be filled in.
    if (!cc::ends_with(value, " ")) return {};

    const std::vector<std::string> names =
        argument_names_of(name, custom_commands_, mcp_commands_);
    if (names.empty()) return {};

    return commands::progressive_argument_hint(names, value.substr(space + 1));
}

/// The context warning Claude Code shows in the column of the notifications.
///
/// The session is compacted automatically once the conversation grows past
/// the threshold, so the warning says how much room is left. The reference
/// says that the context is low when the auto-compact is switched off.
std::string App::context_warning() const {
    if (!engine_) return {};

    const int tokens = session::estimate_tokens(engine_->messages());

    session::CompactionOptions options;
    const bool auto_compact = auto_compact_enabled();

    // The threshold is the window of the model, less the room the summary
    // needs, when the session compacts itself.
    const int threshold =
        auto_compact ? options.context_window_tokens - 13'000 : options.context_window_tokens;
    if (threshold <= 0) return {};

    // The warning starts 20,000 tokens before the threshold.
    if (tokens < threshold - 20'000) return {};

    const double left = static_cast<double>(threshold - tokens) / threshold;
    const int percent = std::max(0, static_cast<int>(std::lround(left * 100)));

    if (auto_compact) {
        // The reference dims the warning.
        ui::Style dim;
        dim.dim = true;
        return ui::styled(std::to_string(percent) + "% until auto-compact", dim);
    }

    // The session does not compact itself: the reference says that the
    // context is low, in the colour of the error.
    ui::Style style;
    style.foreground = theme_.error;
    return ui::styled("Context low (" + std::to_string(percent) +
                          "% remaining) · Run /compact to compact & continue",
                      style);
}

/// The footer line below the prompt box.
///
/// It carries the mode indicator when the session is not in the default
/// permission mode, and otherwise leaves the prompt box to draw its own
/// hint.
std::string App::footer_text(bool loading) const {
    ui::Style dim;
    dim.foreground = theme_.inactive;

    // The mode of the session is named at the left of the footer, and the
    // rest of the footer follows it, the way the reference draws them.
    //
    // The reference leaves the mode out while the list of the suggestions is
    // showing: the footer is the list then, and nothing else — the status
    // line is left out as well.
    std::string text = suggestions_.empty() ? mode_indicator() : std::string();

    const auto join = [&text, &dim](const std::string& part) {
        if (part.empty()) return;
        if (!text.empty()) text += ui::styled(" · ", dim);
        text += part;
    };

    // The message about the exit comes before the rest of the footer.
    const std::string message = prompt_hint();
    if (!message.empty()) {
        join(ui::styled(message, dim));
        return text;
    }

    // The reference names the bash mode in the footer instead of the mode
    // pill.
    if (input_ && input_->mode() == ui::InputMode::Bash) return {};

    // The reference names the paste in the footer while it is going on.
    if (input_ && input_->pasting()) {
        join(ui::styled("Pasting text…", dim));
        return text;
    }

    // The reference adds the hint only when the hint is shown at all: the
    // box is empty, no status line is configured and the vim mode is not in
    // the insert mode.
    const bool hint_shown = input_ && input_->text().empty() &&
                             !settings_.status_line_command.has_value() &&
                             !(vim_.enabled() && vim_.mode() == ui::vim::Mode::Insert);
    if (hint_shown && input_) {
        join(ui::styled(input_->hint_line(loading), dim));
    }

    // The reference names the key that shows the todo list as soon as the
    // session has one.
    if (const auto* todos = todo_list(registry_.get())) {
        if (!todos->todos().empty()) {
            join(ui::styled(show_todos_ ? "ctrl+t to hide tasks" : "ctrl+t to show tasks", dim));
        }
    }

    // The pull request of the branch is named at the end of the footer, the
    // way the reference names it.
    {
        const std::string badge = pr_badge();
        if (!badge.empty()) join(badge);
    }

    return text;
}

/// The pull request of the branch, as the footer shows it.
///
/// The reference writes "PR" in the grey of the inactive text and the
/// number of the pull request in the colour of its review, underlined and
/// joined to the address of the pull request.
std::string App::pr_badge() const {
    if (!pr_status_.has_value()) return {};
    if (!config::global_flag("prStatusFooterEnabled", true)) return {};

    const git::PrStatus& status = *pr_status_;

    ui::Style colour;
    switch (status.review) {
    case git::PrReviewState::Approved:
        colour.foreground = theme_.success;
        break;
    case git::PrReviewState::ChangesRequested:
        colour.foreground = theme_.error;
        break;
    case git::PrReviewState::Pending:
        colour.foreground = theme_.warning;
        break;
    case git::PrReviewState::Draft:
        colour.foreground = theme_.merged;
        break;
    }
    colour.underline = true;

    ui::Style dim;
    dim.foreground = theme_.inactive;

    const std::string number = "#" + std::to_string(status.number);
    const std::string link = ui::supports_hyperlinks()
                                 ? ui::hyperlink(number, status.url)
                                 : number;

    return ui::styled("PR", dim) + " " + link;
}

/// Runs the user's status line command and keeps what it wrote.
///
/// The reference runs the command in the background and updates the line
/// whenever the conversation moves on; here it runs when the session starts,
/// when a turn ends and when the permission mode changes.
/// Compacts the conversation when the context of the session is full.
///
/// The reference compacts itself when the context window is nearly full: the
/// model writes a summary of the conversation, and the conversation becomes
/// that summary and the last few messages. The decision is made from the
/// size of the last call of the model, the way the reference reads it out of
/// the last message that carries a usage.
void App::maybe_auto_compact() {
    if (in_auto_compact_) return;
    if (!engine_ || !auto_compact_enabled()) return;

    const Usage& usage = engine_->last_usage();
    const int64_t context = usage.input_tokens + usage.cache_creation_input_tokens +
                            usage.cache_read_input_tokens;
    if (context <= 0) return;

    session::CompactionOptions options;
    const int64_t threshold = static_cast<int64_t>(static_cast<double>(options.context_window_tokens) *
                                                    options.compact_threshold);
    if (context < threshold) return;

    const std::vector<Message> messages = engine_->messages();
    if (messages.empty()) return;

    // The last few messages are kept as they are; the rest is summarised.
    const size_t keep = options.keep_recent_messages > 0
                            ? static_cast<size_t>(options.keep_recent_messages)
                            : 0;
    std::vector<Message> dropped;
    std::vector<Message> kept = messages;
    if (keep < messages.size()) {
        const auto split = static_cast<std::ptrdiff_t>(messages.size() - keep);
        dropped.assign(messages.begin(), messages.begin() + split);
        kept.assign(messages.begin() + split, messages.end());
    }

    // The model writes the summary: it is given the conversation that is
    // summarised and the prompt of the summary, the way the reference does
    // it.
    in_auto_compact_ = true;
    engine_->set_messages(dropped);
    const std::string summary = ask_model(session::compact_prompt(options.instructions));
    in_auto_compact_ = false;

    Message marker;
    marker.role = Role::User;
    marker.uuid = uuid_v4();
    marker.session_id = session_id_;
    marker.cwd = options_.cwd;
    marker.version = version();
    marker.is_meta = true;
    marker.is_compact_summary = true;
    marker.content.push_back(
        TextBlock{session::summary_message_text(summary, !kept.empty()), false});

    std::vector<Message> compacted;
    compacted.push_back(std::move(marker));
    compacted.insert(compacted.end(), kept.begin(), kept.end());
    engine_->set_messages(compacted);

    ui::Terminal& terminal = ui::Terminal::instance();
    terminal.clear_dynamic();
    terminal.write_static(std::string(ui::figures::kMessageResponse) +
                          ui::styled("Conversation compacted · the model wrote the summary",
                                     ui::Style::fg(theme_.inactive)) +
                          "\n");
}

void App::refresh_status_line() {
    if (!settings_.status_line_command) {
        status_line_text_.clear();
        return;
    }

    ui::StatusLineState state;
    state.session_id = session_id_;
    state.transcript_path = transcript_ ? transcript_->path() : std::string();
    // The folder the session is in follows the shell: a `cd` of a command
    // moves it, the way the reference moves it.
    state.cwd = shell_session_ ? shell_session_->cwd() : options_.cwd;
    state.project_dir = options_.cwd;
    state.model_id = model_name_;
    state.model_display_name = cc::model_display_name(model_name_);
    state.version = cc::version();

    if (engine_) {
        // The context of the session is the size of the last call of the
        // model, not the sum of every call: the reference reads it out of
        // the last message that carries a usage, the same way.
        const Usage& usage = engine_->last_usage();
        state.total_input_tokens =
            usage.input_tokens + usage.cache_creation_input_tokens + usage.cache_read_input_tokens;
        state.total_output_tokens = usage.output_tokens;
        state.has_usage = usage.input_tokens > 0 || usage.output_tokens > 0 ||
                          usage.cache_creation_input_tokens > 0 || usage.cache_read_input_tokens > 0;
        state.usage_input_tokens = usage.input_tokens;
        state.usage_output_tokens = usage.output_tokens;
        state.usage_cache_creation_input_tokens = usage.cache_creation_input_tokens;
        state.usage_cache_read_input_tokens = usage.cache_read_input_tokens;

        session::CompactionOptions options;
        state.context_window_size = options.context_window_tokens;
    }

    const auto input = ui::build_status_line_input(state);
    status_line_text_ = ui::run_status_line(*settings_.status_line_command, input, state.cwd);
}

/// Asks the command of GitHub about the pull request of the branch.
///
/// The reference asks the command in the background, every minute, and
/// shows the pull request in the footer of the prompt box. The thread
/// stops when the session ends.
std::optional<DetectedIde> App::ide_of_the_folder() {
    for (const auto& ide : detect_ides(options_.cwd)) {
        if (ide.matches_cwd && ide.is_running && !ide.url.empty()) return ide;
    }
    return std::nullopt;
}

/// Installs the extension of Claude Code in the IDE of the terminal.
///
/// The reference does it in the background when the session starts, so that
/// the IDE is ready for the session. The extension is only installed when it
/// is missing or older than the session, and the tool of the differences is
/// then the automatic one, the way the reference sets it.
void App::install_ide_extension() {
    // The installation may be turned off, and the environment may turn it
    // off as well.
    if (!config::global_flag("autoInstallIdeExtension", true)) return;
    if (env_truthy("CLAUDE_CODE_IDE_SKIP_AUTO_INSTALL")) return;

    const std::string type = terminal_ide_type();
    if (type.empty()) return;

    // The extension is only installed in the family of VS Code; the IDEs of
    // JetBrains fetch it from their market, the way the reference tells
    // them.
    if (!is_vscode_ide(type)) return;

    const std::string command = vscode_command(type);
    if (command.empty()) return;

    std::thread([command] {
        const auto installed = installed_extension_version(command);
        if (installed && !older_version(*installed, cc::version())) return;

        if (!install_extension(command)) return;

        // The tool of the differences is the automatic one, the way the
        // reference sets it when the extension is installed.
        if (config::global_string("diffTool").empty()) {
            config::set_global_string("diffTool", "auto");
        }
    }).detach();
}

/// Sets the goal of the session, or takes it away.
///
/// The reference keeps the goal of the session until it is met or until the
/// user takes it away; the session goes on working until the goal is met.
/// The goal is a directive for the model: the session goes on working
/// towards it at once, the way the reference does it.
std::string App::set_goal(const std::string& argument) {
    const std::string trimmed = cc::trim(argument);

    // `/goal clear` takes the goal away.
    if (trimmed == "clear") {
        if (goal_.empty()) return "No goal set\n";
        const std::string condition = goal_;
        goal_.clear();
        goal_rounds_ = 0;
        goal_reason_.clear();
        return "Goal cleared: " + condition + "\n";
    }

    // A goal that is not asked for is the goal that is there.
    if (trimmed.empty()) {
        if (goal_.empty()) return "No goal set. Usage: `/goal <condition>`\n";

        // The reference says how many turns the goal has been worked on and
        // what the last check said.
        const std::string rounds = goal_rounds_ == 0
                                      ? "not yet evaluated"
                                      : std::to_string(goal_rounds_) +
                                            (goal_rounds_ == 1 ? " turn" : " turns");
        std::string out = "Goal active: " + goal_ + " (" + rounds + ")";
        if (!goal_reason_.empty()) out += "\nLast check: " + goal_reason_;
        return out + "\n";
    }

    // The condition is looked at before it is kept.
    std::string error;
    if (!goal::check_condition(trimmed, &error)) return error + "\n";

    goal_ = trimmed;
    goal_rounds_ = 0;
    goal_reason_.clear();

    // The session goes on working towards the goal at once: the goal is a
    // directive for the model, the way the reference makes it.
    ui::Terminal::instance().write_static("Goal set: " + goal_ + "\n");
    run_turn(goal::goal_prompt(goal_));
    return {};
}

/// Checks the goal of the session and goes on working when it is not met.
///
/// The reference asks a model of its own whether the goal is met, and the
/// session goes on working when it is not.
void App::check_goal() {
    if (goal_.empty() || !engine_) return;

    // The number of rounds is counted, so that a goal that is never met
    // does not keep the session working for ever.
    if (goal_rounds_ >= kMaxGoalRounds) {
        goal_.clear();
        goal_rounds_ = 0;
        ui::Terminal::instance().write_static(
            std::string(ui::figures::kMessageResponse) +
            ui::styled("The goal was not met after " + std::to_string(kMaxGoalRounds) +
                           " rounds; the session stops working on it",
                       ui::Style::fg(theme_.inactive)) +
            "\n");
        return;
    }
    // The transcript of the session: the words of the user and the answers
    // of the assistant.
    std::string transcript;
    for (const auto& message : engine_->messages()) {
        std::string text;
        for (const auto& block : message.content) {
            const TextBlock* part = as_text(block);
            if (part == nullptr) continue;
            if (!text.empty()) text += "\n";
            text += part->text;
        }
        if (text.empty()) continue;
        transcript += (message.role == Role::User ? "User: " : "Assistant: ") + text + "\n\n";
    }

    // The question is asked in an engine of its own: the tools of the
    // session are taken away, and only the tool of the evaluator is left.
    query::Engine::Options options = engine_->options();
    options.tools.clear();
    options.max_turns = 1;
    options.hooks = query::EngineHooks{};
    options.tools_provider = nullptr;
    options.system_prompt_provider = nullptr;
    options.system_prompt = goal::evaluator_prompt(goal_, transcript);

    auto evaluator = std::make_shared<goal::EvaluatorTool>();
    options.tools.push_back(evaluator);

    query::Engine side(std::move(options));
    side.set_messages({});

    auto task = side.run_turn(
        "Check whether the goal of the session has been met. Answer with the goal_result tool.");
    task.start();

    while (!task.done()) {
        io_context_.run_one_for(std::chrono::milliseconds(16));
        render_live();
    }

    try {
        task.get();
    } catch (const std::exception&) {
        return;
    }

    // The model did not answer: the session waits for the user.
    if (!evaluator->answered()) return;

    // The round is counted, so that `/goal` can say how many turns the
    // goal has been worked on.
    ++goal_rounds_;

    ui::Style bold;
    bold.bold = true;
    ui::Style dim;
    dim.foreground = theme_.inactive;

    ui::Terminal& terminal = ui::Terminal::instance();

    // The goal is met: the session stops working on it.
    if (evaluator->met()) {
        const std::string condition = goal_;
        goal_.clear();
        goal_rounds_ = 0;
        goal_reason_.clear();
        terminal.write_static(std::string(ui::figures::kBlackCircle) + " " +
                              ui::styled("Goal met: " + condition, bold) + "\n");
        if (!evaluator->reason().empty()) {
            terminal.write_static(std::string(ui::figures::kMessageResponse) +
                                  ui::styled(evaluator->reason(), dim) + "\n");
        }
        return;
    }

    // The reason of the check is kept, so that `/goal` can tell it.
    goal_reason_ = evaluator->reason();

    // The goal is not met: the session goes on working.
    const std::string checkin = "Goal check-in: " + goal_ + "\n" +
                                "The goal is not met yet" +
                                (evaluator->reason().empty() ? std::string()
                                                            : ": " + evaluator->reason()) +
                                "\nKeep working towards the goal.";

    terminal.write_static(std::string(ui::figures::kBlackCircle) + " " +
                          ui::styled("Goal check-in: " + goal_, bold) + "\n");
    if (!evaluator->reason().empty()) {
        terminal.write_static(std::string(ui::figures::kMessageResponse) +
                              ui::styled(evaluator->reason(), dim) + "\n");
    }

    // The session goes on working towards the goal.
    run_turn(checkin);
}

/// True when the session talks to an IDE.
///
/// The reference offers the settings of the IDE only when the IDE is
/// connected, the way it offers them.
bool App::connected_to_ide() const {
    if (!mcp_) return false;

    for (const auto& server : mcp_->status()) {
        if (server.name == "ide" && server.connected) return true;
    }

    return false;
}

/// The IDE the session talks to.
///
/// The IDE is the one that holds the folder of the session, and it is only
/// there when it is connected.
std::optional<DetectedIde> App::connected_ide() const {
    if (!connected_to_ide()) return std::nullopt;
    return ide_;
}

/// Opens the change of a file in the IDE.
///
/// The reference opens the change in the IDE and waits for the answer of
/// the user there: the file may be saved, which accepts the change, or the
/// change may be rejected there, which refuses it. The answer is given back
/// through the flag, which the dialog of the permission reads.
void App::open_diff_in_ide(const std::string& path, const boost::json::object& input,
                           const std::shared_ptr<std::atomic<int>>& answer, const std::string& tab) {
    if (!mcp_) return;

    // The server of the IDE is kept alive by the thread, so that the call
    // may be made even when the session ends in the meantime.
    const std::shared_ptr<mcp::Server> server = mcp_->server("ide");
    if (!server) return;

    const auto contents = contents_after_edit(path, input);
    if (!contents) return;

    boost::json::object args;
    args["old_file_path"] = boost::json::value(path);
    args["new_file_path"] = boost::json::value(path);
    args["new_file_contents"] = boost::json::value(*contents);
    args["tab_name"] = boost::json::value(tab);

    std::thread([server, args, answer] {
        std::string error;
        const boost::json::value result = server->call_tool("openDiff", args, &error);
        if (!error.empty()) return;

        const std::string text = result.is_string() ? std::string(result.as_string().c_str())
                                                    : json_tool_result_text(result);
        const IdeDiffAnswer ide = ide_answer(text);
        if (ide == IdeDiffAnswer::Accepted) {
            answer->store(1);
        } else if (ide == IdeDiffAnswer::Refused) {
            answer->store(2);
        }
    }).detach();
}

/// Closes the tab of the IDE that shows the change.
///
/// The reference closes the tab when the user answered in the terminal, so
/// that the IDE does not keep a tab of a change that was settled.
void App::close_diff_in_ide(const std::string& tab) {
    if (!mcp_ || tab.empty()) return;

    const std::shared_ptr<mcp::Server> server = mcp_->server("ide");
    if (!server) return;

    boost::json::object args;
    args["tab_name"] = boost::json::value(tab);

    std::thread([server, args] {
        std::string error;
        server->call_tool("closeTab", args, &error);
    }).detach();
}

void App::refresh_pr_status() {
    auto alive = alive_;
    std::thread([this, alive] {
        while (alive->load()) {
            const auto status = git::pr_status(options_.cwd);
            if (!alive->load()) return;

            post_ui([this, status] {
                pr_status_ = status;
                render_prompt_box(false);
            });

            // The reference asks the command of GitHub every minute.
            for (int second = 0; second < 60 && alive->load(); ++second) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        }
    }).detach();
}

/// Shows the whole conversation in a full-screen view.
///
/// The view shows the conversation the way the conversation itself is
/// shown — the same messages, the same tool calls and the same results —
/// with the raw tool output left out until the user asks for everything
/// with Ctrl+E, the way the reference does.
void App::show_transcript() {
    if (!engine_) return;

    ui::Terminal& terminal = ui::Terminal::instance();
    const int width = terminal.width();

    const auto& messages = engine_->messages();

    // The results of the tools, so that a call and its result can be drawn
    // together.
    std::map<std::string, const ToolResultBlock*> results;
    std::map<std::string, boost::json::value> payloads;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            const auto* result = as_tool_result(block);
            if (result == nullptr) continue;
            results[result->tool_use_id] = result;
            // The tool's own output is kept with the message, so that the
            // summary of the result can be shown here.
            if (!message.tool_use_result.is_null()) {
                payloads[result->tool_use_id] = message.tool_use_result;
            }
        }
    }

    std::vector<ui::TranscriptEntry> entries;

    for (const auto& message : messages) {
        // The blocks of one message belong together, the way the reference
        // keeps them: the empty line that separates two entries falls
        // between the messages, not between the blocks of one of them.
        ui::TranscriptEntry entry;

        for (const auto& block : message.content) {
            if (const auto* text = as_text(block)) {
                if (text->text.empty()) continue;

                if (message.role == Role::User) {
                    // The command of the shell and its output are shown the
                    // way the screen shows them, the way the reference shows
                    // them in the transcript.
                    const std::string command = tag_content(text->text, "bash-input");
                    if (!command.empty()) {
                        entry.lines.push_back(
                            ui::styled("! ", ui::Style::fg(theme_.bash_border)) + command);
                        continue;
                    }

                    if (text->text.find("<bash-stdout>") != std::string::npos) {
                        // The whole output of the command: the transcript is
                        // the view the "ctrl+o to expand" of the output
                        // refers to.
                        for (const auto& line :
                             ui::render_tool_result_lines(tag_content(text->text, "bash-stdout"),
                                                          theme_, width, false,
                                                          /*truncate=*/false)) {
                            entry.lines.push_back(line);
                        }
                        const std::string error_text = tag_content(text->text, "bash-stderr");
                        if (!error_text.empty()) {
                            for (const auto& line :
                                 ui::render_tool_result_lines(error_text, theme_, width, true,
                                                              /*truncate=*/false)) {
                                entry.lines.push_back(line);
                            }
                        }
                        continue;
                    }

                    // The user's own message.
                    entry.lines.push_back(user_message_line(text->text));
                } else {
                    // The model's own text, as markdown, with the marker in
                    // front of it.
                    for (const auto& line :
                         ui::render_assistant_text(text->text, theme_, width,
                                                   /*with_marker=*/true)) {
                        entry.lines.push_back(line);
                    }
                }
                continue;
            }

            if (const auto* thinking = as_thinking(block)) {
                if (thinking->thinking.empty()) continue;

                ui::Style dim;
                dim.foreground = theme_.inactive;
                dim.italic = true;
                entry.lines.push_back(ui::styled("⇔ Thinking…", dim));
                entry.lines.push_back("");
                for (const auto& line :
                     ui::render_markdown(thinking->thinking, theme_, std::max(1, width - 2), 0,
                                         /*dim=*/true)) {
                    entry.lines.push_back("  " + line);
                }
                // The thinking is shown in full here: the transcript is the
                // view the "ctrl+o to expand" of the thinking refers to.
                continue;
            }

            if (const auto* use = as_tool_use(block)) {
                query::ToolCallInfo call;
                call.id = use->id;
                call.name = use->name;
                if (use->input.is_object()) call.input = use->input.as_object();

                for (const auto& line : render_tool_call(call)) {
                    entry.lines.push_back(line);
                }

                // The result of the call, drawn the way the conversation
                // draws it.
                const auto found = results.find(use->id);
                if (found != results.end()) {
                    ToolResult result;
                    result.content = found->second->content;
                    result.is_error = found->second->is_error;
                    const auto payload = payloads.find(use->id);
                    if (payload != payloads.end()) result.tool_use_result = payload->second;
                    for (const auto& line : render_tool_result(call, result, /*expand=*/true)) {
                        entry.lines.push_back(line);
                    }
                }
                continue;
            }
        }

        // The message is one entry of the transcript, unless it holds
        // nothing but the results of the tools.
        if (!entry.lines.empty()) entries.push_back(std::move(entry));
    }

    ui::TranscriptView view(theme_, std::move(entries));
    view.run(terminal, transcript_show_all_);

    terminal.clear_dynamic();
    render_prompt_box(false);
}

namespace {

/// One turn of the conversation that changed files.
///
/// The reference gathers the changes of the turns out of the results of the
/// tools that wrote or changed a file, so that the pane of the diff can
/// show the changes of a turn on their own.
struct TurnDiff {
    /// The number of the turn.
    int index = 0;
    /// The beginning of the prompt of the user.
    std::string preview;
    /// The files that changed in the turn.
    std::vector<git::ChangedFile> files;
};

/// The number of characters of the prompt of the user the preview holds.
///
/// The reference cuts the preview of the prompt at thirty characters.
constexpr size_t kPreviewLength = 30;

/// The beginning of the prompt of the user, the way the reference shows it.
std::string prompt_preview(const Message& message) {
    std::string text = message.text();
    if (text.size() <= kPreviewLength) return text;
    return text.substr(0, kPreviewLength - 1) + "…";
}

/// The changes of the turns of the conversation.
///
/// The turns come back with the most recent one first, the way the
/// reference shows them.
std::vector<TurnDiff> turn_diffs(const std::vector<Message>& messages) {
    // The input of every call of a tool, so that the path of a file that
    // was changed can be found.
    std::map<std::string, boost::json::object> inputs;
    for (const auto& message : messages) {
        for (const auto& block : message.content) {
            const auto* use = as_tool_use(block);
            if (use == nullptr) continue;
            if (use->input.is_object()) inputs[use->id] = use->input.as_object();
        }
    }

    std::vector<TurnDiff> turns;
    TurnDiff current;
    int index = 0;

    // The turn is kept when the next one starts.
    const auto keep = [&turns](TurnDiff& turn) {
        if (turn.files.empty()) return;
        // The reference sorts the files of a turn by their path.
        std::sort(turn.files.begin(), turn.files.end(),
                  [](const git::ChangedFile& a, const git::ChangedFile& b) { return a.path < b.path; });
        turns.push_back(std::move(turn));
    };

    for (const auto& message : messages) {
        // A prompt of the user starts a turn.
        if (message.role == Role::User && !message.has_tool_result() && !message.is_meta) {
            keep(current);
            current = TurnDiff{};
            current.index = ++index;
            current.preview = prompt_preview(message);
            continue;
        }

        // The output of the tool is kept with the message, so that the
        // changes of the turn can be read out of it.
        if (!message.has_tool_result() || !message.tool_use_result.is_object()) continue;
        const auto& payload = message.tool_use_result.as_object();

        // The text of the file before the change and the text of it after.
        std::string path = jget_string(payload, "filePath");
        std::string before;
        std::string after;
        if (!path.empty() && payload.contains("originalFile")) {
            // A file that was written over.
            before = jget_string(payload, "originalFile");
            after = jget_string(payload, "content");
        } else if (!path.empty() && jget_string(payload, "type") == "create") {
            // A file that was created.
            after = jget_string(payload, "content");
        } else if (payload.contains("oldContent") && payload.contains("newContent")) {
            // A file that was edited.
            before = jget_string(payload, "oldContent");
            after = jget_string(payload, "newContent");
        } else {
            continue;
        }

        // The path of a file that was edited is not in the result of the
        // tool; it is in the input of the call.
        if (path.empty()) {
            const auto* result = message.first_tool_result();
            if (result == nullptr) continue;
            const auto found = inputs.find(result->tool_use_id);
            if (found == inputs.end()) continue;
            path = jget_string(found->second, "file_path");
            if (path.empty()) continue;
        }

        const std::string diff = ui::unified_diff(before, after);
        if (diff.empty()) continue;

        // The counts of the lines that were added and removed.
        int added = 0;
        int removed = 0;
        for (const auto& line : cc::split_lines(diff)) {
            if (line.empty()) continue;
            if (line[0] == '+') ++added;
            else if (line[0] == '-') ++removed;
        }

        // The file joins the turn; a file that is changed twice keeps the
        // hunks of both changes.
        git::ChangedFile* file = nullptr;
        for (auto& candidate : current.files) {
            if (candidate.path == path) {
                file = &candidate;
                break;
            }
        }
        if (file == nullptr) {
            git::ChangedFile created;
            created.path = path;
            current.files.push_back(std::move(created));
            file = &current.files.back();
        }
        file->diff += diff;
        file->added += added;
        file->removed += removed;
    }

    if (!current.files.empty()) keep(current);

    // The most recent turn comes first.
    std::reverse(turns.begin(), turns.end());
    return turns;
}

} // namespace

/// Shows the changes in the working tree, the way `/diff` does.
void App::show_diff() {
    ui::Terminal& terminal = ui::Terminal::instance();

    // The sources of the pane: the working tree first, and the changes of
    // the turns of the conversation after it.
    std::vector<ui::DiffSource> sources;
    ui::DiffSource working_tree;
    working_tree.name = "Current";
    working_tree.title = "Uncommitted changes";
    working_tree.subtitle = " (git diff HEAD)";
    sources.push_back(std::move(working_tree));

    if (engine_) {
        for (const auto& turn : turn_diffs(engine_->messages())) {
            ui::DiffSource source;
            source.name = "T" + std::to_string(turn.index);
            source.title = "Turn " + std::to_string(turn.index);
            if (!turn.preview.empty()) source.subtitle = " \"" + turn.preview + "\"";
            source.files = turn.files;
            sources.push_back(std::move(source));
        }
    }

    // The reference opens the pane before the diff is fetched, so that it
    // can say that it is loading while git is working.
    ui::DiffView view(theme_, std::move(sources));
    view.set_modal(terminal.fullscreen());
    view.set_state(ui::DiffView::State::Loading);
    terminal.render_dynamic(view.render(terminal.width(), terminal.height()));

    // The totals are asked for first: a diff of hundreds of files is not
    // read at all, the way the reference does not read it.
    const int count = git::changed_file_count(options_.cwd);
    if (count > kMaxFilesForDiffDetails) {
        view.set_state(ui::DiffView::State::TooMany);
    } else {
        view.set_files(git::changed_files(options_.cwd));
        view.set_state(ui::DiffView::State::Ready);
    }

    view.run(terminal);

    terminal.clear_dynamic();
    render_prompt_box(false);
}

/// Shows the code of the address of the mobile application, the way
/// `/mobile` does.
void App::show_mobile() {
    ui::Terminal& terminal = ui::Terminal::instance();

    ui::MobileView view(theme_);
    view.set_modal(terminal.fullscreen());
    view.run(terminal);

    terminal.clear_dynamic();
    render_prompt_box(false);
}

/// The bar that shows how much of a limit is used.
/// The text the `/usage` command prints.
///
/// The limits of the plan of the user are asked for from the API of the
/// account.
/// The text the side question is wrapped in.
///
/// The question is asked in a thread of its own, so the model is told that
/// it has no tools and that the conversation carries on without it.
std::string side_question_prompt(const std::string& question) {
    return "<system-reminder>This is a side question from the user. You must answer this question "
           "directly in a single response.\n"
           "\n"
           "IMPORTANT CONTEXT:\n"
           "- You are a separate, lightweight agent spawned to answer this one question\n"
           "- The main agent is NOT interrupted - it continues working independently in the "
           "background\n"
           "- You share the conversation context but are a completely separate instance\n"
           "- Do NOT reference being interrupted or what you were \"previously doing\" - that "
           "framing is incorrect\n"
           "\n"
           "CRITICAL CONSTRAINTS:\n"
           "- You have NO tools available - you cannot read files, run commands, search, or take "
           "any actions\n"
           "- This is a one-off response - there will be no follow-up turns\n"
           "- You can ONLY provide information based on what you already know from the "
           "conversation context\n"
           "- NEVER say things like \"Let me try...\", \"I'll now...\", \"Let me check...\", or "
           "promise to take any action\n"
           "- If you don't know the answer, say so - do not offer to look it up or investigate\n"
           "\n"
           "Simply answer the question with the information you have.</system-reminder>\n"
           "\n" +
           question;
}

void App::show_btw(const std::string& question) {
    ui::Terminal& terminal = ui::Terminal::instance();

    const std::string trimmed = cc::trim(question);
    if (trimmed.empty()) {
        terminal.write_static_line("Usage: /btw <your question>");
        render_prompt_box(false);
        return;
    }

    // The reference counts the uses, so that it leaves out the tip about the
    // command once the user has used it.
    config::count_btw_use();
    ++btw_use_count_;

    // The question has been echoed to the transcript already; the box is
    // empty while the answer is being written.
    input_->clear();

    // The question is asked in a thread of its own: the tools are taken
    // away, and the turn ends as soon as the model answers.
    query::Engine::Options options = engine_->options();
    options.tools.clear();
    options.max_turns = 1;
    options.hooks = query::EngineHooks{};

    std::string answer;
    std::string failure;
    options.hooks.on_text = [&answer](const std::string& chunk) { answer += chunk; };
    options.hooks.on_error = [&failure](const std::string& message) { failure = message; };

    query::Engine side(std::move(options));
    side.set_messages(engine_->messages());

    const int width = terminal.width();

    // The panel holds the question and the answer as it grows.
    const auto panel = [&]() {
        ui::Style warning = ui::Style::fg(theme_.warning);
        warning.bold = true;

        std::vector<std::string> lines;
        lines.push_back(ui::styled("/btw", warning) + " " + trimmed);
        lines.push_back("");
        if (!failure.empty()) {
            lines.push_back("  " + ui::styled(failure, ui::Style::fg(theme_.error)));
        } else if (!answer.empty()) {
            // The answer of the side question is indented by two columns,
            // the way the panel of the reference indents it.
            for (const auto& line : ui::render_assistant_text(answer, theme_, width - 2)) {
                lines.push_back(line);
            }
        } else {
            lines.push_back("  " + ui::styled("Answering…", ui::Style::fg(theme_.warning)));
        }
        return lines;
    };

    auto task = side.run_turn(side_question_prompt(trimmed));
    task.start();

    while (!task.done()) {
        io_context_.run_one_for(std::chrono::milliseconds(16));
        render_prompt_box(/*loading=*/true, panel());
    }

    try {
        task.get();
    } catch (const std::exception& error) {
        failure = error.what();
    }

    // The panel waits for the user to dismiss it.
    std::vector<std::string> lines = panel();
    lines.push_back(
        ui::styled("↑/↓ to scroll · Space, Enter, or Escape to dismiss",
                   ui::Style::fg(theme_.inactive)));
    render_prompt_box(/*loading=*/false, lines);

    while (true) {
        const auto key = terminal.read_key(100);
        if (!key) {
            if (terminal.closed()) break;
            continue;
        }
        if (key->type == ui::Key::Type::Escape || key->type == ui::Key::Type::Enter ||
            key->type == ui::Key::Type::Character || key->type == ui::Key::Type::CtrlC ||
            key->type == ui::Key::Type::CtrlD) {
            break;
        }
    }

    // The question and the answer join the transcript, so that they stay
    // where the user can read them.
    terminal.clear_dynamic();
    for (const auto& line : panel()) terminal.write_static_line(line);
    terminal.write_static_line(
        ui::styled("↑/↓ to scroll · Space, Enter, or Escape to dismiss",
                   ui::Style::fg(theme_.inactive)));

    render_prompt_box(false);
}

std::string App::build_session_prompt() {
    query::SystemPromptOptions prompt_options;
    prompt_options.cwd = options_.cwd;
    prompt_options.model = model_name_;
    prompt_options.version = version();
    prompt_options.permission_mode = settings_.default_mode;
    prompt_options.tool_names = registry_->names();
    // The files that the project pulls in from outside its folder are only
    // read when the user allowed them, the way the reference reads them.
    prompt_options.memory_files =
        config::load_memory_files(options_.cwd, config::external_includes_approved(options_.cwd));
    prompt_options.output_style = settings_.output_style;

    // The mode of the session is the one in force, which the user may have
    // changed with Shift+Tab.
    if (engine_) prompt_options.permission_mode = engine_->context().permission_context.mode;

    // The plan mode names the file the plan is written to.
    if (prompt_options.permission_mode == PermissionMode::Plan) {
        prompt_options.plan_file_path = session::plan_path(session_id_);
    }

    if (options_.args.system_prompt) {
        prompt_options.custom_prompt = *options_.args.system_prompt;
    } else if (options_.args.system_prompt_file) {
        if (const auto contents = read_file(*options_.args.system_prompt_file)) {
            prompt_options.custom_prompt = *contents;
        }
    }
    if (options_.args.append_system_prompt) {
        prompt_options.append_prompt = *options_.args.append_system_prompt;
    } else if (options_.args.append_system_prompt_file) {
        if (const auto contents = read_file(*options_.args.append_system_prompt_file)) {
            prompt_options.append_prompt = *contents;
        }
    }

    return query::build_system_prompt(prompt_options);
}

/// One plugin of the screen of the plugins.
struct PluginEntry {
    /// The name shown in the list, with the marketplace it comes from.
    std::string label;
    /// The explanation shown underneath the name.
    std::string description;
    /// The name of the plugin.
    std::string name;
    /// The marketplace the plugin comes from.
    std::string marketplace;
    /// True when the plugin is in the cache of the user.
    bool installed = false;
    /// True when the plugin is on.
    bool enabled = false;
};

/// The plugins the screen of the plugins offers.
///
/// The plugins that are installed come first, the ones the marketplaces
/// offer after them.
std::vector<PluginEntry> plugin_entries() {
    std::vector<PluginEntry> entries;

    const std::vector<plugins::Plugin> installed = plugins::installed_plugins();
    for (const plugins::Plugin& plugin : installed) {
        PluginEntry entry;
        entry.label = plugin.name + "@" + plugin.marketplace;
        entry.description = plugin.description;
        if (!plugin.version.empty()) entry.description += " (" + plugin.version + ")";
        if (entry.description.empty()) entry.description = plugin.enabled ? "On" : "Off";
        entry.name = plugin.name;
        entry.marketplace = plugin.marketplace;
        entry.installed = true;
        entry.enabled = plugin.enabled;
        entries.push_back(std::move(entry));
    }

    for (const plugins::Marketplace& marketplace : plugins::known_marketplaces()) {
        for (const plugins::MarketplacePlugin& offered :
             plugins::marketplace_plugins(marketplace.name)) {
            const bool known = std::any_of(
                installed.begin(), installed.end(), [&](const plugins::Plugin& plugin) {
                    return plugin.name == offered.name && plugin.marketplace == marketplace.name;
                });
            if (known) continue;

            PluginEntry entry;
            entry.label = offered.name + "@" + marketplace.name;
            entry.description = offered.description;
            if (entry.description.empty()) entry.description = "Not installed";
            entry.name = offered.name;
            entry.marketplace = marketplace.name;
            entries.push_back(std::move(entry));
        }
    }

    return entries;
}

/// The rules of the permissions, in one line.
///
/// The names of the rules are joined with commas, the way the panel of the
/// settings shows them.
std::string rules_summary(const PermissionRulesBySource& rules) {
    std::vector<std::string> names;
    for (const auto& [source, entries] : rules) {
        for (const auto& entry : entries) names.push_back(entry);
    }
    if (names.empty()) return "(none)";
    return cc::join(names, ", ");
}

/// The files the settings are read from.
std::vector<std::pair<std::string, bool>> settings_files(const std::string& cwd) {
    std::vector<std::pair<std::string, bool>> files;
    files.emplace_back(config::user_settings_path(), cc::file_exists(config::user_settings_path()));
    files.emplace_back(config::project_settings_path(cwd),
                       cc::file_exists(config::project_settings_path(cwd)));
    files.emplace_back(config::local_settings_path(cwd),
                       cc::file_exists(config::local_settings_path(cwd)));
    return files;
}

/// The name the reference gives a source of the settings.
///
/// The reference names the sources of the settings the way it names them
/// in the panel of the state of the session.
std::string settings_source_name(const std::string& path, const std::string& cwd) {
    if (path == config::user_settings_path()) return "User settings";
    if (path == config::project_settings_path(cwd)) return "Shared project settings";
    if (path == config::local_settings_path(cwd)) return "Project local settings";
    return cc::tilde_path(path);
}

/// The name of the theme, the way the reference writes it.
///
/// The reference writes the words of the theme in the list of the settings:
/// "Dark mode", "Light mode" and "Auto (match terminal)".
std::string theme_label(const std::optional<ui::ThemeName>& theme) {
    if (!theme.has_value()) return "Auto (match terminal)";
    switch (*theme) {
    case ui::ThemeName::Dark:
        return "Dark mode";
    case ui::ThemeName::Light:
        return "Light mode";
    case ui::ThemeName::DarkDaltonized:
        return "Dark mode (colorblind-friendly)";
    case ui::ThemeName::LightDaltonized:
        return "Light mode (colorblind-friendly)";
    case ui::ThemeName::DarkAnsi:
        return "Dark mode (ANSI colors only)";
    case ui::ThemeName::LightAnsi:
        return "Light mode (ANSI colors only)";
    }
    return "Dark mode";
}

/// The name of the channel of the notifications, the way the reference
/// writes it in the list of the settings.
std::string notification_label(const std::string& channel) {
    if (channel == "auto") return "Auto";
    if (channel == "iterm2") return "iTerm2 (OSC 9)";
    if (channel == "terminal_bell") return "Terminal Bell (\a)";
    if (channel == "iterm2_with_bell") return "iTerm2 w/ Bell";
    if (channel == "kitty") return "Kitty (OSC 99)";
    if (channel == "ghostty") return "Ghostty (OSC 777)";
    if (channel == "notifications_disabled") return "Disabled";
    return channel;
}

/// Writes one setting of the user, keeping the rest of the file as it was.
void write_user_setting(const std::string& key, const boost::json::value& value) {
    const std::string path = config::user_settings_path();

    boost::json::object object;
    if (const auto existing = read_file(path)) {
        const boost::json::value parsed = json_parse(*existing);
        if (parsed.is_object()) object = parsed.as_object();
    }

    object[key] = value;
    config::write_settings(path, boost::json::value(std::move(object)));
}

/// Writes the settings of the folder, keeping the rest of the file as it was.
void write_local_setting(const std::string& cwd, const boost::json::object& values) {
    const std::string path = config::local_settings_path(cwd);

    boost::json::object object;
    if (const auto existing = read_file(path)) {
        const boost::json::value parsed = json_parse(*existing);
        if (parsed.is_object()) object = parsed.as_object();
    }

    for (const auto& [key, value] : values) object[key] = value;
    config::write_settings(path, boost::json::value(std::move(object)));
}

/// Asks the user about the servers of the `.mcp.json` file of the folder.
///
/// A repository must not be able to start a program of its own on its own:
/// every server of the folder is asked for, unless the settings of the user
/// say that it may run. The answer is remembered in the settings of the
/// folder, the way the reference remembers it.
void App::approve_project_mcp_servers(bool interactive) {
    std::vector<std::string> allowed;
    std::vector<std::string> refused;

    for (const auto& [name, server] : settings_.mcp_servers) {
        if (server.scope != "project") {
            allowed.push_back(name);
            continue;
        }

        const config::McpServerApproval approval =
            config::mcpjson_server_approval(settings_, name);
        if (approval == config::McpServerApproval::Approved) {
            allowed.push_back(name);
            continue;
        }
        if (approval == config::McpServerApproval::Rejected) {
            refused.push_back(name);
            continue;
        }

        // There is nobody to ask: the server is left alone, the way the
        // reference leaves it alone.
        if (!interactive) {
            refused.push_back(name);
            continue;
        }

        // The reference opens a dialog of its own for the server, in the
        // colour of the warning.
        ui::McpServerDialog dialog(theme_, name);
        const std::string answer = dialog.run(ui::Terminal::instance());
        if (answer == "yes-all") {
            allowed.push_back(name);
            settings_.enable_all_project_mcp_servers = true;
            continue;
        }
        if (answer == "yes") {
            allowed.push_back(name);
            settings_.enabled_mcpjson_servers.push_back(name);
            continue;
        }
        refused.push_back(name);
        settings_.disabled_mcpjson_servers.push_back(name);
    }

    // The servers that were not allowed are taken out of the list.
    for (const auto& name : refused) settings_.mcp_servers.erase(name);

    // The answer is remembered in the settings of the folder.
    if (!settings_.enabled_mcpjson_servers.empty() ||
        !settings_.disabled_mcpjson_servers.empty() || settings_.enable_all_project_mcp_servers) {
        boost::json::object local;
        if (settings_.enable_all_project_mcp_servers) local["enableAllProjectMcpServers"] = true;
        if (!settings_.enabled_mcpjson_servers.empty()) {
            boost::json::array names;
            for (const auto& name : settings_.enabled_mcpjson_servers) names.emplace_back(name);
            local["enabledMcpjsonServers"] = std::move(names);
        }
        if (!settings_.disabled_mcpjson_servers.empty()) {
            boost::json::array names;
            for (const auto& name : settings_.disabled_mcpjson_servers) names.emplace_back(name);
            local["disabledMcpjsonServers"] = std::move(names);
        }
        write_local_setting(options_.cwd, local);
    }
}

/// The permission modes the panel of the settings offers.
///
/// The mode that bypasses the questions is left out when the settings of the
/// user take it away.
std::vector<commands::CommandOption> App::permission_mode_options() {
    std::vector<commands::CommandOption> options = {
        {"Default", "default", "Ask before a tool that may change something"},
        {"Accept edits", "acceptEdits", "Edit the files without asking"},
        {"Plan", "plan", "Plan the work first"},
    };
    if (!settings_.disable_auto_mode) {
        options.push_back({"Auto mode", "auto", "Let the session decide when to ask"});
    }
    if (!settings_.disable_bypass_permissions_mode) {
        options.push_back(
            {"Bypass permissions", "bypassPermissions", "Run every tool without asking"});
    }
    return options;
}

/// Asks the API how much of the plan of the user is used.
///
/// The failure of the answer is reported through `error`, the way the
/// reference reports it: the tab of the usage shows it and offers to ask
/// again.
auth::Utilization fetch_usage_of_plan(boost::asio::io_context& io_context, std::string* error) {
    auth::Utilization usage;
    if (error) error->clear();

    const auto tokens = auth::current_tokens();
    if (!tokens || tokens->access_token.empty()) {
        // The reference does not ask the API when the user is not a
        // subscriber: the tab of the usage says that the command is only
        // there for the subscriptions.
        return usage;
    }

    std::string message;
    auth::OauthConfig oauth_config;
    oauth_config.base_api_url = auth::base_api_url();
    auth::fetch_utilization(io_context, oauth_config, tokens->access_token, &usage, &message);
    if (error) *error = message;
    return usage;
}

/// The usage of the machine, the way the panel of the statistics shows it.
///
/// The limits of the plan are only known for a subscription; the usage of
/// the machine is always there.
ui::UsageTab::LocalUsage App::local_usage() const {
    ui::UsageTab::LocalUsage usage;

    // The tokens of the session that is running.
    if (engine_) usage.session_tokens = session::estimate_tokens(engine_->messages());

    // The tokens of every session of the machine, and the ones of today.
    const session::Stats stats = session::collect_stats(options_.cwd);
    usage.total_tokens = stats.total_tokens();

    const std::string today = session::today_date_string();
    const auto found = stats.daily_model_tokens.find(today);
    if (found != stats.daily_model_tokens.end()) {
        for (const auto& [model, tokens] : found->second) usage.today_tokens += tokens;
    }

    return usage;
}

/// The panel of the settings.
///
/// The reference opens the panel with one of its tabs chosen — `/config`
/// opens the tab of the configuration, `/status` the one of the state of the
/// session and `/usage` the one of the limits of the plan. Picking a setting
/// opens the picker of that setting.
void App::show_settings(const std::string& tab) {
    // The tab of the configuration keeps the mode it was in when the panel
    // is opened again after a setting was changed.
    bool config_searching = true;

    while (true) {
        std::vector<std::unique_ptr<ui::SettingsTab>> tabs;
        tabs.push_back(std::make_unique<ui::StatusTab>(theme_, status_properties()));
        auto config_tab = std::make_unique<ui::ConfigTab>(theme_, config_settings());
        config_tab->set_searching(config_searching);
        tabs.push_back(std::move(config_tab));
        // The limits and the extra usage come from one answer of the API,
        // which is asked for again every time the tab of the usage is shown,
        // the way the reference asks for it.
        auto usage_tab = std::make_unique<ui::UsageTab>(theme_);
        usage_tab->set_reload([this](ui::UsageTab& tab) {
            std::string error;
            const auth::Utilization usage = fetch_usage_of_plan(io_context_, &error);
            tab.set_data(usage_limits(usage), usage_extra(usage), error);
            // The usage of the machine is shown when the plan of the user
            // does not say how much of it is used.
            tab.set_local_usage(local_usage());
        });
        tabs.push_back(std::move(usage_tab));

        // The tab that is opened is the one the command named.
        size_t selected = 1;
        for (size_t i = 0; i < tabs.size(); ++i) {
            if (tabs[i]->title() == tab) selected = i;
        }

        ui::SettingsTabs panel(theme_, std::move(tabs), selected);
        // The reference paints the pane of the settings with the permission
        // blue.
        panel.set_pane_color(theme_.permission);
        const std::string picked = panel.run(ui::Terminal::instance());
        if (picked.empty()) return;

        // The setting was changed from the list of the settings, so the tab
        // of the configuration keeps its list when it is opened again.
        config_searching = false;

        // The setting that was picked is changed.
        if (picked == "model") {
            commands::CommandContext context;
            context.model = model_name_;
            context.settings = &settings_;
            const auto options = commands::model_options(context);
            const std::string chosen = select_option("Select model", options);
            if (chosen.empty()) continue;
            settings_.model = chosen == "default" ? std::optional<std::string>{} : chosen;
            model_name_ = chosen == "default" ? model_name_ : chosen;
            commands::set_model(model_name_);
            write_user_setting("model", boost::json::value(chosen));
        } else if (picked == "theme") {
            const auto options = commands::theme_options();
            const std::string chosen = select_option("Theme", options);
            if (chosen.empty()) continue;
            ui::ThemeName name = ui::ThemeName::Dark;
            if (ui::parse_theme_name(chosen, name)) {
                settings_.theme = name;
                theme_ = ui::theme_for(name);
                write_user_setting("theme", boost::json::value(chosen));
            }
        } else if (picked == "output-style") {
            const auto options = commands::output_style_options(options_.cwd);
            const std::string chosen = select_option("Preferred output style", options);
            if (chosen.empty()) continue;
            settings_.output_style = chosen;
            write_user_setting("outputStyle", boost::json::value(chosen));
        } else if (picked == "permission-mode") {
            const auto options = permission_mode_options();
            const std::string chosen = select_option("Default permission mode", options);
            if (chosen.empty()) continue;
            if (chosen == "acceptEdits") {
                settings_.default_mode = PermissionMode::AcceptEdits;
            } else if (chosen == "plan") {
                settings_.default_mode = PermissionMode::Plan;
            } else if (chosen == "bypassPermissions") {
                settings_.default_mode = PermissionMode::BypassPermissions;
            } else {
                settings_.default_mode = PermissionMode::Default;
            }
            write_user_setting("defaultMode", boost::json::value(chosen));
        } else if (picked == "cleanup-period") {
            ui::InputDialog dialog(theme_, "Cleanup period",
                                   "How many days the transcripts of the sessions are kept.");
            const ui::InputDialogResult given = dialog.run(ui::Terminal::instance());
            if (given.outcome != ui::InputDialogOutcome::Confirmed) continue;
            const auto days = cc::parse_int(cc::trim(given.value));
            if (!days || *days < 0) continue;
            settings_.cleanup_period_days = static_cast<int>(*days);
            write_user_setting("cleanupPeriodDays", boost::json::value(*days));
        } else if (picked == "co-authored-by") {
            settings_.include_co_authored_by = !settings_.include_co_authored_by;
            write_user_setting("includeCoAuthoredBy",
                               boost::json::value(settings_.include_co_authored_by));
        } else if (picked == "auto-compact") {
            config::set_global_flag("autoCompactEnabled", !auto_compact_enabled());
        } else if (picked == "file-checkpointing") {
            config::set_global_flag("fileCheckpointingEnabled",
                                     !config::global_flag("fileCheckpointingEnabled", true));
        } else if (picked == "verbose") {
            verbose_ = !verbose_;
            config::set_global_flag("verbose", verbose_);
        } else if (picked == "terminal-progress-bar") {
            config::set_global_flag("terminalProgressBarEnabled",
                                     !config::global_flag("terminalProgressBarEnabled", false));
        } else if (picked == "show-turn-duration") {
            config::set_global_flag("showTurnDuration",
                                     !config::global_flag("showTurnDuration", false));
        } else if (picked == "auto-mode-during-plan") {
            settings_.use_auto_mode_during_plan = !settings_.use_auto_mode_during_plan;
            write_user_setting("useAutoModeDuringPlan",
                               boost::json::value(settings_.use_auto_mode_during_plan));
        } else if (picked == "copy-full-response") {
            config::set_global_flag("copyFullResponse",
                                     !config::global_flag("copyFullResponse", false));
        } else if (picked == "copy-on-select") {
            config::set_global_flag("copyOnSelect", !config::global_flag("copyOnSelect", false));
        } else if (picked == "default-view") {
            const std::string view = settings_.default_view ? *settings_.default_view : "default";
            settings_.default_view = view == "default" ? std::optional<std::string>()
                                                       : std::optional<std::string>(view);
            write_user_setting("defaultView", boost::json::value(settings_.default_view.value_or("")));
        } else if (picked == "diff-tool") {
            const std::vector<commands::CommandOption> options = {
                {"terminal", "terminal", ""},
                {"auto", "auto", ""},
            };
            const std::string chosen = select_option("Diff tool", options);
            if (chosen.empty()) continue;
            config::set_global_string("diffTool", chosen);
        } else if (picked == "external-includes") {
            // The reference opens the dialog of the files that are pulled
            // in from outside the folder of the session.
            const std::vector<std::string> includes = config::external_includes(options_.cwd);
            ui::ExternalIncludesDialog dialog(theme_, includes, /*standalone=*/false);
            const bool allowed = dialog.run(ui::Terminal::instance());
            config::remember_external_includes(options_.cwd, allowed);
        } else if (picked == "api-key") {
            const std::string key = auth::custom_api_key();
            if (key.empty()) continue;
            // The key is approved, or refused; it is never both, the way
            // the reference keeps it.
            config::remember_custom_api_key(key, !config::custom_api_key_approved(key));
        } else if (picked == "auto-connect-ide") {
            config::set_global_flag("autoConnectIde", !config::global_flag("autoConnectIde", false));
        } else if (picked == "auto-install-ide-extension") {
            config::set_global_flag("autoInstallIdeExtension",
                                     !config::global_flag("autoInstallIdeExtension", true));
        } else if (picked == "pr-status-footer") {
            config::set_global_flag("prStatusFooterEnabled",
                                     !config::global_flag("prStatusFooterEnabled", true));
        } else if (picked == "claude-in-chrome") {
            config::set_claude_in_chrome_default_enabled(
                !config::global_claude_in_chrome_default_enabled());
        } else if (picked == "tips") {
            settings_.spinner_tips_enabled = !settings_.spinner_tips_enabled;
            write_user_setting("spinnerTipsEnabled",
                               boost::json::value(settings_.spinner_tips_enabled));
        } else if (picked == "reduce-motion") {
            settings_.prefers_reduced_motion = !settings_.prefers_reduced_motion;
            spinner_->set_reduced_motion(settings_.prefers_reduced_motion);
            write_user_setting("prefersReducedMotion",
                               boost::json::value(settings_.prefers_reduced_motion));
        } else if (picked == "thinking-mode") {
            settings_.always_thinking_enabled = !settings_.always_thinking_enabled;
            write_user_setting("alwaysThinkingEnabled",
                               boost::json::value(settings_.always_thinking_enabled));
        } else if (picked == "prompt-suggestions") {
            settings_.prompt_suggestion_enabled = !settings_.prompt_suggestion_enabled;
            write_user_setting("promptSuggestionEnabled",
                               boost::json::value(settings_.prompt_suggestion_enabled));
        } else if (picked == "respect-gitignore") {
            settings_.respect_gitignore = !settings_.respect_gitignore;
            write_user_setting("respectGitignore", boost::json::value(settings_.respect_gitignore));
        } else if (picked == "auto-update-channel") {
            const std::vector<commands::CommandOption> options = {
                {"latest", "latest", ""},
                {"stable", "stable", ""},
            };
            const std::string chosen = select_option("Auto-update channel", options);
            if (chosen.empty()) continue;
            settings_.auto_updates_channel = chosen;
            write_user_setting("autoUpdatesChannel", boost::json::value(chosen));
        } else if (picked == "notifications") {
            std::vector<commands::CommandOption> options;
            for (const auto& name : ui::notification_channel_names()) {
                // The choices carry the names the reference writes, and the
                // values that are kept in the configuration.
                options.push_back({notification_label(name), name, ""});
            }
            const std::string chosen = select_option("Notifications", options);
            if (chosen.empty()) continue;
            config::set_global_string("preferredNotifChannel", chosen);
        } else if (picked == "language") {
            ui::InputDialog dialog(theme_, "Language",
                                   "The language the answers are written in.");
            const ui::InputDialogResult given = dialog.run(ui::Terminal::instance());
            if (given.outcome != ui::InputDialogOutcome::Confirmed) continue;
            const std::string language = cc::trim(given.value);
            settings_.language = language.empty() ? std::optional<std::string>{} : language;
            write_user_setting("language", boost::json::value(language));
        } else if (picked == "editor-mode") {
            const std::vector<commands::CommandOption> options = {
                {"Normal", "normal", ""},
                {"Vim", "vim", ""},
            };
            const std::string chosen = select_option("Editor mode", options);
            if (chosen.empty()) continue;
            config::set_global_editor_mode(chosen);
            vim_.set_enabled(chosen == "vim");
            vim_.set_mode(ui::vim::Mode::Insert);
            input_->set_vim_mode(vim_.enabled(), true);
        } else if (picked == "stats") {
            // The statistics of the sessions are a section of the panel of
            // the configuration, the way the user asked for them.
            show_stats();
        }
    }
}

/// The settings of the session, as the tab of the configuration shows them.
std::vector<ui::Setting> App::config_settings() {
    std::vector<ui::Setting> settings;

    // The rows of the panel of the reference, in the order it shows them.
    settings.push_back({"Auto-compact", auto_compact_enabled() ? "true" : "false", "auto-compact",
                         "Whether the conversation is made shorter on its own"});
    settings.push_back({"Show tips", settings_.spinner_tips_enabled ? "true" : "false", "tips",
                         "Whether the tips of the spinner are shown"});
    settings.push_back({"Reduce motion", settings_.prefers_reduced_motion ? "true" : "false",
                         "reduce-motion", "Whether the animations are turned off"});
    settings.push_back({"Thinking mode", settings_.always_thinking_enabled ? "true" : "false",
                         "thinking-mode", "Whether the model thinks before it answers"});
    settings.push_back({"Prompt suggestions", settings_.prompt_suggestion_enabled ? "true" : "false",
                         "prompt-suggestions", "Whether the prompt box suggests what to write"});
    settings.push_back({"Rewind code (checkpoints)",
                         config::global_flag("fileCheckpointingEnabled", true) ? "true" : "false",
                         "file-checkpointing", "Whether the copies of the changed files are kept"});
    settings.push_back({"Verbose output", verbose_ ? "true" : "false", "verbose",
                         "Whether the details of the session are shown"});
    settings.push_back({"Terminal progress bar",
                         config::global_flag("terminalProgressBarEnabled", false) ? "true" : "false",
                         "terminal-progress-bar", "Whether the bar of the usage is drawn"});
    settings.push_back({"Show turn duration",
                         config::global_flag("showTurnDuration", false) ? "true" : "false",
                         "show-turn-duration", "Whether the time a turn took is shown"});
    settings.push_back({"Default permission mode",
                         std::string(permission_mode_title(settings_.default_mode)),
                         "permission-mode", "The mode the session starts in"});
    settings.push_back({"Use auto mode during plan",
                         settings_.use_auto_mode_during_plan ? "true" : "false",
                         "auto-mode-during-plan",
                         "Whether the plan mode lets the session work on its own"});
    settings.push_back({"Respect .gitignore in file picker",
                         settings_.respect_gitignore ? "true" : "false", "respect-gitignore",
                         "Whether the file picker leaves the files of the .gitignore alone"});
    settings.push_back({"Always copy full response (skip /copy picker)",
                         config::global_flag("copyFullResponse", false) ? "true" : "false",
                         "copy-full-response", "Whether the whole of the answer is copied"});
    // The text that is selected is copied in the full-screen layout only:
    // in the other one the terminal of the user owns the selection, the way
    // the reference leaves it to the terminal.
    if (!scrollback_requested()) {
        settings.push_back({"Copy on select",
                             config::global_flag("copyOnSelect", true) ? "true" : "false",
                             "copy-on-select", "Whether the text that is selected is copied"});
    }
    settings.push_back({"Auto-update channel",
                         settings_.auto_updates_channel ? *settings_.auto_updates_channel : "latest",
                         "auto-update-channel", "The channel the updates come from"});
    settings.push_back({"Theme", theme_label(settings_.theme), "theme",
                         "The colours of the session"});
    settings.push_back({"Notifications",
                         notification_label(config::global_string("preferredNotifChannel", "auto")),
                         "notifications",
                         "The way the terminal is told that the session wants the user"});
    settings.push_back({"Output style",
                         settings_.output_style ? *settings_.output_style : "default", "output-style",
                         "The way the answers are written"});
    settings.push_back({"What you see by default",
                         settings_.default_view ? *settings_.default_view : "default", "default-view",
                         "The view the session starts in"});
    settings.push_back({"Language", settings_.language ? *settings_.language : "Default (English)",
                         "language", "The language the answers are written in"});
    settings.push_back({"Editor mode", config::global_editor_mode() == "vim" ? "vim" : "normal",
                         "editor-mode", "The way the keys of the prompt box are read"});
    settings.push_back({"Show PR status footer",
                         config::global_flag("prStatusFooterEnabled", true) ? "true" : "false",
                         "pr-status-footer", "Whether the pull request of the branch is shown"});
    const std::string model = settings_.model ? *settings_.model : model_display_name(model_name_);
    settings.push_back({"Model", model.empty() ? "Default (recommended)" : model, "model",
                         "The model the session talks to"});

    // The tool of the differences is only offered when the session talks to
    // an IDE, the way the reference offers it.
    if (connected_to_ide()) {
        settings.push_back({"Diff tool", config::global_string("diffTool", "auto"), "diff-tool",
                             "The tool the differences are shown with"});
    }

    settings.push_back({"Auto-connect to IDE (external terminal)",
                         config::global_flag("autoConnectIde", false) ? "true" : "false",
                         "auto-connect-ide", "Whether the session connects to the IDE on its own"});
    settings.push_back({"Auto-install IDE extension",
                         config::global_flag("autoInstallIdeExtension", true) ? "true" : "false",
                         "auto-install-ide-extension",
                         "Whether the extension of Claude Code is installed in the IDE"});
    settings.push_back({"Claude in Chrome enabled by default",
                         config::global_claude_in_chrome_default_enabled() ? "true" : "false",
                         "claude-in-chrome", "Whether the browser of Claude is on by default"});

    // The files the project pulls in from outside its folder are only
    // offered when the project has some.
    if (!config::external_includes(options_.cwd).empty()) {
        settings.push_back({"External CLAUDE.md includes",
                             config::external_includes_approved(options_.cwd) ? "true" : "false",
                             "external-includes",
                             "Whether the files outside the folder of the session are read"});
    }

    // The key of the API that was found in the environment is only offered
    // when there is one, the way the reference offers it.
    //
    // The key of the API is named at the end of the row, in bold, the way
    // the reference names it.
    const std::string api_key = auth::custom_api_key();
    if (!api_key.empty() && !auth::running_on_homespace()) {
        ui::Style bold;
        bold.bold = true;
        settings.push_back({"Use custom API key: " + ui::styled(api_key, bold),
                             config::custom_api_key_approved(api_key) ? "true" : "false",
                             "api-key",
                             "Whether the session talks to the API with the key of the environment"});
    }

    // The settings of the session that the reference does not list.
    settings.push_back({"Cleanup period", std::to_string(settings_.cleanup_period_days) + " days",
                         "cleanup-period", "How long the transcripts are kept"});
    settings.push_back({"Include Co-Authored-By", settings_.include_co_authored_by ? "true" : "false",
                         "co-authored-by", "Whether the commits name Claude as the author"});

    // The environment the session runs in.
    if (!settings_.env.empty()) {
        std::string value;
        for (const auto& [key, entry] : settings_.env) {
            if (!value.empty()) value += ", ";
            value += key;
        }
        settings.push_back({"Env", value, "env", "The variables of the session"});
    }

    // The rules of the permissions.
    settings.push_back({"Allow rules", rules_summary(settings_.allow_rules), "allow-rules",
                         "The tools that run without asking"});
    settings.push_back({"Deny rules", rules_summary(settings_.deny_rules), "deny-rules",
                         "The tools that never run"});

    // The files the settings are read from.
    for (const auto& [path, exists] : settings_files(options_.cwd)) {
        settings.push_back({"Settings file", cc::tilde_path(path), "settings-file",
                             exists ? "Found" : "Missing"});
    }

    // The statistics of the sessions, which the panel of `/stats` shows.
    settings.push_back({"Usage statistics", "Open", "stats",
                        "The statistics of the sessions"});

    return settings;
}

/// The state of the session, as the tab of the state shows it.
///
/// The reference lists the version, the name and the id of the session, the
/// folder it runs in, the account it runs under, the address of the API, the
/// model, the servers of MCP and the files the settings are read from.
std::vector<ui::StatusTab::Property> App::status_properties() {
    std::vector<ui::StatusTab::Property> properties;

    properties.push_back({"Version", cc::version()});
    properties.push_back({"Session name", session_name()});
    properties.push_back({"Session ID", session_id_});
    properties.push_back({"cwd", options_.cwd});

    // The account the session runs under.
    if (const auto tokens = auth::current_tokens()) {
        if (!tokens->subscription_type.empty()) {
            properties.push_back({"Login method", tokens->subscription_type + " Account"});
        }
    } else if (api_key().has_value()) {
        properties.push_back({"API key", "API key"});
    }

    // The address of the API, when the user wrote one.
    const std::string base_url = auth::base_api_url();
    if (base_url != "https://api.anthropic.com") {
        properties.push_back({"Anthropic base URL", base_url});
    }

    // The reference leaves an empty line between the two blocks of rows.
    properties.push_back({"", ""});

    properties.push_back({"Model", model_display_name(model_name_)});

    // The servers of MCP, counted by their state, the way the reference
    // counts them. Every part of the row has its own colour.
    if (mcp_) {
        size_t connected = 0;
        size_t pending = 0;
        size_t needs_auth = 0;
        size_t failed = 0;
        for (const mcp::ServerStatus& server : mcp_->status()) {
            if (server.connected) {
                ++connected;
            } else if (server.needs_auth) {
                ++needs_auth;
            } else if (server.connecting) {
                ++pending;
            } else {
                ++failed;
            }
        }
        if (connected > 0 || pending > 0 || needs_auth > 0 || failed > 0) {
            std::string value;
            if (connected > 0) {
                value += ui::styled(std::to_string(connected) + " connected",
                                    ui::Style::fg(theme_.success));
            }
            if (needs_auth > 0) {
                if (!value.empty()) value += ", ";
                value += ui::styled(std::to_string(needs_auth) + " need auth",
                                    ui::Style::fg(theme_.warning));
            }
            if (pending > 0) {
                if (!value.empty()) value += ", ";
                value += ui::styled(std::to_string(pending) + " pending",
                                    ui::Style::fg(theme_.inactive));
            }
            if (failed > 0) {
                if (!value.empty()) value += ", ";
                value += ui::styled(std::to_string(failed) + " failed",
                                    ui::Style::fg(theme_.error));
            }
            value += " " + ui::styled("· /mcp", ui::Style::fg(theme_.inactive));
            properties.push_back({"MCP servers", value});
        }
    }

    // The files the settings are read from. The reference names the sources
    // of the settings and joins them with a comma on a single line.
    std::string sources;
    for (const auto& [path, exists] : settings_files(options_.cwd)) {
        if (!exists) continue;
        if (!sources.empty()) sources += ", ";
        sources += settings_source_name(path, options_.cwd);
    }
    if (!sources.empty()) properties.push_back({"Setting sources", sources});

    return properties;
}

/// The name the user gave the session.
std::string App::session_name() {
    const std::string name = session::Transcript::title(options_.cwd, session_id_);
    if (name.empty()) {
        // The reference writes the placeholder in the dim colour.
        return ui::styled("/rename to add a name", ui::Style::fg(theme_.inactive));
    }
    return name;
}

/// Asks the API how much of the plan of the user is used.
///
/// The answer is asked for once, and both the limits and the extra usage
/// come out of it.
auth::Utilization App::fetch_usage() {
    return fetch_usage_of_plan(io_context_, nullptr);
}

/// The limits of the plan, as the tab of the usage shows them.
std::vector<ui::UsageTab::Limit> App::usage_limits(const auth::Utilization& usage) {
    std::vector<ui::UsageTab::Limit> limits;

    const auto tokens = auth::current_tokens();
    if (!tokens || tokens->access_token.empty()) return limits;
    if (!usage.known) return limits;

    const bool sonnet = tokens->subscription_type == "max" ||
                        tokens->subscription_type == "team" ||
                        tokens->subscription_type.empty();

    const auto add = [&limits](const std::string& title, const auth::RateLimit& limit) {
        if (!limit.has_utilization) return;
        ui::UsageTab::Limit entry;
        entry.title = title;
        entry.utilization = limit.utilization / 100.0;
        if (limit.resets_at > 0) entry.resets_at = cc::format_reset_time(limit.resets_at);
        limits.push_back(std::move(entry));
    };

    add("Current session", usage.five_hour);
    add("Current week (all models)", usage.seven_day);
    if (sonnet) add("Current week (Sonnet only)", usage.seven_day_sonnet);

    return limits;
}

/// The extra usage of the plan, when the API reports one.
///
/// The reference shows it only for the subscriptions that may spend beyond
/// the limits of the plan.
std::optional<ui::UsageTab::ExtraUsage> App::usage_extra(const auth::Utilization& usage) {
    const auto tokens = auth::current_tokens();
    if (!tokens || tokens->access_token.empty()) return std::nullopt;

    // The reference shows the extra usage to the Pro and the Max accounts.
    const std::string& subscription = tokens->subscription_type;
    if (subscription != "pro" && subscription != "max") return std::nullopt;

    if (!usage.extra_usage) return std::nullopt;

    ui::UsageTab::ExtraUsage extra;
    extra.is_enabled = usage.extra_usage->is_enabled;
    extra.monthly_limit = usage.extra_usage->monthly_limit;
    extra.used_credits = usage.extra_usage->used_credits;
    extra.utilization = usage.extra_usage->utilization.value_or(0.0);

    // The reference shows the section of the extra usage only when the
    // account may buy it: the line that names the command is shown only
    // when the command is there to be run.
    if (!extra.is_enabled && !auth::overage_provisioning_allowed()) return std::nullopt;

    // The reference leaves the section out when the API does not say how
    // much of the extra usage is used.
    if (extra.is_enabled && extra.monthly_limit.has_value() &&
        (!usage.extra_usage->used_credits.has_value() ||
         !usage.extra_usage->utilization.has_value())) {
        return std::nullopt;
    }

    return extra;
}

/// Sends the feedback of the user and returns the text of what happened.
///
/// The user is asked for the description of the report, the report is sent
/// to the API of Anthropic, and the issue of the report is offered in the
/// browser.
std::string App::send_feedback(const std::string& description) {
    const std::string trimmed = cc::trim(description);

    // The description is asked for when the user did not write one.
    std::string text = trimmed;
    if (text.empty()) {
        // The reference asks for the report in a dialog of its own, and
        // says what the field is for.
        ui::InputDialog dialog(theme_, "Submit Feedback / Bug Report", "");
        dialog.set_lead("Describe the issue below:");
        dialog.set_hint("Enter to continue · Esc to cancel");
        // The field of the report takes the Ctrl-C for itself.
        dialog.set_exit_on_ctrl_c(false);

        const ui::InputDialogResult given = dialog.run(ui::Terminal::instance());
        if (given.outcome == ui::InputDialogOutcome::Cancelled) {
            return "Feedback / bug report cancelled\n";
        }
        if (given.outcome == ui::InputDialogOutcome::Interrupted) {
            return "Feedback cancelled\n";
        }
        text = cc::trim(given.value);
        if (text.empty()) return "The feedback was not sent.\n";
    }

    // The report holds the conversation it was written about.
    feedback::Report report;
    report.description = text;
    report.message_count = engine_ ? engine_->messages().size() : 0;
    report.version = version();
    if (engine_) {
        for (const Message& message : engine_->messages()) {
            report.transcript.push_back(message.to_json());
        }
    }

    const std::optional<std::string> key = api_key();
    std::string access_token;
    if (!key) {
        if (const auto tokens = auth::current_tokens()) access_token = tokens->access_token;
    }

    const std::string id = feedback::submit(key.value_or(std::string()), access_token, report);
    if (id.empty()) return "The feedback could not be sent.\n";

    std::string out = "Thank you for your report!\n";
    out += "Feedback ID: " + id + "\n";

    // The issue of the report may be opened in the browser, the way the
    // reference offers it.
    const std::string address = feedback::github_issue_url(id, "Claude Code feedback",
                                                           report.description, report.version);
    if (confirm("Open the issue of the report in the browser?")) open_browser(address);

    return out;
}

/// Asks the user for the options a plugin declares.
///
/// The plugin is asked for its options when it is turned on, the way the
/// reference asks for them; the values are kept in the settings of the user.
void App::ask_plugin_options(const plugins::Plugin& plugin) {
    const std::vector<plugins::PluginOption> options = plugins::plugin_options(plugin);
    if (options.empty()) return;

    boost::json::object values = plugins::load_plugin_options(plugin.id);

    // The reference writes nothing when the dialog is left before the last
    // field is saved.
    bool cancelled = false;

    for (size_t index = 0; index < options.size(); ++index) {
        const plugins::PluginOption& option = options[index];
        const std::string title = option.title.empty() ? option.key : option.title;

        std::string value;
        const auto stored = values.find(option.key);
        if (stored != values.end() && stored->value().is_string()) {
            value = stored->value().as_string().c_str();
        } else if (!option.default_value.empty()) {
            value = option.default_value;
        }

        // The dialog carries the name of the plugin, and the field has a
        // title of its own, the way the reference draws them.
        ui::InputDialog dialog(theme_, "Configure " + plugin.name, "Plugin options", value);
        dialog.set_field(title, option.description, index, options.size());
        // The reference leaves the Ctrl-C to the field when a field is
        // being written in.
        dialog.set_exit_on_ctrl_c(false);

        const ui::InputDialogResult given = dialog.run(ui::Terminal::instance());
        if (given.outcome != ui::InputDialogOutcome::Confirmed) {
            // The reference leaves the whole dialog when the field is left.
            cancelled = true;
            break;
        }

        if (given.value.empty()) {
            if (option.required) continue;
            values.erase(option.key);
            continue;
        }

        if (option.type == "boolean") {
            const bool yes =
                cc::to_lower(given.value) == "true" || given.value == "y" || given.value == "1";
            values[option.key] = boost::json::value(yes);
        } else if (option.type == "number") {
            const auto number = cc::parse_int(given.value);
            if (number) values[option.key] = boost::json::value(*number);
        } else {
            values[option.key] = boost::json::value(given.value);
        }
    }

    if (cancelled) return;

    std::string error;
    plugins::save_plugin_options(plugin.id, values, &error);
}

/// Asks the user for the options a plugin declares.
///
/// The dialog is the one the reference opens from the menu of the details of
/// a plugin: the title names the plugin and the subtitle says "Plugin
/// options". The values the user gives are kept in the settings of the user,
/// under `pluginConfigs[<plugin>].options`.
///
/// The plugin is named the way the reference names it: `name@marketplace`.
void App::configure_plugin(const std::string& plugin_id) {
    // The plugin the name stands for. The plugin is kept by value: the list
    // it comes from is gone when the search is over.
    std::optional<plugins::Plugin> found;
    for (const plugins::Plugin& plugin : plugins::installed_plugins()) {
        if (plugin.id != plugin_id) continue;
        found = plugin;
        break;
    }
    if (!found) return;

    // A plugin that asks for nothing has no dialog.
    const std::vector<plugins::PluginOption> options = plugins::plugin_options(*found);
    if (options.empty()) return;

    ui::PluginConfigure dialog(theme_, "Configure " + found->name, "Plugin options");

    const std::optional<boost::json::object> values =
        dialog.run(options, plugins::load_plugin_options(plugin_id), ui::Terminal::instance());

    // The reference writes nothing when the dialog is left.
    if (!values) return;

    // The reference keeps the values that are secret in the keychain of the
    // machine; the port has no such place for them yet, so they are kept in
    // the settings of the user as well. The dialog never shows them again.
    std::string error;
    if (!plugins::save_plugin_options(plugin_id, *values, &error)) {
        ui::Terminal::instance().write_static(
            ui::styled("The options of the plugin could not be kept: " + error,
                       ui::Style::fg(theme_.error)) +
            "\n");
    }
}

std::string App::show_plugin_screen() {
    const std::vector<PluginEntry> entries = plugin_entries();

    std::vector<commands::CommandOption> options;
    for (const PluginEntry& entry : entries) {
        options.push_back({entry.label, entry.name + "@" + entry.marketplace, entry.description});
    }
    options.push_back({"Marketplaces", "marketplaces", "The marketplaces that are known"});

    const std::string chosen = select_option("Plugins", options);
    if (chosen.empty()) return "";

    // The marketplaces are shown on their own.
    if (chosen == "marketplaces") {
        const std::vector<plugins::Marketplace> marketplaces = plugins::known_marketplaces();
        if (marketplaces.empty()) return "No marketplace is known.\n";

        std::vector<commands::CommandOption> marketplace_options;
        for (const plugins::Marketplace& marketplace : marketplaces) {
            marketplace_options.push_back({marketplace.name, marketplace.name, marketplace.source});
        }
        const std::string picked = select_option("Marketplaces", marketplace_options);
        if (picked.empty()) return "";

        std::string error;
        if (!plugins::remove_marketplace(picked, &error)) return "Error: " + error + "\n";
        return "Removed the marketplace " + picked + ".\n";
    }

    // The plugin that was picked.
    const PluginEntry* entry = nullptr;
    for (const PluginEntry& candidate : entries) {
        if (candidate.name + "@" + candidate.marketplace == chosen) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) return "";

    const std::string id = entry->name + "@" + entry->marketplace;

    // What can be done with the plugin.
    std::vector<commands::CommandOption> actions;
    if (entry->installed) {
        actions.push_back({entry->enabled ? "Disable" : "Enable", entry->enabled ? "disable" : "enable",
                           entry->enabled ? "Turn the plugin off" : "Turn the plugin on"});
        actions.push_back({"Uninstall", "uninstall", "Remove the plugin from the cache"});
    } else {
        actions.push_back({"Install", "install", "Install the plugin of the marketplace"});
    }

    const std::string action = select_option(entry->label, actions);
    if (action.empty()) return "";

    std::string error;
    if (action == "enable" || action == "disable") {
        if (!plugins::set_plugin_enabled(id, action == "enable", &error)) {
            return "Error: " + error + "\n";
        }
        reload_plugins();
        if (action == "enable") {
            for (const plugins::Plugin& plugin : plugins::installed_plugins()) {
                if (plugin.id == id) ask_plugin_options(plugin);
            }
        }
        return std::string("The plugin ") + id + (action == "enable" ? " is on.\n" : " is off.\n");
    }

    if (action == "uninstall") {
        if (!plugins::uninstall_plugin(id, &error)) return "Error: " + error + "\n";
        reload_plugins();
        return "Removed the plugin " + id + ".\n";
    }

    if (action == "install") {
        if (!plugins::install_plugin(entry->marketplace, entry->name, &error)) {
            return "Error: " + error + "\n";
        }
        reload_plugins();
        for (const plugins::Plugin& plugin : plugins::installed_plugins()) {
            if (plugin.id == id) ask_plugin_options(plugin);
        }
        return "Installed the plugin " + id + ".\n";
    }

    return "";
}

std::string App::reload_plugins() {
    // The commands of the user are read again, and the commands of the
    // plugins are added to them.
    custom_commands_ = commands::load_custom_commands(options_.cwd);
    for (const plugins::PluginCommand& plugin_command : plugins::plugin_commands()) {
        commands::CustomCommand command;
        command.name = plugin_command.name;
        command.description = plugin_command.description;
        command.argument_hint = plugin_command.argument_hint;
        command.content = plugin_command.content;
        command.path = plugin_command.path;
        command.plugin_root = plugin_command.plugin_root;
        command.plugin_id = plugin_command.plugin + "@" + plugin_command.marketplace;
        custom_commands_.push_back(std::move(command));
    }
    std::sort(custom_commands_.begin(), custom_commands_.end(),
              [](const commands::CustomCommand& a, const commands::CustomCommand& b) {
                  return a.name < b.name;
              });

    // The prompt box paints the commands the session knows.
    update_known_commands(custom_commands_, mcp_commands_);

    // The agents of the user are read again, and the agents of the plugins
    // are added to them.
    agent_definitions_ = agents::load_definitions(options_.cwd, home_dir());
    for (agents::Definition& definition : plugins::plugin_agents()) {
        const auto existing = std::find_if(
            agent_definitions_.begin(), agent_definitions_.end(),
            [&](const agents::Definition& candidate) { return candidate.name == definition.name; });
        if (existing != agent_definitions_.end()) {
            *existing = std::move(definition);
        } else {
            agent_definitions_.push_back(std::move(definition));
        }
    }

    // The hooks are read again.
    hooks_.configure(settings_.to_json().as_object());
    for (hooks::Hook& hook : plugins::plugin_hooks()) hooks_.add(std::move(hook));

    // The report names what the session holds, the way the reference names
    // it.
    const auto installed = plugins::installed_plugins();
    size_t enabled = 0;
    for (const plugins::Plugin& plugin : installed) {
        if (plugin.enabled) ++enabled;
    }

    const auto counts = std::vector<size_t>{enabled, plugins::plugin_commands().size(),
                                            agent_definitions_.size(), plugins::plugin_hooks().size(),
                                            plugins::plugin_mcp_servers().size()};
    const std::vector<std::string> nouns = {"plugin", "skill", "agent", "hook",
                                            "plugin MCP server"};

    std::string report = "Reloaded: ";
    for (size_t i = 0; i < counts.size(); ++i) {
        if (i > 0) report += " · ";
        report += std::to_string(counts[i]) + " " + nouns[i];
        if (counts[i] != 1) report += "s";
    }
    report += "\n";

    return report;
}

std::string App::usage_text() {
    const std::optional<auth::Tokens> tokens = auth::current_tokens();
    if (!tokens || tokens->access_token.empty()) {
        return "Log in with a Claude account to see the limits of the plan: /login\n";
    }

    auth::Utilization usage;
    std::string error;
    auth::OauthConfig oauth_config;
    oauth_config.base_api_url = auth::base_api_url();
    if (!auth::fetch_utilization(io_context_, oauth_config, tokens->access_token, &usage,
                                 &error)) {
        return "Error: " + error + "\n";
    }

    ui::Style title_style;
    title_style.bold = true;

    ui::Style dim;
    dim.foreground = theme_.inactive;

    std::string out;
    for (const ui::UsageTab::Limit& limit : usage_limits(usage)) {
        out += ui::styled(limit.title, title_style) + "\n";
        out += ui::progress_bar(theme_, limit.utilization, 50) + " " +
               std::to_string(static_cast<int>(std::floor(limit.utilization * 100))) + "% used\n";
        if (!limit.resets_at.empty()) {
            out += ui::styled("Resets " + limit.resets_at, dim) + "\n";
        }
        out += "\n";
    }

    if (out.empty()) out += ui::styled("/usage is only available for subscription plans.", dim) + "\n";
    return out;
}

/// Opens the panel of the sandbox.
///
/// The reference opens a panel with the tabs of the mode, of the overrides,
/// of the configuration and of the dependencies.
std::string App::show_sandbox(const std::string& argument) {
    // The command may be given a pattern to exclude from the sandbox.
    const std::string trimmed = cc::trim(argument);
    if (!trimmed.empty()) {
        const std::vector<std::string> parts = cc::split(trimmed, ' ');
        if (parts.front() != "exclude") {
            return "Error: Unknown subcommand \"" + parts.front() +
                   "\". Available subcommand: exclude";
        }

        std::string pattern = cc::trim(trimmed.substr(std::string("exclude").size()));
        if (pattern.empty()) {
            return "Error: Please provide a command pattern to exclude (e.g., /sandbox exclude "
                   "\"npm run test:*\")";
        }

        // The quotes the user wrote are not part of the pattern.
        if (pattern.size() >= 2 && ((pattern.front() == '"' && pattern.back() == '"') ||
                                    (pattern.front() == '\'' && pattern.back() == '\''))) {
            pattern = pattern.substr(1, pattern.size() - 2);
        }

        // The command is added to the excluded commands of the local
        // settings; the rest of the object of the sandbox stays as it is.
        const std::string path = config::local_settings_path(options_.cwd);

        boost::json::value contents(boost::json::object{});
        if (const auto existing = read_file(path)) {
            contents = json_parse(*existing, boost::json::value(boost::json::object{}));
        }
        if (!contents.is_object()) contents = boost::json::value(boost::json::object{});

        boost::json::object object = contents.as_object();
        boost::json::value existing_sandbox(boost::json::object{});
        if (const auto* sandbox = object.find("sandbox");
            sandbox != object.end() && sandbox->value().is_object()) {
            existing_sandbox = sandbox->value();
        }
        object["sandbox"] = sandbox::add_excluded_command(existing_sandbox, pattern);

        config::write_settings(path, boost::json::value(std::move(object)));

        sandbox::Settings settings = sandbox_.settings();
        if (std::find(settings.excluded_commands.begin(), settings.excluded_commands.end(),
                      pattern) == settings.excluded_commands.end()) {
            settings.excluded_commands.push_back(pattern);
        }
        sandbox_.set_settings(settings);
        update_sandbox_description();

        std::string relative = path;
        const std::string prefix = options_.cwd + "/";
        if (cc::starts_with(relative, prefix)) relative = relative.substr(prefix.size());

        return "Added \"" + pattern + "\" to excluded commands in " + relative;
    }

    ui::SandboxPanel panel(theme_, sandbox_, [this](const sandbox::Settings& settings) {
        write_sandbox_settings(settings);
    });
    return panel.run(ui::Terminal::instance());
}

/// Writes the description of the `/sandbox` command, so that it shows the
/// state of the sandbox.
void App::update_sandbox_description() {
    commands::SandboxState state;
    state.enabled = sandbox_.is_sandboxing_enabled();
    state.auto_allow = sandbox_.is_auto_allow_bash_if_sandboxed_enabled();
    state.allow_unsandboxed = sandbox_.are_unsandboxed_commands_allowed();
    state.has_dependencies = sandbox_.dependency_errors().empty();
    commands::set_sandbox_state(state);
}

/// True when the instructions about the commits and the pull requests belong
/// in the prompt of the `Bash` tool.
bool App::include_git_instructions() const {
    bool included = true;
    for (const config::SettingsFile& file : settings_.files) {
        if (!file.contents.is_object()) continue;
        const auto* value = file.contents.as_object().find("includeGitInstructions");
        if (value == nullptr) continue;
        if (value->value().is_bool()) included = value->value().as_bool();
    }
    return included;
}

/// Writes the settings of the sandbox to the settings of the folder.
///
/// The reference writes them to the local settings of the folder, the way
/// the settings of the tools are written. Only the keys the panel changed
/// are written; the rest of the object that is in the file stays as it is.
void App::write_sandbox_settings(const sandbox::Settings& settings) {
    const std::string path = config::local_settings_path(options_.cwd);

    boost::json::value contents(boost::json::object{});
    if (const auto existing = read_file(path)) {
        contents = json_parse(*existing, boost::json::value(boost::json::object{}));
    }
    if (!contents.is_object()) contents = boost::json::value(boost::json::object{});

    boost::json::object object = contents.as_object();

    // The object of the sandbox that is already in the file is the base of
    // the one that is written.
    boost::json::value existing_sandbox(boost::json::object{});
    if (const auto* sandbox = object.find("sandbox");
        sandbox != object.end() && sandbox->value().is_object()) {
        existing_sandbox = sandbox->value();
    }
    object["sandbox"] = sandbox::apply_changes(existing_sandbox, settings);

    config::write_settings(path, boost::json::value(std::move(object)));

    sandbox_.set_settings(settings);
    update_sandbox_description();
}

/// Opens the panel of the statistics.
///
/// The reference shows the statistics of the sessions of the folder in two
/// tabs, and lets the user cycle through the ranges of days.
void App::show_stats() {
    ui::StatsPanel panel(theme_, options_.cwd);
    panel.run(ui::Terminal::instance());
}

/// Configures the servers of MCP and connects them.
///
/// The reference connects them again when the user asks for it in the panel
/// of the servers.
void App::configure_mcp_servers() {
    mcp_ = std::make_unique<mcp::Registry>(io_context_);

    {
        // The plugins that are on bring servers of their own; they are named
        // after the plugin they come from, so that they cannot collide with
        // the servers of the user.
        boost::json::object settings = settings_.to_json().as_object();
        boost::json::object servers = jget_object(settings, "mcpServers");
        for (const auto& [name, server] : plugins::plugin_mcp_servers()) servers[name] = server;

        // The IDE of the folder is connected to when the user asked for it,
        // the way the reference connects to it.
        if (config::global_flag("autoConnectIde", false)) {
            if (const auto ide = ide_of_the_folder()) {
                boost::json::object server;
                server["type"] = "http";
                server["url"] = ide->url;
                if (!ide->auth_token.empty()) server["headers"] = boost::json::object{
                    {"Authorization", boost::json::value("Bearer " + ide->auth_token)}};
                servers["ide"] = boost::json::value(std::move(server));

                // The IDE is remembered, so that the name of it can be shown
                // in the dialog of the permission.
                ide_ = ide;
            }
        }

        settings["mcpServers"] = std::move(servers);
        mcp_->configure(settings);
    }
    mcp_->connect_all();

    // The prompts of the servers are slash commands.
    mcp_commands_ = mcp_->commands();
}

/// Connects the servers of MCP again.
///
/// The reference connects the server that was chosen again; the port
/// connects them all again, because the servers of the session are held by
/// one registry.
void App::reconnect_mcp_server(const std::string& name) {
    (void)name;

    configure_mcp_servers();

    ui::Terminal& terminal = ui::Terminal::instance();
    terminal.write_static(std::string(ui::figures::kMessageResponse) +
                          ui::styled("The servers of MCP are connected again",
                                     ui::Style::fg(theme_.inactive)) +
                          "\n");
}

/// Turns a server of MCP on or off.
///
/// The servers of the `.mcp.json` file of a project are turned off in the
/// settings of the folder, the way the reference turns them off.
void App::set_mcp_server_enabled(const std::string& name, bool enabled) {
    const auto it = settings_.mcp_servers.find(name);
    if (it == settings_.mcp_servers.end()) return;

    // The answer is remembered in the list of the servers that are on and
    // in the list of the ones that are off.
    auto& on = settings_.enabled_mcpjson_servers;
    auto& off = settings_.disabled_mcpjson_servers;
    on.erase(std::remove(on.begin(), on.end(), name), on.end());
    off.erase(std::remove(off.begin(), off.end(), name), off.end());
    if (enabled) {
        on.push_back(name);
    } else {
        off.push_back(name);
    }

    // The answer is remembered in the settings of the folder.
    boost::json::object local;
    boost::json::array names;
    for (const auto& entry : on) names.emplace_back(entry);
    if (!names.empty()) local["enabledMcpjsonServers"] = std::move(names);
    boost::json::array disabled;
    for (const auto& entry : off) disabled.emplace_back(entry);
    if (!disabled.empty()) local["disabledMcpjsonServers"] = std::move(disabled);
    write_local_setting(options_.cwd, local);

    // The servers are connected again, so that the one that was turned off
    // is not there any more.
    configure_mcp_servers();
}

/// Authenticates a server of MCP.
///
/// The reference lets the user authenticate a server that waits for an
/// authentication; the port runs the flow of the session and shows the
/// dialog of it. The flow runs on a thread of its own, so the dialog is
/// drawn while the browser is open.
///
/// The server is connected again when the flow is through, so that the
/// token of the new authentication is used.
void App::authenticate_mcp_server(const std::string& name) {
    if (!mcp_) return;

    const std::shared_ptr<mcp::Server> server = mcp_->server(name);
    if (!server) return;

    // Only a server that is reached over the network is authenticated; the
    // others have no tokens at all.
    if (server->config().transport != "http") return;

    ui::McpAuthDialog dialog(theme_, name, server->config());

    std::string error;
    if (!dialog.run(ui::Terminal::instance(), &error)) {
        if (error.empty()) return;
        ui::Terminal::instance().write_static(
            ui::styled("The authentication of " + name + " failed: " + error,
                       ui::Style::fg(theme_.error)) +
            "\n");
        return;
    }

    // The server is connected again, so that the token of the new
    // authentication is used.
    reconnect_mcp_server(name);
}

/// Fetches the number of the installs of the plugins.
///
/// The numbers come from the statistics of the marketplace of Anthropic; the
/// fetch is done in a thread of its own, so that the interface is not held up
/// by the network. The numbers are good for a day, so they are fetched again
/// only when they are not there.
///
/// The thread that fetches them does not draw: it only fills the numbers in,
/// and the panel of the plugins is given them by the thread that owns the
/// terminal.
void App::fetch_install_counts() {
    {
        std::lock_guard<std::mutex> lock(install_counts_->mutex);
        if (install_counts_->counts) return;
    }

    // The numbers that are on the disk are good for a day.
    const auto cached = plugins::install_counts();
    if (cached) {
        std::lock_guard<std::mutex> lock(install_counts_->mutex);
        install_counts_->counts = cached;
        return;
    }

    // The fetch is done in a thread of its own: the interface is not held
    // up by the network. The thread holds the numbers of its own, so that
    // it does not need the session to be alive.
    const std::shared_ptr<InstallCounts> counts = install_counts_;
    std::thread([counts] {
        // The fetch has an event loop of its own: the one of the session
        // belongs to the session.
        boost::asio::io_context io_context;
        plugins::fetch_install_counts(io_context);

        const auto fetched = plugins::install_counts();
        std::lock_guard<std::mutex> lock(counts->mutex);
        counts->counts = fetched;
    }).detach();
}

/// Opens the panel of the plugins.
///
/// The panel has the tabs of the plugins that may be installed, of the ones
/// that are installed, of the marketplaces and of the errors, the way the
/// reference has them.
void App::show_plugin() {
    // The data of the panel is read again after every change, the way the
    // reference reads it again after it has installed a plugin.
    const auto read_data = [this](ui::PluginPanel& panel) {
        // The plugins that are installed.
        std::vector<ui::PluginPanelEntry> installed;
        for (const auto& plugin : plugins::installed_plugins()) {
            ui::PluginPanelEntry entry;
            entry.name = plugin.name;
            entry.id = plugin.id.empty() ? plugin.name : plugin.id;
            entry.description = plugin.description;
            entry.marketplace = plugin.marketplace;
            entry.version = plugin.version;
            entry.installed = true;
            entry.enabled = plugin.enabled;

            // The days the plugin was not used for: the reference shows the
            // plugins that were not used for a while in a group of their
            // own.
            const int64_t last_used = config::skill_last_used(plugin.id);
            if (last_used > 0) {
                const int64_t days = (cc::now_ms() - last_used) / (1000 * 60 * 60 * 24);
                if (days >= kUnusedDays) entry.unused_days = static_cast<int>(days);
            }

            // What the plugin brings: the details of it name the commands,
            // the agents, the skills, the hooks and the servers of MCP.
            entry.components = components_of(entry);

            // The plugin asks the user for options; the reference offers
            // them in the menu of the details of the plugin.
            entry.configurable = !plugins::plugin_options(plugin).empty();

            installed.push_back(std::move(entry));
        }

        // The servers of MCP stand in the same list, the way the reference
        // shows them.
        if (mcp_) {
            for (const auto& state : mcp_->status()) {
                ui::PluginPanelEntry entry;
                entry.name = state.name;
                entry.is_mcp = true;
                if (state.connected) {
                    entry.status = "connected";
                } else if (state.needs_auth) {
                    entry.status = "needs-auth";
                } else if (state.connecting) {
                    entry.status = "connecting";
                } else if (!state.error.empty()) {
                    // The server failed; it waits for an authentication when
                    // it is reached over the network and has no token of its
                    // own.
                    entry.status = waits_for_auth(state.name, *mcp_) ? "needs-auth" : "failed";
                } else {
                    entry.status = "disabled";
                }
                installed.push_back(std::move(entry));
            }
        }

        // The plugins the marketplaces offer, and the marketplaces themselves.
        std::vector<ui::PluginPanelEntry> discover;
        std::vector<ui::PluginPanelMarketplace> marketplaces;
        for (const auto& marketplace : plugins::known_marketplaces()) {
            ui::PluginPanelMarketplace entry;
            entry.name = marketplace.name;
            entry.source = marketplace.source;
            entry.last_updated = marketplace.last_updated;
            entry.auto_update = marketplace.auto_update;

            const auto offered = plugins::marketplace_plugins(marketplace.name);
            entry.plugin_count = offered.size();

            // The plugins of the marketplace that are installed.
            for (const auto& plugin : installed) {
                if (plugin.marketplace != marketplace.name) continue;
                ui::PluginPanelInstalledPlugin item;
                item.name = plugin.name;
                item.description = plugin.description;
                entry.installed_plugins.push_back(std::move(item));
                ++entry.installed_count;
            }

            marketplaces.push_back(std::move(entry));

            for (const auto& plugin : offered) {
                ui::PluginPanelEntry item;
                item.name = plugin.name;
                item.id = plugin.name + "@" + marketplace.name;
                item.description = plugin.description;
                item.marketplace = marketplace.name;
                item.category = plugin.category;
                item.version = plugin.version;
                item.installed = std::any_of(
                    installed.begin(), installed.end(),
                    [&](const ui::PluginPanelEntry& entry) { return entry.id == item.id; });

                // What the plugin will bring: the details of it name the
                // commands, the agents, the skills, the hooks and the
                // servers of MCP.
                item.components = components_of(item);

                discover.push_back(std::move(item));
            }
        }

        panel.set_data(std::move(discover), std::move(installed), std::move(marketplaces), {});
    };

    ui::PluginPanel panel(theme_, {}, {}, {}, {});
    read_data(panel);
    panel.set_reload(read_data);

    // The reason a change did not go through is shown under the panel.
    const auto failed = [&panel](const std::string& error) {
        panel.message = "Error: " + error;
        return false;
    };

    ui::PluginPanel::Actions actions;

    actions.install_plugin = [this, &failed](const std::string& marketplace,
                                              const std::string& plugin) {
        std::string error;
        if (!plugins::install_plugin(marketplace, plugin, &error)) return failed(error);
        reload_plugins();
        return true;
    };

    actions.set_plugin_enabled = [this, &failed](const std::string& id, bool enabled) {
        std::string error;
        if (!plugins::set_plugin_enabled(id, enabled, &error)) return failed(error);
        reload_plugins();
        return true;
    };

    actions.uninstall_plugin = [this, &failed](const std::string& id) {
        std::string error;
        if (!plugins::uninstall_plugin(id, &error)) return failed(error);
        reload_plugins();
        return true;
    };

    actions.add_marketplace = [this, &failed](const std::string& source) {
        std::string error;
        if (!plugins::add_marketplace("", source, &error)) return failed(error);

        // The marketplace is fetched at once, the way the reference fetches
        // it when it is added.
        const std::vector<plugins::Marketplace> known = plugins::known_marketplaces();
        for (const auto& marketplace : known) {
            if (marketplace.source != source) continue;
            std::string failure;
            plugins::refresh_marketplace(io_context_, marketplace.name, &failure);
            break;
        }
        return true;
    };

    actions.update_marketplace = [this, &failed](const std::string& name) {
        std::string error;
        if (!plugins::refresh_marketplace(io_context_, name, &error)) return failed(error);
        return true;
    };

    actions.set_marketplace_auto_update = [&failed](const std::string& name, bool enabled) {
        std::string error;
        if (!plugins::set_marketplace_auto_update(name, enabled, &error)) return failed(error);
        return true;
    };

    actions.reconnect_server = [this](const std::string& name) { reconnect_mcp_server(name); };

    // The options of a plugin are asked of the user by the session; the
    // panel only says that the user picked them.
    actions.configure_plugin = [this](const std::string& id) { configure_plugin(id); };

    actions.remove_marketplace = [this, &failed](const std::string& name) {
        std::string error;
        if (!plugins::remove_marketplace(name, &error)) return failed(error);
        reload_plugins();
        return true;
    };

    panel.set_actions(std::move(actions));

    // The number of the installs of the plugins, when they have been fetched.
    {
        std::lock_guard<std::mutex> lock(install_counts_->mutex);
        panel.set_install_counts(install_counts_->counts);
    }

    panel.run(ui::Terminal::instance());

    // The reason a change did not go through is written under the panel.
    if (!panel.message.empty()) {
        ui::Terminal::instance().write_static(
            ui::styled(panel.message, ui::Style::fg(theme_.error)) + "\n");
    }
}

/// Opens the picker of the conversations and resumes the one that is picked.
///
/// The conversation of the session that was picked takes the place of the
/// one of the session, the way the reference does it.
std::string App::resume_picked_session() {
    const auto picked = pick_session();
    if (!picked) return {};

    session_id_ = picked->id;
    resumed_cwd_ = picked->path.empty() ? options_.cwd : picked->path;

    const auto transcript = session::Transcript::open(resumed_cwd_, session_id_);
    engine_->set_messages(transcript.messages());
    resumed_ = !transcript.messages().empty();

    // The conversation of the session that was picked is shown, the way the
    // reference shows it.
    ui::Terminal::instance().clear_dynamic();
    if (resumed_) render_resumed_conversation();

    return "Resumed the conversation " + session_id_ + "\n";
}

/// Opens the panel of the servers of MCP.
///
/// The servers stand under the scope they are in, the way the reference
/// groups them, and the state of each of them is named.
void App::show_mcp() {
    // The scopes of the reference, in the order it shows them, and the file
    // the servers of each of them are written in.
    const std::vector<std::pair<std::string, std::string>> scopes = {
        {"project", "Project MCPs"},
        {"local", "Local MCPs"},
        {"user", "User MCPs"},
    };

    // The file the servers of a scope are written in.
    const auto path_of_scope = [this](const std::string& scope) {
        if (scope == "project") return cc::path_join(options_.cwd, ".mcp.json");
        if (scope == "local") {
            return cc::path_join(cc::path_join(options_.cwd, ".claude"), "settings.local.json");
        }
        return config::global_config_path();
    };

    // The state of every server of the session.
    std::vector<mcp::ServerStatus> states;
    if (mcp_) states = mcp_->status();

    // The state of a server is the one of the session.
    const auto state_of = [this, &states](const std::string& name, ui::McpPanelServer& entry) {
        bool found = false;
        for (const auto& state : states) {
            if (state.name != name) continue;
            found = true;
            if (state.connected) {
                entry.state = "connected";
                entry.tool_count = state.tool_count;
            } else if (state.needs_auth) {
                entry.state = "needs-auth";
            } else if (state.connecting) {
                entry.state = "connecting";
            } else if (!state.error.empty()) {
                // The server failed; it waits for an authentication when it
                // is reached over the network and has no token of its own.
                entry.state = (mcp_ && waits_for_auth(name, *mcp_)) ? "needs-auth" : "failed";
                entry.error = state.error;
            } else {
                entry.state = "disabled";
            }
        }
        if (!found) entry.state = "disabled";
    };

    std::vector<ui::McpPanelGroup> groups;
    for (const auto& [scope, title] : scopes) {
        ui::McpPanelGroup group;
        group.title = title;
        group.path = path_of_scope(scope);

        for (const auto& [name, server] : settings_.mcp_servers) {
            if (server.scope != scope) continue;

            ui::McpPanelServer entry;
            entry.name = name;
            state_of(name, entry);

            // The servers of the `.mcp.json` file of a project may be
            // turned off, the way the reference turns them off.
            entry.can_disable = scope == "project";

            group.servers.push_back(std::move(entry));
        }

        if (!group.servers.empty()) groups.push_back(std::move(group));
    }

    // The servers the plugins bring are always there, the way the reference
    // keeps them out of the reach of the settings.
    {
        ui::McpPanelGroup group;
        group.title = "Built-in MCPs";
        group.path = "always available";

        for (const auto& [name, server] : plugins::plugin_mcp_servers()) {
            (void)server;
            ui::McpPanelServer entry;
            entry.name = name;
            state_of(name, entry);
            group.servers.push_back(std::move(entry));
        }

        if (!group.servers.empty()) groups.push_back(std::move(group));
    }

    // The servers the agents of the session use. The reference shows them
    // under the name of the agent that uses them; the group has no file
    // behind it.
    {
        ui::McpPanelGroup group;
        group.title = "Agent MCPs";
        group.has_file = false;
        group.agents = true;

        for (const auto& [name, source_agents] : agent_mcp_servers(agent_definitions_)) {
            // Only the servers the session has are shown; the others are
            // not there to be used.
            const std::shared_ptr<mcp::Server> server = mcp_ ? mcp_->server(name) : nullptr;
            if (!server) continue;

            ui::McpPanelServer entry;
            entry.name = name;
            entry.source_agents = source_agents;

            // The reference names the servers of the agents that are reached
            // over the network "may need auth"; the others are "agent-only".
            entry.state = server->config().transport == "http" ? "needs-auth" : "disabled";

            group.servers.push_back(std::move(entry));
        }

        if (!group.servers.empty()) groups.push_back(std::move(group));
    }

    // The tools of every server, so that the panel can show them.
    if (mcp_) {
        for (auto& group : groups) {
            for (auto& server : group.servers) {
                const auto connected = mcp_->server(server.name);
                if (!connected) continue;
                for (const auto& tool : connected->tools()) {
                    ui::McpPanelTool entry;
                    entry.name = tool.tool_name;
                    entry.description = tool.description;
                    server.tools.push_back(std::move(entry));
                }
            }
        }
    }

    ui::McpPanel panel(theme_, std::move(groups));

    ui::McpPanel::Actions actions;
    actions.reconnect = [this](const std::string& name) { reconnect_mcp_server(name); };
    actions.set_enabled = [this](const std::string& name, bool enabled) {
        set_mcp_server_enabled(name, enabled);
    };

    // A server that waits for an authentication is authenticated by the
    // session: the flow of the browser is run and the dialog of it is
    // shown.
    actions.authenticate = [this](const std::string& name) { authenticate_mcp_server(name); };
    panel.set_actions(actions);

    panel.run(ui::Terminal::instance());
}

/// Opens the view that lets the user rewind the conversation.
///
/// The user picks one of the prompts of the session and decides what is to
/// happen: the conversation is rewound to the point before that prompt, or
/// the messages from there on are summarised.
void App::show_message_selector() {    if (!engine_) return;

    const auto messages = engine_->messages();
    const int64_t now = cc::now_ms();

    std::vector<ui::RewindOption> options;
    std::vector<size_t> indices;

    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& message = messages[i];
        if (message.role != Role::User) continue;

        // The reference leaves the messages the user did not write out of
        // the list: the summaries of a compaction, the meta messages and
        // the output of the commands.
        if (message.is_meta || message.is_compact_summary) continue;

        const std::string text = message.text();
        if (cc::trim(text).empty()) continue;
        if (is_command_output(text)) continue;

        ui::RewindOption option;
        option.text = text;
        option.ago = cc::relative_time_ago(cc::parse_timestamp(message.timestamp), now);
        // The code that the turn of the prompt changed, and the code that a
        // rewind to it would put back.
        option.diff = rewind_diff(file_history_.diff_to_next(message.uuid));
        option.restore = rewind_diff(file_history_.diff_to_now(message.uuid));
        options.push_back(std::move(option));
        indices.push_back(i);
    }

    // The prompt that is being written is the last entry; it cannot be
    // rewound to.
    ui::RewindOption current;
    current.current = true;
    options.push_back(std::move(current));

    ui::Terminal& terminal = ui::Terminal::instance();
    ui::MessageSelector selector(theme_, std::move(options));
    const ui::RewindAction action = selector.run(terminal);

    const size_t chosen = selector.selected();
    if (action == ui::RewindAction::None || chosen >= indices.size()) {
        terminal.clear_dynamic();
        render_prompt_box(false);
        return;
    }

    const size_t index = indices[chosen];

    ui::Style dim;
    dim.foreground = theme_.inactive;

    if (action == ui::RewindAction::RestoreCode) {
        // Only the files are put back; the conversation stays as it is.
        file_history_.rewind(messages[index].uuid);

        terminal.clear_dynamic();
        render_prompt_box(false);
        return;
    }

    std::vector<Message> before(messages.begin(), messages.begin() + static_cast<long>(index));

    if (action == ui::RewindAction::Restore || action == ui::RewindAction::RestoreBoth) {
        // The conversation stops before the prompt, and the prompt goes back
        // into the input so that it can be edited and sent again.
        engine_->set_messages(before);

        input_->set_text(messages[index].text());
        history_index_ = -1;

        // The files the session changed are put back the way they were, the
        // way the reference puts them back. The reference says nothing
        // about it.
        file_history_.rewind(messages[index].uuid);
    } else {
        // The messages from the prompt on are summarised, and the summary
        // takes their place. The note the user wrote goes to the model with
        // the request for the summary.
        const std::string summary = ask_model(session::compact_prompt(selector.feedback()));

        Message compacted;
        compacted.role = Role::User;
        compacted.uuid = uuid_v4();
        compacted.session_id = session_id_;
        compacted.cwd = options_.cwd;
        compacted.version = version();
        compacted.is_meta = true;
        compacted.is_compact_summary = true;
        compacted.content.push_back(TextBlock{summary, false});

        before.push_back(compacted);
        engine_->set_messages(before);

        terminal.clear_dynamic();
        terminal.write_static(ui::styled("Conversation summarized (ctrl+o for history)", dim) + "\n");
    }

    render_prompt_box(false);
}

/// The primary input of a tool call: the thing the user would want to copy.
///
/// The reference names the input of every tool it knows, so that the bar can
/// say `copy path` or `copy command`.
std::pair<std::string, std::string> primary_input(const ToolUseBlock& use) {
    if (!use.input.is_object()) return {};

    const auto& input = use.input.as_object();

    const auto take = [&input](const char* key) -> std::string {
        const auto found = input.find(key);
        if (found == input.end() || !found->value().is_string()) return {};
        return std::string(found->value().as_string().c_str());
    };

    const std::string_view name = use.name;
    if (name == tools::READ || name == tools::EDIT || name == tools::WRITE) {
        return {"path", take("file_path")};
    }
    if (name == tools::BASH) return {"command", take("command")};
    if (name == tools::GREP || name == tools::GLOB) return {"pattern", take("pattern")};
    if (name == tools::WEB_FETCH) return {"url", take("url")};
    if (name == tools::WEB_SEARCH) return {"query", take("query")};
    if (name == tools::AGENT) return {"prompt", take("prompt")};
    return {};
}

/// Turns the message actions on.
///
/// The bar lets the user walk through the conversation and act on the
/// message that is selected.
void App::enter_message_actions() {
    if (!engine_) return;

    action_targets_.clear();

    const auto& messages = engine_->messages();

    // The reads and the searches that follow each other are one line in the
    // transcript, so they are one entry of the bar as well.
    std::vector<const ToolUseBlock*> group;
    size_t group_message = 0;

    const auto flush_group = [&]() {
        if (group.empty()) return;

        ActionTarget target;
        target.message_index = group_message;
        target.copy_text = group_line(group);
        target.expandable = true;
        for (const auto* call : group) {
            Tool* tool = find_tool_by_name(registry_->all(), call->name);
            if (!tool) continue;
            const std::string line = ui::render_tool_use_line(*tool, tool_input(*call), options_.cwd,
                                                               theme_, ui::ToolCallState::Succeeded,
                                                               true);
            if (!line.empty()) target.group_lines.push_back(line);
        }
        action_targets_.push_back(std::move(target));
        group.clear();
    };

    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& message = messages[i];
        for (const auto& block : message.content) {
            if (const auto* text = as_text(block)) {
                if (cc::trim(text->text).empty()) continue;
                flush_group();
                ActionTarget target;
                target.copy_text = text->text;
                target.editable = message.role == Role::User;
                target.message_index = i;
                action_targets_.push_back(std::move(target));
                continue;
            }

            if (const auto* use = as_tool_use(block)) {
                if (is_collapsible_tool(use->name)) {
                    if (group.empty()) group_message = i;
                    group.push_back(use);
                    continue;
                }
                flush_group();
                ActionTarget target;
                target.message_index = i;
                const auto [label, value] = primary_input(*use);
                target.input_label = label;
                target.input_value = value;
                target.copy_text = value;
                action_targets_.push_back(std::move(target));
            }
        }
    }
    flush_group();

    if (action_targets_.empty()) return;

    // The reference starts the cursor on the last prompt the user wrote,
    // and not on the last message of any kind.
    action_index_ = action_targets_.size() - 1;
    while (action_index_ > 0 && !action_targets_[action_index_].editable) --action_index_;
    message_actions_ = true;
}

/// The bar that shows the actions of the selected message.
///
/// The reference names the keys of the actions that apply to the selected
/// message, and ends with the keys that walk through the conversation.
std::vector<std::string> App::render_action_bar() const {
    if (!message_actions_ || action_targets_.empty()) return {};

    const ActionTarget& target = action_targets_[action_index_];

    ui::Style bold;
    bold.bold = true;

    ui::Style dim;
    dim.foreground = theme_.inactive;

    std::string actions;

    const auto add = [&actions, &bold, &dim](const std::string& key, const std::string& label) {
        if (!actions.empty()) actions += ui::styled(" · ", dim);
        actions += ui::styled(key, bold);
        actions += ui::styled(" " + label, dim);
    };

    // The reference offers to expand the group of tools and to edit the
    // prompt of the user, and never both.
    if (target.expandable) {
        add("enter", target.expanded ? "collapse" : "expand");
    } else if (target.editable) {
        add("enter", "edit");
    }
    add("c", "copy");
    if (!target.input_label.empty()) add("p", "copy " + target.input_label);

    actions += ui::styled(" · ", dim);
    actions += ui::styled("↑↓", bold);
    actions += ui::styled(" navigate · ", dim);
    actions += ui::styled("esc", bold);
    actions += ui::styled(" back", dim);

    // The rule above the bar and the blank lines around it, the way the
    // reference draws it.
    const int width = ui::Terminal::instance().width();
    ui::Style rule;
    rule.foreground = theme_.inactive;

    std::string line;
    for (int i = 0; i < std::max(width, 1); ++i) line += ui::figures::kHorizontalRule;

    std::vector<std::string> lines;
    // The reference puts a blank row above the rule and two below the text:
    // the box of the bar has a row of padding on every side, and the box of
    // the actions has one of its own.
    lines.push_back("");
    lines.push_back(ui::styled(line, rule));
    lines.push_back("");
    lines.push_back("  " + actions);
    lines.push_back("");

    // The group of tools is shown under the bar when it is expanded.
    if (target.expanded) {
        for (const auto& group : target.group_lines) {
            lines.push_back("  " + group);
        }
    }

    lines.push_back("");

    return lines;
}

/// Handles a key while the message actions are on.
bool App::handle_action_key(const ui::Key& key) {
    if (action_targets_.empty()) {
        message_actions_ = false;
        return false;
    }

    switch (key.type) {
    case ui::Key::Type::Up:
        if (action_index_ > 0) --action_index_;
        return true;
    case ui::Key::Type::Down:
        // The reference leaves the mode when there is nothing below.
        if (action_index_ + 1 < action_targets_.size()) {
            ++action_index_;
        } else {
            message_actions_ = false;
        }
        return true;
    case ui::Key::Type::ShiftUp:
        // The previous prompt of the user.
        while (action_index_ > 0 && !action_targets_[action_index_].editable) --action_index_;
        if (action_index_ > 0) --action_index_;
        return true;
    case ui::Key::Type::ShiftDown:
        while (action_index_ + 1 < action_targets_.size() &&
               !action_targets_[action_index_ + 1].editable) {
            ++action_index_;
        }
        if (action_index_ + 1 < action_targets_.size()) ++action_index_;
        return true;
    case ui::Key::Type::Escape:
        // The first Escape leaves the bar.
        message_actions_ = false;
        return true;
    case ui::Key::Type::Enter: {
        // The group of tools is expanded and collapsed in place.
        if (action_targets_[action_index_].expandable) {
            action_targets_[action_index_].expanded = !action_targets_[action_index_].expanded;
            render_live();
            return true;
        }

        // The prompt that is selected goes back into the input, and the
        // conversation is rewound to the point before it, so that it can be
        // edited and sent again.
        const ActionTarget& target = action_targets_[action_index_];
        if (!target.editable) return true;

        if (engine_) {
            const auto& messages = engine_->messages();
            if (target.message_index <= messages.size()) {
                std::vector<Message> before(messages.begin(),
                                            messages.begin() +
                                                static_cast<long>(target.message_index));
                engine_->set_messages(before);
            }
        }
        input_->set_text(target.copy_text);
        history_index_ = -1;
        message_actions_ = false;
        return true;
    }
    default:
        break;
    }

    if (key.type != ui::Key::Type::Character) return false;

    if (key.text == "c") {
        // The text of the selected message goes on the clipboard.
        ui::copy_to_clipboard(action_targets_[action_index_].copy_text);
        message_actions_ = false;
        return true;
    }

    if (key.text == "p") {
        // The primary input of the tool call goes on the clipboard.
        const std::string& value = action_targets_[action_index_].input_value;
        if (value.empty()) return true;
        ui::copy_to_clipboard(value);
        message_actions_ = false;
        return true;
    }

    if (key.text == "j") {
        // The reference leaves the mode when there is nothing below.
        if (action_index_ + 1 < action_targets_.size()) {
            ++action_index_;
        } else {
            message_actions_ = false;
        }
        return true;
    }

    if (key.text == "k") {
        if (action_index_ > 0) --action_index_;
        return true;
    }

    return false;
}

///
/// The indicator only appears when the mode is not the default one. It
/// carries the mode's symbol, its name and the shortcut that cycles it.
std::string App::mode_indicator() const {
    if (!engine_) return {};

    const PermissionMode mode = engine_->context().permission_context.mode;

    std::string symbol;
    std::string title;
    ui::Rgb colour = theme_.text;
    switch (mode) {
    case PermissionMode::Plan:
        symbol = "⏸";
        title = "plan mode";
        colour = theme_.plan_mode;
        break;
    case PermissionMode::AcceptEdits:
        symbol = "⏵⏵";
        title = "accept edits";
        colour = theme_.auto_accept;
        break;
    case PermissionMode::BypassPermissions:
        symbol = "⏵⏵";
        title = "bypass permissions";
        colour = theme_.error;
        break;
    case PermissionMode::DontAsk:
        symbol = "⏵⏵";
        title = "don't ask";
        colour = theme_.error;
        break;
    case PermissionMode::Auto:
        // The reference writes the auto mode in the colour of the warning.
        symbol = "⏵⏵";
        title = "auto mode";
        colour = theme_.warning;
        break;
    case PermissionMode::Default:
        symbol = "⏸";
        title = "manual mode";
        // The manual mode is written in the grey of the inactive text.
        colour = theme_.inactive;
        break;
    default:
        return {};
    }

    // The reference draws the mode in the colour of the mode, and leaves
    // the shortcut that cycles it dim.
    ui::Style mode_style;
    mode_style.foreground = colour;

    ui::Style dim;
    dim.foreground = theme_.inactive;

    return ui::styled(symbol + " " + title + " on", mode_style) +
           ui::styled(" (shift+tab to cycle)", dim);
}

/// The width of the column of the names of the commands.
///
/// The reference measures it over all the commands and not only over the
/// ones that are shown, so that the list does not jump while the user
/// types.
size_t App::command_name_width() const {
    size_t longest = 0;
    for (const auto& command : commands::all()) {
        if (!commands::is_available(command)) continue;
        longest = std::max(longest, command.name.size());
    }
    for (const auto& command : custom_commands_) {
        longest = std::max(longest, command.name.size());
    }
    for (const auto& command : mcp_commands_) {
        longest = std::max(longest, command.user_facing_name.size());
    }
    // The reference adds a column for the slash and five for the padding.
    return longest + 6;
}

void App::refresh_mcp_commands() {
    if (!mcp_) return;
    const std::vector<mcp::McpCommand> commands = mcp_->commands();
    if (commands == mcp_commands_) return;

    mcp_commands_ = commands;
    // The list of the commands is built again, for the commands of the
    // servers have changed.
    suggestions_dirty_ = true;

    // The prompt box paints the commands the session knows.
    update_known_commands(custom_commands_, mcp_commands_);
}

std::vector<ui::AgentEntry> App::agent_entries() const {
    std::vector<ui::AgentEntry> entries;
    entries.reserve(agent_definitions_.size());
    for (const auto& agent : agent_definitions_) {
        entries.push_back({agent.name, agent.description});
    }
    return entries;
}

std::string App::apply_suggestion(const ui::Suggestion& suggestion) {
    const std::string text = input_->text();
    const size_t from = std::min(suggestion.insert_at, text.size());
    const size_t to = std::min(from + suggestion.insert_length, text.size());
    const std::string replaced = text.substr(0, from) + suggestion.insert + text.substr(to);
    input_->set_text(replaced);
    // The cursor stands behind the text that was written, the way the
    // reference leaves it.
    input_->cursor() = from + suggestion.insert.size();
    return replaced;
}

void App::render_prompt_box(bool loading, const std::vector<std::string>& above) {
    ui::Terminal& terminal = ui::Terminal::instance();
    const int width = terminal.width();

    // The servers connect in the background, so the prompts of a server may
    // have arrived since the last time the box was drawn.
    refresh_mcp_commands();

    // The footer names the paste for a moment, the way the reference names
    // it, and the status line stays out for that time.
    if (input_->pasting() && std::chrono::steady_clock::now() - paste_at_ >=
                                std::chrono::milliseconds(100)) {
        input_->set_pasting(false);
    }

    // The suggestions replace the footer of the prompt box. The help
    // overlay takes their place while it is open.
    if (!help_overlay_) {
        // The list of the commands does not come back while the input is the
        // one it was dismissed for.
        if (dismissed_list().dismissed && dismissed_list().input == input_->text()) {
            suggestions_.clear();
        } else {
            dismissed_list().dismissed = false;
            // The list of the commands is built again only when the text of
            // the input has changed, or when the commands of the session
            // have changed. The box is drawn twenty times a second, and
            // building the list is dearer than everything else the session
            // does while it waits for the user.
            if (suggestions_dirty_ || suggestions_input_ != input_->text()) {
                suggestions_ = ui::suggestions_for(input_->text(), options_.cwd, custom_commands_,
                                                   mcp_commands_, agent_entries());
                suggestions_input_ = input_->text();
                suggestions_dirty_ = false;
            }
        }
        if (suggestion_index_ >= suggestions_.size()) suggestion_index_ = 0;
    }

    std::vector<std::string> lines;

    // The message actions take the place of the prompt box: the reference
    // does not draw the input while the cursor is on a message, and the bar
    // stands where the box stood.
    if (message_actions_) {
        for (const auto& line : render_action_bar()) {
            lines.push_back(line);
        }
        terminal.render_dynamic(lines);
        return;
    }

    // The lines that sit above the box, the panel of the side question for
    // one.
    for (const auto& line : above) lines.push_back(line);

    // The `/help` overlay sits above the prompt box while it is open.
    if (help_overlay_) {
        for (const auto& line : help_overlay_->render(width, terminal.height())) {
            lines.push_back(line);
        }
    } else {
        // The todo list sits above the prompt box while it is showing.
        for (const auto& line : render_todos()) {
            lines.push_back(line);
        }
    }

    // The notice of the prompt that was put aside stands above the box, the
    // way the reference draws it.
    if (!stashed_prompt().empty()) {
        ui::Style dim;
        dim.dim = true;
        lines.push_back("  " + ui::styled("▸ Stashed (auto-restores after submit)", dim));
    }

    // The usual hint line is left out as soon as a status line is
    // configured or the suggestions are showing.
    const bool suppress_hint =
        settings_.status_line_command.has_value() || (!help_overlay_ && !suggestions_.empty());

    // The status line is left out while the suggestions are showing and
    // while the message about the exit is there, the way the reference
    // leaves it out.
    const std::string status_line =
        (suggestions_.empty() && prompt_hint().empty()) ? status_line_text_ : std::string();

    // The context warning is raised in the column of the notifications of
    // the footer, the way the reference raises it; the box does not move.
    const std::string notification = footer_notification(prompt_notification(), context_warning());

    // The reference leaves a blank row above the box.
    if (!help_overlay_) lines.push_back("");

    const auto box = input_->render(width, loading, footer_text(loading), status_line, suppress_hint,
                                    argument_hint(), notification, settings_.status_line_padding);
    for (const auto& line : box) {
        lines.push_back(line);
    }

    // The suggestions sit below the box, the way the reference puts them.
    if (!help_overlay_) {
        // The reference indents them by two columns, and shows as many of
        // them as fit on the screen: the rows that are left once the box and
        // the footer of it have taken theirs, and never more than six.
        const int rows = terminal.height();
        const size_t max_visible = static_cast<size_t>(std::min(6, std::max(1, rows - 3)));
        for (const auto& line : ui::render_suggestions(suggestions_, suggestion_index_, theme_, width,
                                                       max_visible, command_name_width())) {
            lines.push_back("  " + line);
        }
    }

    // The bar of the message actions sits below the prompt box.
    for (const auto& line : render_action_bar()) {
        lines.push_back(line);
    }

    terminal.render_dynamic(lines);
}

std::string App::select_option(const std::string& title,
                                const std::vector<commands::CommandOption>& options) {
    if (options.empty()) return {};

    std::vector<ui::SelectOption> items;
    items.reserve(options.size());
    for (const auto& option : options) {
        items.push_back({option.label, option.description});
    }

    ui::Select select(theme_, std::move(items), title);
    if (!select.run(ui::Terminal::instance())) return {};

    const size_t index = select.selected();
    if (index >= options.size()) return {};
    return options[index].value;
}

bool App::confirm(const std::string& question) {
    const std::vector<commands::CommandOption> options = {
        {"Yes", "yes", ""},
        {"No", "no", ""},
    };
    return select_option(question, options) == "yes";
}

void App::open_in_editor(const std::string& path) {
    const char* editor = std::getenv("EDITOR");
    const std::string command = std::string(editor && *editor ? editor : "vi") + " " + path;

    ui::Terminal& terminal = ui::Terminal::instance();

    // The editor wants the terminal to itself.
    terminal.leave_raw_mode();
    const int status = std::system(command.c_str());
    terminal.enter_raw_mode();

    if (status != 0) {
        terminal.write_static(
            ui::styled("The editor exited with an error.\n", ui::Style::fg(theme_.error)));
    }
}

std::string App::ask_model(const std::string& prompt,
                           const std::vector<std::string>& allowed_tools) {
    // The command's text becomes a user message and the model's answer is
    // the command's output.
    if (allowed_tools.empty()) {
        run_turn(prompt);
        return last_answer_;
    }

    // The reference gives the model only the tools the command names, and
    // allows their rules without asking.
    const std::vector<std::string> names = tool_names_of(allowed_tools);

    std::vector<std::string> rules;
    if (engine_) {
        auto& list = engine_->context().permission_context.always_allow_rules[PermissionRuleSource::CliArg];
        rules = list;
        for (const std::string& rule : allowed_tools) list.push_back(rule);
    }

    turn_tool_names_ = names;
    run_turn(prompt);
    turn_tool_names_.clear();

    if (engine_) {
        auto& list = engine_->context().permission_context.always_allow_rules[PermissionRuleSource::CliArg];
        list = rules;
    }

    return last_answer_;
}

/// The offset of the last top-level block boundary of `text`.
///
/// The reference splits the answer of the model at a block boundary, the
/// way the lexer of the reference does, so that a block of code is never
/// cut in half. An empty line that lies inside a block of code is not a
/// boundary.
size_t last_block_boundary(const std::string& text) {
    bool in_fence = false;
    char fence_char = '\0';
    size_t fence_length = 0;
    size_t boundary = std::string::npos;

    size_t at = 0;
    while (at <= text.size()) {
        const size_t end = text.find('\n', at);
        const size_t stop = end == std::string::npos ? text.size() : end;
        const std::string line = text.substr(at, stop - at);
        const std::string trimmed = cc::trim(line);

        // A fence is opened by a run of three backticks or three tildes, and
        // it is closed by a run of the same character that is at least as
        // long, the way the lexer of the reference reads it.
        char run_char = '\0';
        size_t run = 0;
        while (run < trimmed.size() && (trimmed[run] == '`' || trimmed[run] == '~')) {
            if (run_char == '\0') run_char = trimmed[run];
            if (trimmed[run] != run_char) break;
            ++run;
        }
        if (run >= 3) {
            if (!in_fence) {
                in_fence = true;
                fence_char = run_char;
                fence_length = run;
            } else if (run_char == fence_char && run >= fence_length) {
                in_fence = false;
            }
        }

        if (!in_fence && end != std::string::npos && end + 1 < text.size() && text[end + 1] == '\n') {
            boundary = end + 2;
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return boundary;
}

void App::commit_answer() {
    if (answer_.empty()) return;

    ui::Terminal& terminal = ui::Terminal::instance();

    // The answer of the turn that just finished, for the commands that ask
    // the model something.
    last_answer_ = answer_;

    // The answer leaves the live region and becomes part of the transcript.
    // Only the part that has not been committed yet is written.
    terminal.clear_dynamic();
    const auto lines = ui::render_assistant_text(answer_.substr(committed_), theme_, terminal.width(),
                                                 /*with_marker=*/committed_ == 0);
    if (!lines.empty()) {
        start_assistant_message();
        if (answer_gap_) {
            // The empty line that stands between the part of the answer
            // that is in the transcript and the part that is still live.
            terminal.write_static("\n");
            answer_gap_ = false;
        }
        // The answer is written to the transcript and the answer that grew
        // is taken away in the same drawing, so that the answer is never on
        // the screen twice.
        std::string text;
        for (const auto& line : lines) {
            text += line;
            text += "\n";
        }
        terminal.commit_live(text);
    } else {
        answer_gap_ = false;
    }
    answer_.clear();
    committed_ = 0;
}

/// Writes the empty line that stands above the message of the assistant.
///
/// The reference leaves one empty line above the message of the assistant,
/// and none between its blocks.
void App::start_assistant_message() {
    if (assistant_message_started_) return;
    assistant_message_started_ = true;
    ui::Terminal::instance().write_static("\n");
}

/// What a keybinding the user wrote asks the session to do.
///
/// Returns nothing when the port does not know the action, so that the
/// default handling of the key happens instead.
std::optional<App::PromptAction> App::handle_keybinding(const std::string& action, std::string& out,
                                                        bool loading) {
    ui::Terminal& terminal = ui::Terminal::instance();

    if (action == "app:redraw") {
        // Redraw the screen from scratch.
        terminal.write_static("\x1b[2J\x1b[H");
        return PromptAction::Handled;
    }
    if (action == "app:toggleTodos") {
        toggle_todos();
        return PromptAction::Handled;
    }
    if (action == "app:toggleTranscript") {
        show_transcript();
        return PromptAction::Handled;
    }
    if (action == "history:search") {
        search_history();
        return PromptAction::Handled;
    }
    if (action == "chat:cycleMode") {
        cycle_permission_mode();
        return PromptAction::Handled;
    }
    if (action == "chat:messageActions") {
        enter_message_actions();
        return PromptAction::Handled;
    }
    if (action == "chat:modelPicker") {
        bool handled = false;
        handle_command("/model", handled);
        return PromptAction::Handled;
    }
    if (action == "app:interrupt") {
        // The first press clears the input and asks for a second one
        // before the session ends.
        if (press_again(pending_ctrl_c_, "Ctrl-C")) {
            terminal.clear_dynamic();
            return PromptAction::Exit;
        }
        input_->clear();
        history_index_ = -1;
        return PromptAction::Handled;
    }
    if (action == "app:exit") {
        // Ctrl-D only leaves the session when the input is empty.
        if (!input_->text().empty()) return PromptAction::Handled;
        if (press_again(pending_ctrl_d_, "Ctrl-D")) {
            terminal.clear_dynamic();
            return PromptAction::Exit;
        }
        return PromptAction::Handled;
    }

    (void)out;
    (void)loading;
    return std::nullopt;
}

/// The line the spinner shows under itself.
///
/// The reference names the next task of the todo list, or a tip about the
/// commands the user has not used yet.
std::string App::spinner_tip() const {
    // The next task of the todo list.
    if (registry_) {
        const Tool* tool = find_tool_by_name(registry_->all(), "TodoWrite");
        const auto* todo_tool = dynamic_cast<const tools::TodoWriteTool*>(tool);
        if (todo_tool != nullptr) {
            for (const auto& todo : todo_tool->todos()) {
                if (todo.status != "pending") continue;
                return "Next: " + todo.content;
            }
        }
    }

    // The tips the reference shows once the turn has been going for a
    // while.
    const int64_t elapsed = spinner_ ? spinner_->elapsed_ms() : 0;
    if (elapsed > 1800000) {
        return "Tip: Use /clear to start fresh when switching topics and free up context";
    }
    if (elapsed > 30000 && btw_use_count_ == 0) {
        return "Tip: Use /btw to ask a quick side question without interrupting Claude's "
               "current work";
    }
    return {};
}

/// Turns the marker of the running tool on and off.
///
/// Claude Code blinks it every 600 ms while a tool runs.
void App::tick_blink() {
    if (!active_tool_call_) return;

    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    if (last_blink_ms_ == 0) {
        last_blink_ms_ = now;
        return;
    }
    if (now - last_blink_ms_ < 600) return;

    last_blink_ms_ = now;
    blink_on_ = !blink_on_;
}

void App::render_live() {
    ui::Terminal& terminal = ui::Terminal::instance();
    const int width = terminal.width();

    // The answer that grows.
    std::vector<std::string> growing;

    // The part of the answer that has already been committed to the
    // transcript is written there; only the rest is still live.
    const auto commit = [&](size_t end) {
        const size_t was = committed_;
        const auto done = ui::render_assistant_text(answer_.substr(was, end - was),
                                                    theme_, width,
                                                    /*with_marker=*/was == 0);
        committed_ = end;

        // The part of the answer that is still growing takes the place of
        // the part that is written to the transcript, in the same drawing:
        // the answer is never on the screen twice.
        growing = ui::render_assistant_text(answer_.substr(committed_), theme_, width,
                                            /*with_marker=*/committed_ == 0);

        if (done.empty()) {
            terminal.set_live_lines(growing);
            return;
        }

        start_assistant_message();

        // The whole of the part is written in one go, so that the screen is
        // drawn again once and not once per line.
        std::string text;
        for (const auto& line : done) {
            text += line;
            text += "\n";
        }
        terminal.commit_live(text, growing);

        // The empty line that stands between the part of the answer
        // that is in the transcript and the part that is still live. The
        // reference leaves one line between two blocks of an answer.
        answer_gap_ = true;
    };

    // Everything up to the last block of the answer has already been
    // committed to the transcript; only the last block is still live. The
    // boundary is a block boundary, so that a block of code is never cut in
    // half.
    //
    // The full-screen layout keeps the whole answer in the transcript it
    // holds in memory, the way the reference does: the answer grows in the
    // place where it began, and it is written to the transcript when the
    // turn is over.
    const bool split = !terminal.fullscreen();
    if (split) {
        const size_t boundary = last_block_boundary(answer_);
        if (boundary != std::string::npos && boundary > committed_) {
            commit(boundary);
        }
    }

    // The lines of the region that are not the answer: the tool that is
    // running, the spinner, the messages that wait and the prompt box.
    std::vector<std::string> others;

    // The progress line of the tool that is running, and the output it has
    // produced so far.
    if (awaiting_permission_) {
        others.push_back(std::string(ui::figures::kMessageResponse) +
                         ui::styled("Waiting for permission…", ui::Style::fg(theme_.inactive)));
    }

    // The tools that wait for their turn are not shown in the region: their
    // lines join the transcript when the tool ends, the way the reference
    // shows them, so that they do not move.

    // The spinner, then the prompt box. The reference counts the tokens the
    // answer is worth — a quarter of the characters it has — rather than
    // the tokens the session has used.
    const int64_t tokens = static_cast<int64_t>(answer_.size() / 4);

    // While a todo is being worked on, the spinner names it.
    if (registry_) {
        const Tool* tool = find_tool_by_name(registry_->all(), "TodoWrite");
        const auto* todo_tool = dynamic_cast<const tools::TodoWriteTool*>(tool);
        if (todo_tool != nullptr) {
            for (const auto& todo : todo_tool->todos()) {
                if (todo.status != "in_progress") continue;
                const std::string& verb = todo.active_form.empty() ? todo.content : todo.active_form;
                if (!verb.empty()) spinner_->set_verb(verb);
                break;
            }
        }
    }

    // The reference hides the spinner while the answer streams: the text
    // itself is the feedback then.
    if (answer_.size() <= committed_) {
        others.push_back(spinner_->line(width, tokens, verbose_));

        // The line under the spinner names the next task of the todo list,
        // or a tip.
        const std::string tip = spinner_tip();
        if (!tip.empty()) {
            others.push_back(std::string(ui::figures::kMessageResponse) +
                             ui::styled(tip, ui::Style::fg(theme_.inactive)));
        }
    }

    // The messages the user sent while the turn was running wait above the
    // prompt box, the way the reference shows its queue.
    for (const auto& queued : queued_prompts_) {
        others.push_back(user_message_line(queued));
    }

    // The status line is left out while the suggestions are showing and
    // while the message about the exit is there, the way the reference
    // leaves it out.
    const std::string status_line =
        (suggestions_.empty() && prompt_hint().empty()) ? status_line_text_ : std::string();

    const auto box = input_->render(width, /*loading=*/true, footer_text(true), status_line,
                                    settings_.status_line_command.has_value(), argument_hint(),
                                    footer_notification(prompt_notification(), context_warning()),
                                    settings_.status_line_padding);
    for (const auto& line : box) {
        others.push_back(line);
    }

    // The answer as it grows.
    if (!answer_.empty()) {
        growing = ui::render_assistant_text(answer_.substr(committed_), theme_, width,
                                            /*with_marker=*/committed_ == 0);
        // The reference leaves an empty line above the answer that grows, so
        // the answer does not jump down a line when it is committed.
        if (!growing.empty()) start_assistant_message();
    }

    // The empty line that stands between the part of the answer that is
    // already in the transcript and the part that is still live.
    if (answer_gap_ && !growing.empty()) {
        terminal.write_static("\n");
        answer_gap_ = false;
    }

    // The region may not be taller than the screen: the region is erased
    // and drawn again on every frame, so a region that is taller than the
    // screen could not be erased and the frames would pile up in the
    // scrollback. The part of the answer that does not fit is written to
    // the transcript instead, the way the reference scrolls it up.
    //
    // The height of the region is counted in rows, not in lines: a line
    // that is wider than the terminal takes a row for every part of it that
    // does not fit.
    //
    // The full-screen layout keeps the answer in the transcript it holds in
    // memory, so the region of the prompt is the only thing that has to fit
    // on the screen there.
    const int room = std::max(1, terminal.height() - terminal.rows_of_lines(others));
    while (split && terminal.rows_of_lines(growing) > room && committed_ < answer_.size()) {
        size_t cut = answer_.find('\n', committed_);
        if (cut != std::string::npos) {
            ++cut;
        } else {
            // A streamed paragraph can be one physical line for its entire
            // lifetime. Commit a word-bounded prefix in that case so the
            // live region stays short enough to redraw with the prompt.
            std::vector<size_t> cuts;
            size_t at = committed_;
            while ((at = answer_.find_first_of(" \t\r\n", at)) != std::string::npos) {
                cuts.push_back(++at);
            }
            cuts.push_back(answer_.size());

            size_t low = 0;
            size_t high = cuts.size();
            while (low < high) {
                const size_t middle = low + (high - low) / 2;
                const auto tail = ui::render_assistant_text(answer_.substr(cuts[middle]),
                                                            theme_, width,
                                                            /*with_marker=*/false);
                if (terminal.rows_of_lines(tail) <= room) high = middle;
                else low = middle + 1;
            }
            if (low == cuts.size()) break;
            cut = cuts[low];
        }
        if (cut <= committed_) break;

        commit(cut);
        growing = ui::render_assistant_text(answer_.substr(committed_), theme_, width,
                                            /*with_marker=*/committed_ == 0);
    }

    // The answer that grows stands right under the text that came before it:
    // in the classic layout it is the first part of the dynamic region, in
    // the full-screen layout it is a part of the transcript, which is kept
    // in memory. The empty space of the screen is between it and the box of
    // the prompt, so that the box stands at the bottom of the screen.
    terminal.set_live_lines(growing);
    terminal.render_dynamic(others);
}

/// The title of the permission dialog for a tool.
///
/// The reference has a dialog per tool, each with its own title.
std::string permission_title(const Tool& tool, const boost::json::object& input,
                             const std::string& cwd) {
    const std::string_view name = tool.name();
    if (name == tools::READ) return "Read file";
    if (name == tools::EDIT) return "Edit file";
    if (name == tools::WRITE) {
        // The reference says whether the file is made or written over.
        const std::string path = cc::jget_string(input, "file_path");
        return cc::file_exists(cc::resolve(path, cwd)) ? "Overwrite file" : "Create file";
    }
    if (name == tools::NOTEBOOK_EDIT) return "Edit notebook";
    if (name == tools::SKILL) {
        // The reference names the skill in the title of the dialog.
        std::string skill = cc::jget_string(input, "skill");
        if (skill.empty()) skill = cc::jget_string(input, "name");
        return "Use skill \"" + skill + "\"?";
    }
    if (name == tools::BASH) return "Bash command";
    if (name == tools::WEB_FETCH) return "Fetch";
    if (name == tools::ENTER_PLAN_MODE) return "Enter plan mode?";
    if (name == tools::EXIT_PLAN_MODE) return "Exit plan mode?";
    return "Tool use";
}

/// The question the permission dialog asks for a tool.
///
/// The reference asks a different question for every tool.
std::string permission_question(const Tool& tool, const boost::json::object& input,
                                const std::string& cwd) {
    const std::string_view name = tool.name();

    // The name of the file, in bold, the way the reference writes it.
    ui::Style bold;
    bold.bold = true;

    if (name == tools::EDIT) {
        const std::string path = cc::jget_string(input, "file_path");
        return "Do you want to make this edit to " + ui::styled(cc::basename(path), bold) + "?";
    }
    if (name == tools::WRITE) {
        const std::string path = cc::jget_string(input, "file_path");
        const bool exists = cc::file_exists(cc::resolve(path, cwd));
        return std::string("Do you want to ") + (exists ? "overwrite " : "create ") +
               ui::styled(cc::basename(path), bold) + "?";
    }
    if (name == tools::NOTEBOOK_EDIT) {
        const std::string path = cc::jget_string(input, "notebook_path");
        const std::string mode = cc::jget_string(input, "edit_mode");
        std::string what = "make this edit to";
        if (mode == "insert") what = "insert this cell into";
        if (mode == "delete") what = "delete this cell from";
        return "Do you want to " + what + " " + ui::styled(cc::basename(path), bold) + "?";
    }
    return "Do you want to proceed?";
}

std::string App::permission_scope(const Tool& tool, const boost::json::object& input) const {
    // The rule names the tool and, where it makes sense, the exact action:
    // `Bash(git status)` or `Read(/path/to/file)`.
    if (tool.name() == "Bash") {
        // A shell command is saved as a prefix rule, the way the reference
        // saves it: `git status` becomes `Bash(git status:*)`, so that the
        // rule still holds when the command grows an argument.
        const std::string command = jget_string(input, "command");
        const std::string prefix = permissions::command_prefix_rule(command);
        if (!prefix.empty()) return "Bash(" + prefix + ":*)";
        return "Bash(" + command + ")";
    }
    const auto path = tool.get_path(input);
    if (path) {
        return std::string(tool.name()) + "(" + *path + ")";
    }
    return {};
}

std::optional<permissions::ClassifierOutcome> App::classify_action(
    const Tool& tool, const boost::json::object& input) {
    if (!engine_) return std::nullopt;

    // The tools that only read are not classified, the way the reference
    // does not classify them.
    if (permissions::classifier_allowlisted(tool.name())) return std::nullopt;

    // The action is described the way the reference describes it: the tool
    // with its arguments.
    std::string action = std::string(tool.name());
    const std::string args = ui::user_facing_tool_args(tool, input, options_.cwd);
    if (!args.empty()) action += "(" + args + ")";

    // The transcript of the session: the words of the user and the calls of
    // the tools, the way the reference sends it.
    std::string transcript;
    for (const auto& message : engine_->messages()) {
        if (message.role != Role::User) continue;
        std::string text;
        for (const auto& block : message.content) {
            const TextBlock* part = as_text(block);
            if (part == nullptr) continue;
            if (!text.empty()) text += "\n";
            text += part->text;
        }
        if (text.empty()) continue;
        transcript += "User: " + text + "\n\n";
    }

    // The question is asked in an engine of its own: the tools of the
    // session are taken away, and only the tool of the classifier is left.
    query::Engine::Options options = engine_->options();
    options.tools.clear();
    options.max_turns = 1;
    options.hooks = query::EngineHooks{};

    // The tools of the session are not asked for again: the classifier has
    // only the tool of its own.
    options.tools_provider = nullptr;

    // The prompt of the session is not the prompt of the classifier.
    options.system_prompt_provider = nullptr;
    options.system_prompt = permissions::classifier_prompt(action, transcript);

    auto classifier = std::make_shared<permissions::ClassifierTool>();
    options.tools.push_back(classifier);

    query::Engine side(std::move(options));
    side.set_messages({});

    auto task = side.run_turn(
        "Classify the action that is described above. Answer with the classify_result tool.");
    task.start();

    while (!task.done()) {
        io_context_.run_one_for(std::chrono::milliseconds(16));
        render_live();
    }

    try {
        task.get();
    } catch (const std::exception&) {
        // The classifier could not answer: the user is asked.
        return std::nullopt;
    }

    if (!classifier->answered()) return std::nullopt;

    permissions::ClassifierOutcome outcome;
    outcome.decision = classifier->should_block() ? permissions::ClassifierDecision::Deny
                                                  : permissions::ClassifierDecision::Allow;
    outcome.reason = classifier->reason();
    return outcome;
}

/// The message the terminal is given when the session wants the user.
///
/// The reference words the message after the tool that is waiting, the way
/// it does.
std::string App::permission_notification(const Tool& tool, const boost::json::object& input) {
    const std::string_view name = tool.name();
    if (name == tools::EXIT_PLAN_MODE) return "Claude Code needs your approval for the plan";
    if (name == tools::ENTER_PLAN_MODE) return "Claude Code wants to enter plan mode";

    const std::string tool_name = ui::user_facing_tool_name(tool, input);
    if (tool_name.empty()) return "Claude Code needs your attention";
    return "Claude needs your permission to use " + tool_name;
}

PermissionResult App::request_permission(const Tool& tool, const boost::json::object& input,
                                         const PermissionResult& request) {
    // The auto mode asks a model of its own whether the call may run. The
    // reference runs its classifier before the question is asked, so the
    // user is only interrupted when the model says that it is needed.
    if (engine_ && engine_->context().permission_context.mode == PermissionMode::Auto) {
        // The tools that only read are not classified, the way the reference
        // does not classify them: they run without a question.
        if (permissions::classifier_allowlisted(tool.name())) {
            PermissionResult result;
            result.behavior = PermissionBehaviorKind::Allow;
            DecisionReason reason;
            reason.type = DecisionReasonType::Classifier;
            reason.classifier = "auto-mode";
            reason.reason = "The tool only reads.";
            result.decision_reason = std::move(reason);
            return result;
        }

        const auto decision = classify_action(tool, input);
        if (decision.has_value()) {
            PermissionResult result;
            DecisionReason reason;
            reason.type = DecisionReasonType::Classifier;
            reason.classifier = "auto-mode";

            reason.reason = decision->reason;

            if (decision->decision == permissions::ClassifierDecision::Allow) {
                result.behavior = PermissionBehaviorKind::Allow;
                result.decision_reason = std::move(reason);
                return result;
            }

            // The classifier blocked the call: the model is told why.
            result.behavior = PermissionBehaviorKind::Deny;
            result.message = permissions::classifier_rejection_message(decision->reason);
            result.decision_reason = std::move(reason);
            return result;
        }
    }

    // The questions of the model are asked where the permission of the tool
    // would be asked: the reference shows the choices of the question in the
    // dialog of the permission, so that the user answers the question and
    // the tool runs at once, instead of approving the tool and then being
    // asked the question.
    if (tool.name() == tools::ASK_USER_QUESTION) {
        return ask_the_questions(input);
    }

    ui::Terminal& terminal = ui::Terminal::instance();

    const std::string scope = permission_scope(tool, input);

    // What the request is about: the reference words the choices of a file
    // differently from the ones of a shell command.
    const std::string_view name = tool.name();
    ui::PermissionKind kind = ui::PermissionKind::Shell;
    if (name == tools::READ) {
        kind = ui::PermissionKind::Read;
    } else if (name == tools::EDIT || name == tools::WRITE || name == tools::NOTEBOOK_EDIT) {
        kind = ui::PermissionKind::Write;
    }

    std::string path = cc::jget_string(input, "file_path");
    if (path.empty()) path = cc::jget_string(input, "notebook_path");

    const auto options = ui::permission_options(std::string(tool.name()), kind, path, scope,
                                                 options_.cwd, /*always_allow=*/!scope.empty(), theme_);

    awaiting_permission_ = true;
    render_live();

    // The dialog says what is being approved, the way the reference says
    // it: the tool with its arguments. A shell command is shown on its
    // own.
    std::string body;
    std::string description;
    if (name == tools::BASH) {
        body = cc::jget_string(input, "command");
        // The reference shows what the command does underneath it.
        description = cc::jget_string(input, "description");
    } else {
        body = ui::user_facing_tool_name(tool, input);
        const std::string args = ui::user_facing_tool_args(tool, input, options_.cwd);
        if (!args.empty()) body += "(" + args + ")";
    }

    // The change is shown in the IDE when the session talks to one and the
    // tool of the differences is the automatic one, the way the reference
    // shows it. The answer of the IDE is waited for: the file may be saved
    // there, which accepts the change.
    std::shared_ptr<std::atomic<int>> ide_answer;
    std::string ide_tab;
    if (kind == ui::PermissionKind::Write && !path.empty() &&
        config::global_string("diffTool", "auto") == "auto" && connected_to_ide()) {
        ide_answer = std::make_shared<std::atomic<int>>(0);
        ide_tab = ide_tab_name(path);
        open_diff_in_ide(path, input, ide_answer, ide_tab);

        // The change is in the IDE, so the dialog says so, and the diff is
        // not shown in the terminal.
        body.clear();
        description = "Save file to continue…";
    }

    std::string title = permission_title(tool, input, options_.cwd);
    if (ide_answer) {
        const auto ide = connected_ide();
        title = "Opened changes in " + (ide ? ide->name : std::string("IDE")) + " ⧉";
    }

    ui::PermissionPrompt prompt(theme_, title, permission_question(tool, input, options_.cwd),
                                options, body, description, kind);

    // The IDE may answer while the user is reading the question.
    if (ide_answer) {
        prompt.set_answer_provider([ide_answer] {
            const int answer = ide_answer->load();
            if (answer == 1) return std::string("yes");
            if (answer == 2) return std::string("no");
            return std::string();
        });
    }

    // The terminal is told that the session wants the user, when nobody has
    // touched the keyboard for a while; the reference words the message the
    // same way.
    prompt.set_notification(permission_notification(tool, input));
    std::string feedback;
    const std::string choice = prompt.run(terminal, &feedback);

    // The tab of the IDE is closed when the user answered in the terminal.
    if (ide_answer) close_diff_in_ide(ide_tab);

    awaiting_permission_ = false;

    PermissionResult result;
    if (choice == "yes" || choice == "yes-session" || choice == "yes-dont-ask-again") {
        result.behavior = PermissionBehaviorKind::Allow;
        // The instructions the user gave while they approved the call
        // travel with the decision, the way the reference hands them over.
        result.accept_feedback = feedback;

        if (choice == "yes-session") {
            // The reference keeps the rule of a file in the memory of the
            // session: it writes nothing to the settings.
            const std::string rule = ui::permission_session_rule(kind, path, options_.cwd);
            if (!rule.empty()) {
                engine_->context().permission_context.always_allow_rules[PermissionRuleSource::Session]
                    .push_back(rule);
            }
        } else if (choice == "yes-dont-ask-again" && !scope.empty()) {
            // Remember the choice, so the same call does not ask again. The
            // reference writes it to the local settings file.
            auto& rules = engine_->context().permission_context.always_allow_rules;
            rules[PermissionRuleSource::LocalSettings].push_back(scope);
            remember_permission(scope);
        }
        return result;
    }

    result.behavior = PermissionBehaviorKind::Deny;
    result.message = request.message.empty() ? messages::auto_reject_message(std::string(tool.name()))
                                             : request.message;
    return result;
}

PermissionResult App::ask_the_questions(const boost::json::object& input) {
    ui::Terminal& terminal = ui::Terminal::instance();

    const std::vector<UserQuestion> questions = tools::user_questions(input);
    if (questions.empty()) {
        // There is nothing to ask: the tool runs as it is.
        PermissionResult result;
        result.behavior = PermissionBehaviorKind::Allow;
        return result;
    }

    awaiting_permission_ = true;
    render_live();

    // The reference shows every question of the call in one dialog: the
    // bar at the top holds them all, and the arrow keys move from one to
    // the next.
    ui::QuestionDialog dialog(theme_, questions.front());
    dialog.set_questions(questions, 0);

    const UserAnswers answers = dialog.run(terminal);

    awaiting_permission_ = false;

    // No answer means the user declined to answer.
    if (answers.empty()) {
        PermissionResult result;
        result.behavior = PermissionBehaviorKind::Deny;
        result.message = "User declined to answer questions";
        return result;
    }

    // The answers go into the input of the tool, the way the reference puts
    // them there: the tool reads them out of its input and does not ask
    // again.
    boost::json::object updated = input;
    boost::json::object answers_object;
    for (const auto& [question, answer] : answers) {
        answers_object[question] = boost::json::value(answer);
    }
    updated["answers"] = boost::json::value(std::move(answers_object));

    PermissionResult result;
    result.behavior = PermissionBehaviorKind::Allow;
    result.updated_input = std::move(updated);
    return result;
}

void App::remember_permission(const std::string& scope) {
    const std::string path = config::local_settings_path(options_.cwd);

    boost::json::value contents(boost::json::object{});
    if (const auto existing = read_file(path)) {
        contents = json_parse(*existing, boost::json::value(boost::json::object{}));
    }
    if (!contents.is_object()) {
        contents = boost::json::value(boost::json::object{});
    }

    boost::json::object object = contents.as_object();
    boost::json::object permissions = jget_object(object, "permissions");
    boost::json::array allow = jget_array(permissions, "allow");
    allow.emplace_back(scope);
    permissions["allow"] = std::move(allow);
    object["permissions"] = std::move(permissions);

    config::write_settings(path, boost::json::value(std::move(object)));
}

std::string App::run_sub_agent(const std::string& system_prompt, const std::string& task,
                                const std::vector<std::string>& allowed,
                                const std::string& model) {
    // The sub-agent gets its own event loop, so the session's loop keeps
    // running while the sub-agent works.
    boost::asio::io_context sub_context;

    const std::string environment_token = auth::auth_token();
    const std::string sub_model_name = model.empty() ? model_name_ : model;
    ai::providers::anthropic::AnthropicOptions provider_options{
        .api_key = api_key(),
        .auth_token = environment_token.empty() ? std::nullopt
                                                : std::optional<std::string>(environment_token),
        .base_url = auth::base_api_url(),
        .anthropic_beta = beta_headers(sub_model_name, /*oauth=*/false),
        .io_context = sub_context,
    };
    // The client of the API is one whose answer can be cut short: the
    // request is aborted when the user interrupts the turn.
    provider_options.http_client = std::make_shared<query::CancellableClient>(sub_context);
    auto provider = std::static_pointer_cast<ai::providers::anthropic::AnthropicProvider>(
        ai::providers::anthropic::create_anthropic(provider_options));

    query::Engine::Options sub_options;
    sub_options.model = provider->language_model(query::model_for_api(sub_model_name));
    sub_options.model_name = sub_model_name;
    sub_options.cwd = options_.cwd;
    sub_options.system_prompt = system_prompt;
    sub_options.max_turns = options_.args.max_turns;

    // The sub-agent only gets the tools it is allowed to use.
    for (const auto& tool : registry_->all()) {
        if (!tool) continue;
        if (!allowed.empty() &&
            std::find(allowed.begin(), allowed.end(), std::string(tool->name())) == allowed.end()) {
            continue;
        }
        sub_options.tools.push_back(tool);
    }

    std::string answer;
    sub_options.hooks.on_text = [&answer](const std::string& delta) { answer += delta; };

    query::Engine sub_engine(std::move(sub_options));

    std::thread worker([&] {
        auto sub_task = sub_engine.run_turn(task);
        sub_task.start();
        while (!sub_task.done()) {
            sub_context.run_one();
        }
        try {
            sub_task.get();
        } catch (const std::exception&) {
            // The answer holds whatever the sub-agent managed to produce.
        }
    });
    worker.join();

    return answer;
}

void App::show_help_overlay() {
    ui::Terminal& terminal = ui::Terminal::instance();

    std::vector<ui::HelpCommand> builtin;
    for (const auto& command : commands::all()) {
        if (!commands::is_available(command)) continue;
        builtin.push_back({"/" + command.name, command.description});
    }

    // The commands the user wrote themselves are listed in their own tab.
    // Their description names the place they come from, the way the
    // reference names it.
    std::vector<ui::HelpCommand> custom;
    for (const auto& command : custom_commands_) {
        custom.push_back({"/" + command.name, commands::format_description_with_source(command)});
    }

    // The prompts of the MCP servers are listed there as well; the reference
    // lists every command that is not built-in in that tab.
    for (const auto& command : mcp_commands_) {
        custom.push_back({"/" + command.name, command.description});
    }

    help_overlay_ = std::make_unique<ui::HelpOverlay>(theme_, builtin, std::move(custom));

    while (help_overlay_) {
        render_prompt_box(false);
        const auto key = terminal.read_key(100);
        if (!key) {
            if (terminal.closed()) break;
            continue;
        }
        if (!help_overlay_->handle_key(*key)) help_overlay_.reset();
    }

    render_prompt_box(false);
}

std::optional<ui::Key> App::show_shortcuts_menu() {
    ui::Terminal& terminal = ui::Terminal::instance();
    const int width = terminal.width();

    // The reference indents the menu by two columns.
    const auto menu = ui::PromptInput::shortcuts_menu(width > 2 ? width - 2 : width);

    while (true) {
        // The menu sits below the prompt box, the way the reference puts
        // it; the box itself stays on the screen.
        std::vector<std::string> lines;
        const auto box = input_->render(width, /*loading=*/false, /*hint=*/{}, /*status_line=*/{},
                                        /*suppress_hint=*/true);
        for (const auto& line : box) {
            lines.push_back(line);
        }
        for (const auto& line : menu) {
            lines.push_back("  " + line);
        }
        terminal.render_dynamic(lines);

        const auto key = terminal.read_key(100);
        if (!key) {
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return std::nullopt;
            }
            continue;
        }
        if (key->type == ui::Key::Type::Escape || key->type == ui::Key::Type::Enter ||
            key->type == ui::Key::Type::CtrlC || key->type == ui::Key::Type::Character) {
            terminal.clear_dynamic();
            // The key that closed the menu is given back, so that a
            // character typed while the menu was up still reaches the input.
            return key;
        }
    }
}

std::vector<std::string> App::render_tool_call(const query::ToolCallInfo& call, bool marker_visible,
                                               ui::ToolCallState state) {
    Tool* tool = find_tool_by_name(registry_->all(), call.name);
    if (!tool) return {};

    // The tool that enters the plan mode has no line of its own; the mode
    // is named by its result.
    if (call.name == tools::ENTER_PLAN_MODE) return {};

    const std::string line = ui::render_tool_use_line(*tool, call.input, options_.cwd, theme_, state,
                                                      marker_visible);
    // Some tools have no line of their own.
    if (line.empty()) return {};
    return {line};
}

std::vector<std::string> App::render_tool_result(const query::ToolCallInfo& call,
                                                  const ToolResult& result, bool expand) {
    const int width = ui::Terminal::instance().width();

    // The tool that enters the plan mode names the mode and explains it,
    // the way the reference does.
    if (call.name == tools::ENTER_PLAN_MODE && !result.is_error) {
        ui::Style marker;
        marker.foreground = theme_.plan_mode;

        ui::Style dim;
        dim.foreground = theme_.inactive;

        return {ui::styled(std::string(ui::figures::kBlackCircle), marker) + " Entered plan mode",
                "  " + ui::styled("Claude is now exploring and designing an implementation approach.",
                                 dim)};
    }

    // A file edit shows the diff it produced rather than its message.
    std::vector<std::string> lines;
    if (result.tool_use_result.is_object()) {
        const auto& payload = result.tool_use_result.as_object();
        const auto before = payload.find("oldContent");
        const auto after = payload.find("newContent");
        if (before != payload.end() && after != payload.end() && before->value().is_string() &&
            after->value().is_string()) {
            lines = ui::render_diff_result(std::string(before->value().as_string().c_str()),
                                           std::string(after->value().as_string().c_str()),
                                           theme_, width);
        }
    }
    if (lines.empty()) {
        if (expand) {
            // The transcript shows the whole of the result: it is the view
            // the "ctrl+o to expand" of the result refers to.
            lines = ui::render_tool_result_lines(call.name, result.content, result.tool_use_result,
                                                 options_.cwd, theme_, width, result.is_error,
                                                 /*verbose=*/true);
        } else if (verbose_) {
            // The detailed view shows the tool's own output rather than the
            // summary of it.
            lines = ui::render_tool_result_lines(result.content, theme_, width, result.is_error);
        } else {
            lines = ui::render_tool_result_lines(call.name, result.content, result.tool_use_result,
                                                 options_.cwd, theme_, width, result.is_error,
                                                 verbose_);
        }
    }

    return lines;
}

/// True for the tools the reference collapses into one line when they run
/// one after another.
bool App::is_collapsible_tool(const std::string& name) const {
    return name == tools::READ || name == tools::GREP || name == tools::GLOB;
}

/// True for the tools that show a progress message of their own.
///
/// The reference gives the shell a line of its own under the tool; the file
/// tools have none.
bool App::tool_has_progress_message(const std::string& name) const {
    return name == tools::BASH || name == tools::POWERSHELL;
}

std::string App::tool_group_line(bool active) const {
    // The reference counts the files and not the calls, so that a file that
    // is read twice is counted once.
    const size_t reads = group_read_paths_.empty() ? group_reads_ : group_read_paths_.size();

    // The number stands out of the dim line it is in.
    const auto count = [](size_t number) {
        return "\x1b[1m" + std::to_string(number) + "\x1b[22m";
    };

    std::string text;
    if (group_searches_ > 0) {
        const std::string verb = active ? "Searching for" : "Searched for";
        text += verb + " " + count(group_searches_) + " " +
                (group_searches_ == 1 ? "pattern" : "patterns");
    }
    if (reads > 0) {
        // The first part of the line begins with a capital and the parts that
        // follow it do not, the way the reference writes it.
        const bool first = text.empty();
        if (!first) text += ", ";
        const std::string verb = active ? (first ? "Reading" : "reading")
                                        : (first ? "Read" : "read");
        text += verb + " " + count(reads) + " " + (reads == 1 ? "file" : "files");
    }
    return text;
}

void App::flush_tool_group() {
    if (group_count_ == 0) return;

    ui::Terminal& terminal = ui::Terminal::instance();

    if (verbose_) {
        // The detailed view shows every call of the group.
        for (const auto& line : group_lines_) {
            terminal.write_static(line + "\n");
        }
    } else {
        // The reads and searches collapse into one line, the way Claude
        // Code shows them: a single read is folded into it as well.
        ui::Style dim;
        dim.foreground = theme_.inactive;

        // The line sits in the gutter the reference leaves for it.
        terminal.write_static("  " + ui::styled(tool_group_line(false), dim) + " " +
                              ui::styled("(ctrl+o to expand)", dim) + "\n");
    }

    group_lines_.clear();
    group_count_ = 0;
    group_searches_ = 0;
    group_reads_ = 0;
    group_read_paths_.clear();
    group_hint_.clear();
}

/// Says how long the turn that has just ended took.
///
/// The reference says it under the answer, with a verb of its own and the
/// time the turn took; the message is left out when the user asked for the
/// turn not to be timed.
void App::show_turn_duration() {
    if (!config::global_flag("showTurnDuration", false)) return;
    if (turn_started_ms_ == 0) return;

    const int64_t elapsed = cc::now_ms() - turn_started_ms_;
    turn_started_ms_ = 0;

    // The verb is picked at random, the way the reference picks it.
    const std::string& verb = kTurnVerbs[static_cast<size_t>(cc::now_ms() % kTurnVerbs.size())];

    ui::Style dim;
    dim.foreground = theme_.inactive;

    ui::Terminal::instance().write_static(
        std::string(ui::figures::kMessageResponse) +
        ui::styled(verb + " for " + ui::format_duration(elapsed), dim) + "\n");
}

void App::render_assistant_text(const std::string& text) {
    ui::Terminal& terminal = ui::Terminal::instance();
    const auto lines = ui::render_markdown(text, theme_, terminal.width());
    for (const auto& line : lines) {
        terminal.write_static(line + "\n");
    }
}

void App::run_turn(const std::string& prompt, bool record_prompt) {
    ui::Terminal& terminal = ui::Terminal::instance();

    answer_.clear();
    committed_ = 0;
    spinner_->reset();
    // The request is on its way to the model.
    spinner_->set_mode(ui::SpinnerMode::Requesting);
    queued_tool_calls_.clear();

    // The time the turn takes is measured from here.
    turn_started_ms_ = cc::now_ms();

    auto task = engine_->run_turn(prompt, record_prompt);
    task.start();

    // The live region holds the answer as it grows, the spinner and the
    // prompt box.
    render_live();

    while (!task.done()) {
        io_context_.run_one_for(std::chrono::milliseconds(16));

        // The work the engine asked for is done here, by the thread that
        // owns the terminal.
        drain_ui();

        // The prompt box stays usable while the turn runs. A message the
        // user sends now joins the turn that is running, the way Claude
        // Code does it.
        while (const auto key = terminal.read_key(0)) {
            if (key->type == ui::Key::Type::Escape || key->type == ui::Key::Type::CtrlC) {
                // Escape and Ctrl-C interrupt the turn: the request is
                // cancelled and the answer is cut short. The reference
                // claims the key of the cancel while a task is running,
                // so the second press is not asked for.
                interrupted_ = true;
                engine_->cancel();
                break;
            }
            std::string queued;
            handle_prompt_key(*key, queued, /*loading=*/true);
        }

        spinner_->tick();
        tick_blink();
        render_live();
    }

    // The work the engine asked for before it finished.
    drain_ui();

    try {
        task.get();
    } catch (const std::exception& error) {
        terminal.clear_dynamic();
        terminal.write_static("\n" +
                              ui::styled(query::api_error_message(error), ui::Style::fg(theme_.error)) +
                              "\n");
    }

    // The answer joins the transcript and the prompt box comes back.
    commit_answer();

    // The reads and searches that ran one after another are shown now.
    flush_tool_group();

    // The time the turn took is said, the way the reference says it.
    show_turn_duration();

    // An interrupted turn says so, the way the reference does. The marker
    // also joins the conversation, so the model knows the request was cut
    // short.
    if (interrupted_) {
        interrupted_ = false;

        Message marker;
        marker.role = Role::User;
        marker.uuid = uuid_v4();
        marker.session_id = session_id_;
        marker.cwd = options_.cwd;
        marker.version = version();
        marker.content.push_back(TextBlock{"[Request interrupted by user]", false});

        std::vector<Message> messages = engine_->messages();
        messages.push_back(marker);
        engine_->set_messages(std::move(messages));
        if (transcript_) transcript_->append(marker);

        terminal.clear_dynamic();
        terminal.write_static(std::string(ui::figures::kMessageResponse) +
                              ui::styled("Interrupted · What should Claude do instead?",
                                         ui::Style::fg(theme_.inactive)) +
                              "\n");
    }

    terminal.clear_dynamic();

    // The goal of the session is checked when the turn is over: the
    // session goes on working until the goal is met, the way the reference
    // goes on working.
    if (!goal_.empty() && !interrupted_) {
        check_goal();
        return;
    }

    // The session compacts itself when the context is full, the way the
    // reference does it.
    maybe_auto_compact();

    // The status line carries the state of the conversation, so it is
    // refreshed once the turn is over.
    refresh_status_line();

    render_prompt_box(false);
}

bool App::handle_command(const std::string& text, bool& handled) {
    handled = false;
    if (!starts_with(text, "/")) return false;

    handled = true;

    // The servers connect in the background, so the prompts of a server may
    // have arrived since the last time the box was drawn.
    refresh_mcp_commands();

    const auto parsed = parse_slash_command(text);
    if (!parsed) {
        // The reference says the commands are written `/command [args]`.
        ui::Terminal::instance().write_static(
            ui::styled("Commands are in the form `/command [args]`", ui::Style::fg(theme_.error)) + "\n");
        return true;
    }

    const std::string& name = parsed->command_name;
    const std::string& argument = parsed->args;

    // A prompt of an MCP server is a command as well: the server is asked
    // for the prompt and its text is sent to the model.
    if (parsed->is_mcp) {
        for (const auto& command : mcp_commands_) {
            if (command.user_facing_name != name) continue;

            const std::vector<std::string> arguments = split_any(argument, " ");
            std::string error;
            const std::string text = mcp_->run_command(command, arguments, &error);
            if (!error.empty()) {
                ui::Terminal::instance().write_static(
                    ui::styled("Error: " + error, ui::Style::fg(theme_.error)) + "\n");
                return true;
            }

            run_turn(commands::command_metadata(command.name, argument) + "\n" + text);
            return true;
        }
    }

    // A command the user wrote is a prompt: its body is sent to the model.
    for (const auto& command : custom_commands_) {
        if (command.name != name) continue;

        // The use of the command is remembered, so that the ones that are
        // used often float to the top of the list.
        config::record_skill_usage(command.name);

        std::string prompt = commands::substitute_arguments(command.content, argument);
        // A command of a plugin knows the place its plugin lives in.
        if (!command.plugin_root.empty()) {
            prompt = plugins::substitute_plugin_variables(prompt, command.plugin_root,
                                                          command.plugin_id);
        }
        run_turn(commands::command_metadata(command.name, argument) + "\n" + prompt);
        return true;
    }

    // The commands live in their own module; this only fills in what they
    // need to know about the session.
    commands::CommandContext context;
    context.cwd = options_.cwd;
    context.model = model_name_;
    context.version = version();
    context.settings = &settings_;
    context.messages = &engine_->messages();
    context.usage = &engine_->usage();
    context.sessions = &session_ids_;
    context.clear_conversation = [this] {
        engine_->set_messages({});
        ui::Terminal::instance().write_static("\x1b[2J\x1b[H");
    };
    context.replace_conversation = [this](const std::string& summary) {
        // The summary replaces the conversation as a single meta message.
        Message compacted;
        compacted.role = Role::User;
        compacted.uuid = uuid_v4();
        compacted.session_id = session_id_;
        compacted.cwd = options_.cwd;
        compacted.version = version();
        compacted.is_meta = true;
        compacted.is_compact_summary = true;
        compacted.content.push_back(TextBlock{summary, false});
        engine_->set_messages({compacted});

        // The transcript says that the conversation was compacted.
        const size_t count = engine_->messages().size();
        ui::Terminal& terminal = ui::Terminal::instance();

        ui::Style bold;
        bold.bold = true;
        ui::Style dim;
        dim.foreground = theme_.inactive;

        terminal.write_static(std::string(ui::figures::kBlackCircle) + " " +
                              ui::styled("Summarized conversation", bold) + "\n");
        terminal.write_static(std::string(ui::figures::kMessageResponse) +
                              ui::styled("Summarized " + std::to_string(count) +
                                             " messages from this point",
                                         dim) +
                              "\n");
        terminal.write_static(std::string(ui::figures::kMessageResponse) +
                              ui::styled("(ctrl+o to expand)", dim) + "\n");
    };
    context.exit_repl = [this] { running_ = false; };
    // The goal of the session is kept by the app.
    context.set_goal = [this](const std::string& argument) { return set_goal(argument); };
    // The help overlay waits for a key press, so it is only offered when
    // there is somebody to press one.
    if (ui::Terminal::instance().interactive()) {
        context.show_help = [this] { show_help_overlay(); };
    }
    context.show_diff = [this] { show_diff(); };
    context.show_stats = [this] { show_stats(); };
    // The panel of the plugins needs somebody to press a key, so it is only
    // offered when the session is interactive.
    if (ui::Terminal::instance().interactive()) {
        context.show_plugin = [this] { show_plugin(); };
    }
    // The picker of the conversations needs somebody to press a key, so it
    // is only offered when the session is interactive.
    if (ui::Terminal::instance().interactive()) {
        context.resume_picker = [this] { return resume_picked_session(); };
    }
    // The panel of the servers of MCP needs somebody to press a key, so it
    // is only offered when the session is interactive.
    if (ui::Terminal::instance().interactive()) {
        context.show_mcp = [this] { show_mcp(); };
    }
    context.show_sandbox = [this](const std::string& argument) { return show_sandbox(argument); };
    context.show_mobile = [this] { show_mobile(); };
    // The panel of the settings needs somebody to press a key, so it is only
    // offered when the session is interactive.
    if (ui::Terminal::instance().interactive()) {
        context.show_settings = [this](const std::string& tab) { show_settings(tab); };
    }
    context.session_id = session_id_;
    context.select_option =
        [this](const std::string& title, const std::vector<commands::CommandOption>& options) {
            return select_option(title, options);
        };
    context.confirm = [this](const std::string& question) { return confirm(question); };
    context.open_in_editor = [this](const std::string& path) { open_in_editor(path); };
    context.open_browser = [](const std::string& url) { return open_browser(url); };
    context.open_session = [](const std::string& session_id, const std::string& cwd) {
        return desktop::open_session(session_id, cwd);
    };
    context.usage_limits = [this] { return usage_text(); };
    context.refresh_marketplace = [this](const std::string& marketplace) {
        std::string error;
        if (plugins::refresh_marketplace(io_context_, marketplace, &error)) return std::string();
        return error;
    };
    context.reload_plugins = [this] { return reload_plugins(); };
    context.plugin_screen = [this] { return show_plugin_screen(); };
    context.show_btw = [this](const std::string& question) { show_btw(question); };
    context.is_plan_mode = [this] {
        return engine_ && engine_->context().permission_context.mode == PermissionMode::Plan;
    };
    context.enable_plan_mode = [this](const std::string& description) {
        if (!engine_) return;
        engine_->context().permission_context.mode = PermissionMode::Plan;
        refresh_status_line();

        ui::Terminal::instance().write_static("Enabled plan mode\n");

        // A description of what the user wants is sent to the model, the way
        // the reference does.
        if (!description.empty() && description != "open") {
            run_turn("Enabled plan mode");
        }
    };
    context.show_message_selector = [this] { show_message_selector(); };
    context.pick_theme = [this] { return show_theme_picker(); };
    context.send_feedback = [this](const std::string& description) {
        return send_feedback(description);
    };
    context.set_editor_mode = [this](bool vim) {
        vim_.set_enabled(vim);
        vim_.set_mode(ui::vim::Mode::Insert);
        input_->set_vim_mode(vim, true);
    };
    context.plan_path = [this] { return session::plan_path(session_id_); };
    context.current_plan = [this] {        const auto plan = session::read_plan(session_id_);
        if (!plan) return std::string();
        if (cc::trim(*plan).empty()) return std::string();

        // The plan is shown the way the reference shows it: its path, and
        // then the plan itself.
        std::string out = "Current Plan\n\n";
        out += ui::styled(session::plan_path(session_id_), ui::Style::fg(theme_.inactive));
        out += "\n\n";
        out += *plan;
        if (!out.empty() && out.back() != '\n') out += "\n";
        return out;
    };
    context.ask_model = [this](const std::string& prompt,
                               const std::vector<std::string>& allowed_tools) {
        return ask_model(prompt, allowed_tools);
    };
    context.mcp_states = [this] {
        std::vector<commands::McpServerState> states;
        if (!mcp_) return states;
        for (const auto& server : mcp_->status()) {
            commands::McpServerState state;
            state.name = server.name;
            state.connected = server.connected;
            state.connecting = server.connecting;
            state.error = server.error;
            state.tool_count = server.tool_count;
            state.resource_count = server.resource_count;
            state.prompt_count = server.prompt_count;
            states.push_back(std::move(state));
        }
        return states;
    };
    context.login = [this] { return run_login(); };
    context.logout = [this] { return run_logout(); };

    // The use of a command that is a prompt is remembered, so that the ones
    // that are used often float to the top of the list, the way the
    // reference remembers it.
    if (const commands::Command* command = commands::find_command(name)) {
        if (command->type == "prompt") config::record_skill_usage(command->name);
    }

    const auto result = commands::run_command(name, argument, context);
    if (!result.handled) {
        ui::Terminal& terminal = ui::Terminal::instance();
        terminal.write_static(ui::styled("Unknown command: /" + name,
                                        ui::Style::fg(theme_.error)) +
                              "\nType /help for a list of commands.\n");
        return true;
    }

    if (!result.output.empty()) {
        ui::Terminal::instance().write_static(result.output);
    }
    return true;
}

void App::run_bash_command(const std::string& command) {
    ui::Terminal& terminal = ui::Terminal::instance();

    // The command line, the way Claude Code shows it.
    terminal.write_static(ui::styled("! ", ui::Style::fg(theme_.bash_border)) + command + "\n");

    // The command runs in the shell of the session, so that a `cd` of it
    // moves the folder the session is in, the way the reference does it.
    const auto result = shell_session_ ? shell_session_->run(command, 120000)
                                       : tools::run_shell_command(command, options_.cwd, 120000);

    // The output of the command, under the usual gutter.
    const auto lines =
        ui::render_tool_result_lines(result.output, theme_, terminal.width(), result.exit_code != 0);
    for (const auto& line : lines) {
        terminal.write_static(line + "\n");
    }

    // The standard error of the command, the way the reference shows it: the
    // line that says the folder of the session was put back is there.
    if (!result.error_output.empty()) {
        const auto error_lines = ui::render_tool_result_lines(result.error_output, theme_,
                                                               terminal.width(), true);
        for (const auto& line : error_lines) {
            terminal.write_static(line + "\n");
        }
    }

    // The command and its output join the conversation, the way the
    // reference's `<bash-input>` and `<bash-stdout>` messages do. The model
    // is not asked anything.
    Message input_message;
    input_message.role = Role::User;
    input_message.uuid = uuid_v4();
    input_message.session_id = session_id_;
    input_message.cwd = options_.cwd;
    input_message.version = version();
    input_message.content.push_back(TextBlock{"<bash-input>" + command + "</bash-input>", false});

    Message output_message;
    output_message.role = Role::User;
    output_message.uuid = uuid_v4();
    output_message.session_id = session_id_;
    output_message.cwd = options_.cwd;
    output_message.version = version();
    output_message.content.push_back(
        TextBlock{"<bash-stdout>" + result.output + "</bash-stdout><bash-stderr>" + result.error_output +
                      "</bash-stderr>",
                  false});

    std::vector<Message> messages = engine_->messages();
    messages.push_back(input_message);
    messages.push_back(output_message);
    engine_->set_messages(std::move(messages));
    if (transcript_) {
        transcript_->append(input_message);
        transcript_->append(output_message);
    }

    // The shell may have moved with the command: the status line carries
    // the folder the session is in.
    refresh_status_line();
}

bool App::needs_onboarding() const {
    // The reference asks the questions of the setup when the theme of the
    // user is not known yet, or when the setup was not done before.
    if (!settings_.theme.has_value()) return true;
    return !config::has_completed_onboarding();
}

void App::run_onboarding() {
    // The session has set the program up for the first time: the banner of
    // such a session is the plain one.
    first_run_ = true;

    ui::Terminal& terminal = ui::Terminal::instance();

    const ui::ThemeName name = settings_.theme.value_or(ui::ThemeName::Dark);

    ui::Onboarding onboarding(theme_, name, std::string(ui::to_string(name)), cc::version());
    const ui::Onboarding::Answers answers = onboarding.run(terminal);

    // The theme the user chose is put in place at once, so that the rest of
    // the session is drawn with the colours of that theme.
    if (!answers.theme.empty()) {
        ui::ThemeName chosen = ui::ThemeName::Dark;
        if (ui::parse_theme_name(answers.theme, chosen)) {
            settings_.theme = chosen;
            theme_ = ui::theme_for(chosen);

            // The theme is written in the settings of the user, the way
            // the reference writes it there.
            const std::string path = config::user_settings_path();
            boost::json::object root;
            if (const auto text = cc::read_file(path)) {
                const auto parsed = cc::json_parse(*text, boost::json::value(nullptr));
                if (parsed.is_object()) root = parsed.as_object();
            }
            root["theme"] = boost::json::value(std::string(ui::to_string(chosen)));

            std::string error;
            config::write_settings(path, boost::json::value(std::move(root)), &error);
        }
    }

    // The setup is done; the questions are not asked again.
    config::set_onboarding_completed();
}

bool App::ask_for_trust() {
    if (config::trust_accepted(options_.cwd)) {
        trusted_ = true;
        return true;
    }

    ui::TrustDialog dialog(theme_, options_.cwd);
    if (!dialog.run(ui::Terminal::instance())) return false;

    // The answer is remembered, so the question is only asked once.
    config::remember_trust(options_.cwd);
    trusted_ = true;
    return true;
}

/// Asks whether the files the project pulls in from outside its folder may
/// be read.
///
/// The reference asks the question once, after the folder is trusted: a
/// project that pulls a file in from outside its folder may not be trusted,
/// and the file is only read when the user says yes.
void App::ask_about_external_includes() {
    // The question was asked before: the answer is remembered, and the
    // reference does not ask it again.
    if (config::external_includes_approved(options_.cwd)) return;
    if (config::external_includes_warning_shown(options_.cwd)) return;

    const std::vector<std::string> includes = config::external_includes(options_.cwd);
    if (includes.empty()) return;

    ui::ExternalIncludesDialog dialog(theme_, includes);
    const bool allowed = dialog.run(ui::Terminal::instance());

    // The answer is remembered, so the question is only asked once.
    config::remember_external_includes(options_.cwd, allowed);
}

int App::run_repl() {
    ui::Terminal& terminal = ui::Terminal::instance();

    // The user's own status line, when they have configured one.
    refresh_status_line();

    while (running_) {
        if (!terminal.interactive()) {
            std::string line;
            if (!read_line(line)) break;
            const std::string prompt = trim(line);
            if (prompt.empty()) continue;

            bool handled = false;
            if (handle_command(prompt, handled) && handled) continue;
            if (starts_with(prompt, "!")) {
                run_bash_command(trim(prompt.substr(1)));
                continue;
            }
            run_turn(prompt);
            continue;
        }

        std::string prompt;
        if (!read_prompt(prompt)) break;

        const std::string trimmed = trim(prompt);
        if (trimmed.empty()) continue;

        // A command that starts with `!` runs in the shell, the way Claude
        // Code does; the model is not asked.
        if (starts_with(trimmed, "!")) {
            run_bash_command(trim(trimmed.substr(1)));
            // The model is asked about the command and its output, the way
            // the reference asks it: the command and its output are in the
            // conversation, and the model has the last word on them.
            run_turn("", /*record_prompt=*/false);
            
            continue;
        }

        bool handled = false;
        handle_command(trimmed, handled);
        if (handled) continue;

        run_turn(trimmed);

        // A message the user sent while the turn was running joins the
        // conversation at the end of the next tool round. The turn that was
        // running may have ended before that, in which case the message
        // starts a turn of its own.
        while (!queued_prompts_.empty()) {
            const std::string queued = queued_prompts_.front();
            queued_prompts_.erase(queued_prompts_.begin());
            echo_user_message(queued);
            run_turn(queued);
        }
    }

    terminal.leave_fullscreen();
    terminal.leave_raw_mode();

    // Claude Code says goodbye when the session ends.
    static const char* kGoodbyes[] = {"Goodbye!", "See ya!", "Bye!", "Catch you later!"};
    std::random_device device;
    const std::string goodbye = kGoodbyes[device() % (sizeof(kGoodbyes) / sizeof(kGoodbyes[0]))];
    std::cout << goodbye << "\n";
    std::cout.flush();

    // The reference says how the session is taken up again, the way it does
    // it when the session ends: only when the session was written down and
    // the terminal is there to show it.
    if (terminal.interactive() && !session_id_.empty() && transcript_ &&
        cc::file_exists(transcript_->path())) {
        std::cout << ui::styled("\nResume this session with:\ncc++ --resume " + session_id_ + "\n",
                                ui::Style::fg(theme_.inactive));
        std::cout.flush();
    }

    return 0;
}

} // namespace cc::cli
