#pragma once

#include "agents/agent.hpp"
#include "hooks/hooks.hpp"

#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cc::plugins {

/// The name of the marketplace of Anthropic.
///
/// The reference shows the number of the installs of a plugin only when it
/// comes from this marketplace.
inline constexpr std::string_view kOfficialMarketplace = "claude-plugins-official";

/// One marketplace of plugins that is known.
struct Marketplace {
    /// The name of the marketplace.
    std::string name;

    /// Where the marketplace comes from: an address or a folder.
    std::string source;

    /// True when the marketplace comes from the settings of the user, and
    /// not from the file of the known marketplaces.
    bool from_settings = false;

    /// The folder the marketplace was fetched into, when it is known.
    std::string install_location;

    /// The time the marketplace was fetched last, as the reference writes
    /// it: an ISO 8601 stamp.
    std::string last_updated;

    /// True when the marketplace is updated on its own.
    ///
    /// The reference keeps the wish in the settings of the user and in the
    /// file of the known marketplaces.
    bool auto_update = false;
};

/// One plugin that a marketplace offers.
struct MarketplacePlugin {
    /// The name of the plugin.
    std::string name;

    /// The one-line description of the plugin.
    std::string description;

    /// The version of the plugin, when the marketplace gives one.
    std::string version;

    /// The category the plugin is filed under.
    std::string category;

    /// Where the plugin comes from, as the marketplace writes it.
    std::string source;

    /// The kind of the source: `path`, `github`, `git`, `git-subdir`, `url`,
    /// `npm` or `pip`.
    std::string source_kind;

    /// The address of the repository, when the source names one.
    std::string source_url;

    /// The folder in the repository, when the source names one.
    std::string source_path;

    /// The marketplace that offers the plugin.
    std::string marketplace;

    /// The folder of the plugin, when the marketplace holds it locally.
    std::string path;
};

/// One plugin that is installed.
struct Plugin {
    /// The name of the plugin.
    std::string name;

    /// The marketplace the plugin comes from.
    std::string marketplace;

    /// The name the plugin is known by: `name@marketplace`.
    std::string id;

    /// The version of the installed plugin.
    std::string version;

    /// The one-line description of the plugin.
    std::string description;

    /// The folder of the plugin in the cache.
    std::string path;

    /// True when the plugin is on.
    bool enabled = false;
};

/// One option a plugin asks the user for.
///
/// A plugin declares the options it needs in the `userConfig` of its
/// manifest; the user is asked for them when the plugin is turned on.
struct PluginOption {
    /// The name of the option; it is the name of the variable the plugin
    /// sees.
    std::string key;

    /// The kind of the value: `string`, `number`, `boolean`, `directory` or
    /// `file`.
    std::string type = "string";

    /// The label of the option.
    std::string title;

    /// The explanation shown underneath the label.
    std::string description;

    /// True when the option must be given.
    bool required = false;

    /// True when the option holds several values.
    bool multiple = false;

    /// True when the value is a secret; it is kept out of the settings.
    bool sensitive = false;

    /// The value the option has when the user gives none.
    std::string default_value;
};

/// One command that a plugin brings.
///
/// A plugin names its commands after itself: the command `build.md` of the
/// plugin `my-plugin` is the command `/my-plugin:build`.
struct PluginCommand {
    /// The name of the command, without the leading slash.
    std::string name;

    /// The one-line description shown in the help and in the suggestions.
    std::string description;

    /// The hint about the arguments the command takes.
    std::string argument_hint;

    /// The body of the file: the prompt that is sent to the model.
    std::string content;

    /// The file the command was read from.
    std::string path;

    /// The name of the plugin the command comes from.
    std::string plugin;

    /// The marketplace the plugin comes from.
    std::string marketplace;

    /// The folder of the plugin the command comes from.
    std::string plugin_root;

    /// True when the command is a skill rather than a plain command.
    bool is_skill = false;
};

/// The folder the plugins of the user are kept in.
///
/// The reference reads the folder from `CLAUDE_CODE_PLUGIN_CACHE_DIR` when it
/// is set, and from `~/.claude/plugins` otherwise.
std::string plugins_directory();

/// The folder the installed plugins are cached in.
std::string plugin_cache_directory();

/// The marketplaces that are known.
///
/// The marketplaces of the settings of the user come first, the ones of the
/// file of the known marketplaces after them.
std::vector<Marketplace> known_marketplaces();

/// The plugins a marketplace offers.
///
/// The marketplace is read from the folder it was fetched into, or from the
/// folder its source names. A marketplace that cannot be read offers nothing.
std::vector<MarketplacePlugin> marketplace_plugins(const std::string& marketplace);

/// The plugins that are installed, in the order they are shown.
///
/// The plugins are the ones in the cache; the settings of the user say which
/// of them are on.
std::vector<Plugin> installed_plugins();

/// The components a plugin brings.
///
/// The reference shows the components of a plugin in the details of it: the
/// commands, the agents, the skills, the hooks and the servers of MCP.
struct PluginComponents {
    /// The names of the commands of the plugin.
    std::vector<std::string> commands;

    /// The names of the agents of the plugin.
    std::vector<std::string> agents;

    /// The names of the skills of the plugin.
    std::vector<std::string> skills;

    /// The names of the hooks of the plugin.
    ///
    /// A hook is named after the event it is registered for.
    std::vector<std::string> hooks;

    /// The names of the servers of MCP of the plugin.
    std::vector<std::string> mcp_servers;

    /// True when the components of the plugin could not be read.
    ///
    /// A plugin that is not on this machine has components that are found
    /// when it is installed; the reference says so.
    bool unknown = false;

    /// True when the plugin brings nothing at all.
    bool empty() const {
        return commands.empty() && agents.empty() && skills.empty() && hooks.empty() &&
               mcp_servers.empty();
    }
};

/// The components the plugin that is installed brings.
///
/// The plugin is named the way the reference names it: `name@marketplace`.
/// A plugin that is not installed brings nothing.
PluginComponents plugin_components(const std::string& plugin_id);

/// The components the plugin of a marketplace brings.
///
/// The components are read from the folder of the plugin, when the
/// marketplace holds it on this machine; the components of a plugin that is
/// not on this machine are unknown.
PluginComponents marketplace_plugin_components(const std::string& marketplace,
                                                const std::string& plugin);

/// The commands the plugins that are on bring.
std::vector<PluginCommand> plugin_commands();

/// The agents the plugins that are on bring.
std::vector<agents::Definition> plugin_agents();

/// The hooks the plugins that are on bring.
std::vector<hooks::Hook> plugin_hooks();

/// The MCP servers the plugins that are on bring.
///
/// The servers are named after the plugin they come from, the way the
/// reference names them: the server `tools` of the plugin `my-plugin` is the
/// server `plugin:my-plugin:tools`.
boost::json::object plugin_mcp_servers();

/// Installs a plugin of a marketplace and turns it on.
///
/// The plugin is copied into the cache of the user, the way the reference
/// copies it there.
bool install_plugin(const std::string& marketplace, const std::string& plugin,
                    std::string* error);

/// Removes an installed plugin from the cache.
bool uninstall_plugin(const std::string& id, std::string* error);

/// Turns a plugin on or off.
///
/// The plugin is named the way the reference names it: `name@marketplace`.
bool set_plugin_enabled(const std::string& plugin, bool enabled, std::string* error);

/// Adds a marketplace to the settings of the user.
///
/// The name of the marketplace is the name of the source when none is given.
bool add_marketplace(const std::string& name, const std::string& source, std::string* error);

/// Removes a marketplace from the settings of the user.
bool remove_marketplace(const std::string& name, std::string* error);

/// Turns the automatic update of a marketplace on or off.
///
/// The wish is kept in the settings of the user and in the file of the known
/// marketplaces, the way the reference keeps it.
bool set_marketplace_auto_update(const std::string& name, bool enabled, std::string* error);

/// Fetches a marketplace and keeps it on the disk.
///
/// A marketplace that comes from an address is downloaded into the folder of
/// the marketplaces; a marketplace that comes from a repository is cloned
/// there. A marketplace that is already on the disk is fetched again.
bool refresh_marketplace(boost::asio::io_context& io_context, const std::string& marketplace,
                          std::string* error);

/// The number of the installs of the plugins.
///
/// The numbers come from the statistics of the marketplace of Anthropic and
/// are kept on the disk for a day. There are none when the statistics are
/// not there, and the panel shows no number then.
std::optional<std::map<std::string, int64_t>> install_counts();

/// Fetches the number of the installs of the plugins.
///
/// The numbers are taken from the statistics of the marketplace of Anthropic
/// and written in the folder of the plugins, in the file
/// `install-counts-cache.json`; they are good for a day. A network that is
/// not there is not an error: the numbers that are on the disk are kept.
///
/// The fetch is a slow one: it is meant to be run in a thread of its own.
bool fetch_install_counts(boost::asio::io_context& io_context);

/// The number of the installs of a plugin, as it is shown.
///
/// The number is written the way the reference writes it: `42`, `1.2K` or
/// `3.4M`.
std::string format_install_count(int64_t count);

/// The options a plugin asks the user for.
std::vector<PluginOption> plugin_options(const Plugin& plugin);

/// The values the user gave a plugin.
///
/// The values are kept in the settings of the user, under
/// `pluginConfigs[<plugin>].options`.
boost::json::object load_plugin_options(const std::string& plugin_id);

/// Remembers the values the user gave a plugin.
///
/// The rest of the settings is left alone.
bool save_plugin_options(const std::string& plugin_id, const boost::json::object& values,
                         std::string* error);

/// Replaces the variables of a plugin in the text of a command.
///
/// The reference gives a plugin the place it lives in, so that the plugin can
/// name the files it brings: `${CLAUDE_PLUGIN_ROOT}` becomes the folder of
/// the plugin and `${CLAUDE_PLUGIN_DATA}` the folder of its data. The data
/// folder is left as it is when the plugin has no name of its own.
std::string substitute_plugin_variables(std::string_view content,
                                        const std::string& plugin_root,
                                        const std::string& plugin_id);

} // namespace cc::plugins
