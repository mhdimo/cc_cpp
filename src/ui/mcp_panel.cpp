#include "ui/mcp_panel.hpp"

#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>

namespace cc::ui {
namespace {

/// The name of the state of a server, and the sign that goes with it.
struct StateLabel {
    /// The name of the state.
    const char* text;

    /// The sign that goes with it.
    const char* sign;

    /// True when the sign is the one of the warning.
    bool warning = false;
};

/// The label of a state of a server.
///
/// The reference names the state of the server and puts a sign in front of
/// it: the tick of the servers that are connected, the cross of the ones
/// that failed, and the sign of the warning for the ones that wait for an
/// authentication. The state of a server that is connected is not written;
/// the number of its tools is.
StateLabel label_of(const std::string& state) {
    if (state == "connected") return {"", "✔", false};
    if (state == "failed") return {"", "✘", false};
    if (state == "connecting") return {"connecting…", "◌", false};
    if (state == "needs-auth") return {"needs authentication", "⚠", true};
    if (state == "disabled") return {"", "◻", false};
    if (state == "pending") return {"", "◻", false};
    return {"", "◻", false};
}

/// The names of the agents that use the servers of a group, in the order
/// they are met.
///
/// The reference draws the servers of an agent under the name of it.
std::vector<std::string> agent_names(const McpPanelGroup& group) {
    std::vector<std::string> names;
    for (const McpPanelServer& server : group.servers) {
        for (const std::string& agent : server.source_agents) {
            if (std::find(names.begin(), names.end(), agent) != names.end()) continue;
            names.push_back(agent);
        }
    }
    return names;
}

/// True when the agent uses the server.
bool uses_agent(const McpPanelServer& server, const std::string& agent) {
    return std::find(server.source_agents.begin(), server.source_agents.end(), agent) !=
           server.source_agents.end();
}

} // namespace

/// The choices of the menu of a server.
///
/// The reference offers the tools of the server, the connection of it again,
/// the turning of it on and off, and the authentication of it when it waits
/// for one.
std::vector<McpPanel::Choice> McpPanel::menu_entries() const {
    std::vector<Choice> out;

    // The server that is turned off is turned on again; the one that is on
    // is turned off.
    const bool enabled = menu_enabled();
    out.push_back({"View tools (" + std::to_string(menu_tool_count()) + ")", Choice::Action::Tools});

    // The reference lets the user authenticate a server that waits for an
    // authentication, and offers nothing else for it.
    if (menu_needs_auth()) out.push_back({"Authenticate", Choice::Action::Authenticate});

    out.push_back({"Reconnect", Choice::Action::Reconnect});
    if (menu_can_disable()) out.push_back({enabled ? "Disable" : "Enable", Choice::Action::Toggle});
    out.push_back({"Back", Choice::Action::Back});

    return out;
}

std::vector<McpPanelTool> McpPanel::menu_tools() const {
    for (const auto& group : groups_) {
        for (const auto& server : group.servers) {
            if (server.name != menu_server_) continue;
            return server.tools;
        }
    }
    return {};
}

bool McpPanel::menu_enabled() const {
    for (const auto& group : groups_) {
        for (const auto& server : group.servers) {
            if (server.name != menu_server_) continue;
            // The reference turns off every server that is not turned off
            // already; a server that waits for an authentication is turned
            // on.
            return server.state == "connected" || server.state == "failed" ||
                   server.state == "connecting" || server.state == "pending" ||
                   server.state == "needs-auth";
        }
    }
    return false;
}

bool McpPanel::menu_needs_auth() const {
    for (const auto& group : groups_) {
        for (const auto& server : group.servers) {
            if (server.name != menu_server_) continue;
            return server.state == "needs-auth";
        }
    }
    return false;
}

size_t McpPanel::menu_tool_count() const {
    for (const auto& group : groups_) {
        for (const auto& server : group.servers) {
            if (server.name != menu_server_) continue;
            return server.tools.size();
        }
    }
    return 0;
}

bool McpPanel::menu_can_disable() const {
    for (const auto& group : groups_) {
        for (const auto& server : group.servers) {
            if (server.name != menu_server_) continue;
            return server.can_disable;
        }
    }
    return false;
}

McpPanel::McpPanel(Theme theme, std::vector<McpPanelGroup> groups)
    : theme_(std::move(theme)), groups_(std::move(groups)) {}

size_t McpPanel::count() const {
    size_t total = 0;
    for (const auto& group : groups_) total += group.servers.size();
    return total;
}

void McpPanel::open_menu(const std::string& name) {
    menu_server_ = name;
    menu_open_ = true;
    menu_selected_ = 0;
}

std::vector<std::string> McpPanel::menu_choices() const {
    std::vector<std::string> out;
    for (const Choice& choice : menu_entries()) out.push_back(choice.label);
    return out;
}

void McpPanel::pick(size_t index) {
    const std::vector<Choice> entries = menu_entries();
    if (index >= entries.size()) return;

    switch (entries[index].action) {
    case Choice::Action::Tools:
        // The tools of the server are shown in a view of their own, the way
        // the reference shows them.
        tools_open_ = true;
        break;
    case Choice::Action::Authenticate:
        if (actions_.authenticate) actions_.authenticate(menu_server_);
        break;
    case Choice::Action::Reconnect:
        if (actions_.reconnect) actions_.reconnect(menu_server_);
        break;
    case Choice::Action::Toggle:
        if (actions_.set_enabled) actions_.set_enabled(menu_server_, !menu_enabled());
        break;
    case Choice::Action::Back:
        // The way back to the list of the servers.
        menu_open_ = false;
        menu_selected_ = 0;
        break;
    }
}

std::vector<McpPanel::Row> McpPanel::rows() const {
    std::vector<Row> out;

    for (const McpPanelGroup& group : groups_) {
        if (group.servers.empty()) continue;

        // The servers of a group stand under the name of the group.
        if (!group.agents) {
            for (const McpPanelServer& server : group.servers) {
                out.push_back({&group, &server, {}});
            }
            continue;
        }

        // The servers of the agents stand under the name of the agent that
        // uses them, the way the reference draws them.
        for (const std::string& agent : agent_names(group)) {
            for (const McpPanelServer& server : group.servers) {
                if (!uses_agent(server, agent)) continue;
                out.push_back({&group, &server, agent});
            }
        }
    }

    return out;
}

std::string McpPanel::server_line(const McpPanelServer& server, bool agent_group, bool selected,
                                  size_t name_column) const {
    Style dim;
    dim.foreground = theme_.inactive;

    Style suggestion;
    suggestion.foreground = theme_.suggestion;

    Style normal;
    normal.foreground = theme_.text;

    const StateLabel label = label_of(server.state);

    // The sign of the state carries the colour of it.
    Style sign_style = dim;
    if (label.warning) {
        sign_style.foreground = theme_.warning;
    } else if (server.state == "connected") {
        sign_style.foreground = theme_.success;
    } else if (server.state == "failed") {
        sign_style.foreground = theme_.error;
    }

    // The note of the server: the number of the tools of the ones that are
    // connected, the state of the others.
    std::string note = label.text;
    if (server.state == "connected") {
        note = std::to_string(server.tool_count) +
               (server.tool_count == 1 ? " tool" : " tools");
    }

    // The servers of the agents are not connected; they may wait for an
    // authentication, the way the reference names them.
    if (agent_group) note = server.state == "needs-auth" ? "may need auth" : "agent-only";

    const std::string pointer = selected ? std::string(figures::kPointer) + " " : "  ";
    std::string line = selected ? styled(pointer, suggestion) : pointer;
    line += styled(label.sign, sign_style);
    line += " ";
    line += selected ? styled(server.name, suggestion) : styled(server.name, normal);

    // The name is padded, so that the notes stand in a column.
    const size_t used = cc::display_width(server.name);
    if (used < name_column) line += std::string(name_column - used, ' ');
    if (!note.empty()) line += styled(note, dim);

    return line;
}

std::vector<std::string> McpPanel::render(int width) const {
    (void)width;

    Style title_style;
    title_style.bold = true;
    title_style.foreground = theme_.permission;

    Style bold;
    bold.bold = true;

    Style dim;
    dim.foreground = theme_.inactive;

    Style suggestion;
    suggestion.foreground = theme_.suggestion;

    Style normal;
    normal.foreground = theme_.text;

    std::vector<std::string> lines;

    // The title of the panel, and the number of the servers under it.
    lines.push_back(styled("Manage MCP servers", title_style));
    const size_t servers = count();
    lines.push_back(styled(std::to_string(servers) + (servers == 1 ? " server" : " servers"), dim));
    lines.push_back("");

    bool failed = false;
    size_t index = 0;

    // The lines of the list of the servers, in the order they are drawn.
    const std::vector<Row> lines_of_servers = rows();

    // The names of the servers stand in a column of their own, and the note
    // of the server stands in the column after it.
    size_t name_column = 0;
    for (const Row& row : lines_of_servers) {
        name_column = std::max(name_column, cc::display_width(row.server->name));
    }
    name_column += 2;

    // The groups stand one after the other; the name of the scope, and the
    // file the servers of it are in, head each of them.
    const McpPanelGroup* current = nullptr;
    std::string current_agent;

    for (const Row& row : lines_of_servers) {
        if (row.group != current) {
            // The empty line that stands between two groups.
            if (current) lines.push_back("");
            current = row.group;

            std::string heading = "  " + styled(row.group->title, bold);
            if (row.group->has_file && !row.group->path.empty()) {
                heading += styled(" (" + row.group->path + ")", dim);
            }
            lines.push_back(heading);

            current_agent.clear();
        }

        // The servers of the agents stand under the name of the agent that
        // uses them.
        if (row.group->agents && row.agent != current_agent) {
            lines.push_back("");
            lines.push_back("  " + styled("@" + row.agent, dim));
            current_agent = row.agent;
        }

        if (row.server->state == "failed") failed = true;

        lines.push_back(server_line(*row.server, row.group->agents, index == selected_,
                                    name_column));
        ++index;
    }

    // The empty line that stands under the last group.
    if (current) lines.push_back("");

    // The note about the logs of the servers that failed.
    if (failed) {
        lines.push_back(styled("› Run claude --debug to see error logs", dim));
    }

    // The place where the documentation of the servers lives.
    lines.push_back(styled("https://code.claude.com/docs/en/mcp for help", dim));
    lines.push_back("");

    // The tools of the server that is chosen.
    if (tools_open_) {
        lines.push_back(styled("Tools of " + menu_server_, title_style));
        lines.push_back("");

        const std::vector<McpPanelTool> tools = menu_tools();
        if (tools.empty()) {
            lines.push_back(styled("The server offers no tools", dim));
        } else {
            for (const McpPanelTool& tool : tools) {
                lines.push_back("  " + styled(tool.name, normal));
                if (!tool.description.empty()) {
                    lines.push_back("    " + styled(cc::truncate(tool.description, 70), dim));
                }
            }
        }

        lines.push_back("");
        lines.push_back(styled("↑↓ to move · Esc to go back", dim));
        return lines;
    }

    // The menu of the server that is chosen.
    if (menu_open_) {
        lines.push_back(styled("  " + menu_server_, title_style));
        lines.push_back("");

        const std::vector<std::string> choices = menu_choices();
        for (size_t i = 0; i < choices.size(); ++i) {
            const bool selected = i == menu_selected_;
            const std::string pointer = selected ? std::string(figures::kPointer) + " " : "  ";
            lines.push_back(selected ? styled(pointer + choices[i], suggestion)
                                     : styled(pointer + choices[i], normal));
        }
        lines.push_back("");
        lines.push_back(styled("↑↓ to move · Enter to pick · Esc to go back", dim));
        return lines;
    }

    // The keys the user may press.
    lines.push_back(styled("↑↓ to navigate · Enter to confirm · Esc to cancel", dim));

    return lines;
}

void McpPanel::run(Terminal& terminal) {
    const int width = terminal.width();

    // The lines of the list of the servers; the user walks through them.
    const std::vector<Row> lines_of_servers = rows();
    const size_t servers = lines_of_servers.size();

    while (true) {
        terminal.render_dynamic(render(width));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return;
            }
            continue;
        }

        // The keys of the tools of a server are its own.
        if (tools_open_) {
            switch (key->type) {
            case Key::Type::Escape:
                tools_open_ = false;
                break;
            case Key::Type::CtrlC:
            case Key::Type::CtrlD:
                terminal.clear_dynamic();
                return;
            default:
                break;
            }
            continue;
        }

        // The keys of the menu of a server are its own.
        if (menu_open_) {
            const std::vector<std::string> menu = menu_choices();
            switch (key->type) {
            case Key::Type::Up:
                if (menu_selected_ > 0) --menu_selected_;
                break;
            case Key::Type::Down:
                if (menu_selected_ + 1 < menu.size()) ++menu_selected_;
                break;
            case Key::Type::Enter:
                pick(menu_selected_);
                break;
            case Key::Type::Escape:
                // The way back to the list of the servers.
                menu_open_ = false;
                menu_selected_ = 0;
                break;
            case Key::Type::CtrlC:
            case Key::Type::CtrlD:
                terminal.clear_dynamic();
                return;
            default:
                break;
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Up:
            if (selected_ == 0) break;
            --selected_;
            break;
        case Key::Type::Down:
            if (servers == 0 || selected_ + 1 >= servers) break;
            ++selected_;
            break;
        case Key::Type::Enter: {
            // The reference opens the menu of the server that is chosen.
            if (selected_ < lines_of_servers.size()) {
                open_menu(lines_of_servers[selected_].server->name);
            }
            break;
        }
        case Key::Type::Escape:
        case Key::Type::CtrlC:
        case Key::Type::CtrlD:
            terminal.clear_dynamic();
            return;
        default:
            break;
        }
    }
}

} // namespace cc::ui
