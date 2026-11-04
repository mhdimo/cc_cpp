#pragma once

#include "plugins.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cc::ui {

/// One plugin of the panel.
struct PluginPanelEntry {
    /// The name of the plugin.
    std::string name;

    /// The name the plugin is known by: `name@marketplace`.
    ///
    /// The actions of the panel take the plugin by this name.
    std::string id;

    /// The one-line description of the plugin.
    std::string description;

    /// The marketplace the plugin comes from.
    std::string marketplace;

    /// The category the plugin is filed under.
    std::string category;

    /// The version of the plugin.
    std::string version;

    /// True when the plugin is installed.
    bool installed = false;

    /// True when the plugin is turned on.
    bool enabled = false;

    /// The number of errors of the plugin.
    size_t error_count = 0;

    /// True when the row is a server of MCP and not a plugin.
    bool is_mcp = false;

    /// The state of the server, when the row is one.
    std::string status;

    /// The days the plugin was not used for.
    ///
    /// The reference shows the plugins that were not used for a while in a
    /// group of their own.
    int unused_days = 0;

    /// True when the plugin comes with the CLI and not from a marketplace.
    bool builtin = false;

    /// What the plugin brings.
    ///
    /// The details of the plugin name the components of it, the way the
    /// reference names them.
    plugins::PluginComponents components;

    /// True when the plugin asks the user for options.
    ///
    /// The reference offers the options of a plugin in the menu of its
    /// details; the port asks the session to open the dialog of them.
    bool configurable = false;
};

/// One plugin of a marketplace that is installed.
struct PluginPanelInstalledPlugin {
    /// The name of the plugin.
    std::string name;

    /// The one-line description of the plugin.
    std::string description;
};

/// One marketplace of the panel.
struct PluginPanelMarketplace {
    /// The name of the marketplace.
    std::string name;

    /// Where the marketplace comes from.
    std::string source;

    /// The number of the plugins the marketplace offers.
    size_t plugin_count = 0;

    /// The number of the plugins of the marketplace that are installed.
    size_t installed_count = 0;

    /// The time the marketplace was fetched last, as an ISO stamp.
    std::string last_updated;

    /// True when the marketplace is updated on its own.
    bool auto_update = false;

    /// The plugins of the marketplace that are installed.
    std::vector<PluginPanelInstalledPlugin> installed_plugins;

    /// True when the marketplace is marked for an update.
    bool pending_update = false;

    /// True when the marketplace is marked to be taken away.
    bool pending_remove = false;
};

/// The panel of the plugins.
///
/// The panel is the one the reference shows for `/plugin`: the plugins that
/// may be installed, the ones that are installed, the marketplaces and the
/// errors stand in four tabs, and the tab of the marketplaces has the list of
/// the marketplaces, the menu of the one that is chosen and the question
/// that asks whether it is to be taken away.
class PluginPanel {
public:
    PluginPanel(Theme theme, std::vector<PluginPanelEntry> discover,
                std::vector<PluginPanelEntry> installed,
                std::vector<PluginPanelMarketplace> marketplaces,
                std::vector<std::string> errors);

    /// The actions the panel offers.
    struct Actions {
        /// Installs a plugin of a marketplace and turns it on.
        std::function<bool(const std::string& marketplace, const std::string& plugin)> install_plugin;

        /// Turns an installed plugin on or off.
        std::function<bool(const std::string& id, bool enabled)> set_plugin_enabled;

        /// Takes an installed plugin away.
        std::function<bool(const std::string& id)> uninstall_plugin;

        /// Adds a marketplace from the source the user gave.
        std::function<bool(const std::string& source)> add_marketplace;

        /// Fetches a marketplace again.
        std::function<bool(const std::string& name)> update_marketplace;

        /// Turns the automatic update of a marketplace on or off.
        std::function<bool(const std::string& name, bool enabled)> set_marketplace_auto_update;

        /// Takes a marketplace away.
        std::function<bool(const std::string& name)> remove_marketplace;

        /// Connects a server of MCP again.
        std::function<void(const std::string& name)> reconnect_server;

        /// Asks the user for the options a plugin declares.
        ///
        /// The reference opens the dialog of the options of a plugin from
        /// the menu of its details; the port asks the session to open it.
        ///
        /// The plugin is named the way the reference names it:
        /// `name@marketplace`.
        std::function<void(const std::string& id)> configure_plugin;
    };

    /// The actions the panel offers.
    void set_actions(Actions actions) { actions_ = std::move(actions); }

    /// The actions the panel offers.
    const Actions& actions() const { return actions_; }

    /// The way the panel asks for its data again.
    using Reload = std::function<void(PluginPanel&)>;

    /// The panel asks for its data again through this callback.
    void set_reload(Reload reload) { reload_ = std::move(reload); }

    /// Runs the panel until the user closes it.
    void run(Terminal& terminal);

    /// Fills the panel with its data again.
    void set_data(std::vector<PluginPanelEntry> discover, std::vector<PluginPanelEntry> installed,
                  std::vector<PluginPanelMarketplace> marketplaces, std::vector<std::string> errors);

    /// Asks for the data of the panel again, when the callback is set.
    void reload_data();

    /// The plugins that may be installed.
    const std::vector<PluginPanelEntry>& discover() const { return discover_; }

    /// The plugins that are installed.
    const std::vector<PluginPanelEntry>& installed() const { return installed_; }

    /// The marketplaces that are known.
    const std::vector<PluginPanelMarketplace>& marketplaces() const { return marketplaces_; }

    /// The errors of the plugins.
    const std::vector<std::string>& errors() const { return errors_; }

    /// The name of the plugin the user picked.
    std::string picked;

    /// The tab the plugin was picked in.
    std::string picked_tab;

    /// The message the panel ends with.
    std::string message;

    /// The marketplace whose plugins are to be browsed.
    ///
    /// The reference shows the plugins of one marketplace in the tab of the
    /// plugins that may be installed, when they are browsed from the menu
    /// of the marketplace.
    std::string browse;

    /// Asks the panel to browse the plugins of a marketplace.
    void browse_marketplace(const std::string& name) { browse = name; }

    /// The lines of the details of the plugin that is given.
    ///
    /// The details name the components the plugin brings, the way the
    /// reference names them: the heading is `Will install:` for a plugin
    /// that is not installed yet and `Installed components:` for one that is.
    ///
    /// The `reserved` rows are the ones the menu of the plugin takes; they
    /// are kept, and the details are cut around them, so that the details
    /// are never taller than the screen.
    std::vector<std::string> render_details(const PluginPanelEntry& entry, bool installed,
                                            int width, int height, size_t reserved);

    /// The number of the installs of the plugins.
    ///
    /// The numbers come from the statistics of the marketplace of Anthropic;
    /// there are none when the statistics are not there, and the panel shows
    /// no number then.
    void set_install_counts(std::optional<std::map<std::string, int64_t>> counts) {
        install_counts_ = std::move(counts);
    }

    /// The number of the installs of the plugins.
    const std::optional<std::map<std::string, int64_t>>& install_counts() const {
        return install_counts_;
    }

    /// Reads the number of the installs again, when they have been fetched.
    ///
    /// The fetch is done in a thread of its own; the panel asks for the
    /// numbers again when the thread is through.
    void read_install_counts();

private:
    Theme theme_;
    std::vector<PluginPanelEntry> discover_;
    std::vector<PluginPanelEntry> installed_;
    std::vector<PluginPanelMarketplace> marketplaces_;
    std::vector<std::string> errors_;

    /// The number of the installs of the plugins.
    std::optional<std::map<std::string, int64_t>> install_counts_;

    /// The sign that the number of the installs has been fetched.
    std::shared_ptr<std::atomic<bool>> install_fetched_;

    /// True when the number of the installs has been read again.
    bool install_read_ = false;

    /// The components of the plugin whose details are shown.
    plugins::PluginComponents components_;

    /// The id of the plugin the components belong to.
    std::string components_id_;

    /// The actions the panel offers.
    Actions actions_;

    /// The way the panel asks for its data again.
    Reload reload_;
};

} // namespace cc::ui
