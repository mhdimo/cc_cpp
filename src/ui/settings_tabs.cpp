#include "ui/settings_tabs.hpp"

#include "ui/progress_bar.hpp"
#include "utils/string.hpp"
#include "utils/time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace cc::ui {
namespace {

/// The rule that closes the header of the panel.
std::string rule(int width) {
    std::string out;
    for (int i = 0; i < width; ++i) out += "─";
    return out;
}

/// The price of something, the way the reference writes it.
///
/// A price that is more than half a dollar is written with two decimals;
/// a smaller one keeps as many as are asked for.
std::string format_cost(double cost, int decimals = 2) {
    char buffer[32] = {};
    if (cost > 0.5) {
        std::snprintf(buffer, sizeof(buffer), "$%.2f", std::round(cost * 100.0) / 100.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "$%.*f", decimals, cost);
    }
    return buffer;
}

/// The first of the next month, in milliseconds.
///
/// The extra usage of the plan is given up on that day.
int64_t first_of_next_month() {
    const std::time_t seconds = static_cast<std::time_t>(cc::now_ms() / 1000);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif

    std::tm first = local;
    first.tm_mday = 1;
    first.tm_hour = 0;
    first.tm_min = 0;
    first.tm_sec = 0;
    first.tm_isdst = -1;

    // The first of the next month is the first of this one, a month later.
    std::tm next = first;
    next.tm_mon = first.tm_mon + 1;
    if (next.tm_mon > 11) {
        next.tm_mon = 0;
        next.tm_year += 1;
    }
    return static_cast<int64_t>(std::mktime(&next)) * 1000;
}

} // namespace

SettingsTabs::SettingsTabs(Theme theme, std::vector<std::unique_ptr<SettingsTab>> tabs,
                           size_t selected)
    : theme_(theme), tabs_(std::move(tabs)), selected_(0) {
    set_selected(selected);

    // The tab that is opened says whether the strip of the tabs has the
    // focus: the tab of the configuration opens in its search mode, so the
    // focus is in its search field.
    if (!tabs_.empty()) header_focused_ = tabs_[selected_]->header_focused();
}

void SettingsTabs::set_selected(size_t index) {
    if (tabs_.empty()) {
        selected_ = 0;
        return;
    }
    selected_ = index % tabs_.size();
    first_ = 0;
}

std::vector<std::string> SettingsTabs::render(int width, int height) const {
    const int w = width > 0 ? width : 1;

    std::vector<std::string> out;

    // The pane of the reference leaves an empty line above its rule.
    out.push_back("");

    // The rule across the top, in the colour of the pane.
    out.push_back(styled(rule(w), Style::fg(pane_color_)));

    // The tab that is shown is told whether the strip of the tabs has the
    // focus: the state belongs to the panel.
    if (!tabs_.empty()) tabs_[selected_]->set_header_focused(header_focused_);

    // The names of the tabs. The one that is shown is painted with the
    // colour of the pane when the header has the focus, and with the
    // inverted video when it does not. The title of the panel is drawn in
    // front of them, in the same colour.
    std::string header;
    if (!title_.empty()) {
        Style title_style;
        title_style.bold = true;
        title_style.foreground = pane_color_;
        header += styled(title_, title_style);
    }
    const bool focused = header_focused_;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (!header.empty()) header += " ";

        Style style;
        style.bold = i == selected_;
        if (i == selected_) {
            if (focused) {
                style.background = pane_color_;
                style.foreground = theme_.inverse_text;
            } else {
                style.inverse = true;
            }
        }

        header += styled(" " + tabs_[i]->title() + " ", style);
    }
    out.push_back("  " + header);
    out.push_back("");

    // The content of the tab that is shown.
    std::string hint;
    const bool margin = !tabs_.empty() && tabs_[selected_]->margin();
    if (!tabs_.empty()) {
        // The tab of the configuration is wrapped in a margin of its own.
        if (margin) out.push_back("");
        for (const std::string& line : tabs_[selected_]->render(w, height)) {
            out.push_back("  " + line);
        }
        hint = tabs_[selected_]->hint();
    }

    if (!hint.empty()) {
        out.push_back("");
        Style dim;
        dim.foreground = theme_.inactive;
        out.push_back("  " + styled(hint, dim));
    }

    // The margin of the tab is below its footer as well.
    if (margin) out.push_back("");

    return out;
}

void SettingsTabs::reload_selected() {
    if (tabs_.empty()) return;
    tabs_[selected_]->reload();
}

std::string SettingsTabs::run(Terminal& terminal) {
    // The tab that is shown is asked for what it shows, the way the
    // reference asks the tab of the usage for the limits of the plan.
    reload_selected();

    while (true) {
        terminal.render_dynamic(render(terminal.width(), terminal.height()));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return "";
            }
            continue;
        }

        // The tab is asked first: the tab of the configuration owns the keys
        // of its search.
        if (!tabs_.empty()) {
            const TabResult result = tabs_[selected_]->handle(*key);
            if (result.focus == TabResult::Focus::Header) header_focused_ = true;
            if (result.focus == TabResult::Focus::Content) header_focused_ = false;
            if (result.used) {
                // The tab asks the panel for another tab.
                if (result.switch_to >= 0) {
                    set_selected(static_cast<size_t>(result.switch_to));
                    header_focused_ = true;
                    reload_selected();
                }
                if (result.close || !result.picked.empty()) {
                    terminal.clear_dynamic();
                    return result.picked;
                }
                continue;
            }
        }

        switch (key->type) {
        case Key::Type::Tab:
        case Key::Type::Right:
            set_selected(selected_ + 1);
            header_focused_ = true;
            reload_selected();
            break;
        case Key::Type::ShiftTab:
        case Key::Type::Left:
            set_selected(selected_ + tabs_.size() - 1);
            header_focused_ = true;
            reload_selected();
            break;
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            terminal.clear_dynamic();
            return {};
        default:
            break;
        }
    }
}

StatusTab::StatusTab(Theme theme, std::vector<Property> properties)
    : theme_(theme), properties_(std::move(properties)) {}

std::vector<std::string> StatusTab::render(int width, int height) const {
    std::vector<std::string> out;

    for (const Property& property : properties_) {
        if (property.label.empty()) {
            // An empty row is an empty line: the reference leaves one
            // between the two blocks of rows.
            out.push_back(property.value);
            continue;
        }

        Style label_style;
        label_style.bold = true;

        // The value may hold more than one line, the way the reference shows
        // the sources of the settings.
        const std::vector<std::string> lines = cc::split_lines(property.value);
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i == 0) {
                out.push_back(styled(property.label + ":", label_style) + " " + lines[i]);
            } else {
                out.push_back("  " + lines[i]);
            }
        }
    }

    return out;
}

std::string StatusTab::hint() const {
    // The reference says that the panel is closed with the escape key.
    return "Esc to cancel";
}

ConfigTab::ConfigTab(Theme theme, std::vector<Setting> settings)
    : theme_(theme), panel_(theme, std::move(settings), "Settings", /*framed=*/false) {
    // The reference opens the tab of the configuration in its search mode:
    // the list is there, but no row of it is marked.
    panel_.set_searching(true);
}

std::vector<std::string> ConfigTab::render(int width, int height) const {
    // The body of the tab sits inside the pane of the panel, so it is two
    // columns narrower than the terminal on each side.
    return panel_.render(width - 4, height);
}

std::string ConfigTab::hint() const {
    // The reference names the keys of the tab, and the keys change when the
    // search takes over.
    if (header_focused_) {
        return "←/→ tab · ↓ return · Esc to close";
    }
    if (panel_.searching()) {
        return "Type to filter · Enter/↓ to select · ↑ to tabs · Esc to clear";
    }
    return "Space to change · Enter to save · / to search · Esc to cancel";
}

TabResult ConfigTab::handle(const Key& key) {
    TabResult result;

    if (header_focused_) {
        // The strip of the tabs has the focus: the down arrow gives it back
        // to the search field, and the other keys are the ones of the strip.
        if (key.type == Key::Type::Down) {
            result.used = true;
            result.focus = TabResult::Focus::Content;
        }
        return result;
    }

    // The up arrow in the search mode gives the focus to the strip of the
    // tabs, the way the reference gives it.
    if (key.type == Key::Type::Up && panel_.searching()) {
        result.used = true;
        result.focus = TabResult::Focus::Header;
        return result;
    }

    const bool was_searching = panel_.searching();
    result.used = panel_.handle(key, result.picked, result.close);

    // The up arrow at the top of the list takes the search back, and it
    // gives the focus to the strip of the tabs as well.
    if (result.used && key.type == Key::Type::Up && !was_searching && panel_.searching()) {
        result.focus = TabResult::Focus::Header;
    }
    return result;
}

UsageTab::UsageTab(Theme theme, std::vector<Limit> limits, std::optional<ExtraUsage> extra_usage)
    : theme_(theme), limits_(std::move(limits)), extra_usage_(std::move(extra_usage)) {}

void UsageTab::set_data(std::vector<Limit> limits, std::optional<ExtraUsage> extra_usage,
                         std::string error) {
    limits_ = std::move(limits);
    extra_usage_ = std::move(extra_usage);
    error_ = std::move(error);
}

bool UsageTab::reload() {
    if (!reload_) return false;
    reload_(*this);
    return true;
}

std::vector<std::string> UsageTab::render(int width, int height) const {
    std::vector<std::string> out;

    Style title_style;
    title_style.bold = true;

    Style dim;
    dim.foreground = theme_.inactive;

    // The answer of the API that fails is shown, the way the reference shows
    // it.
    if (!error_.empty()) {
        Style error;
        error.foreground = theme_.error;
        out.push_back(styled("Error: " + error_, error));
        return out;
    }

    // The width the tab is measured against is the width of the terminal
    // less two columns, and never more than eighty.
    const int max_width = std::min(width - 2, 80);

    for (const Limit& limit : limits_) {
        const std::string used =
            std::to_string(static_cast<int>(std::floor(limit.utilization * 100))) + "% used";
        const std::string subtext =
            limit.resets_at.empty() ? std::string() : "Resets " + limit.resets_at;

        if (max_width >= 62) {
            // The wide shape: the title, the bar and the time it resets at
            // are three lines of their own.
            out.push_back(styled(limit.title, title_style));
            out.push_back(progress_bar(theme_, limit.utilization, 50) + " " + used);
            if (!subtext.empty()) out.push_back(styled(subtext, dim));
        } else {
            // The narrow shape: the title and the time it resets at share
            // one line, and the bar is as wide as the panel.
            out.push_back(styled(limit.title, title_style) +
                          (subtext.empty() ? std::string() : " " + styled("· " + subtext, dim)));
            out.push_back(progress_bar(theme_, limit.utilization, std::max(10, max_width)));
            out.push_back(used);
        }
        out.push_back("");
    }

    if (limits_.empty()) {
        // The reference says that the command is only there for the
        // subscriptions.
        out.push_back(styled("/usage is only available for subscription plans.", dim));
        out.push_back("");

        // The usage of the machine is shown anyway: it is the usage the
        // panel of the statistics shows, and it is the only usage there is
        // without a subscription.
        out.push_back(styled("Usage of this machine", title_style));
        out.push_back(styled("Current session: " + cc::format_number(local_usage_.session_tokens),
                              dim));
        out.push_back(styled("Today: " + cc::format_number(local_usage_.today_tokens), dim));
        out.push_back(styled("All time: " + cc::format_number(local_usage_.total_tokens), dim));
    }

    // The extra usage of the plan, when there is one.
    if (extra_usage_) {
        const ExtraUsage& extra = *extra_usage_;
        if (!extra.is_enabled) {
            out.push_back(styled("Extra usage", title_style));
            out.push_back(styled("Extra usage not enabled · /extra-usage to enable", dim));
        } else if (!extra.monthly_limit.has_value()) {
            out.push_back(styled("Extra usage", title_style));
            out.push_back(styled("Unlimited", dim));
        } else {
            const std::string used =
                std::to_string(static_cast<int>(std::floor(extra.utilization))) + "% used";
            const std::string spending =
                format_cost(extra.used_credits.value_or(0.0) / 100.0) + " / " +
                format_cost(extra.monthly_limit.value_or(0.0) / 100.0) + " spent";

            // The extra usage is given up on the first of the next month.
            const int64_t resets_at = first_of_next_month();
            const std::string subtext =
                spending + " · Resets " + cc::format_reset_time(resets_at, false);

            if (max_width >= 62) {
                out.push_back(styled("Extra usage", title_style));
                out.push_back(progress_bar(theme_, extra.utilization / 100.0, 50) + " " + used);
                out.push_back(styled(subtext, dim));
            } else {
                // The narrow shape has three lines: the spending is part of
                // the subtext of the title.
                out.push_back(styled("Extra usage", title_style) + " " + styled("· " + subtext, dim));
                out.push_back(progress_bar(theme_, extra.utilization / 100.0, std::max(10, max_width)));
                out.push_back(used);
            }
        }
        out.push_back("");
    }

    return out;
}

std::string UsageTab::hint() const {
    // The reference says that the panel is closed with the escape key, and
    // that the answer of the API is asked again with the `r` key.
    if (!error_.empty()) return "r to retry · Esc to cancel";
    return "Esc to cancel";
}

TabResult UsageTab::handle(const Key& key) {
    TabResult result;

    // The answer of the API is asked again with the `r` key, the way the
    // reference asks it again.
    if (key.type == Key::Type::Character && key.text == "r" && !error_.empty()) {
        result.used = true;
        reload();
    }

    return result;
}

} // namespace cc::ui
