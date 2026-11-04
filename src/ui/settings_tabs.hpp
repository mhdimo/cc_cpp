#pragma once

#include "ui/ansi.hpp"
#include "ui/settings_panel.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cc::ui {

/// What a tab did with a key.
struct TabResult {
    /// True when the tab used the key.
    bool used = false;

    /// True when the tab wants the panel to be closed.
    bool close = false;

    /// The id of the setting the user picked, when there is one.
    std::string picked;

    /// The tab the panel is to show, when the tab asks for another one.
    ///
    /// The reference goes from the marketplaces to the plugins of the
    /// marketplace that is browsed; the tab asks the panel for the change.
    int switch_to = -1;

    /// What the tab did with the focus of the panel.
    enum class Focus {
        Unchanged,  ///< The tab did not ask for the focus.
        Header,     ///< The tab asks for the strip of the tabs to have it.
        Content,    ///< The tab asks for the content of the tab to have it.
    };

    /// The focus the tab asks for.
    Focus focus = Focus::Unchanged;
};

/// The content of one tab of the panel of the settings.
class SettingsTab {
public:
    virtual ~SettingsTab() = default;

    /// The name of the tab, as it is shown in the header.
    virtual std::string title() const = 0;

    /// The lines of the tab.
    ///
    /// The width and the height are the ones of the terminal: the tab of the
    /// configuration cuts its list to the height of the pane, and the tab of
    /// the usage measures itself against the width of the terminal.
    virtual std::vector<std::string> render(int width, int height = 0) const = 0;

    /// Handles a key of the tab.
    ///
    /// The tab of the configuration owns the keys of its search, so the
    /// panel asks the tab before it moves from one tab to the next.
    virtual TabResult handle(const Key&) { return {}; }

    /// The line the tab shows under its rows.
    ///
    /// The reference gives every tab its own hint, and the panel of the
    /// sandbox has none at all.
    virtual std::string hint() const { return {}; }

    /// True when the body of the tab is wrapped in a margin.
    ///
    /// The reference wraps the tab of the configuration in a box with a row
    /// of padding above it and below it, so that the body of the tab sits one
    /// row further from the strip of the tabs.
    virtual bool margin() const { return false; }

    /// True when the header of the panel has the focus.
    ///
    /// The reference paints the tab that is shown with the colour of the
    /// pane when the header has the focus, and with the inverted video when
    /// it does not.
    virtual bool header_focused() const { return true; }

    /// The panel tells the tab whether the strip of the tabs has the focus.
    ///
    /// The state belongs to the panel: the tab of the configuration starts
    /// without it, because its search field has the focus, and takes it back
    /// when the user presses the up arrow.
    virtual void set_header_focused(bool) {}

    /// Asks the tab again for what it shows, when it has to be asked.
    ///
    /// The reference asks the API again every time the tab of the usage is
    /// shown.
    virtual bool reload() { return false; }
};

/// The panel of the settings, with its tabs.
///
/// The reference opens the panel with one of its tabs chosen — `/config`
/// opens the tab of the configuration, `/status` the one of the state of the
/// session and `/usage` the one of the limits of the plan. The tab key and
/// the arrow keys on the side move from one tab to the next.
class SettingsTabs {
public:
    SettingsTabs(Theme theme, std::vector<std::unique_ptr<SettingsTab>> tabs, size_t selected = 0);

    /// Renders the panel.
    ///
    /// The width and the height are the ones of the terminal.
    std::vector<std::string> render(int width, int height = 0) const;

    /// Runs the panel until the user closes it.
    ///
    /// Returns the id of the setting the user picked, or an empty string
    /// when the panel was closed without a pick.
    std::string run(Terminal& terminal);

    /// The index of the tab that is shown.
    size_t selected() const { return selected_; }

    /// The title of the panel, drawn in front of the names of the tabs.
    ///
    /// The reference gives the panel of the sandbox the title `Sandbox:`.
    void set_title(std::string title) { title_ = std::move(title); }

    /// The colour of the pane of the panel.
    ///
    /// The reference paints the rule of the panel and the tab that is shown
    /// with it: the permission blue for the settings and the sandbox, and
    /// the colour of Claude for the statistics.
    void set_pane_color(Rgb color) { pane_color_ = color; }

    /// Moves to the tab at `index`.
    void set_selected(size_t index);

private:
    /// Asks the tab that is shown again for what it shows.
    void reload_selected();

    Theme theme_;
    std::vector<std::unique_ptr<SettingsTab>> tabs_;
    std::string title_;
    Rgb pane_color_ = Rgb{0, 0, 0};
    size_t selected_ = 0;

    /// True when the strip of the tabs has the focus.
    ///
    /// The state belongs to the panel: the tab of the configuration starts
    /// without it, because its search field has the focus, and takes it back
    /// when the user presses the up arrow.
    bool header_focused_ = true;

    /// The number of lines of the tab that are shown.
    size_t first_ = 0;
};

/// The tab that shows the state of the session.
///
/// The reference lists the version, the name and the id of the session, the
/// folder it runs in, the account it runs under, the address of the API, the
/// model, the IDE, the servers of MCP and the sources of the settings.
class StatusTab : public SettingsTab {
public:
    /// One line of the tab.
    struct Property {
        /// The name of the property. May be empty.
        std::string label;
        /// The value of the property.
        std::string value;
    };

    StatusTab(Theme theme, std::vector<Property> properties);

    std::string title() const override { return "Status"; }
    std::vector<std::string> render(int width, int height = 0) const override;
    std::string hint() const override;

private:
    Theme theme_;
    std::vector<Property> properties_;
};

/// The tab that shows the settings of the session.
///
/// The list is the one of the panel of the settings: the tab opens in its
/// search mode, what is typed filters the list, the arrow keys move the
/// highlight and Space picks a setting.
class ConfigTab : public SettingsTab {
public:
    ConfigTab(Theme theme, std::vector<Setting> settings);

    std::string title() const override { return "Config"; }
    std::vector<std::string> render(int width, int height = 0) const override;
    TabResult handle(const Key& key) override;
    std::string hint() const override;
    bool margin() const override { return true; }
    bool header_focused() const override { return header_focused_; }
    void set_header_focused(bool focused) override { header_focused_ = focused; }

    /// Puts the tab in its search mode, or takes it out of it.
    ///
    /// The panel is opened again after a setting was changed, and the tab
    /// keeps the mode it was in.
    void set_searching(bool searching) { panel_.set_searching(searching); }

private:
    Theme theme_;
    SettingsPanel panel_;

    /// True when the strip of the tabs has the focus.
    ///
    /// The tab of the configuration starts without it, because its search
    /// field has the focus.
    bool header_focused_ = false;
};

/// The tab that shows the limits of the plan.
///
/// The reference draws a bar for every limit, with the share of it that is
/// used and the time it resets at.
class UsageTab : public SettingsTab {
public:
    /// One limit of the plan.
    struct Limit {
        /// The name of the limit.
        std::string title;
        /// The share of the limit that is used, from 0 to 1.
        double utilization = 0.0;
        /// The time the limit resets at, when it is known.
        std::string resets_at;
    };

    /// The extra usage of the plan.
    ///
    /// The reference shows it under the limits of the plan, for the
    /// subscriptions that may spend beyond them.
    struct ExtraUsage {
        /// True when the user turned the extra usage on.
        bool is_enabled = false;

        /// The limit of the month, in cents; null when there is none.
        std::optional<double> monthly_limit;

        /// What was spent of the month, in cents.
        std::optional<double> used_credits;

        /// The share of the limit that is used, from 0 to 100.
        double utilization = 0.0;
    };

    UsageTab(Theme theme, std::vector<Limit> limits = {},
             std::optional<ExtraUsage> extra_usage = {});

    std::string title() const override { return "Usage"; }
    std::vector<std::string> render(int width, int height = 0) const override;
    std::string hint() const override;
    TabResult handle(const Key& key) override;
    bool reload() override;

    /// Fills the tab with the answer of the API.
    ///
    /// The error of the answer is kept, so that the tab can show it the way
    /// the reference shows it.
    void set_data(std::vector<Limit> limits, std::optional<ExtraUsage> extra_usage,
                  std::string error);

    /// The usage of the machine, the way the panel of the statistics shows
    /// it.
    ///
    /// The limits of the plan are only known for a subscription; the usage
    /// of the machine is always there.
    struct LocalUsage {
        /// The tokens of the session that is running.
        int64_t session_tokens = 0;
        /// The tokens of the day that is going on.
        int64_t today_tokens = 0;
        /// The tokens of every session of the machine.
        int64_t total_tokens = 0;
    };

    /// Sets the usage of the machine, which is shown when the plan does
    /// not say how much of it is used.
    void set_local_usage(LocalUsage usage) { local_usage_ = usage; }

    /// The way the tab asks the API again for the limits of the plan.
    using Reload = std::function<void(UsageTab&)>;

    /// The tab asks the API again through this callback.
    void set_reload(Reload reload) { reload_ = std::move(reload); }

private:
    Theme theme_;
    std::vector<Limit> limits_;
    std::optional<ExtraUsage> extra_usage_;

    /// The error of the answer of the API, when it could not be asked.
    std::string error_;

    /// The usage of the machine, shown when the plan does not say how much
    /// of it is used.
    LocalUsage local_usage_;

    /// The way the tab asks the API again.
    Reload reload_;
};

} // namespace cc::ui
