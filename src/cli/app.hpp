#pragma once

#include "agents/agent.hpp"
#include "auth/oauth.hpp"
#include "cli/args.hpp"
#include "commands/custom.hpp"
#include "commands/registry.hpp"
#include "config/settings.hpp"
#include "hooks/hooks.hpp"
#include "ide.hpp"
#include "mcp/mcp.hpp"
#include "permissions/classifier.hpp"
#include "permissions/decision.hpp"
#include "plugins.hpp"
#include "sandbox/manager.hpp"
#include "query/engine.hpp"
#include "session/file_history.hpp"
#include "session/transcript.hpp"
#include "tools/registry.hpp"
#include "utils/git.hpp"
#include "ui/help_overlay.hpp"
#include "ui/keybindings.hpp"
#include "ui/logo.hpp"
#include "ui/plugin_panel.hpp"
#include "ui/prompt_input.hpp"
#include "ui/session_picker.hpp"
#include "ui/settings_panel.hpp"
#include "ui/sandbox_panel.hpp"
#include "ui/tool_display.hpp"
#include "ui/settings_tabs.hpp"
#include "ui/stats_panel.hpp"
#include "ui/status_line.hpp"
#include "ui/suggestions.hpp"
#include "ui/spinner.hpp"
#include "ui/theme.hpp"
#include "ui/vim.hpp"

#include <ai/providers/anthropic/anthropic.hpp>

#include <boost/asio.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <mutex>

namespace cc::cli {

/// The title of the permission dialog for a tool.
///
/// The reference has a dialog per tool, each with its own title.
std::string permission_title(const Tool& tool, const boost::json::object& input,
                             const std::string& cwd);

/// The question the permission dialog asks for a tool.
///
/// The reference asks a different question for every tool.
std::string permission_question(const Tool& tool, const boost::json::object& input,
                                const std::string& cwd);

/// The components the plugin of the panel brings.
///
/// The panel names the components of a plugin in the details of it: the
/// commands, the agents, the skills, the hooks and the servers of MCP.
///
/// The components of a plugin that is installed are read from the cache of
/// the user; the ones of a plugin that a marketplace offers are read from
/// the folder of the marketplace.
plugins::PluginComponents components_of(const ui::PluginPanelEntry& entry);

/// The servers the agents of the session use.
///
/// The reference reads the servers of an agent out of the `mcpServers` of
/// the frontmatter of its definition; the port reads the names of them out
/// of the same place, and takes what is known about them from the servers
/// of the session.
///
/// The names of the servers that are only referred to are left out, the way
/// the reference leaves them out: they are the servers of the configuration
/// of the user, and they stand in their own group.
///
/// The name of the server is the key of the map, and the names of the agents
/// that use it are the value of it.
std::map<std::string, std::vector<std::string>> agent_mcp_servers(
    const std::vector<agents::Definition>& definitions);

/// The interactive Claude Code session.
class App {
public:
    struct Options {
        Args args;
        std::string cwd;
    };

    explicit App(Options options);
    ~App();

    /// Runs the session and returns the process exit code.
    int run();

private:
    /// One frame of the mascot's animation.
    struct MascotFrame {
        /// The pose the mascot is in.
        ui::MascotPose pose = ui::MascotPose::Default;
        /// How far the mascot has moved down.
        int offset = 0;
    };

    /// A key that has to be pressed a second time before it takes effect.
    struct PendingPress {
        /// The name of the key, e.g. `Ctrl-C`.
        std::string key;
        /// When the key was pressed.
        std::chrono::steady_clock::time_point at{};
    };

    /// A message the actions of the bar can be applied to.
    struct ActionTarget {
        /// The text the "copy" action puts on the clipboard.
        std::string copy_text;
        /// The name of the primary input of the tool call, e.g. `path`.
        std::string input_label;
        /// The value of the primary input.
        std::string input_value;
        /// True when the message is the user's own and can be edited.
        bool editable = false;
        /// The index of the message in the conversation.
        size_t message_index = 0;
        /// The lines of the group of tools, when the target is one.
        std::vector<std::string> group_lines;
        /// True when the target is a group of tools that can be expanded.
        bool expandable = false;
        /// True when the group of tools is expanded.
        bool expanded = false;
    };

    /// What a key in the prompt box did.
    enum class PromptAction {
        Handled,   ///< The key was used by the prompt box.
        Submitted, ///< The prompt was sent; the text is in the output.
        Exit,      ///< The user asked to leave the session.
    };

    /// Sets up the provider, the model, the tools and the system prompt.
    bool setup();

    /// Runs the interactive prompt loop.
    int run_repl();

    /// Runs a single prompt and prints the answer, for `--print`.
    int run_print();

    /// Reads one line of input from the terminal, without the UI.
    bool read_line(std::string& out);

    /// Reads keys into the prompt box until the user submits or exits.
    ///
    /// Returns false when the user asked to exit.
    bool read_prompt(std::string& out);

    /// Handles a key in the prompt box.
    ///
    /// While a turn is running the prompt is queued rather than sent, the
    /// way Claude Code queues it: the message joins the turn that is
    /// running instead of starting a turn of its own.
    PromptAction handle_prompt_key(const ui::Key& key, std::string& out, bool loading);

    /// Writes the prompt the user sent to the transcript.
    ///
    /// The prompt keeps the pointer the reference puts in front of it.
    void echo_user_message(const std::string& text);

    /// The line that shows a message of the user.
    ///
    /// The reference draws the message on the background it keeps for the
    /// user's own words, with the pointer in front of it.
    std::string user_message_line(const std::string& text) const;

    /// The prompts the user sent while a turn was running.
    ///
    /// They join the conversation at the end of the next tool round.
    std::vector<std::string> queued_prompts_;

    /// Runs one turn and renders it.
    /// Runs a turn of the conversation.
    ///
    /// The prompt is recorded as a message of the user. A turn that follows
    /// a command of the shell records none: the command and its output are
    /// already in the conversation.
    void run_turn(const std::string& prompt, bool record_prompt = true);

    /// Renders the assistant's answer.
    void render_assistant_text(const std::string& text);

    /// Renders a tool call and its result.
    ///
    /// Both return the lines they draw, so the caller can decide whether
    /// they go to the transcript straight away or are held back.
    std::vector<std::string> render_tool_call(const query::ToolCallInfo& call,
                                              bool marker_visible = true,
                                              ui::ToolCallState state = ui::ToolCallState::Running);

    /// Finds the session that is resumed, and remembers that it is one.
    ///
    /// Returns false when the session the user named is not there.
    bool resolve_resumed_session();

    /// Asks the user which conversation is resumed.
    ///
    /// Returns the conversation that was picked; nothing when the user
    /// gave the picker up.
    std::optional<ui::SessionPicker::Entry> pick_session();

    /// Opens the picker of the theme and returns the name of the theme that
    /// was chosen.
    ///
    /// The string is empty when the picker was dismissed.
    std::string show_theme_picker();

    /// The usage of the machine, the way the panel of the statistics shows
    /// it.
    ui::UsageTab::LocalUsage local_usage() const;

    /// Writes the conversation of a resumed session to the transcript.
    ///
    /// The reference shows the conversation of a session that is resumed,
    /// the way it was written down.
    void render_resumed_conversation();

    /// Turns the marker of the running tool on and off.
    void tick_blink();

    /// The line the spinner shows under itself.
    std::string spinner_tip() const;

    /// What a keybinding the user wrote asks the session to do.
    std::optional<PromptAction> handle_keybinding(const std::string& action, std::string& out,
                                                  bool loading);
    /// The lines that show the result of a tool.
    ///
    /// The transcript asks for the whole of the result: it is the view the
    /// "ctrl+o to expand" of the result refers to.
    std::vector<std::string> render_tool_result(const query::ToolCallInfo& call,
                                                 const ToolResult& result, bool expand = false);

    /// True for the tools the reference collapses into one line when they
    /// run one after another.
    bool is_collapsible_tool(const std::string& name) const;

    /// True for the tools that show a progress message of their own.
    ///
    /// The reference gives the shell a line of its own under the tool; the
    /// file tools have none.
    bool tool_has_progress_message(const std::string& name) const;

    /// Writes the reads and searches that ran one after another, either as
    /// their own lines or as the one line that stands for all of them.
    void flush_tool_group();

    /// The width of the column of the names of the commands in the overlay
    /// of the suggestions.
    size_t command_name_width() const;

    /// The line of the group of reads and searches.
    ///
    /// The reference counts the files and not the calls, and it writes the
    /// number in bold inside the dim line. While the group runs the line is
    /// in the present tense and carries an ellipsis.
    std::string tool_group_line(bool active) const;

    /// Writes the empty line that stands above the message of the assistant.
    void start_assistant_message();

    /// Handles a slash command. Returns false when the command is unknown.
    bool handle_command(const std::string& text, bool& handled);

    /// True when the command runs as soon as its suggestion is taken.
    ///
    /// The reference runs a command that takes no arguments straight away;
    /// one that takes arguments waits for them.
    bool command_runs_right_away(const std::string& name) const;

    /// Starts the mascot's animation.
    void start_mascot_animation();

    /// Runs the login flow and returns true when the user is logged in.
    bool run_login();

    /// Forgets the credentials of the login.
    bool run_logout();

    /// Makes the session talk to the account of `access_token`.
    ///
    /// An empty token takes the session back to the key of the environment.
    void apply_access_token(const std::string& access_token);

    /// Reads the settings of the session and the model they name.
    ///
    /// The servers the folder configures in its `.mcp.json` file are only
    /// read when `include_project_servers` is set, which the session does
    /// once the folder is trusted.
    void load_settings(bool include_project_servers);

    /// Works out the model of the session.
    ///
    /// The flag of the command line comes first, then the model of the
    /// environment, then the settings of the user. The model is asked for
    /// again after the environment of the settings is applied, for the
    /// environment may name it.
    void resolve_model();

    /// Applies the environment of the settings that may be applied before the
    /// folder is trusted.
    ///
    /// The variables of the user are applied whole, because the user wrote
    /// them; the ones of the project only when they are safe, so that a
    /// folder that is not trusted cannot send the session somewhere else.
    /// The reference applies the same two groups of variables at the same
    /// moment.
    void apply_user_env();

    /// The frames of the mascot's jump.
    static std::vector<MascotFrame> mascot_jump();

    /// The frames of the mascot looking around.
    static std::vector<MascotFrame> mascot_look();

    /// Moves the mascot's animation on, when it is running.
    void tick_mascot();

    /// Draws the frame of the mascot's animation that is showing.
    void draw_mascot_frame();

    /// Handles a click of the mouse.
    void handle_mouse_click(int x, int y);

    /// The message the terminal is given when the session wants the user.
    std::string permission_notification(const Tool& tool, const boost::json::object& input);

    /// Asks the user to approve a tool call and returns the decision.
    PermissionResult request_permission(const Tool& tool, const boost::json::object& input,
                                        const PermissionResult& request);

    /// Asks the classifier of the auto mode whether the call may run.
    ///
    /// The reference asks a model of its own when the auto mode is on: the
    /// call runs without the user when the model says that it is safe, and
    /// is refused when the model says that it is not.
    ///
    /// The optional holds nothing when the classifier could not answer; the
    /// user is asked then.
    std::optional<permissions::ClassifierOutcome> classify_action(const Tool& tool,
                                                                  const boost::json::object& input);

    /// Asks the questions of the model and allows the tool.
    ///
    /// The reference asks the questions where the permission of the tool
    /// would be asked, so that the user answers the question and the tool
    /// runs at once. The answers are put into the input of the tool, the way
    /// the reference puts them there.
    PermissionResult ask_the_questions(const boost::json::object& input);

    /// Asks the user whether they trust the folder the session runs in.
    ///
    /// Returns false when the user does not trust it, in which case the
    /// session ends.
    bool ask_for_trust();

    /// Sets the goal of the session, or takes it away.
    ///
    /// The text the command prints is given back.
    std::string set_goal(const std::string& argument);

    /// Checks the goal of the session and goes on working when it is not
    /// met.
    void check_goal();

    /// Compacts the conversation when the context of the session is full.
    ///
    /// The reference compacts itself when the context window is nearly
    /// full: the model writes a summary of the conversation, and the
    /// conversation becomes that summary and the last few messages.
    void maybe_auto_compact();

    /// Installs the extension of Claude Code in the IDE of the terminal.
    ///
    /// The installation is done in the background, the way the reference
    /// does it.
    void install_ide_extension();

    /// True when the session talks to an IDE.
    ///
    /// The reference offers the settings of the IDE only when the IDE is
    /// connected.
    bool connected_to_ide() const;

    /// The IDE the session talks to.
    ///
    /// The IDE is the one that holds the folder of the session, and it is
    /// only there when it is connected.
    std::optional<DetectedIde> connected_ide() const;

    /// Opens the change of a file in the IDE.
    ///
    /// The call is made in the background: the IDE answers when the user
    /// has saved the file or rejected the change, and the answer is given
    /// back through the flag.
    void open_diff_in_ide(const std::string& path, const boost::json::object& input,
                           const std::shared_ptr<std::atomic<int>>& answer, const std::string& tab);

    /// Closes the tab of the IDE that shows the change.
    void close_diff_in_ide(const std::string& tab);

    /// Asks whether the files the project pulls in from outside its folder
    /// may be read.
    ///
    /// The question is asked once; the answer is remembered in the settings
    /// of the project.
    void ask_about_external_includes();

    /// True when the questions of the setup are to be asked.
    ///
    /// The reference asks them when the theme of the user is not known
    /// yet, or when the setup was not done before.
    bool needs_onboarding() const;

    /// Asks the questions of the setup of the session.
    ///
    /// The reference asks them before the trust of the folder: the theme
    /// of the session, the key of the API that was found in the
    /// environment, the notes of the safety and the settings of the
    /// terminal.
    void run_onboarding();

    /// The rule a "don't ask again" choice would add for `tool`.
    std::string permission_scope(const Tool& tool, const boost::json::object& input) const;

    /// Shows the `?` shortcuts menu until the user dismisses it.
    /// Shows the menu of the shortcuts.
    ///
    /// The key that closed the menu is given back, so that a character
    /// typed while the menu was up still reaches the input.
    std::optional<ui::Key> show_shortcuts_menu();

    /// Shows the `/help` overlay until the user dismisses it.
    void show_help_overlay();

    /// Runs a shell command the user asked for with `!`.
    ///
    /// The command and its output join the conversation, the way the
    /// reference's `<bash-input>` and `<bash-stdout>` messages do; the
    /// model is not asked anything.
    void run_bash_command(const std::string& command);

    /// Asks the user to pick one of `options` and returns the value they
    /// chose. Returns an empty string when the picker is dismissed.
    std::string select_option(const std::string& title,
                               const std::vector<commands::CommandOption>& options);

    /// Asks the user a yes/no question.
    bool confirm(const std::string& question);

    /// Opens a file in the user's editor.
    void open_in_editor(const std::string& path);

    /// Sends a prompt to the model and returns the answer it gave.
    std::string ask_model(const std::string& prompt,
                          const std::vector<std::string>& allowed_tools = {});

    /// Records a press of `key` and reports whether it is the second one
    /// within the double-press window.
    ///
    /// Claude Code asks for a second press before it exits or clears the
    /// input, so a stray key press cannot end the session.
    bool press_again(PendingPress& press, const std::string& key);

    /// Moves the session on to the next permission mode.
    ///
    /// Shift+Tab cycles through default, accept edits, plan and bypass
    /// permissions, the way Claude Code does.
    void cycle_permission_mode();

    /// The hint shown below the prompt box, or an empty string when there
    /// is nothing to say.
    std::string prompt_hint() const;

    /// The message the footer raises at its right, or an empty string.
    ///
    /// The reference raises it as a notification of its own column, so it
    /// does not take the place of the hint.
    std::string prompt_notification() const;

    /// The hint about the arguments of the command that is being typed.
    ///
    /// The reference shows it when the input holds the name of a command and
    /// one space, and nothing else.
    std::string argument_hint() const;

    /// The footer line below the prompt box.
    ///
    /// It carries the permission mode indicator when the session is not in
    /// the default mode.
    std::string footer_text(bool loading) const;

    /// The context warning shown above the prompt box, or an empty string.
    std::string context_warning() const;

    /// The mode indicator Claude Code shows in the footer.
    std::string mode_indicator() const;

    /// Writes a permission rule to the local settings file.
    void remember_permission(const std::string& scope);

    /// Runs a sub-agent and returns the text it produced.
    ///
    /// The sub-agent runs on its own event loop, in its own thread, so the
    /// session's loop is not disturbed.
    std::string run_sub_agent(const std::string& system_prompt, const std::string& task,
                              const std::vector<std::string>& tools, const std::string& model);

    /// Prints the startup banner.
    void print_banner();

    /// Says how long the turn that has just ended took.
    void show_turn_duration();

    /// The IDE whose folders hold the folder of the session.
    std::optional<cc::DetectedIde> ide_of_the_folder();

    /// The pull request of the branch, as the footer shows it.
    std::string pr_badge() const;

    /// Asks the command of GitHub about the pull request of the branch.
    ///
    /// The reference asks the command in the background, every minute, and
    /// shows the pull request in the footer of the prompt box.
    void refresh_pr_status();

    /// Redraws the prompt box, with the suggestions overlay above it.
    void render_prompt_box(bool loading, const std::vector<std::string>& above = {});

    /// Reads the commands the MCP servers offer again.
    ///
    /// The servers connect in the background, so the prompts of a server
    /// arrive while the session is already running.
    void refresh_mcp_commands();

    /// The sub-agents the list of the `@` offers.
    ///
    /// The reference offers the agents under the files, with the type of
    /// the agent as their name and the moment they are to be used as their
    /// description.
    std::vector<ui::AgentEntry> agent_entries() const;

    /// Writes a suggestion into the prompt box.
    ///
    /// A command replaces the whole of the input; a file replaces the `@`
    /// and the text that follows it, the way the reference replaces the
    /// token it completes. The text of the prompt is returned.
    std::string apply_suggestion(const ui::Suggestion& suggestion);

    /// Redraws the live region: the assistant's answer as it grows, the
    /// spinner and the prompt box.
    void render_live();

    /// Writes the assistant's answer to the transcript and clears it.
    void commit_answer();

    /// Writes the model's thinking to the transcript, the way Claude Code
    /// shows it: a dim `⇔ Thinking` line that Ctrl+O expands.
    void flush_thinking();

    /// Turns the todo list on or off.
    void toggle_todos();

    /// Searches the prompts the user has entered and recalls one of them.
    void search_history();

    /// Runs the user's status line command and keeps its output.
    ///
    /// The text is empty when no status line is configured, or when the
    /// command failed.
    void refresh_status_line();

    /// True while the session is compacting itself.
    ///
    /// The turn that writes the summary would compact itself again, so the
    /// compaction is done only once.
    bool in_auto_compact_ = false;

    /// Shows the whole conversation in a full-screen view.
    ///
    /// Claude Code opens the view with Ctrl+O. It lists every message of
    /// the session; Ctrl+E brings the raw tool results and the thinking
    /// blocks along.
    void show_transcript();

    /// Shows the changes in the working tree, the way `/diff` does.
    void show_diff();

    /// Shows the code of the address of the mobile application, the way
    /// `/mobile` does.
    void show_mobile();

    /// The text the `/usage` command prints: the limits of the plan of the
    /// user.
    std::string usage_text();

    /// Reads the plugins again and reports what the session holds.
    ///
    /// The report names the plugins that are on, the commands, the agents,
    /// the hooks and the MCP servers they bring, the way the reference
    /// reports them.
    std::string reload_plugins();

    /// The panel of the settings.
    ///
    /// The reference opens the panel with one of its tabs chosen — the tab of
    /// the configuration, the one of the state of the session or the one of
    /// the limits of the plan. Picking a setting opens the picker of that
    /// setting.
    void show_settings(const std::string& tab = "Config");

    /// The settings of the session, as the tab of the configuration shows
    /// them.
    std::vector<ui::Setting> config_settings();

    /// The state of the session, as the tab of the state shows it.
    std::vector<ui::StatusTab::Property> status_properties();

    /// The name the user gave the session.
    std::string session_name();

    /// The limits of the plan, as the tab of the usage shows them.
    /// Asks the user about the servers of the `.mcp.json` file of the folder.
    ///
    /// A server the user did not allow is not started, and the answer is
    /// remembered in the settings of the folder.
    void approve_project_mcp_servers(bool interactive);

    /// The permission modes the panel of the settings offers.
    std::vector<commands::CommandOption> permission_mode_options();

    /// Asks the API how much of the plan of the user is used.
    auth::Utilization fetch_usage();

    /// The limits of the plan, as the tab of the usage shows them.
    std::vector<ui::UsageTab::Limit> usage_limits(const auth::Utilization& usage);

    /// The extra usage of the plan, when the API reports one.
    std::optional<ui::UsageTab::ExtraUsage> usage_extra(const auth::Utilization& usage);

    /// Sends the feedback of the user and returns the text of what happened.
    std::string send_feedback(const std::string& description);

    /// Asks the user for the options a plugin declares.
    void ask_plugin_options(const plugins::Plugin& plugin);

    /// Asks the user for the options a plugin declares.
    ///
    /// The dialog is the one the reference opens from the menu of the
    /// details of a plugin: the title names the plugin and the subtitle
    /// says "Plugin options". The values the user gives are kept in the
    /// settings of the user.
    ///
    /// The plugin is named the way the reference names it:
    /// `name@marketplace`.
    void configure_plugin(const std::string& plugin_id);

    /// The screen that manages the plugins.
    ///
    /// The screen lists the plugins that are installed and the ones the
    /// marketplaces offer; picking one opens what can be done with it.
    /// The text of what happened is returned.
    std::string show_plugin_screen();

    /// Builds the system prompt of the session.
    ///
    /// The prompt is built again for every request, the way the reference
    /// does it, so that a change of the permission mode is picked up.
    std::string build_session_prompt();

    /// Asks the model a side question and shows the answer.
    ///
    /// The question is asked in a thread of its own, with the tools taken
    /// away and the turn ending as soon as the model answers, so that the
    /// conversation of the session is left alone.
    void show_btw(const std::string& question);

    /// Puts the work the engine asks for in the queue.
    ///
    /// The coroutine of the engine is resumed on a thread of the SDK's own,
    /// so the hooks must not touch the terminal themselves: they put their
    /// work here, and the thread that owns the terminal does it, in the
    /// order it was asked for.
    void post_ui(std::function<void()> work);

    /// Asks the thread that owns the terminal to run `work` and waits for
    /// its answer.
    ///
    /// The dialogs of the tools are shown this way: the engine waits for the
    /// answer while the thread that owns the terminal shows the question.
    template <typename T>
    T run_on_ui(std::function<T()> work) {
        auto promise = std::make_shared<std::promise<T>>();
        std::future<T> future = promise->get_future();
        post_ui([work = std::move(work), promise]() mutable { promise->set_value(work()); });
        return future.get();
    }

    /// Does the work the engine asked for.
    void drain_ui();

    /// Opens the picker of the conversations and resumes the one that is
    /// picked.
    std::string resume_picked_session();

    /// Configures the servers of MCP and connects them.
    void configure_mcp_servers();

    /// Connects the servers of MCP again.
    void reconnect_mcp_server(const std::string& name);

    /// Turns a server of MCP on or off.
    void set_mcp_server_enabled(const std::string& name, bool enabled);

    /// Authenticates a server of MCP.
    ///
    /// The reference lets the user authenticate a server that waits for an
    /// authentication; the port runs the flow of the session and shows the
    /// dialog of it. The server is connected again when the flow is through,
    /// so that the token of the new authentication is used.
    void authenticate_mcp_server(const std::string& name);

    /// Fetches the number of the installs of the plugins.
    ///
    /// The numbers come from the statistics of the marketplace of Anthropic;
    /// the fetch is done in a thread of its own, so that the interface is
    /// not held up by the network. The numbers are given to the panel of the
    /// plugins when they arrive.
    void fetch_install_counts();

    /// Opens the panel of the plugins.
    void show_plugin();

    /// Opens the panel of the servers of MCP.
    void show_mcp();

    /// Opens the panel of the statistics.
    void show_stats();

    /// Opens the panel of the sandbox.
    ///
    /// The argument may name a command to exclude from the sandbox.
    std::string show_sandbox(const std::string& argument);

    /// Writes the settings of the sandbox to the settings of the folder.
    void write_sandbox_settings(const sandbox::Settings& settings);

    /// Writes the description of the `/sandbox` command, so that it shows the
    /// state of the sandbox.
    void update_sandbox_description();

    /// True when the instructions about the commits and the pull requests
    /// belong in the prompt of the `Bash` tool.
    ///
    /// The setting is read from the files of the settings; the more specific
    /// scope wins.
    bool include_git_instructions() const;

    /// Opens the view that lets the user rewind the conversation.
    ///
    /// Claude Code opens it when the user presses Escape twice on an empty
    /// prompt. The user picks one of the prompts of the session and decides
    /// whether the conversation is rewound to that point or summarised from
    /// there.
    void show_message_selector();

    /// Turns the message actions on.
    ///
    /// The bar lets the user walk through the conversation and act on the
    /// message that is selected: copy it, copy the path of the file a tool
    /// call touched, or edit the prompt again.
    void enter_message_actions();

    /// Handles a key while the message actions are on.
    ///
    /// Returns false when the key is not one of the actions, so the prompt
    /// can have it.
    bool handle_action_key(const ui::Key& key);

    /// The bar that shows the actions of the selected message.
    std::vector<std::string> render_action_bar() const;

    /// The text the status line command wrote.
    std::string status_line_text_;

    /// True when the transcript shows everything, the raw tool results and
    /// the thinking blocks included.
    bool transcript_show_all_ = false;

    /// True while the message actions are on.
    bool message_actions_ = false;

    /// The messages the actions can be applied to.
    std::vector<ActionTarget> action_targets_;

    /// The message that is selected.
    size_t action_index_ = 0;

    /// The lines of the todo list, when it is showing.
    std::vector<std::string> render_todos() const;

    /// The offset in `answer_` up to which the answer has been committed.
    size_t committed_ = 0;

    /// The assistant's answer for the turn in progress.
    std::string answer_;

    /// The model's thinking for the turn in progress.
    std::string thinking_;

    /// The answer of the turn that finished last.
    std::string last_answer_;

    /// True when the detailed output is shown.
    bool verbose_ = false;

    /// True when the user has said that they trust the folder.
    bool trusted_ = false;

    /// True when the session runs on a terminal and can ask the user.
    bool interactive_ = false;

    /// The information the logo shows.
    ui::Logo::Info logo_info_;

    /// The names of the tools the model may use in the turn that is
    /// running.
    ///
    /// A command that names its tools — the commit, for one — gives the
    /// model only those, the way the reference does.
    std::vector<std::string> turn_tool_names_;

    /// The feeds that stand in the column of the panel of the banner.
    std::vector<ui::Feed> banner_feeds_;

    /// True when the notes of the release are new to the user.
    ///
    /// The reference draws the full logo only when there are such notes,
    /// or when the full logo is asked for.
    bool banner_has_release_notes_ = false;

    /// True when the user asked for the full logo.
    bool banner_force_full_logo_ = false;

    /// The frames of the animation that is running.
    std::vector<MascotFrame> mascot_frames_;

    /// The frame of the animation that is showing.
    size_t mascot_frame_ = 0;

    /// When the frame that is showing was drawn.
    std::chrono::steady_clock::time_point mascot_frame_at_{};

    /// When the last paste landed in the input.
    ///
    /// The footer names the paste for a moment after it, the way the
    /// reference names it.
    std::chrono::steady_clock::time_point paste_at_{};

    /// True while an animation of the mascot is running.
    bool mascot_animating_ = false;

    /// True when the todo list is shown.
    bool show_todos_ = false;

    /// The lines of the reads and searches that are waiting to be shown.
    std::vector<std::string> group_lines_;

    /// How many tool calls the group holds, and how many of them were
    /// searches and how many were reads.
    size_t group_count_ = 0;
    size_t group_searches_ = 0;
    size_t group_reads_ = 0;

    /// The files the group has read.
    ///
    /// The reference counts the files and not the calls, so that a file that
    /// is read twice is counted once.
    std::set<std::string> group_read_paths_;

    /// The file the group is reading at the moment, or the pattern it is
    /// searching for.
    ///
    /// The reference shows it under the line of the group while the group
    /// runs.
    std::string group_hint_;

    /// True when the empty line that stands between the part of the answer
    /// that is in the transcript and the part that is still live has not
    /// been written yet.
    bool answer_gap_ = false;

    /// True when the message of the assistant has already started.
    ///
    /// The reference leaves one empty line above every block of the message
    /// of the assistant, and none between the blocks of the tool that runs.
    bool assistant_message_started_ = false;

    /// The theme in force.
    ui::Theme theme_;

    Options options_;
    boost::asio::io_context io_context_;
    std::shared_ptr<ai::providers::anthropic::AnthropicProvider> provider_;
    ai::LanguageModelPtr model_;
    std::unique_ptr<tools::Registry> registry_;

    /// The shell of the session.
    ///
    /// It stays alive for the whole session, so that a `cd` of a command
    /// moves the folder the session is in, the way the reference does it.
    std::unique_ptr<tools::ShellSession> shell_session_;
    std::unique_ptr<query::Engine> engine_;
    std::unique_ptr<session::Transcript> transcript_;
    std::unique_ptr<ui::PromptInput> input_;
    std::unique_ptr<ui::Spinner> spinner_;

    /// The commands the user wrote in their own folders.
    std::vector<commands::CustomCommand> custom_commands_;

    /// The work the engine asks the interface to do.
    ///
    /// The coroutine of the engine is resumed on a thread of the SDK's own,
    /// so the hooks must not touch the terminal themselves: they put their
    /// work here, and the thread that owns the terminal does it, in the
    /// order it was asked for.
    std::mutex ui_mutex_;
    std::deque<std::function<void()>> ui_queue_;

    /// True while the queue is being emptied.
    bool ui_draining_ = false;

    /// The sandbox the commands of the session run in.
    sandbox::Manager sandbox_;

    /// The state of the vim mode of the prompt box.
    ui::vim::State vim_;

    /// The prompts the MCP servers offer as commands.
    std::vector<mcp::McpCommand> mcp_commands_;

    /// The number of the installs of the plugins.
    ///
    /// The numbers are fetched in a thread of its own; they are given to the
    /// panel of the plugins when they arrive. The holder is shared with the
    /// thread that fetches them, so that the thread does not need the
    /// session to be alive.
    struct InstallCounts {
        /// The lock of the numbers.
        std::mutex mutex;

        /// The numbers themselves.
        std::optional<std::map<std::string, int64_t>> counts;
    };

    /// The number of the installs of the plugins.
    std::shared_ptr<InstallCounts> install_counts_ = std::make_shared<InstallCounts>();

    /// How many times the user has used the `/btw` command.
    int64_t btw_use_count_ = 0;

    /// The keys the user has bound to something else.
    ui::keybindings::Bindings keybindings_;

    /// The copies of the files the session changes.
    session::FileHistory file_history_;
    config::Settings settings_;
    hooks::Registry hooks_;
    std::unique_ptr<mcp::Registry> mcp_;
    std::vector<agents::Definition> agent_definitions_;

    /// The goal of the session.
    ///
    /// The session goes on working until the goal is met, the way the
    /// reference goes on working.
    std::string goal_;

    /// The reason the last check of the goal gave.
    std::string goal_reason_;

    /// The number of times the goal was checked.
    int goal_rounds_ = 0;

    /// The IDE the session talks to.
    ///
    /// The IDE is remembered when it is connected, so that the name of it
    /// can be shown in the dialog of the permission.
    std::optional<DetectedIde> ide_;

    /// The ids of the sessions that can be resumed.
    std::vector<std::string> session_ids_;

    /// The call of the tool that is running.
    ///
    /// It stays in the dynamic region while the tool runs, so that its
    /// marker can blink, the way Claude Code's does.
    std::optional<query::ToolCallInfo> active_tool_call_;


    /// The calls of the tools that wait for their turn.
    ///
    /// The reference shows them under a line of their own, with a marker
    /// that does not blink.
    std::vector<query::ToolCallInfo> queued_tool_calls_;

    /// True while the marker of the running tool is shown.
    ///
    /// Claude Code blinks it every 600 ms.
    bool blink_on_ = true;

    /// The moment the marker last changed.
    int64_t last_blink_ms_ = 0;

    /// True while the model is waiting for the user to approve a tool.
    bool awaiting_permission_ = false;

    /// The output of the tool that is running.
    std::string tool_output_;

    /// The number of the lines and of the bytes the running tool printed.
    size_t tool_output_lines_ = 0;
    size_t tool_output_bytes_ = 0;

    /// The moment the running tool began, in milliseconds.
    int64_t tool_started_ms_ = 0;

    /// The time the turn that is running began, in milliseconds.
    int64_t turn_started_ms_ = 0;
    std::string session_id_;

    /// True when the session continues a conversation of an earlier one.
    bool resumed_ = false;

    /// The folder the conversation that is resumed belongs to, when it is
    /// not the folder the session runs in.
    std::string resumed_cwd_;

    /// The model the session runs on.
    std::string model_name_;

    /// True when the banner of the session is still to be drawn.
    ///
    /// The banner is drawn after the environment of the settings is applied,
    /// so that it names the model that the environment asks for.
    bool banner_pending_ = false;

    /// The pull request of the branch of the session, when it has one.
    std::optional<git::PrStatus> pr_status_;

    /// True while the session is alive.
    ///
    /// The threads that run in the background look at it, so that they do
    /// not write to a session that is gone.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    bool running_ = true;
    bool interrupted_ = false;

    /// The prompts the user has entered, oldest first.
    std::vector<std::string> history_;

    /// How far back in `history_` the user is browsing, or -1 when they are
    /// not. Zero is the newest entry.
    int history_index_ = -1;

    /// The text the user had typed before they started browsing the
    /// history. It comes back when they walk past the newest entry.
    std::string draft_;

    /// The suggestions shown above the prompt box.
    std::vector<ui::Suggestion> suggestions_;

    /// The index of the highlighted suggestion.
    size_t suggestion_index_ = 0;

    /// Ctrl-C, waiting for a second press to exit.
    PendingPress pending_ctrl_c_;

    /// Ctrl-D, waiting for a second press to exit.
    PendingPress pending_ctrl_d_;

    /// Escape, waiting for a second press to clear the input.
    PendingPress pending_escape_;

    /// The `/help` overlay, while it is open.
    std::unique_ptr<ui::HelpOverlay> help_overlay_;
};

} // namespace cc::cli
