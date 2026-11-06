#include <catch2/catch_test_macros.hpp>

#include "plugins.hpp"
#include "ui/ansi.hpp"
#include "ui/plugin_panel.hpp"
#include "ui/theme.hpp"
#include "utils/fs.hpp"
#include "utils/json_util.hpp"
#include "utils/path.hpp"

#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace cc;

namespace {

/// A folder that is removed when the test is over.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const char* base = std::getenv("TMPDIR");
        std::string root = base != nullptr ? base : "/tmp";
        while (!root.empty() && root.back() == '/') root.pop_back();

        path_ = path_join(path_join(root, "cc-plugin-test-" + std::to_string(::getpid())),
                          "home");
        remove_dir_all(path_join(path_join(root, "cc-plugin-test-" + std::to_string(::getpid())),
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

/// Writes the settings of the user.
void write_settings(const std::string& home, const std::string& contents) {
    write(path_join(home, "settings.json"), contents);
}

} // namespace

TEST_CASE("A plugin of the cache is found") {
    TemporaryDirectory home;
    put_plugin(home.path(), "official", "my-plugin", "1.2.3");

    const auto installed = plugins::installed_plugins();
    REQUIRE(installed.size() == 1);
    REQUIRE(installed[0].name == "my-plugin");
    REQUIRE(installed[0].marketplace == "official");
    REQUIRE(installed[0].id == "my-plugin@official");
    REQUIRE(installed[0].version == "1.2.3");
    REQUIRE(installed[0].description == "A plugin of the test");
    REQUIRE(installed[0].enabled == false);
}

TEST_CASE("A plugin is on when the settings say so") {
    TemporaryDirectory home;
    put_plugin(home.path(), "official", "my-plugin", "1.2.3");
    write_settings(home.path(), "{\"enabledPlugins\": {\"my-plugin@official\": true}}");

    const auto installed = plugins::installed_plugins();
    REQUIRE(installed.size() == 1);
    REQUIRE(installed[0].enabled == true);
}

TEST_CASE("A plugin brings its commands, its agents and its hooks") {
    TemporaryDirectory home;
    put_plugin(home.path(), "official", "my-plugin", "1.2.3");
    write_settings(home.path(), "{\"enabledPlugins\": {\"my-plugin@official\": true}}");

    const std::string root = path_join(
        path_join(path_join(path_join(path_join(home.path(), "plugins"), "cache"), "official"),
                  "my-plugin"),
        "1.2.3");

    write(path_join(path_join(root, "commands"), "build.md"),
          "---\ndescription: Builds the thing\nargument-hint: <target>\n---\nBuild $ARGUMENTS "
          "with ${CLAUDE_PLUGIN_ROOT}.\n");
    write(path_join(path_join(root, "agents"), "reviewer.md"),
          "---\nname: reviewer\ndescription: Reviews the code\n---\nYou review the code.\n");
    write(path_join(path_join(root, "hooks"), "hooks.json"),
          "{\"hooks\": {\"PreToolUse\": [{\"matcher\": \"Write\", \"hooks\": "
          "[{\"type\": \"command\", \"command\": \"echo ${CLAUDE_PLUGIN_ROOT}\"}]}]}}");

    const auto commands = plugins::plugin_commands();
    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0].name == "my-plugin:build");
    REQUIRE(commands[0].description == "Builds the thing");
    REQUIRE(commands[0].argument_hint == "<target>");
    REQUIRE(commands[0].content == "Build $ARGUMENTS with ${CLAUDE_PLUGIN_ROOT}.");
    REQUIRE(commands[0].plugin == "my-plugin");
    REQUIRE(commands[0].marketplace == "official");

    const auto agents = plugins::plugin_agents();
    REQUIRE(agents.size() == 1);
    REQUIRE(agents[0].name == "reviewer");
    REQUIRE(agents[0].description == "Reviews the code");
    REQUIRE(agents[0].prompt == "You review the code.");

    const auto hooks = plugins::plugin_hooks();
    REQUIRE(hooks.size() == 1);
    REQUIRE(hooks[0].matcher == "Write");
    REQUIRE(hooks[0].command == "echo " + root);
}

TEST_CASE("A plugin that is off brings nothing") {
    TemporaryDirectory home;
    put_plugin(home.path(), "official", "my-plugin", "1.2.3");

    const std::string root = path_join(
        path_join(path_join(path_join(path_join(home.path(), "plugins"), "cache"), "official"),
                  "my-plugin"),
        "1.2.3");
    write(path_join(path_join(root, "commands"), "build.md"), "---\ndescription: Builds\n---\nBuild.\n");

    REQUIRE(plugins::plugin_commands().empty());
    REQUIRE(plugins::plugin_agents().empty());
    REQUIRE(plugins::plugin_hooks().empty());
}

TEST_CASE("The marketplace of a folder offers its plugins") {
    TemporaryDirectory home;

    // A marketplace that holds its plugins in a folder of its own.
    write(path_join(path_join(path_join(home.path(), "shop"), ".claude-plugin"), "marketplace.json"),
          "{\"name\": \"shop\", \"plugins\": [{\"name\": \"greeter\", \"description\": \"Says hello\", "
          "\"version\": \"0.1.0\", \"source\": \"./plugins/greeter\"}]}");
    write_settings(home.path(),
                   "{\"extraKnownMarketplaces\": {\"shop\": {\"source\": \"" +
                       path_join(home.path(), "shop") + "\"}}}");

    const auto marketplaces = plugins::known_marketplaces();
    REQUIRE(marketplaces.size() == 1);
    REQUIRE(marketplaces[0].name == "shop");

    const auto offered = plugins::marketplace_plugins("shop");
    REQUIRE(offered.size() == 1);
    REQUIRE(offered[0].name == "greeter");
    REQUIRE(offered[0].description == "Says hello");
    REQUIRE(offered[0].version == "0.1.0");
    REQUIRE(offered[0].path == path_join(path_join(path_join(home.path(), "shop"), "plugins"),
                                         "greeter"));
}

TEST_CASE("A marketplace that was fetched is read from where it was fetched to") {
    TemporaryDirectory home;

    // The marketplace was fetched into a file of its own.
    const std::string fetched = path_join(home.path(), "shop.json");
    write(fetched,
          "{\"name\": \"shop\", \"plugins\": [{\"name\": \"greeter\", \"source\": \"./p\"}]}");

    write_settings(home.path(),
                   "{\"extraKnownMarketplaces\": {\"shop\": {\"source\": "
                   "\"https://example.com/shop.json\", \"installLocation\": \"" +
                       fetched + "\"}}}");

    const auto offered = plugins::marketplace_plugins("shop");
    REQUIRE(offered.size() == 1);
    REQUIRE(offered[0].name == "greeter");
}

TEST_CASE("The source of a plugin of a marketplace is read") {
    TemporaryDirectory home;

    // A marketplace that offers a plugin of its own repository.
    write(path_join(path_join(path_join(home.path(), "shop"), ".claude-plugin"), "marketplace.json"),
          "{\"name\": \"shop\", \"plugins\": [{\"name\": \"greeter\", \"source\": "
          "{\"source\": \"github\", \"repo\": \"owner/greeter\"}}, {\"name\": \"nested\", "
          "\"source\": {\"source\": \"git-subdir\", \"url\": \"https://example.com/repo.git\", "
          "\"path\": \"tools/plugin\"}}]}");
    write_settings(home.path(),
                   "{\"extraKnownMarketplaces\": {\"shop\": {\"source\": \"" +
                       path_join(home.path(), "shop") + "\"}}}");

    const auto offered = plugins::marketplace_plugins("shop");
    REQUIRE(offered.size() == 2);

    REQUIRE(offered[0].name == "greeter");
    REQUIRE(offered[0].source_kind == "github");
    REQUIRE(offered[0].source_url == "https://github.com/owner/greeter.git");

    REQUIRE(offered[1].name == "nested");
    REQUIRE(offered[1].source_kind == "git-subdir");
    REQUIRE(offered[1].source_url == "https://example.com/repo.git");
    REQUIRE(offered[1].source_path == "tools/plugin");
}

TEST_CASE("A plugin declares the options it asks the user for") {
    TemporaryDirectory home;
    put_plugin(home.path(), "official", "my-plugin", "1.2.3");
    write_settings(home.path(), "{\"enabledPlugins\": {\"my-plugin@official\": true}}");

    // The plugin asks for a name and for a token.
    const std::string root = path_join(
        path_join(path_join(path_join(path_join(home.path(), "plugins"), "cache"), "official"),
                  "my-plugin"),
        "1.2.3");
    write(path_join(path_join(root, ".claude-plugin"), "plugin.json"),
          "{\"name\": \"my-plugin\", \"version\": \"1.2.3\", \"userConfig\": {"
          "\"NAME\": {\"type\": \"string\", \"title\": \"The name\", \"description\": \"Who you are\", "
          "\"required\": true}, \"TOKEN\": {\"type\": \"string\", \"title\": \"The token\", "
          "\"sensitive\": true, \"default\": \"none\"}}}");

    const auto installed = plugins::installed_plugins();
    REQUIRE(installed.size() == 1);

    const auto options = plugins::plugin_options(installed[0]);
    REQUIRE(options.size() == 2);
    REQUIRE(options[0].key == "NAME");
    REQUIRE(options[0].type == "string");
    REQUIRE(options[0].title == "The name");
    REQUIRE(options[0].required == true);
    REQUIRE(options[1].key == "TOKEN");
    REQUIRE(options[1].sensitive == true);
    REQUIRE(options[1].default_value == "none");
}

TEST_CASE("The values the user gave a plugin are kept") {
    TemporaryDirectory home;

    boost::json::object values;
    values["NAME"] = boost::json::value(std::string("Ada"));

    std::string error;
    REQUIRE(plugins::save_plugin_options("my-plugin@official", values, &error));
    REQUIRE(error.empty());

    const auto stored = plugins::load_plugin_options("my-plugin@official");
    REQUIRE(stored.size() == 1);
    REQUIRE(jget_string(stored, "NAME") == "Ada");
}

TEST_CASE("The values of a plugin are put in its text") {
    TemporaryDirectory home;

    boost::json::object values;
    values["NAME"] = boost::json::value(std::string("Ada"));
    std::string error;
    plugins::save_plugin_options("my-plugin@official", values, &error);

    const std::string text = "Hello ${user_config.NAME}, the root is ${CLAUDE_PLUGIN_ROOT}.";
    const std::string replaced = plugins::substitute_plugin_variables(text, "/tmp/plugin",
                                                                     "my-plugin@official");
    REQUIRE(replaced == "Hello Ada, the root is /tmp/plugin.");
}

TEST_CASE("A plugin of a marketplace is installed and turned on") {    TemporaryDirectory home;

    write(path_join(path_join(path_join(home.path(), "shop"), ".claude-plugin"), "marketplace.json"),
          "{\"name\": \"shop\", \"plugins\": [{\"name\": \"greeter\", \"version\": \"0.1.0\", "
          "\"source\": \"./plugins/greeter\"}]}");
    write(path_join(path_join(path_join(path_join(path_join(home.path(), "shop"), "plugins"),
                                      "greeter"),
                                  "commands"),
                    "greet.md"),
          "---\ndescription: Greets\n---\nSay hello.\n");
    write_settings(home.path(),
                   "{\"extraKnownMarketplaces\": {\"shop\": {\"source\": \"" +
                       path_join(home.path(), "shop") + "\"}}}");

    std::string error;
    REQUIRE(plugins::install_plugin("shop", "greeter", &error));
    REQUIRE(error.empty());

    const auto installed = plugins::installed_plugins();
    REQUIRE(installed.size() == 1);
    REQUIRE(installed[0].name == "greeter");
    REQUIRE(installed[0].id == "greeter@shop");
    REQUIRE(installed[0].enabled == true);

    // The command of the plugin is offered under the name of the plugin.
    const auto commands = plugins::plugin_commands();
    REQUIRE(commands.size() == 1);
    REQUIRE(commands[0].name == "greeter:greet");
    REQUIRE(commands[0].description == "Greets");
}

TEST_CASE("A plugin that is installed can be removed") {
    TemporaryDirectory home;
    put_plugin(home.path(), "official", "my-plugin", "1.2.3");
    write_settings(home.path(), "{\"enabledPlugins\": {\"my-plugin@official\": true}}");

    std::string error;
    REQUIRE(plugins::uninstall_plugin("my-plugin@official", &error));
    REQUIRE(error.empty());
    REQUIRE(plugins::installed_plugins().empty());
    REQUIRE(plugins::plugin_commands().empty());
}

TEST_CASE("The variables of a plugin are replaced") {
    const std::string text = "the root is ${CLAUDE_PLUGIN_ROOT} and the data ${CLAUDE_PLUGIN_DATA}";
    const std::string replaced = plugins::substitute_plugin_variables(text, "/tmp/plugin",
                                                                     "my-plugin@official");
    REQUIRE(replaced == "the root is /tmp/plugin and the data " +
                            path_join(path_join(plugins::plugins_directory(), "data"),
                                      "my-plugin-official"));
}

TEST_CASE("The components of a plugin are read from its folder") {
    TemporaryDirectory home;
    put_plugin(home.path(), "official", "my-plugin", "1.2.3");
    write_settings(home.path(), "{\"enabledPlugins\": {\"my-plugin@official\": true}}");

    const std::string root = path_join(
        path_join(path_join(path_join(path_join(home.path(), "plugins"), "cache"), "official"),
                  "my-plugin"),
        "1.2.3");

    write(path_join(path_join(root, "commands"), "build.md"), "---\ndescription: Builds\n---\nBuild.\n");
    write(path_join(path_join(root, "agents"), "reviewer.md"),
          "---\nname: reviewer\ndescription: Reviews the code\n---\nYou review the code.\n");
    write(path_join(path_join(path_join(root, "skills"), "summarise"), "SKILL.md"),
          "---\nname: summarise\ndescription: Summarises\n---\nSummarise.\n");
    write(path_join(path_join(root, "hooks"), "hooks.json"),
          "{\"hooks\": {\"PreToolUse\": [{\"matcher\": \"Write\", \"hooks\": "
          "[{\"type\": \"command\", \"command\": \"echo hi\"}]}]}}");
    write(path_join(root, ".mcp.json"), "{\"mcpServers\": {\"tools\": {\"command\": \"echo\"}}}");

    const auto components = plugins::plugin_components("my-plugin@official");
    REQUIRE(components.commands == std::vector<std::string>{"build"});
    REQUIRE(components.agents == std::vector<std::string>{"reviewer"});
    REQUIRE(components.skills == std::vector<std::string>{"summarise"});
    REQUIRE(components.hooks == std::vector<std::string>{"PreToolUse"});
    REQUIRE(components.mcp_servers == std::vector<std::string>{"tools"});
    REQUIRE(components.unknown == false);
    REQUIRE(components.empty() == false);
}

TEST_CASE("The details of a plugin name the components it brings") {
    TemporaryDirectory home;

    // A marketplace that holds a plugin in a folder of its own.
    write(path_join(path_join(path_join(home.path(), "shop"), ".claude-plugin"), "marketplace.json"),
          "{\"name\": \"shop\", \"plugins\": [{\"name\": \"greeter\", \"version\": \"0.1.0\", "
          "\"source\": \"./plugins/greeter\"}]}");
    write_settings(home.path(),
                   "{\"extraKnownMarketplaces\": {\"shop\": {\"source\": \"" +
                       path_join(home.path(), "shop") + "\"}}}");

    // The plugin brings a command, an agent and a hook.
    const std::string plugin = path_join(path_join(home.path(), "shop"), "plugins/greeter");
    write(path_join(path_join(plugin, "commands"), "greet.md"),
          "---\ndescription: Greets\n---\nSay hello.\n");
    write(path_join(path_join(plugin, "agents"), "reviewer.md"),
          "---\nname: reviewer\ndescription: Reviews the code\n---\nYou review the code.\n");
    write(path_join(path_join(plugin, "hooks"), "hooks.json"),
          "{\"hooks\": {\"PreToolUse\": [{\"matcher\": \"Write\", \"hooks\": "
          "[{\"type\": \"command\", \"command\": \"echo hi\"}]}]}}");

    const ui::Theme theme = ui::Theme::dark();
    ui::PluginPanel panel(theme, {}, {}, {}, {});

    ui::PluginPanelEntry entry;
    entry.name = "greeter";
    entry.id = "greeter@shop";
    entry.marketplace = "shop";
    entry.version = "0.1.0";
    entry.description = "A plugin of the test";

    const auto lines = panel.render_details(entry, /*installed=*/false, 80, 24, 4);

    std::string text;
    for (const std::string& line : lines) {
        text += ui::strip(line) + "\n";
    }

    // The three sections are named, under the heading of the reference.
    REQUIRE(text.find("Will install:") != std::string::npos);
    REQUIRE(text.find("Commands: greet") != std::string::npos);
    REQUIRE(text.find("Agents: reviewer") != std::string::npos);
    REQUIRE(text.find("Hooks: PreToolUse") != std::string::npos);
}

TEST_CASE("The details of a plugin that is installed name its components") {
    TemporaryDirectory home;
    put_plugin(home.path(), "official", "my-plugin", "1.2.3");
    write_settings(home.path(), "{\"enabledPlugins\": {\"my-plugin@official\": true}}");

    const std::string root = path_join(
        path_join(path_join(path_join(path_join(home.path(), "plugins"), "cache"), "official"),
                  "my-plugin"),
        "1.2.3");
    write(path_join(path_join(root, "commands"), "build.md"), "---\ndescription: Builds\n---\nBuild.\n");

    const ui::Theme theme = ui::Theme::dark();
    ui::PluginPanel panel(theme, {}, {}, {}, {});

    ui::PluginPanelEntry entry;
    entry.name = "my-plugin";
    entry.id = "my-plugin@official";
    entry.marketplace = "official";
    entry.installed = true;

    const auto lines = panel.render_details(entry, /*installed=*/true, 80, 24, 3);

    std::string text;
    for (const std::string& line : lines) {
        text += ui::strip(line) + "\n";
    }

    REQUIRE(text.find("Installed components:") != std::string::npos);
    REQUIRE(text.find("Commands: build") != std::string::npos);
}

TEST_CASE("The details of a plugin that is not on this machine say so") {
    const ui::Theme theme = ui::Theme::dark();
    ui::PluginPanel panel(theme, {}, {}, {}, {});

    ui::PluginPanelEntry entry;
    entry.name = "my-plugin";
    entry.id = "my-plugin@official";
    entry.marketplace = "official";
    entry.components.unknown = true;

    const auto lines = panel.render_details(entry, /*installed=*/false, 80, 24, 4);

    std::string text;
    for (const std::string& line : lines) {
        text += ui::strip(line) + "\n";
    }

    REQUIRE(text.find("Component summary not available for remote plugin") != std::string::npos);
}

TEST_CASE("The details of a plugin are cut to the room of the panel") {
    const ui::Theme theme = ui::Theme::dark();
    ui::PluginPanel panel(theme, {}, {}, {}, {});

    ui::PluginPanelEntry entry;
    entry.name = "my-plugin";
    entry.id = "my-plugin@official";
    entry.marketplace = "official";
    entry.description = "A plugin of the test";
    entry.components.commands = {"build", "test", "ship"};
    entry.components.agents = {"reviewer", "critic"};
    entry.components.skills = {"summarise"};
    entry.components.hooks = {"PreToolUse", "PostToolUse"};
    entry.components.mcp_servers = {"tools"};

    // The panel takes six rows for itself, and the menu of the plugin takes
    // the four that are reserved here.
    const auto lines = panel.render_details(entry, /*installed=*/false, 80, 12, 4);
    REQUIRE(lines.size() <= 12 - 6 - 4);

    // The details of a panel that is tall enough are whole.
    const auto whole = panel.render_details(entry, /*installed=*/false, 80, 40, 4);
    REQUIRE(whole.size() > lines.size());
}

TEST_CASE("The number of the installs is written the way the reference writes it") {
    REQUIRE(plugins::format_install_count(0) == "0");
    REQUIRE(plugins::format_install_count(42) == "42");
    REQUIRE(plugins::format_install_count(999) == "999");
    REQUIRE(plugins::format_install_count(1000) == "1K");
    REQUIRE(plugins::format_install_count(1280437) == "1.3M");
    REQUIRE(plugins::format_install_count(1137172) == "1.1M");
    REQUIRE(plugins::format_install_count(479872) == "479.9K");
    REQUIRE(plugins::format_install_count(442818) == "442.8K");
}
