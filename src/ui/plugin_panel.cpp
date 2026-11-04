#include "ui/plugin_panel.hpp"

#include "ui/ansi.hpp"
#include "ui/figures.hpp"
#include "ui/input_dialog.hpp"
#include "ui/select.hpp"
#include "ui/settings_tabs.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <set>
#include <thread>

namespace cc::ui {
namespace {

/// The pointer in front of the row that is chosen.
///
/// The reference draws the pointer of the row that is chosen and two spaces
/// in front of the others.
std::string pointer_of(bool selected) {
    return selected ? std::string(figures::kPointer) + " " : "  ";
}

/// Joins the parts of a hint with the middot of the reference.
std::string byline_of(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += " · ";
        out += parts[i];
    }
    return out;
}

/// The badge that says that a row is a plugin.
///
/// The reference draws the word on the colour of the message of the user,
/// so that the eye finds it at once.
std::string plugin_badge(const Theme& theme, bool dim, bool is_mcp = false) {
    Style style;
    style.background = theme.user_message_background;
    if (dim) style.dim = true;
    return styled(is_mcp ? "MCP" : "Plugin", style);
}

/// The field of the search, drawn the way the reference draws it.
///
/// The reference puts the field in a box with rounded corners, and the
/// caret of the field is the first character of the placeholder drawn in
/// reverse video.
std::vector<std::string> search_box(const Theme& theme, const std::string& query, bool focused,
                                    int width) {
    Style border;
    border.foreground = theme.suggestion;

    Style dim;
    dim.foreground = theme.inactive;

    Style caret;
    caret.inverse = true;

    const std::string placeholder = "Search…";
    std::string content;
    if (query.empty()) {
        if (focused) {
            content = styled("S", caret) + styled(placeholder.substr(1), dim);
        } else {
            content = styled(placeholder, dim);
        }
    } else {
        content = query;
    }

    const int w = width > 4 ? width : 4;
    std::string row = " ⌕ " + content;
    const size_t room = static_cast<size_t>(w) - 2;
    if (cc::display_width(cc::strip_ansi(row)) < room) {
        row += std::string(room - cc::display_width(cc::strip_ansi(row)), ' ');
    }

    std::string fill;
    for (size_t i = 0; i < room; ++i) fill += figures::kHorizontalRule;

    return {styled("╭" + fill + "╮", border), styled("│", border) + row + styled("│", border),
            styled("╰" + fill + "╯", border)};
}

/// The style of the text that is not the point of the panel.
Style dim_style(const Theme& theme) {
    Style dim;
    dim.foreground = theme.inactive;
    return dim;
}

/// The style of the text that is the point of the panel.
Style suggestion_style(const Theme& theme) {
    Style suggestion;
    suggestion.foreground = theme.suggestion;
    return suggestion;
}

/// The number of the rows the panel of the plugins takes for itself.
///
/// The panel is drawn inside the pane of the panel of the settings: the
/// empty line, the rule, the strip of the tabs, the empty line under it,
/// the empty line above the hint and the hint itself.
constexpr size_t kPanelChrome = 6;

/// The window of a list that is shown.
///
/// The reference shows a window of the list and the rows that are above and
/// below it; the row that is chosen is always inside the window.
struct ListWindow {
    /// The first row of the window.
    size_t offset = 0;

    /// The number of the rows that are shown at once.
    size_t max_visible = 5;

    /// Moves the window so that the row at `selected` is inside it.
    void follow(size_t selected, size_t total) {
        if (total <= max_visible) {
            offset = 0;
            return;
        }
        if (selected < offset) offset = selected;
        if (selected >= offset + max_visible) offset = selected - max_visible + 1;
        const size_t last = total - max_visible;
        if (offset > last) offset = last;
    }

    /// True when there are rows above the window.
    bool can_scroll_up() const { return offset > 0; }

    /// True when there are rows below the window.
    bool can_scroll_down(size_t total) const { return offset + max_visible < total; }
};

/// The tab of the plugins that may be installed.
///
/// The reference opens the tab with a field of its own, and the plugins that
/// are chosen for the installation are marked with a filled circle.
class PluginDiscoverTab : public SettingsTab {
public:
    PluginDiscoverTab(PluginPanel& panel, Theme theme, Terminal* terminal)
        : panel_(panel), theme_(std::move(theme)), terminal_(terminal) {}

    std::string title() const override { return "Discover"; }

    std::vector<std::string> render(int width, int height = 0) const override {
        std::vector<std::string> lines;

        Style bold;
        bold.bold = true;

        Style dim = dim_style(theme_);
        Style suggestion = suggestion_style(theme_);

        // The body of the tab sits inside the pane of the panel, so it is
        // two columns narrower than the terminal on each side.
        const int w = width - 4 > 0 ? width - 4 : 1;

        // The number of the installs is read again when it has been fetched.
        panel_.read_install_counts();

        // The details of the plugin that is chosen take the place of the
        // list.
        if (view_ == View::Details) {
            return render_details(width, height);
        }

        const std::vector<PluginPanelEntry>& all = panel_.discover();
        const std::vector<size_t> visible = filtered(all);

        std::string heading = panel_.browse.empty() ? "Discover plugins" : "Install plugins";
        // The reference says where in the list the row that is chosen is.
        if (visible.size() > window_.max_visible) {
            heading += " (" + std::to_string(selected_ + 1) + "/" + std::to_string(visible.size()) +
                       ")";
        }
        lines.push_back(styled(heading, bold));
        lines.push_back("");

        for (const std::string& line : search_box(theme_, query_, searching_, w)) {
            lines.push_back(line);
        }
        lines.push_back("");

        // The line of the marketplaces: the number of the plugins of each of
        // them, the way the reference writes it.
        lines.push_back(marketplace_line());
        lines.push_back("");

        if (visible.empty()) {
            if (!query_.empty()) {
                lines.push_back(styled("No plugins match \"" + query_ + "\"", dim));
            } else if (!panel_.browse.empty()) {
                lines.push_back(styled("No new plugins available to install.", dim));
            } else {
                lines.push_back(styled("No plugins to discover", dim));
            }
            return lines;
        }

        // The rows that are above the window.
        if (window_.can_scroll_up()) {
            lines.push_back(styled(std::string(" ") + std::string(figures::kArrowUp) + " more above",
                                   dim));
        }

        const size_t first = window_.offset;
        const size_t last = std::min(first + window_.max_visible, visible.size());
        for (size_t i = first; i < last; ++i) {
            const size_t index = visible[i];
            const PluginPanelEntry& entry = all[index];
            const bool is_selected = i == selected_ && !searching_;

            // The row of the plugin: the pointer, the circle that marks the
            // ones that are chosen for the installation, the name and the
            // marketplace it comes from.
            std::string row = pointer_of(is_selected);
            row += selected_for_install_.count(entry.name) > 0
                       ? std::string(figures::kRadioOn) + " "
                       : std::string(figures::kRadioCircle) + " ";
            row += is_selected ? styled(entry.name, suggestion) : entry.name;
            if (!entry.marketplace.empty()) {
                row += styled(" · " + entry.marketplace, dim);
            }

            // The number of the installs of the plugin; the reference shows
            // it only for the marketplace of Anthropic.
            const std::string installs = install_count_of(entry);
            if (!installs.empty()) {
                row += styled(" · " + installs + " installs", dim);
            }

            if (!entry.category.empty()) {
                row += styled(" [" + entry.category + "]", dim);
            }
            if (!entry.version.empty()) {
                row += styled(" · v" + entry.version, dim);
            }
            if (entry.installed) {
                row += styled(" (installed)", dim);
            }
            lines.push_back(std::move(row));

            // The description of the plugin stands under it, cut to the
            // room of sixty columns, the way the reference cuts it.
            if (!entry.description.empty()) {
                lines.push_back("    " + styled(cc::truncate(entry.description, 60), dim));
            }
            lines.push_back("");
        }

        // The last row has no empty line under it.
        if (!lines.empty() && lines.back().empty()) lines.pop_back();

        // The rows that are below the window.
        if (window_.can_scroll_down(visible.size())) {
            lines.push_back(styled(std::string(" ") + std::string(figures::kArrowDown) + " more below",
                                   dim));
        }

        return lines;
    }

    TabResult handle(const Key& key) override {
        TabResult result;

        const std::vector<PluginPanelEntry>& all = panel_.discover();
        const std::vector<size_t> visible = filtered(all);

        // The details of the plugin that is chosen.
        if (view_ == View::Details) {
            switch (key.type) {
            case Key::Type::Up:
                if (menu_selected_ > 0) --menu_selected_;
                result.used = true;
                return result;
            case Key::Type::Down:
                if (menu_selected_ + 1 < details_menu().size()) ++menu_selected_;
                result.used = true;
                return result;
            case Key::Type::Enter: {
                const std::vector<std::string> menu = details_menu();
                if (menu_selected_ < menu.size()) choose(menu[menu_selected_]);
                result.used = true;
                return result;
            }
            case Key::Type::Escape:
                view_ = View::List;
                menu_selected_ = 0;
                result.used = true;
                return result;
            default:
                break;
            }
            return result;
        }

        // The search field has the focus: the keys are the text of it.
        if (searching_) {
            switch (key.type) {
            case Key::Type::Character:
                query_ += key.text;
                reset_selection();
                result.used = true;
                return result;
            case Key::Type::Backspace:
                if (!query_.empty()) query_.erase(query_.size() - 1);
                reset_selection();
                result.used = true;
                return result;
            case Key::Type::Enter:
                searching_ = false;
                result.used = true;
                return result;
            case Key::Type::Escape:
                // The search is left; the text of it is kept.
                searching_ = false;
                result.used = true;
                return result;
            case Key::Type::Down:
                // The search gives the focus to the list, and the list moves
                // on, the way the panel of the settings does it.
                searching_ = false;
                move_selection(1);
                result.used = true;
                return result;
            case Key::Type::Up:
                searching_ = false;
                move_selection(-1);
                result.used = true;
                return result;
            default:
                break;
            }
            return result;
        }

        switch (key.type) {
        case Key::Type::Up:
            if (selected_ == 0) {
                // The reference opens the search when the list is at its
                // top and the arrow up is pressed.
                searching_ = true;
                result.used = true;
                return result;
            }
            --selected_;
            window_.follow(selected_, visible.size());
            result.used = true;
            return result;
        case Key::Type::Down:
            if (selected_ + 1 < visible.size()) ++selected_;
            window_.follow(selected_, visible.size());
            result.used = true;
            return result;
        case Key::Type::Character:
            // The search is opened by the first character that is typed.
            if (key.text == " ") {
                if (!visible.empty()) {
                    const std::string& name = all[visible[selected_]].name;
                    if (selected_for_install_.count(name) > 0) {
                        selected_for_install_.erase(name);
                    } else {
                        selected_for_install_.insert(name);
                    }
                }
                result.used = true;
                return result;
            }
            if (key.text == "i") {
                install_selected();
                result.used = true;
                return result;
            }
            if (key.ctrl && key.text == "s") {
                choose_marketplace();
                result.used = true;
                return result;
            }
            query_ += key.text;
            searching_ = true;
            reset_selection();
            result.used = true;
            return result;
        case Key::Type::Enter:
            if (!visible.empty()) {
                view_ = View::Details;
                menu_selected_ = 0;
            }
            result.used = true;
            return result;
        default:
            break;
        }

        return result;
    }

    std::string hint() const override {
        if (view_ == View::Details) {
            return byline_of({"Enter to select", "Esc to back"});
        }
        std::vector<std::string> parts;
        if (!selected_for_install_.empty()) parts.push_back("i to install");
        parts.push_back("type to search");
        parts.push_back("Space to toggle");
        parts.push_back("Enter to details");
        parts.push_back("Esc to back");
        return byline_of(parts);
    }

private:
    /// The views of the tab.
    enum class View { List, Details };

    /// The number of the installs of the plugin, as it is shown.
    ///
    /// The reference shows the number only for the marketplace of Anthropic,
    /// and only when the statistics are there.
    std::string install_count_of(const PluginPanelEntry& entry) const {
        const std::optional<std::map<std::string, int64_t>>& counts = panel_.install_counts();
        if (!counts) return {};
        if (entry.marketplace != plugins::kOfficialMarketplace) return {};

        const auto found = counts->find(entry.id);
        const int64_t count = found == counts->end() ? 0 : found->second;
        return plugins::format_install_count(count);
    }

    /// The lines of the details of the plugin that is chosen.
    ///
    /// The details name the components the plugin brings, and the menu of
    /// the plugin stands under them.
    std::vector<std::string> render_details(int width, int height) const {
        const std::vector<PluginPanelEntry>& all = panel_.discover();
        const std::vector<size_t> visible = filtered(all);
        if (visible.empty()) return {};

        const PluginPanelEntry& entry = all[visible[selected_]];

        // The menu of the plugin.
        const std::vector<std::string> menu = details_menu();

        Style suggestion = suggestion_style(theme_);
        std::vector<std::string> lines =
            panel_.render_details(entry, false, width, height, menu.size() + 1);

        lines.push_back("");
        for (size_t i = 0; i < menu.size(); ++i) {
            const bool is_selected = i == menu_selected_;
            std::string row = pointer_of(is_selected);
            row += is_selected ? styled(menu[i], suggestion) : menu[i];
            lines.push_back(std::move(row));
        }

        return lines;
    }

    /// The line that names the marketplaces and the number of the plugins
    /// of each of them.
    ///
    /// The reference writes the line the way it does: the number of all the
    /// plugins, then the number of the plugins of every marketplace, and the
    /// note that the list can be filtered by marketplace.
    std::string marketplace_line() const {
        // The marketplaces, with the number of the plugins they offer, the
        // largest one first.
        std::vector<std::pair<std::string, size_t>> counts;
        size_t total = 0;
        for (const PluginPanelMarketplace& marketplace : panel_.marketplaces()) {
            counts.emplace_back(marketplace.name, marketplace.plugin_count);
            total += marketplace.plugin_count;
        }
        std::sort(counts.begin(), counts.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });

        std::string line = "ALL " + std::to_string(total);
        for (const auto& [name, count] : counts) {
            line += " \u00b7 " + name + " " + std::to_string(count);
        }
        line += "  (ctrl+s to filter by marketplace)";
        return line;
    }

    /// Asks the user which marketplace the list is to show.
    ///
    /// The reference filters the list by marketplace with the key ctrl+s.
    void choose_marketplace() {
        if (terminal_ == nullptr) return;

        std::vector<SelectOption> options;
        options.push_back({"ALL", "Every marketplace"});
        for (const PluginPanelMarketplace& marketplace : panel_.marketplaces()) {
            options.push_back({marketplace.name,
                               std::to_string(marketplace.plugin_count) + " plugins"});
        }
        if (options.size() < 2) return;

        terminal_->clear_dynamic();

        Select select(theme_, std::move(options), "Filter by marketplace");
        if (!select.run(*terminal_)) return;

        const std::string chosen = select.selected_label();
        marketplace_ = chosen == "ALL" ? std::string() : chosen;
        reset_selection();
    }

    /// Puts the highlight at the top of the list.
    ///
    /// The reference does the same when the text of the search changes.
    void reset_selection() {
        selected_ = 0;
        window_.offset = 0;
    }

    /// Moves the highlight of the list, and the window with it.
    ///
    /// The list wraps around at its ends, the way the panel of the settings
    /// does it.
    void move_selection(int step) {
        const std::vector<PluginPanelEntry>& all = panel_.discover();
        const std::vector<size_t> visible = filtered(all);
        if (visible.empty()) return;

        if (step > 0) {
            selected_ = selected_ + 1 < visible.size() ? selected_ + 1 : 0;
        } else {
            selected_ = selected_ > 0 ? selected_ - 1 : visible.size() - 1;
        }
        window_.follow(selected_, visible.size());
    }

    /// The plugins that the search leaves.
    std::vector<size_t> filtered(const std::vector<PluginPanelEntry>& all) const {
        std::vector<size_t> out;
        const std::string needle = cc::to_lower(query_);
        for (size_t i = 0; i < all.size(); ++i) {
            // The plugins of the marketplace that is browsed are the only
            // ones that are shown.
            if (!panel_.browse.empty() && all[i].marketplace != panel_.browse) continue;

            // The marketplace the list is filtered by.
            if (!marketplace_.empty() && all[i].marketplace != marketplace_) continue;

            if (needle.empty()) {
                out.push_back(i);
                continue;
            }
            const std::string haystack =
                cc::to_lower(all[i].name + " " + all[i].description + " " + all[i].marketplace);
            if (haystack.find(needle) != std::string::npos) out.push_back(i);
        }
        return out;
    }

    /// The menu of the plugin that is chosen.
    std::vector<std::string> details_menu() const {
        std::vector<std::string> menu;
        const std::vector<PluginPanelEntry>& all = panel_.discover();
        const std::vector<size_t> visible = filtered(all);
        if (visible.empty()) return menu;

        const PluginPanelEntry& entry = all[visible[selected_]];
        if (!entry.installed) {
            menu.push_back("Install for you (user scope)");
            menu.push_back("Install for all collaborators on this repository (project scope)");
            menu.push_back("Install for you, in this repo only (local scope)");
        } else {
            menu.push_back(entry.enabled ? "Disable plugin" : "Enable plugin");
            menu.push_back("Uninstall plugin");
        }
        menu.push_back("Back to plugin list");
        return menu;
    }

    /// Does what the user picked in the menu of the plugin.
    void choose(const std::string& choice) {
        const std::vector<PluginPanelEntry>& all = panel_.discover();
        const std::vector<size_t> visible = filtered(all);
        if (visible.empty()) return;

        const PluginPanelEntry& entry = all[visible[selected_]];

        if (choice == "Back to plugin list") {
            view_ = View::List;
            menu_selected_ = 0;
            return;
        }

        if (choice == "Enable plugin" || choice == "Disable plugin") {
            if (panel_.actions().set_plugin_enabled) {
                panel_.actions().set_plugin_enabled(entry.name, choice == "Enable plugin");
            }
            panel_.reload_data();
            return;
        }

        if (choice == "Uninstall plugin") {
            if (panel_.actions().uninstall_plugin) panel_.actions().uninstall_plugin(entry.name);
            panel_.reload_data();
            view_ = View::List;
            return;
        }

        // The three ways of installing a plugin are the same for the port:
        // the plugin is installed for the user and turned on.
        if (choice.rfind("Install", 0) == 0) {
            if (panel_.actions().install_plugin) {
                panel_.actions().install_plugin(entry.marketplace, entry.name);
            }
            panel_.reload_data();
            view_ = View::List;
        }
    }

    /// Installs the plugins that were chosen for the installation.
    void install_selected() {
        if (selected_for_install_.empty()) return;

        const std::vector<PluginPanelEntry>& all = panel_.discover();
        for (const PluginPanelEntry& entry : all) {
            if (selected_for_install_.count(entry.name) == 0) continue;
            if (panel_.actions().install_plugin) {
                panel_.actions().install_plugin(entry.marketplace, entry.name);
            }
        }
        selected_for_install_.clear();
        panel_.reload_data();
    }

    PluginPanel& panel_;
    Theme theme_;

    /// The terminal the tab asks the user in.
    Terminal* terminal_ = nullptr;

    /// The marketplace the list is filtered by; empty means every one.
    std::string marketplace_;

    /// The text of the search.
    std::string query_;

    /// True when the search field has the focus.
    bool searching_ = false;

    /// The index of the plugin that is chosen.
    size_t selected_ = 0;

    /// The window of the list that is shown.
    ListWindow window_;

    /// The plugins that are chosen for the installation.
    std::set<std::string> selected_for_install_;

    /// The view of the tab.
    View view_ = View::List;

    /// The index of the choice of the menu that is chosen.
    size_t menu_selected_ = 0;
};

/// The tab of the plugins that are installed.
///
/// The reference groups the plugins under the scope they are in, and each
/// of them carries the sign of its state.
class PluginInstalledTab : public SettingsTab {
public:
    PluginInstalledTab(PluginPanel& panel, Theme theme) : panel_(panel), theme_(std::move(theme)) {
        // The reference shows eight plugins at once in this tab.
        window_.max_visible = 8;
    }

    std::string title() const override { return "Installed"; }

    std::vector<std::string> render(int width, int height = 0) const override {
        std::vector<std::string> lines;

        Style dim = dim_style(theme_);
        Style suggestion = suggestion_style(theme_);

        // The body of the tab sits inside the pane of the panel, so it is
        // two columns narrower than the terminal on each side.
        const int w = width - 4 > 0 ? width - 4 : 1;

        // The details of the plugin that is chosen take the place of the
        // list.
        if (view_ == View::Details) {
            return render_details(width, height);
        }

        for (const std::string& line : search_box(theme_, query_, searching_, w)) {
            lines.push_back(line);
        }
        lines.push_back("");

        const std::vector<PluginPanelEntry>& all = panel_.installed();
        const std::vector<size_t> visible = filtered(all);

        if (visible.empty()) {
            if (!query_.empty()) {
                lines.push_back(styled("No items match \"" + query_ + "\"", dim));
            } else {
                lines.push_back(styled("No plugins installed", dim));
            }
            return lines;
        }

        // The rows that are above the window.
        if (window_.can_scroll_up()) {
            lines.push_back(styled(std::string(" ") + std::string(figures::kArrowUp) + " more above",
                                   dim));
        }

        const size_t first = window_.offset;
        const size_t last = std::min(first + window_.max_visible, visible.size());

        // The groups of the reference: the ones that need attention, the
        // ones that were not used for a while, the ones of the user and the
        // ones that come with the CLI. The name of a group stands above the
        // rows of it.
        Group group = Group::Attention;
        bool group_written = false;

        for (size_t i = first; i < last; ++i) {
            const size_t index = visible[i];
            const PluginPanelEntry& entry = all[index];
            const bool is_selected = i == selected_ && !searching_;

            const Group current = group_of(entry);
            if (!group_written || current != group) {
                if (group_written) lines.push_back("");
                lines.push_back(styled("  " + std::string(group_title(current)), dim));
                group = current;
                group_written = true;
            }

            // The sign of the state of the row, with the colour of it.
            Style sign = dim;
            std::string state;
            if (entry.error_count > 0) {
                sign.foreground = theme_.error;
                state = std::string(figures::kCross);
            } else if (entry.is_mcp) {
                if (entry.status == "failed") {
                    sign.foreground = theme_.error;
                    state = std::string(figures::kCross);
                } else if (entry.status == "needs-auth") {
                    sign.foreground = theme_.warning;
                    state = std::string(figures::kWarning);
                } else if (entry.status == "connected") {
                    sign.foreground = theme_.success;
                    state = std::string(figures::kTick);
                } else {
                    sign.foreground = theme_.inactive;
                    state = std::string(figures::kRadioOff);
                }
            } else if (entry.enabled) {
                sign.foreground = theme_.success;
                state = std::string(figures::kTick);
            } else {
                sign.foreground = theme_.inactive;
                state = std::string(figures::kRadioOff);
            }

            std::string row = pointer_of(is_selected);
            row += styled(state, sign);
            row += " ";
            row += is_selected ? styled(entry.name, suggestion) : entry.name;
            row += " " + plugin_badge(theme_, !is_selected, entry.is_mcp);

            // The note of the row: the marketplace of the plugin, the state
            // of the server.
            std::string note;
            if (entry.is_mcp) {
                note = entry.status;
            } else {
                note = entry.marketplace;
                if (entry.unused_days > 0) {
                    note += " · not used in " + std::to_string(entry.unused_days) +
                            (entry.unused_days == 1 ? " day" : " days");
                }
                if (entry.error_count > 0) {
                    note += " · " + std::to_string(entry.error_count) +
                            (entry.error_count == 1 ? " error" : " errors");
                }
            }
            if (!note.empty()) row += styled(" " + note, dim);

            lines.push_back(std::move(row));
        }

        // The rows that are below the window.
        if (window_.can_scroll_down(visible.size())) {
            lines.push_back(styled(std::string(" ") + std::string(figures::kArrowDown) + " more below",
                                   dim));
        }

        return lines;
    }

    TabResult handle(const Key& key) override {
        TabResult result;

        const std::vector<PluginPanelEntry>& all = panel_.installed();
        const std::vector<size_t> visible = filtered(all);

        if (view_ == View::Details) {
            switch (key.type) {
            case Key::Type::Up:
                if (menu_selected_ > 0) --menu_selected_;
                result.used = true;
                return result;
            case Key::Type::Down:
                if (menu_selected_ + 1 < details_menu().size()) ++menu_selected_;
                result.used = true;
                return result;
            case Key::Type::Enter: {
                const std::vector<std::string> menu = details_menu();
                if (menu_selected_ < menu.size()) choose(menu[menu_selected_]);
                result.used = true;
                return result;
            }
            case Key::Type::Escape:
                view_ = View::List;
                menu_selected_ = 0;
                result.used = true;
                return result;
            default:
                break;
            }
            return result;
        }

        if (searching_) {
            switch (key.type) {
            case Key::Type::Character:
                query_ += key.text;
                reset_selection();
                result.used = true;
                return result;
            case Key::Type::Backspace:
                if (!query_.empty()) query_.erase(query_.size() - 1);
                reset_selection();
                result.used = true;
                return result;
            case Key::Type::Enter:
            case Key::Type::Escape:
                searching_ = false;
                result.used = true;
                return result;
            case Key::Type::Down:
                // The search gives the focus to the list, and the list moves
                // on, the way the panel of the settings does it.
                searching_ = false;
                move_selection(1);
                result.used = true;
                return result;
            case Key::Type::Up:
                searching_ = false;
                move_selection(-1);
                result.used = true;
                return result;
            default:
                break;
            }
            return result;
        }

        switch (key.type) {
        case Key::Type::Up:
            if (selected_ == 0) {
                searching_ = true;
                result.used = true;
                return result;
            }
            --selected_;
            window_.follow(selected_, visible.size());
            result.used = true;
            return result;
        case Key::Type::Down:
            if (selected_ + 1 < visible.size()) ++selected_;
            window_.follow(selected_, visible.size());
            result.used = true;
            return result;
        case Key::Type::Character:
            query_ += key.text;
            searching_ = true;
            reset_selection();
            result.used = true;
            return result;
        case Key::Type::Enter:
            if (!visible.empty()) {
                view_ = View::Details;
                menu_selected_ = 0;
            }
            result.used = true;
            return result;
        default:
            break;
        }

        return result;
    }

    std::string hint() const override {
        if (view_ == View::Details) {
            return byline_of({"↑ to navigate", "Enter to select", "Esc to back"});
        }
        return byline_of({"type to search", "Space to toggle", "Enter to details", "Esc to back"});
    }

private:
    /// The views of the tab.
    enum class View { List, Details };

    /// The lines of the details of the plugin that is chosen.
    ///
    /// The details name the components the plugin brings, and the menu of
    /// the plugin stands under them.
    std::vector<std::string> render_details(int width, int height) const {
        const std::vector<PluginPanelEntry>& all = panel_.installed();
        const std::vector<size_t> visible = filtered(all);
        if (visible.empty()) return {};

        const PluginPanelEntry& entry = all[visible[selected_]];

        // The menu of the plugin.
        const std::vector<std::string> menu = details_menu();

        Style suggestion = suggestion_style(theme_);
        std::vector<std::string> lines =
            panel_.render_details(entry, true, width, height, menu.size() + 1);

        lines.push_back("");
        for (size_t i = 0; i < menu.size(); ++i) {
            const bool is_selected = i == menu_selected_;
            std::string row = pointer_of(is_selected);
            row += is_selected ? styled(menu[i], suggestion) : menu[i];
            lines.push_back(std::move(row));
        }

        return lines;
    }

    /// The groups the reference shows the rows in.
    enum class Group { Attention, Unused, User, Builtin };

    /// The group a row belongs to.
    static Group group_of(const PluginPanelEntry& entry) {
        if (entry.error_count > 0) return Group::Attention;
        if (entry.is_mcp && entry.status == "failed") return Group::Attention;
        if (entry.unused_days > 0) return Group::Unused;
        if (entry.builtin) return Group::Builtin;
        return Group::User;
    }

    /// The name of a group.
    static const char* group_title(Group group) {
        switch (group) {
        case Group::Attention:
            return "Needs attention";
        case Group::Unused:
            return "Not used recently";
        case Group::Builtin:
            return "Built-in";
        case Group::User:
            break;
        }
        return "User";
    }

    /// Puts the highlight at the top of the list.
    ///
    /// The reference does the same when the text of the search changes.
    void reset_selection() {
        selected_ = 0;
        window_.offset = 0;
    }

    /// Moves the highlight of the list, and the window with it.
    ///
    /// The list wraps around at its ends, the way the panel of the settings
    /// does it.
    void move_selection(int step) {
        const std::vector<PluginPanelEntry>& all = panel_.installed();
        const std::vector<size_t> visible = filtered(all);
        if (visible.empty()) return;

        if (step > 0) {
            selected_ = selected_ + 1 < visible.size() ? selected_ + 1 : 0;
        } else {
            selected_ = selected_ > 0 ? selected_ - 1 : visible.size() - 1;
        }
        window_.follow(selected_, visible.size());
    }

    /// The plugins that the search leaves.
    std::vector<size_t> filtered(const std::vector<PluginPanelEntry>& all) const {
        std::vector<size_t> out;
        const std::string needle = cc::to_lower(query_);
        for (size_t i = 0; i < all.size(); ++i) {
            if (needle.empty()) {
                out.push_back(i);
                continue;
            }
            const std::string haystack =
                cc::to_lower(all[i].name + " " + all[i].description + " " + all[i].marketplace);
            if (haystack.find(needle) != std::string::npos) out.push_back(i);
        }
        return out;
    }

    /// The menu of the plugin that is chosen.
    std::vector<std::string> details_menu() const {
        std::vector<std::string> menu;
        const std::vector<PluginPanelEntry>& all = panel_.installed();
        const std::vector<size_t> visible = filtered(all);
        if (visible.empty()) return menu;

        const PluginPanelEntry& entry = all[visible[selected_]];
        if (entry.is_mcp) {
            menu.push_back(entry.status == "connected" ? "Reconnect" : "Reconnect");
            menu.push_back("Back");
            return menu;
        }

        menu.push_back(entry.enabled ? "Disable plugin" : "Enable plugin");
        menu.push_back("Uninstall plugin");

        // The options of the plugin are asked of the user; the reference
        // offers them in the menu of the details of the plugin.
        if (entry.configurable) menu.push_back("Configure options");
        return menu;
    }

    /// Does what the user picked in the menu of the plugin.
    void choose(const std::string& choice) {
        const std::vector<PluginPanelEntry>& all = panel_.installed();
        const std::vector<size_t> visible = filtered(all);
        if (visible.empty()) return;

        const PluginPanelEntry& entry = all[visible[selected_]];

        if (choice == "Reconnect") {
            if (panel_.actions().reconnect_server) panel_.actions().reconnect_server(entry.name);
            return;
        }

        if (choice == "Enable plugin" || choice == "Disable plugin") {
            if (panel_.actions().set_plugin_enabled) {
                panel_.actions().set_plugin_enabled(entry.id, choice == "Enable plugin");
            }
            panel_.reload_data();
            return;
        }

        if (choice == "Uninstall plugin") {
            if (panel_.actions().uninstall_plugin) panel_.actions().uninstall_plugin(entry.id);
            panel_.reload_data();
            view_ = View::List;
        }

        // The dialog of the options of the plugin is opened by the session;
        // the plugin is named the way the reference names it.
        if (choice == "Configure options") {
            if (panel_.actions().configure_plugin) panel_.actions().configure_plugin(entry.id);
        }
    }

    PluginPanel& panel_;
    Theme theme_;

    /// The text of the search.
    std::string query_;

    /// True when the search field has the focus.
    bool searching_ = false;

    /// The index of the plugin that is chosen.
    size_t selected_ = 0;

    /// The window of the list that is shown.
    ListWindow window_;

    /// The view of the tab.
    View view_ = View::List;

    /// The index of the choice of the menu that is chosen.
    size_t menu_selected_ = 0;
};

/// The tab of the marketplaces.
///
/// The reference opens the tab with the list of the marketplaces, the entry
/// that adds one at the top of it, and the menu of the marketplace that is
/// chosen. The question that asks whether the marketplace is to be taken
/// away stands between the two.
class PluginMarketplaceTab : public SettingsTab {
public:
    PluginMarketplaceTab(PluginPanel& panel, Theme theme, Terminal* terminal)
        : panel_(panel), theme_(std::move(theme)), terminal_(terminal) {}

    std::string title() const override { return "Marketplaces"; }

    std::vector<std::string> render(int width, int height = 0) const override {
        (void)width;
        (void)height;

        Style bold;
        bold.bold = true;

        Style dim = dim_style(theme_);
        Style suggestion = suggestion_style(theme_);

        switch (view_) {
        case View::ConfirmRemove:
            return render_confirm_remove(bold, dim);
        case View::Details:
            return render_details(bold, dim, suggestion);
        case View::List:
            break;
        }

        std::vector<std::string> lines;
        lines.push_back(styled("Manage marketplaces", bold));
        lines.push_back("");

        // The entry that adds a marketplace stands at the top of the list.
        {
            const bool is_selected = selected_ == 0;
            std::string row = pointer_of(is_selected);
            row += is_selected ? styled("+", suggestion) : "+";
            row += " ";
            row += is_selected ? styled("Add Marketplace", suggestion)
                               : styled("Add Marketplace", bold);
            lines.push_back(std::move(row));
        }

        const std::vector<PluginPanelMarketplace>& all = panel_.marketplaces();
        if (all.empty()) return lines;

        lines.push_back("");

        for (size_t i = 0; i < all.size(); ++i) {
            const PluginPanelMarketplace& marketplace = all[i];
            const bool is_selected = selected_ == i + 1;
            const bool pending_remove = pending_remove_.count(marketplace.name) > 0;
            const bool pending_update = pending_update_.count(marketplace.name) > 0;

            std::string row = pointer_of(is_selected);
            row += pending_remove ? std::string(figures::kCross) : std::string(figures::kBullet);

            Style name_style;
            name_style.bold = true;
            if (pending_remove) {
                name_style.dim = true;
                name_style.strikethrough = true;
            }

            // The marketplace of the company carries the star of Claude on
            // both of its sides, the way the reference carries it.
            Style claude;
            claude.foreground = theme_.claude;
            if (marketplace.name == "claude-plugins-official") {
                row += " " + styled("✻ ", claude) + styled(marketplace.name, name_style) +
                       styled(" ✻", claude);
            } else {
                row += " " + styled(marketplace.name, name_style);
            }

            // The marks of the changes that wait to be applied.
            std::vector<std::string> marks;
            if (pending_update) marks.push_back("UPDATE");
            if (pending_remove) marks.push_back("REMOVE");
            if (!marks.empty()) {
                Style warning;
                warning.foreground = theme_.warning;
                row += styled("[" + cc::join(marks, ", ") + "]", warning);
            }

            lines.push_back(std::move(row));
            lines.push_back("    " + styled(marketplace.source, dim));

            // The number of the plugins of the marketplace, the number of the
            // ones that are installed and the day it was fetched last.
            std::string counts = std::to_string(marketplace.plugin_count) + " available";
            if (marketplace.installed_count > 0) {
                counts += " • " + std::to_string(marketplace.installed_count) + " installed";
            }
            if (!marketplace.last_updated.empty()) {
                const int64_t stamp = cc::parse_timestamp(marketplace.last_updated);
                const std::string day = cc::format_locale_date(stamp);
                if (!day.empty()) counts += " • Updated " + day;
            }
            lines.push_back("    " + styled(counts, dim));
            lines.push_back("");
        }

        // The changes that wait to be applied.
        if (!pending_update_.empty() || !pending_remove_.empty()) {
            Style warning;
            warning.foreground = theme_.warning;

            lines.push_back(styled("Pending changes:", bold) + styled(" Enter to apply", dim));
            if (!pending_update_.empty()) {
                lines.push_back("• Update " + std::to_string(pending_update_.size()) +
                                (pending_update_.size() == 1 ? " marketplace" : " marketplaces"));
            }
            if (!pending_remove_.empty()) {
                lines.push_back(styled("• Remove " + std::to_string(pending_remove_.size()) +
                                           (pending_remove_.size() == 1 ? " marketplace" : " marketplaces"),
                                       warning));
            }
            lines.push_back("");
        }

        return lines;
    }

    TabResult handle(const Key& key) override {
        TabResult result;

        const std::vector<PluginPanelMarketplace>& all = panel_.marketplaces();

        switch (view_) {
        case View::Details: {
            switch (key.type) {
            case Key::Type::Up:
                if (menu_selected_ > 0) --menu_selected_;
                result.used = true;
                return result;
            case Key::Type::Down:
                if (menu_selected_ + 1 < details_menu().size()) ++menu_selected_;
                result.used = true;
                return result;
            case Key::Type::Enter: {
                const std::vector<std::string> menu = details_menu();
                if (menu_selected_ < menu.size()) choose(menu[menu_selected_]);
                // The plugins of the marketplace are browsed in the tab of
                // the plugins that may be installed.
                if (switch_to_ >= 0) {
                    result.switch_to = switch_to_;
                    switch_to_ = -1;
                }
                result.used = true;
                return result;
            }
            case Key::Type::Escape:
                view_ = View::List;
                menu_selected_ = 0;
                result.used = true;
                return result;
            default:
                break;
            }
            return result;
        }

        case View::ConfirmRemove: {
            if (key.type == Key::Type::Character && (key.text == "y" || key.text == "Y")) {
                const std::string name = selected_name();
                if (panel_.actions().remove_marketplace) panel_.actions().remove_marketplace(name);
                pending_remove_.erase(name);
                panel_.reload_data();
                view_ = View::List;
                selected_ = 0;
                result.used = true;
                return result;
            }
            if (key.type == Key::Type::Character && (key.text == "n" || key.text == "N")) {
                view_ = View::List;
                selected_ = 0;
                result.used = true;
                return result;
            }
            if (key.type == Key::Type::Escape) {
                view_ = View::List;
                selected_ = 0;
                result.used = true;
                return result;
            }
            break;
        }

        case View::List:
            break;
        }

        switch (key.type) {
        case Key::Type::Up:
            if (selected_ > 0) --selected_;
            result.used = true;
            return result;
        case Key::Type::Down:
            if (selected_ + 1 < all.size() + 1) ++selected_;
            result.used = true;
            return result;
        case Key::Type::Enter:
            if (selected_ == 0) {
                add_marketplace();
            } else {
                view_ = View::Details;
                menu_selected_ = 0;
            }
            result.used = true;
            return result;
        case Key::Type::Character:
            if (key.text == "u" || key.text == "U") {
                update_selected();
                result.used = true;
                return result;
            }
            if (key.text == "r" || key.text == "R") {
                if (selected_ > 0) {
                    view_ = View::ConfirmRemove;
                }
                result.used = true;
                return result;
            }
            break;
        case Key::Type::Escape:
            // The changes that wait to be applied are taken back; the panel
            // is left when there are none.
            if (!pending_update_.empty() || !pending_remove_.empty()) {
                pending_update_.clear();
                pending_remove_.clear();
                result.used = true;
                return result;
            }
            break;
        default:
            break;
        }

        return result;
    }

    std::string hint() const override {
        switch (view_) {
        case View::Details:
            return byline_of({"Enter to select", "Esc to go back"});
        case View::ConfirmRemove:
            // The question of the panel says how it is answered.
            return {};
        case View::List:
            break;
        }
        if (!pending_update_.empty() || !pending_remove_.empty()) {
            return byline_of({"Enter to apply changes", "Esc to cancel"});
        }
        // The reference leaves the keys of the marketplaces out when there
        // is none to update or to remove.
        if (panel_.marketplaces().empty()) {
            return byline_of({"Enter to select", "Esc to go back"});
        }
        return byline_of({"Enter to select", "u to update", "r to remove", "Esc to go back"});
    }

private:
    /// The views of the tab.
    enum class View { List, Details, ConfirmRemove };

    /// The name of the marketplace that is chosen.
    std::string selected_name() const {
        const std::vector<PluginPanelMarketplace>& all = panel_.marketplaces();
        if (selected_ == 0 || selected_ > all.size()) return {};
        return all[selected_ - 1].name;
    }

    /// The marketplace that is chosen.
    const PluginPanelMarketplace* selected_marketplace() const {
        const std::vector<PluginPanelMarketplace>& all = panel_.marketplaces();
        if (selected_ == 0 || selected_ > all.size()) return nullptr;
        return &all[selected_ - 1];
    }

    /// The menu of the marketplace that is chosen.
    std::vector<std::string> details_menu() const {
        std::vector<std::string> menu;
        const PluginPanelMarketplace* marketplace = selected_marketplace();
        if (marketplace == nullptr) return menu;

        menu.push_back("Browse plugins (" + std::to_string(marketplace->plugin_count) + ")");
        menu.push_back("Update marketplace");
        menu.push_back(marketplace->auto_update ? "Disable auto-update" : "Enable auto-update");
        menu.push_back("Remove marketplace");
        return menu;
    }

    /// The note that stands after the choice of the menu.
    ///
    /// The reference says when the marketplace was fetched last next to the
    /// choice that fetches it again.
    std::string details_note(size_t index) const {
        const PluginPanelMarketplace* marketplace = selected_marketplace();
        if (marketplace == nullptr || index != 1) return {};
        if (marketplace->last_updated.empty()) return {};

        const int64_t stamp = cc::parse_timestamp(marketplace->last_updated);
        const std::string day = cc::format_locale_date(stamp);
        if (day.empty()) return {};
        return "(last updated " + day + ")";
    }

    /// The lines of the details of the marketplace that is chosen.
    std::vector<std::string> render_details(const Style& bold, const Style& dim, const Style& suggestion) const {
        std::vector<std::string> lines;
        const PluginPanelMarketplace* marketplace = selected_marketplace();
        if (marketplace == nullptr) return lines;

        lines.push_back(styled(marketplace->name, bold));
        lines.push_back(styled(marketplace->source, dim));
        lines.push_back("");
        lines.push_back(std::to_string(marketplace->plugin_count) + " available " +
                        (marketplace->plugin_count == 1 ? "plugin" : "plugins"));

        // The plugins of the marketplace that are installed.
        if (!marketplace->installed_plugins.empty()) {
            lines.push_back("");
            lines.push_back(styled("Installed plugins (" +
                                       std::to_string(marketplace->installed_plugins.size()) + "):",
                                   bold));
            for (const PluginPanelInstalledPlugin& plugin : marketplace->installed_plugins) {
                lines.push_back(" " + std::string(figures::kBullet) + " " + plugin.name);
                if (!plugin.description.empty()) {
                    lines.push_back("   " + styled(plugin.description, dim));
                }
            }
        }

        lines.push_back("");

        const std::vector<std::string> menu = details_menu();
        for (size_t i = 0; i < menu.size(); ++i) {
            const bool is_selected = i == menu_selected_;
            std::string row = pointer_of(is_selected);
            row += is_selected ? styled(menu[i], suggestion) : menu[i];

            // The note of the choice stands after it.
            const std::string note = details_note(i);
            if (!note.empty()) row += styled(" " + note, dim);

            lines.push_back(std::move(row));
        }

        // The note about the marketplace that is updated on its own.
        if (marketplace->auto_update) {
            lines.push_back("");
            lines.push_back(styled("Auto-update enabled. Claude Code will automatically update this "
                                   "marketplace and its installed plugins.",
                                   dim));
        }

        return lines;
    }

    /// The lines of the question that asks whether the marketplace is to be
    /// taken away.
    std::vector<std::string> render_confirm_remove(const Style& bold, const Style& dim) const {
        std::vector<std::string> lines;
        const PluginPanelMarketplace* marketplace = selected_marketplace();
        if (marketplace == nullptr) return lines;

        Style warning;
        warning.foreground = theme_.warning;
        warning.bold = true;

        Style italic;
        italic.italic = true;
        italic.foreground = theme_.warning;

        lines.push_back(styled("Remove marketplace ", warning) + styled(marketplace->name, italic) +
                        styled("?", warning));

        const size_t count = marketplace->installed_plugins.size();
        if (count > 0) {
            lines.push_back("");
            Style plain_warning;
            plain_warning.foreground = theme_.warning;
            lines.push_back(styled("This will also uninstall " + std::to_string(count) + " " +
                                       (count == 1 ? "plugin" : "plugins") + " from this marketplace:",
                                   plain_warning));
            lines.push_back("");
            for (const PluginPanelInstalledPlugin& plugin : marketplace->installed_plugins) {
                lines.push_back("  " + styled(std::string(figures::kBullet) + " " + plugin.name, dim));
            }
        }

        lines.push_back("");
        Style plain;
        plain.bold = true;
        lines.push_back("Press " + styled("y", plain) + " to confirm or " + styled("n", plain) + " to cancel");

        return lines;
    }

    /// Does what the user picked in the menu of the marketplace.
    void choose(const std::string& choice) {
        const PluginPanelMarketplace* marketplace = selected_marketplace();
        if (marketplace == nullptr) return;
        const std::string name = marketplace->name;

        if (choice == "Remove marketplace") {
            view_ = View::ConfirmRemove;
            return;
        }

        if (choice == "Update marketplace") {
            update_selected();
            return;
        }

        if (choice == "Enable auto-update" || choice == "Disable auto-update") {
            if (panel_.actions().set_marketplace_auto_update) {
                panel_.actions().set_marketplace_auto_update(name, choice == "Enable auto-update");
            }
            panel_.reload_data();
            return;
        }

        // The plugins of the marketplace are shown in the tab of the
        // plugins that may be installed.
        if (choice.rfind("Browse plugins", 0) == 0) {
            panel_.browse_marketplace(name);
            switch_to_ = 0;
        }
    }

    /// Fetches the marketplace that is chosen again.
    void update_selected() {
        if (selected_ == 0) return;
        const std::string name = selected_name();
        if (name.empty()) return;

        if (panel_.actions().update_marketplace) panel_.actions().update_marketplace(name);
        pending_update_.insert(name);
        pending_remove_.erase(name);
        panel_.reload_data();
    }

    /// Asks the user for the source of a marketplace and adds it.
    ///
    /// The reference asks for the source in a field of its own and gives
    /// the examples of the sources that are understood.
    void add_marketplace() {
        if (terminal_ == nullptr) return;

        terminal_->clear_dynamic();

        InputDialog dialog(theme_, "Add Marketplace", "Enter marketplace source:");
        dialog.set_lead("Examples:\n · owner/repo (GitHub)\n · git@github.com:owner/repo.git (SSH)\n"
                        " · https://example.com/marketplace.json\n · ./path/to/marketplace");
        const InputDialogResult result = dialog.run(*terminal_);

        if (result.outcome != InputDialogOutcome::Confirmed) return;
        const std::string source = cc::trim(result.value);
        if (source.empty()) return;

        if (panel_.actions().add_marketplace) panel_.actions().add_marketplace(source);
        panel_.reload_data();
    }

    PluginPanel& panel_;
    Theme theme_;

    /// The terminal the tab asks the user in.
    Terminal* terminal_ = nullptr;

    /// The index of the row that is chosen; zero is the entry that adds a
    /// marketplace.
    size_t selected_ = 0;

    /// The view of the tab.
    View view_ = View::List;

    /// The index of the choice of the menu that is chosen.
    size_t menu_selected_ = 0;

    /// The tab the panel is to show, when the marketplace is browsed.
    int switch_to_ = -1;

    /// The marketplaces that are marked for an update.
    std::set<std::string> pending_update_;

    /// The marketplaces that are marked to be taken away.
    std::set<std::string> pending_remove_;
};

/// The tab of the errors of the plugins.
class PluginErrorsTab : public SettingsTab {
public:
    PluginErrorsTab(PluginPanel& panel, Theme theme)
        : panel_(panel), theme_(std::move(theme)) {}

    std::string title() const override {
        const size_t count = panel_.errors().size();
        return count > 0 ? "Errors (" + std::to_string(count) + ")" : "Errors";
    }

    std::vector<std::string> render(int width, int height = 0) const override {
        (void)width;
        (void)height;

        Style warning;
        warning.foreground = theme_.warning;

        Style dim = dim_style(theme_);

        std::vector<std::string> lines;
        const std::vector<std::string>& errors = panel_.errors();
        if (errors.empty()) {
            lines.push_back(styled("No errors", dim));
            return lines;
        }

        for (const std::string& error : errors) {
            lines.push_back(styled(std::string(figures::kWarning) + " " + error, warning));
        }
        return lines;
    }

    std::string hint() const override { return "Esc to close"; }

private:
    PluginPanel& panel_;
    Theme theme_;
};

} // namespace

PluginPanel::PluginPanel(Theme theme, std::vector<PluginPanelEntry> discover,
                         std::vector<PluginPanelEntry> installed,
                         std::vector<PluginPanelMarketplace> marketplaces,
                         std::vector<std::string> errors)
    : theme_(std::move(theme)),
      discover_(std::move(discover)),
      installed_(std::move(installed)),
      marketplaces_(std::move(marketplaces)),
      errors_(std::move(errors)) {}

void PluginPanel::set_data(std::vector<PluginPanelEntry> discover,
                           std::vector<PluginPanelEntry> installed,
                           std::vector<PluginPanelMarketplace> marketplaces,
                           std::vector<std::string> errors) {
    discover_ = std::move(discover);
    installed_ = std::move(installed);
    marketplaces_ = std::move(marketplaces);
    errors_ = std::move(errors);
}

void PluginPanel::reload_data() {
    if (reload_) reload_(*this);
}

std::vector<std::string> PluginPanel::render_details(const PluginPanelEntry& entry, bool installed,
                                                     int width, int height, size_t reserved) {
    Style bold;
    bold.bold = true;

    Style dim = dim_style(theme_);

    // The body of the details sits inside the pane of the panel, so it is
    // two columns narrower than the terminal on each side.
    const int room_width = width - 6 > 20 ? width - 6 : 20;

    // The components of the plugin are read when they are not there yet:
    // the panel is the one that knows the plugins of the user.
    if (entry.id != components_id_) {
        components_id_ = entry.id;
        if (entry.components.empty() && !entry.components.unknown) {
            components_ = entry.installed
                              ? plugins::plugin_components(entry.id)
                              : plugins::marketplace_plugin_components(entry.marketplace, entry.name);
        } else {
            components_ = entry.components;
        }
    }

    // A plugin that is installed is shown with the components it has; a
    // plugin that is not is shown with the ones it will bring.
    const bool is_installed = installed || entry.installed;

    // The head of the details: the name of the plugin, the marketplace it
    // comes from and the version of it.
    std::vector<std::string> head;
    if (is_installed) {
        head.push_back(styled(entry.name + " @ " + entry.marketplace, bold));
    } else {
        head.push_back(styled("Plugin details", bold));
        head.push_back(styled(entry.name, bold));
        if (!entry.marketplace.empty()) head.push_back(styled("from " + entry.marketplace, dim));
    }
    if (!entry.version.empty()) head.push_back(styled("Version: " + entry.version, dim));

    // The description of the plugin.
    std::vector<std::string> about;
    if (!entry.description.empty()) {
        for (const std::string& line : cc::split_lines(entry.description)) {
            about.push_back(cc::truncate(line, static_cast<size_t>(room_width)));
        }
    }

    // The components the plugin brings. The reference names the components
    // of a plugin that is not installed under the heading `Will install:`,
    // and the ones of a plugin that is installed under the heading
    // `Installed components:`.
    std::vector<std::string> parts;
    if (!entry.is_mcp) {
        parts.push_back(styled(is_installed ? "Installed components:" : "Will install:", bold));

        const std::string bullet = is_installed ? "" : "· ";
        if (components_.unknown) {
            parts.push_back(styled(bullet + "Component summary not available for remote plugin",
                                   dim));
        } else if (components_.empty()) {
            parts.push_back(styled(bullet + "Components will be discovered at installation", dim));
        } else {
            const auto row = [&](const std::string& label, const std::vector<std::string>& names) {
                if (names.empty()) return;
                const std::string text = bullet + label + ": " + cc::join(names, ", ");
                parts.push_back(styled(cc::truncate(text, static_cast<size_t>(room_width)), dim));
            };
            row("Commands", components_.commands);
            row("Agents", components_.agents);
            row("Skills", components_.skills);
            row("Hooks", components_.hooks);
            row("MCP Servers", components_.mcp_servers);
        }
    }

    // The details are put together: the head of them, the description of
    // the plugin and the components it brings.
    const auto assemble = [&]() {
        std::vector<std::string> out = head;
        if (!about.empty()) {
            out.insert(out.end(), about.begin(), about.end());
            out.push_back("");
        }
        out.insert(out.end(), parts.begin(), parts.end());
        return out;
    };

    // The room the panel has for the details. The menu of the plugin is
    // kept: it is the part the user acts on.
    if (height <= 0) return assemble();
    const size_t room = height > static_cast<int>(kPanelChrome + reserved)
                            ? static_cast<size_t>(height) - kPanelChrome - reserved
                            : 0;

    // The details are cut when they do not fit: the description goes first,
    // then the components, then the head.
    if (assemble().size() <= room) return assemble();

    about.clear();
    if (assemble().size() <= room) return assemble();

    while (parts.size() > 1 && assemble().size() > room) parts.pop_back();
    if (assemble().size() <= room) return assemble();

    while (head.size() > 1 && assemble().size() > room) head.pop_back();
    if (assemble().size() <= room) return assemble();

    // The head is the last thing that goes.
    head.clear();
    return assemble();
}

void PluginPanel::read_install_counts() {
    if (install_read_) return;
    if (install_fetched_ == nullptr || !install_fetched_->load()) return;

    install_read_ = true;
    install_counts_ = plugins::install_counts();
}

void PluginPanel::run(Terminal& terminal) {
    // The number of the installs of the plugins is read from the disk. When
    // it is not there — or when it is older than a day — it is fetched in a
    // thread of its own, the way the reference fetches it: the panel is not
    // held up by it, and the numbers appear when they are there.
    install_counts_ = plugins::install_counts();
    if (!install_counts_) {
        auto fetched = std::make_shared<std::atomic<bool>>(false);
        install_fetched_ = fetched;
        std::thread([fetched]() {
            boost::asio::io_context io_context;
            plugins::fetch_install_counts(io_context);
            fetched->store(true);
        }).detach();
    }

    auto discover = std::make_unique<PluginDiscoverTab>(*this, theme_, &terminal);
    auto installed = std::make_unique<PluginInstalledTab>(*this, theme_);
    auto marketplaces = std::make_unique<PluginMarketplaceTab>(*this, theme_, &terminal);
    auto errors = std::make_unique<PluginErrorsTab>(*this, theme_);

    std::vector<std::unique_ptr<SettingsTab>> tabs;
    tabs.push_back(std::move(discover));
    tabs.push_back(std::move(installed));
    tabs.push_back(std::move(marketplaces));
    tabs.push_back(std::move(errors));

    SettingsTabs panel(theme_, std::move(tabs));
    // The reference paints the pane of the plugins with the colour of the
    // suggestion.
    panel.set_pane_color(theme_.suggestion);
    panel.set_title("Plugins");

    const std::string picked = panel.run(terminal);
    if (picked.empty()) return;

    // The tab the plugin was picked in is the one that was shown.
    switch (panel.selected()) {
    case 0:
        picked_tab = "discover";
        break;
    case 1:
        picked_tab = "installed";
        break;
    case 2:
        picked_tab = "marketplaces";
        break;
    default:
        picked_tab = "errors";
        break;
    }

    this->picked = picked;
}

} // namespace cc::ui
