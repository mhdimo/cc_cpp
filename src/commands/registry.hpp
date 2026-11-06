#pragma once

#include "config/settings.hpp"
#include "core/message.hpp"

#include <functional>
#include <string>
#include <vector>

namespace cc::commands {

/// One slash command.
struct Command {
    /// The name, without the leading slash.
    std::string name;
    /// The alternative names the command answers to.
    std::vector<std::string> aliases;
    /// The one-line description shown in the help.
    std::string description;
    /// The hint about the arguments the command takes.
    std::string argument_hint;
    /// What the command does: `local` and `local-jsx` run inside the CLI,
    /// `prompt` is expanded into a message that is sent to the model.
    std::string type = "local";
    /// The names of the arguments the command takes.
    ///
    /// A command that is a prompt waits for its arguments when it names
    /// some; the reference's built-in commands name none.
    std::vector<std::string> argument_names;
    /// True when the command is kept out of the help listing, the way the
    /// reference hides its internal commands.
    bool hidden = false;

    /// Says whether the command may be used right now.
    ///
    /// The reference asks the account of the user before it offers the
    /// commands that need one — the extra usage, for one. The predicate is
    /// empty when the command is always there.
    std::function<bool()> enabled;
};

/// Every slash command, in the order the help lists them.
const std::vector<Command>& all();

/// The command named `name`, or one of its aliases. Returns nullptr when
/// there is no such command.
const Command* find_command(const std::string& name);

/// True when the command is offered to the user.
///
/// A command that is hidden is kept out of the listing, and so is a
/// command that may not be used right now — the extra usage, for one, which
/// only the accounts that may buy it can use.
bool is_available(const Command& command);

/// The names of the commands that the session knows.
///
/// The prompt box paints a "/command" in the colour of the suggestions when
/// the session knows the command, the way the reference paints it. The
/// names of the commands of the user, of the plugins and of the servers are
/// kept here; the ones of the CLI itself are in `all`.
const std::vector<std::string>& known_command_names();

/// Fills the list of the names of the commands the session knows.
void set_known_command_names(std::vector<std::string> names);

/// The hint that names the arguments of a command that are still to be
/// filled in.
///
/// The reference builds it from the names the command declares and from the
/// arguments the user has typed already: `/add ` shows `[file]` and
/// `/add foo ` shows `[description]`. The hint is empty when every argument
/// has been filled in.
std::string progressive_argument_hint(const std::vector<std::string>& names,
                                      std::string_view typed);

/// The state of the sandbox, as the description of the `/sandbox` command
/// shows it.
struct SandboxState {
    /// True when the commands run in the sandbox.
    bool enabled = false;

    /// True when the commands that run in the sandbox need no permission.
    bool auto_allow = false;

    /// True when a command may ask to run outside the sandbox.
    bool allow_unsandboxed = true;

    /// True when the settings of the sandbox are held by a policy.
    bool locked = false;

    /// True when everything the sandbox needs is on the machine.
    bool has_dependencies = true;
};

/// The name of the model of the session, as the user sees it.
///
/// The reference shows the public name of the model — `Sonnet 4.5` — and the
/// window of a million tokens with it.
std::string render_model_name(const std::string& model);

/// Records the model of the session, so that the description of the
/// `/model` command names it.
///
/// The reference computes the description of the command when it is asked
/// for; the port keeps it with the commands and changes it when the model of
/// the session changes.
void set_model(const std::string& model);

/// Records the state of the sandbox, so that the description of the command
/// shows it.
///
/// The reference computes the description of the `/sandbox` command when it
/// is asked for; the port keeps it with the commands and changes it when the
/// state of the sandbox changes.
void set_sandbox_state(const SandboxState& state);

/// The help text listing every command.
std::string help_text();

/// The message that tells the model which command was run.
///
/// The reference sends it before the content of the command, so that the
/// model knows which command it is answering. The name is the one of the
/// command itself, without the leading slash.
std::string command_metadata(const std::string& name, const std::string& args);

/// The text the `/status` command prints.
std::string status_text(const std::string& cwd);

/// The text the `/status` command prints, with the model of the session.
std::string status_text(const std::string& cwd, const std::string& model);

/// The text the `/cost` command prints.
std::string cost_text(const Usage& usage);

/// The text the `/context` command prints.
std::string context_text(size_t message_count);

/// The text the `/files` command prints.
std::string files_text(const std::vector<Message>& messages);

/// One choice in a picker.
struct CommandOption {
    /// The text shown for the choice.
    std::string label;
    /// The value the choice carries.
    std::string value;
    /// The explanation shown underneath the label.
    std::string description;
};

/// The state of one MCP server, as the `/mcp` command shows it.
struct McpServerState {
    /// The name of the server.
    std::string name;

    /// True when the server is connected and ready to be called.
    bool connected = false;

    /// True when the server is still being connected.
    bool connecting = false;

    /// The reason the server could not be used.
    std::string error;

    /// The number of tools the server offers.
    size_t tool_count = 0;

    /// The number of resources the server offers.
    size_t resource_count = 0;

    /// The number of prompts the server offers.
    size_t prompt_count = 0;
};

/// What a command handler needs to know about the session it runs in.
struct CommandContext {
    /// The working directory of the session.
    std::string cwd;
    /// The model the session runs on.
    std::string model;
    /// The version of the CLI.
    std::string version;
    /// The merged settings of the session, when they were loaded.
    const config::Settings* settings = nullptr;
    /// The conversation so far.
    const std::vector<Message>* messages = nullptr;
    /// The token usage of the session so far.
    const Usage* usage = nullptr;
    /// The ids of the sessions that can be resumed.
    std::vector<std::string>* sessions = nullptr;
    /// The id of the session the command runs in, when the app knows it.
    std::string session_id;
    /// Called by `/clear` to drop the conversation.
    std::function<void()> clear_conversation;
    /// Called by `/compact` to replace the conversation with a summary. When
    /// it is not set, `/compact` clears the conversation instead.
    std::function<void(const std::string&)> replace_conversation;

    /// Called by `/exit` to leave the REPL.
    std::function<void()> exit_repl;

    /// Called by `/goal` to set the goal of the session, or to clear it.
    ///
    /// The argument is the condition of the goal, or the word `clear`. The
    /// text the command prints is given back.
    std::function<std::string(const std::string& argument)> set_goal;
    /// Called by `/help` to show the help overlay. When it is not set the
    /// command prints the help as text instead.
    std::function<void()> show_help;
    /// Called by `/diff` to show the changes in the working tree. When it is
    /// not set the command lists the changed files as text instead.
    std::function<void()> show_diff;
    /// Shows the code of the address of the mobile application. When it is
    /// not set the command prints the code as text instead.
    std::function<void()> show_mobile;

    /// Opens the panel of the statistics.
    std::function<void()> show_stats;

    /// Opens the picker of the conversations and resumes the one that is
    /// picked.
    ///
    /// The text the command prints is given back.
    std::function<std::string()> resume_picker;

    /// Opens the panel of the plugins.
    ///
    /// The reference opens the panel of the plugins; the port falls back on
    /// the text of the command when there are no arguments and nobody to
    /// press a key.
    std::function<void()> show_plugin;

    /// Opens the panel of the servers of MCP.
    ///
    /// The reference opens the panel of the servers; the port falls back on
    /// the list of them as text when there is nobody to press a key.
    std::function<void()> show_mcp;

    /// Opens the panel of the sandbox.
    ///
    /// The argument may name a command to exclude from the sandbox.
    std::function<std::string(const std::string& argument)> show_sandbox;
    /// Asks the user to pick one of `options` and returns the value they
    /// chose. Returns an empty string when the user dismisses the picker.
    std::function<std::string(const std::string& title,
                               const std::vector<CommandOption>& options)>
        select_option;
    /// Opens the picker of the theme, which shows what each theme looks
    /// like, and returns the name of the theme that was chosen.
    ///
    /// Returns an empty string when the user dismisses the picker.
    std::function<std::string()> pick_theme;
    /// Asks the user a yes/no question. Returns true when they said yes.
    std::function<bool(const std::string& question)> confirm;
    /// Opens a file in the user's editor.
    std::function<void(const std::string& path)> open_in_editor;
    /// Opens an address in the user's browser.
    ///
    /// Returns false when the browser could not be opened, so that the
    /// command can tell the user where to go instead.
    std::function<bool(const std::string& url)> open_browser;
    /// Carries the session to the Claude Desktop application. Returns false
    /// when the application could not be opened.
    std::function<bool(const std::string& session_id, const std::string& cwd)> open_session;
    /// The text the `/usage` command shows: the limits of the plan of the
    /// user.
    std::function<std::string()> usage_limits;
    /// Fetches a marketplace again. Returns the reason it failed, or an
    /// empty string when it worked.
    std::function<std::string(const std::string&)> refresh_marketplace;
    /// Reads the plugins again and returns the report of what the session
    /// holds.
    std::function<std::string()> reload_plugins;
    /// Opens the screen that manages the plugins and returns the text of
    /// what happened there.
    std::function<std::string()> plugin_screen;
    /// Asks the model a side question, without disturbing the conversation.
    std::function<void(const std::string&)> show_btw;
    /// True when the session runs in the plan mode.
    std::function<bool()> is_plan_mode;
    /// Turns the plan mode on. The description is what the user typed after
    /// the command; when there is one, the model is told about the mode.
    std::function<void(const std::string&)> enable_plan_mode;
    /// The plan of the session, when there is one.
    std::function<std::string()> current_plan;
    /// The file the plan of the session is written to.
    std::function<std::string()> plan_path;
    /// Opens the view that lets the user rewind the conversation.
    std::function<void()> show_message_selector;
    /// Turns the keys of the prompt box into the keys of vim, or back into
    /// the usual ones.
    std::function<void(bool)> set_editor_mode;
    /// Opens the panel of the settings.
    /// Opens the panel of the settings with the tab that is named.
    ///
    /// The reference opens the panel with one of its tabs chosen — the tab of
    /// the configuration, the one of the state of the session or the one of
    /// the limits of the plan.
    std::function<void(const std::string& tab)> show_settings;
    /// Sends the feedback of the user and returns the text of what
    /// happened.
    std::function<std::string(const std::string& description)> send_feedback;
    /// Sends a prompt to the model and returns the answer it gave.
    /// Asks the model a question and returns its answer.
    ///
    /// The rules of `allowed_tools` name the tools the model may use and
    /// are allowed without a question; an empty list means every tool of
    /// the session.
    std::function<std::string(const std::string& prompt,
                               const std::vector<std::string>& allowed_tools)>
        ask_model;
    /// The state of the MCP servers that were connected. The `/mcp` command
    /// shows it next to the servers it found in the settings.
    std::function<std::vector<McpServerState>()> mcp_states;
    /// Runs the login flow. Returns true when the user is logged in.
    std::function<bool()> login;
    /// Forgets the credentials of the login. Returns true when they are
    /// gone.
    std::function<bool()> logout;
};

/// The result of running a command.
struct CommandResult {
    /// True when the command was recognised.
    bool handled = false;
    /// The text to show the user.
    std::string output;
};

/// The models the picker of the model offers.
std::vector<CommandOption> model_options(const CommandContext& context);

/// The themes the picker of the theme offers.
std::vector<CommandOption> theme_options();

/// The effort levels the picker of the effort offers.
std::vector<CommandOption> effort_options();

/// The output styles the picker of the output style offers.
std::vector<CommandOption> output_style_options(const std::string& cwd = "");

/// Runs the command named `name` with `argument`.
///
/// Returns a result whose `handled` is false when no such command exists.
CommandResult run_command(const std::string& name, const std::string& argument,
                           const CommandContext& context);

} // namespace cc::commands
