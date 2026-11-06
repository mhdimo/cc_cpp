#pragma once

#include "ui/ansi.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <functional>
#include <string>
#include <vector>

namespace cc::ui {

/// One tool of a server of MCP, as the panel shows it.
struct McpPanelTool {
    /// The name of the tool.
    std::string name;

    /// The description of the tool.
    std::string description;
};

/// One server of MCP, as the panel shows it.
struct McpPanelServer {
    /// The name of the server.
    std::string name;

    /// The state of the server: `connected`, `failed`, `connecting`,
    /// `disabled`, `pending`, `inactive` or `needs-auth`.
    ///
    /// The servers of the group of the agents are in the state
    /// `needs-auth` when they wait for an authentication; the reference
    /// names the other ones `agent-only`.
    std::string state;

    /// The reason the server could not be used.
    std::string error;

    /// The number of the tools the server offers.
    ///
    /// The reference writes the number next to the servers that are
    /// connected.
    size_t tool_count = 0;

    /// The tools the server offers.
    std::vector<McpPanelTool> tools;

    /// True when the server may be turned off.
    ///
    /// The reference turns the servers of the `.mcp.json` file of a project
    /// off; the ones of the user are turned off in the settings of the
    /// user.
    bool can_disable = false;

    /// The names of the agents that use the server.
    ///
    /// The reference draws the servers of the group of the agents under the
    /// name of the agent that uses them.
    std::vector<std::string> source_agents;
};

/// One group of servers, as the panel shows it.
struct McpPanelGroup {
    /// The name of the group, e.g. `Project MCPs`.
    std::string title;

    /// The path of the file the servers of the group are in, when there is
    /// one.
    std::string path;

    /// True when the servers of the group are written in a file.
    ///
    /// The groups of claude.ai and of the agents have no file behind them:
    /// the reference writes the name of those groups alone.
    bool has_file = true;

    /// True when the group is the group of the agents.
    ///
    /// The reference draws the servers of an agent under the name of it.
    bool agents = false;

    /// The servers of the group.
    std::vector<McpPanelServer> servers;
};

/// The panel of the servers of MCP.
///
/// The panel is the one the reference shows for `/mcp`: the servers stand
/// under the scope they are in, and the state of each of them is named.
class McpPanel {
public:
    McpPanel(Theme theme, std::vector<McpPanelGroup> groups);

    /// The actions the menu of a server offers.
    struct Actions {
        /// Reconnects the server.
        std::function<void(const std::string& name)> reconnect;

        /// Turns the server on or off.
        std::function<void(const std::string& name, bool enabled)> set_enabled;

        /// Authenticates the server.
        ///
        /// The reference lets the user authenticate a server that waits for
        /// an authentication; the port asks the session to do it.
        std::function<void(const std::string& name)> authenticate;
    };

    /// The actions the menu offers.
    void set_actions(Actions actions) { actions_ = std::move(actions); }

    /// Renders the panel.
    std::vector<std::string> render(int width) const;

    /// Opens the menu of the server with the given name.
    ///
    /// The reference opens the menu of the server the user chooses.
    void open_menu(const std::string& name);

    /// The choices the menu of the server offers.
    std::vector<std::string> menu_choices() const;

    /// Picks the choice with the given index.
    ///
    /// Nothing happens when the index is out of range.
    void pick(size_t index);

    /// Runs the panel until the user closes it.
    ///
    /// The reference opens the menu of the server that is chosen; the port
    /// only moves the highlight and closes the panel.
    void run(Terminal& terminal);

private:
    /// One choice of the menu of a server.
    struct Choice {
        /// The label of the choice.
        std::string label;

        /// What the choice does.
        enum class Action { Tools, Authenticate, Reconnect, Toggle, Back };

        /// What the choice does.
        Action action = Action::Back;
    };

    /// One line of the list of the servers.
    struct Row {
        /// The group the server is in.
        const McpPanelGroup* group = nullptr;

        /// The server of the line.
        const McpPanelServer* server = nullptr;

        /// The name of the agent the server is drawn under, when there is
        /// one.
        std::string agent;
    };

    /// The number of the servers of the panel.
    size_t count() const;

    /// Every line of the list of the servers, in the order they are drawn.
    ///
    /// The servers of the group of the agents stand once under every agent
    /// that uses them, the way the reference draws them.
    std::vector<Row> rows() const;

    /// The choices of the menu of a server.
    std::vector<Choice> menu_entries() const;

    /// True when the server whose menu is open is turned on.
    bool menu_enabled() const;

    /// True when the server whose menu is open may be turned off.
    bool menu_can_disable() const;

    /// True when the server whose menu is open waits for an authentication.
    bool menu_needs_auth() const;

    /// The number of the tools of the server whose menu is open.
    size_t menu_tool_count() const;

    /// The tools of the server whose menu is open.
    std::vector<McpPanelTool> menu_tools() const;

    /// The line of a server of the panel.
    ///
    /// The name of the server is padded to `name_column`, so that the notes
    /// of the servers stand in a column of their own.
    std::string server_line(const McpPanelServer& server, bool agent_group, bool selected,
                            size_t name_column) const;

    Theme theme_;
    std::vector<McpPanelGroup> groups_;

    /// The index of the server that is highlighted.
    size_t selected_ = 0;

    /// The actions the menu offers.
    Actions actions_;

    /// True when the menu of a server is open.
    bool menu_open_ = false;

    /// True when the tools of the server are shown.
    bool tools_open_ = false;

    /// The index of the choice of the menu that is highlighted.
    size_t menu_selected_ = 0;

    /// The server whose menu is open.
    std::string menu_server_;
};

} // namespace cc::ui
