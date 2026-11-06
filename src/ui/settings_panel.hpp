#pragma once

#include "ui/ansi.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// One setting of the panel.
struct Setting {
    /// The name of the setting.
    std::string label;

    /// The value of the setting, as it is shown on the right.
    std::string value;

    /// The name the setting is known by; it is given back when the setting
    /// is picked.
    std::string id;

    /// The explanation shown underneath the label.
    std::string description;
};

/// The panel of the settings.
///
/// The panel is a list of the settings with the value of each of them. The
/// arrow keys move the highlight, Space picks the setting, and Escape closes
/// the panel. What is typed filters the list, the way the reference filters
/// it.
class SettingsPanel {
public:
    SettingsPanel(Theme theme, std::vector<Setting> settings, std::string title = {},
                  bool framed = true);

    /// Renders the panel.
    ///
    /// The height of the terminal cuts the list of the settings: the
    /// reference shows a window of the list and the rows that are above and
    /// below it. A height of zero shows the whole list.
    std::vector<std::string> render(int width, int height = 0) const;

    /// Runs the panel until the user picks a setting or closes it.
    ///
    /// Returns the id of the setting that was picked, or an empty string
    /// when the panel was closed.
    std::string run(Terminal& terminal);

    /// Handles a key of the panel.
    ///
    /// Returns true when the key was used. The id of the setting the user
    /// picked is given back in `picked`, and `close` is set when the panel
    /// wants to be closed.
    bool handle(const Key& key, std::string& picked, bool& close);

    /// The index of the highlighted setting.
    size_t selected() const { return selected_; }

    /// The settings that are shown, after the filter.
    const std::vector<Setting>& visible() const { return visible_; }

    /// True when the search of the panel has the focus.
    ///
    /// The reference opens the tab of the configuration in its search mode:
    /// the list is there, but no row of it is marked.
    bool searching() const { return searching_; }

    /// Puts the panel in its search mode, or takes it out of it.
    void set_searching(bool searching) { searching_ = searching; }

private:
    /// Applies the filter to the settings.
    void filter();

    /// The number of settings that are shown at once.
    ///
    /// The reference cuts the list to the height of the pane and writes the
    /// rows that are above and below the window.
    size_t max_visible(int height) const;

    /// Keeps the chosen setting inside the window of the list.
    void adjust_scroll();

    Theme theme_;
    std::vector<Setting> settings_;
    std::vector<Setting> visible_;
    std::string title_;
    std::string filter_;
    size_t selected_ = 0;

    /// True when the search of the panel has the focus.
    bool searching_ = false;

    /// The first setting of the list that is shown.
    size_t scroll_ = 0;

    /// The number of settings that were shown the last time.
    ///
    /// The window of the list is as tall as the pane, so the number of the
    /// settings it holds is known only when the panel is drawn.
    mutable size_t max_visible_ = 0;

    /// True when the panel draws the rule and the title of its own.
    ///
    /// The panel is drawn without them when it sits in a tab of the panel of
    /// the settings, which draws its own frame.
    bool framed_ = true;
};

} // namespace cc::ui
