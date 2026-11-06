#include <catch2/catch_test_macros.hpp>

#include "ui/ansi.hpp"
#include "ui/figures.hpp"
#include "ui/mcp_panel.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

using namespace cc;

namespace {

/// The panel of one server, in the state the test gives it.
ui::McpPanel panel_of(ui::McpPanelServer server) {
    ui::McpPanelGroup group;
    group.title = "User MCPs";
    group.path = "/Users/test/.claude.json";
    group.servers.push_back(std::move(server));
    return ui::McpPanel(ui::Theme::dark(), {std::move(group)});
}

/// The index of the choice with the given label.
///
/// The size of the list is returned when there is no such choice.
size_t index_of(const std::vector<std::string>& choices, const std::string& label) {
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i] == label) return i;
    }
    return choices.size();
}

/// The lines of the panel, without the colours.
std::string plain(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) out += ui::strip(line) + "\n";
    return out;
}

} // namespace

TEST_CASE("The menu of a server that waits for an authentication offers the authentication") {
    ui::McpPanelServer server;
    server.name = "waiting";
    server.state = "needs-auth";

    ui::McpPanel panel = panel_of(std::move(server));

    std::string authenticated;
    ui::McpPanel::Actions actions;
    actions.authenticate = [&](const std::string& name) { authenticated = name; };
    panel.set_actions(actions);

    panel.open_menu("waiting");
    const std::vector<std::string> choices = panel.menu_choices();

    // The reference lets the user authenticate a server that waits for an
    // authentication.
    REQUIRE(index_of(choices, "Authenticate") < choices.size());

    // The authentication is asked of the session with the name of the
    // server.
    panel.pick(index_of(choices, "Authenticate"));
    REQUIRE(authenticated == "waiting");
}

TEST_CASE("The menu of a server that does not wait for an authentication offers none") {
    ui::McpPanelServer server;
    server.name = "ready";
    server.state = "connected";
    server.tool_count = 3;

    ui::McpPanel panel = panel_of(std::move(server));

    std::string authenticated;
    ui::McpPanel::Actions actions;
    actions.authenticate = [&](const std::string& name) { authenticated = name; };
    panel.set_actions(actions);

    panel.open_menu("ready");
    const std::vector<std::string> choices = panel.menu_choices();

    REQUIRE(index_of(choices, "Authenticate") == choices.size());
    REQUIRE(index_of(choices, "Reconnect") < choices.size());
    REQUIRE(index_of(choices, "View tools (0)") < choices.size());
}

TEST_CASE("The state that waits for an authentication carries the sign of the warning") {
    ui::McpPanelServer server;
    server.name = "waiting";
    server.state = "needs-auth";

    ui::McpPanel panel = panel_of(std::move(server));
    const std::string body = plain(panel.render(80));

    // The sign of the warning stands in front of the name of the server,
    // and the label of the state stands behind it.
    REQUIRE(body.find(std::string(ui::figures::kWarning) + " waiting") != std::string::npos);
    REQUIRE(body.find("needs authentication") != std::string::npos);
}

TEST_CASE("The group of claude.ai carries no file") {
    ui::McpPanelGroup group;
    group.title = "claude.ai";
    group.has_file = false;

    ui::McpPanelServer server;
    server.name = "connector";
    server.state = "connected";
    server.tool_count = 2;
    group.servers.push_back(std::move(server));

    ui::McpPanel panel(ui::Theme::dark(), {std::move(group)});
    const std::string body = plain(panel.render(80));

    // The name of the group stands alone: there is no file behind it.
    REQUIRE(body.find("claude.ai") != std::string::npos);
    REQUIRE(body.find("claude.ai (") == std::string::npos);

    // The number of the tools of a server that is connected is written.
    REQUIRE(body.find("2 tools") != std::string::npos);
}

TEST_CASE("The servers of the agents stand under the name of the agent that uses them") {
    ui::McpPanelGroup group;
    group.title = "Agent MCPs";
    group.has_file = false;
    group.agents = true;

    ui::McpPanelServer server;
    server.name = "agent-server";
    server.state = "needs-auth";
    server.source_agents = {"explore"};
    group.servers.push_back(std::move(server));

    ui::McpPanel panel(ui::Theme::dark(), {std::move(group)});
    const std::string body = plain(panel.render(80));

    // The name of the group stands alone, and the name of the agent stands
    // under it.
    REQUIRE(body.find("Agent MCPs") != std::string::npos);
    REQUIRE(body.find("@explore") != std::string::npos);

    // The server of the agent may wait for an authentication.
    REQUIRE(body.find("may need auth") != std::string::npos);
}
