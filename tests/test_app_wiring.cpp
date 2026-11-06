#include <catch2/catch_test_macros.hpp>

#include "cli/app.hpp"
#include "ui/ansi.hpp"
#include "ui/mcp_panel.hpp"
#include "ui/plugin_panel.hpp"
#include "ui/theme.hpp"
#include "utils/fs.hpp"
#include "utils/path.hpp"

#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cc;

namespace {

/// A folder that is removed when the test is over.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const char* base = std::getenv("TMPDIR");
        std::string root = base != nullptr ? base : "/tmp";
        while (!root.empty() && root.back() == '/') root.pop_back();

        path_ = path_join(path_join(root, "cc-app-wiring-test-" + std::to_string(::getpid())),
                          "home");
        remove_dir_all(path_join(path_join(root, "cc-app-wiring-test-" + std::to_string(::getpid())),
                                  "home"));
        mkdirp(path_);

        const char* previous = std::getenv("CLAUDE_CONFIG_DIR");
        had_previous_ = previous != nullptr;
        if (previous != nullptr) previous_ = previous;
        ::setenv("CLAUDE_CONFIG_DIR", path_.c_str(), 1);
    }

    ~TemporaryDirectory() {
        if (had_previous_) {
            ::setenv("CLAUDE_CONFIG_DIR", previous_.c_str(), 1);
        } else {
            ::unsetenv("CLAUDE_CONFIG_DIR");
        }
        remove_dir_all(path_);
    }

    /// The folder of the configuration of the user.
    std::string path() const { return path_; }

private:
    std::string path_;
    std::string previous_{};
    bool had_previous_ = false;
};

/// Writes a file, making the folders it needs.
void write(const std::string& path, const std::string& contents) {
    mkdirp(dirname(path));
    write_file(path, contents);
}

/// Puts a plugin into the cache of the user.
void put_plugin(const std::string& home, const std::string& marketplace,
                const std::string& name, const std::string& version) {
    const std::string root = path_join(
        path_join(path_join(path_join(path_join(home, "plugins"), "cache"), marketplace), name),
        version);
    write(path_join(path_join(root, ".claude-plugin"), "plugin.json"),
          "{\"name\": \"" + name + "\", \"version\": \"" + version +
              "\", \"description\": \"A plugin of the test\"}");
}

/// The text of the lines, without the colours.
std::string plain(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) out += ui::strip(line) + "\n";
    return out;
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

} // namespace

TEST_CASE("The panel of the plugins carries the components of a plugin") {
    TemporaryDirectory home;
    put_plugin(home.path(), "official", "my-plugin", "1.2.3");

    const std::string root = path_join(
        path_join(path_join(path_join(path_join(home.path(), "plugins"), "cache"), "official"),
                  "my-plugin"),
        "1.2.3");

    // The plugin brings a command, an agent and a hook.
    write(path_join(path_join(root, "commands"), "build.md"),
          "---\ndescription: Builds the thing\n---\nBuild it.\n");
    write(path_join(path_join(root, "agents"), "reviewer.md"),
          "---\nname: reviewer\ndescription: Reviews the code\n---\nYou review the code.\n");
    write(path_join(path_join(root, "hooks"), "hooks.json"),
          "{\"hooks\": {\"PreToolUse\": [{\"matcher\": \"Write\", \"hooks\": "
          "[{\"type\": \"command\", \"command\": \"echo hi\"}]}]}}");

    // The session fills the components of the plugin in, the way the
    // reference shows them in the details of the plugin.
    ui::PluginPanelEntry entry;
    entry.name = "my-plugin";
    entry.id = "my-plugin@official";
    entry.marketplace = "official";
    entry.installed = true;
    entry.components = cli::components_of(entry);

    REQUIRE(entry.components.commands == std::vector<std::string>{"build"});
    REQUIRE(entry.components.agents == std::vector<std::string>{"reviewer"});
    REQUIRE(entry.components.hooks == std::vector<std::string>{"PreToolUse"});

    // The details of the plugin name the three sections.
    ui::PluginPanel panel(ui::Theme::dark(), {}, {}, {}, {});
    const std::string text = plain(panel.render_details(entry, /*installed=*/true, 80, 24, 3));

    REQUIRE(text.find("Installed components:") != std::string::npos);
    REQUIRE(text.find("Commands: build") != std::string::npos);
    REQUIRE(text.find("Agents: reviewer") != std::string::npos);
    REQUIRE(text.find("Hooks: PreToolUse") != std::string::npos);
}

TEST_CASE("The panel of the plugins carries the components of a plugin of a marketplace") {
    TemporaryDirectory home;

    // A marketplace that holds a plugin in a folder of its own.
    write(path_join(path_join(path_join(home.path(), "shop"), ".claude-plugin"), "marketplace.json"),
          "{\"name\": \"shop\", \"plugins\": [{\"name\": \"greeter\", \"version\": \"0.1.0\", "
          "\"source\": \"./plugins/greeter\"}]}");
    write(path_join(home.path(), "settings.json"),
          "{\"extraKnownMarketplaces\": {\"shop\": {\"source\": \"" +
              path_join(home.path(), "shop") + "\"}}}");

    const std::string plugin = path_join(path_join(home.path(), "shop"), "plugins/greeter");
    write(path_join(path_join(plugin, "commands"), "greet.md"),
          "---\ndescription: Greets\n---\nSay hello.\n");
    write(path_join(path_join(plugin, "agents"), "reviewer.md"),
          "---\nname: reviewer\ndescription: Reviews the code\n---\nYou review the code.\n");
    write(path_join(path_join(plugin, "hooks"), "hooks.json"),
          "{\"hooks\": {\"PreToolUse\": [{\"matcher\": \"Write\", \"hooks\": "
          "[{\"type\": \"command\", \"command\": \"echo hi\"}]}]}}");

    // A plugin that is not installed yet is read out of the folder of the
    // marketplace that offers it.
    ui::PluginPanelEntry entry;
    entry.name = "greeter";
    entry.id = "greeter@shop";
    entry.marketplace = "shop";
    entry.components = cli::components_of(entry);

    REQUIRE(entry.components.commands == std::vector<std::string>{"greet"});
    REQUIRE(entry.components.agents == std::vector<std::string>{"reviewer"});
    REQUIRE(entry.components.hooks == std::vector<std::string>{"PreToolUse"});
}

TEST_CASE("The panel of the servers of MCP shows the authentication of a server that needs one") {
    ui::McpPanelServer server;
    server.name = "waiting";
    server.state = "needs-auth";

    ui::McpPanelGroup group;
    group.title = "User MCPs";
    group.servers.push_back(std::move(server));

    ui::McpPanel panel(ui::Theme::dark(), {std::move(group)});

    // The session is asked to authenticate the server.
    std::string authenticated;
    ui::McpPanel::Actions actions;
    actions.authenticate = [&](const std::string& name) { authenticated = name; };
    panel.set_actions(actions);

    panel.open_menu("waiting");
    const std::vector<std::string> choices = panel.menu_choices();

    // The reference lets the user authenticate a server that waits for an
    // authentication.
    REQUIRE(index_of(choices, "Authenticate") < choices.size());

    // Picking it asks the session to authenticate the server that is named.
    panel.pick(index_of(choices, "Authenticate"));
    REQUIRE(authenticated == "waiting");
}

TEST_CASE("The servers the agents use are read out of their frontmatter") {
    TemporaryDirectory home;

    // An agent that brings a server of its own.
    const std::string path = path_join(path_join(home.path(), "agents"), "explorer.md");
    write(path,
          "---\nname: explorer\ndescription: Explores the code\nmcpServers:\n  - search:\n      "
          "command: search-mcp\n  - other\n---\nYou explore the code.\n");

    agents::Definition definition;
    definition.name = "explorer";
    definition.description = "Explores the code";
    definition.path = path;

    const auto servers = cli::agent_mcp_servers({definition});

    // The server the agent brings is named; the one that is only referred
    // to is left out, the way the reference leaves it out.
    REQUIRE(servers.size() == 1);
    REQUIRE(servers.count("search") == 1);
    REQUIRE(servers.at("search") == std::vector<std::string>{"explorer"});
}
