#include "ui/settings_panel.hpp"

#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>

namespace cc::ui {
SettingsPanel::SettingsPanel(Theme theme, std::vector<Setting> settings, std::string title,
                             bool framed)
    : theme_(theme), settings_(std::move(settings)), title_(std::move(title)), framed_(framed) {
    filter();
}

void SettingsPanel::filter() {
    visible_.clear();
    for (const Setting& setting : settings_) {
        if (filter_.empty()) {
            visible_.push_back(setting);
            continue;
        }
        // The filter looks at the name of the setting and at its value.
        const std::string haystack = cc::to_lower(setting.label + " " + setting.value);
        if (haystack.find(cc::to_lower(filter_)) != std::string::npos) {
            visible_.push_back(setting);
        }
    }
    if (selected_ >= visible_.size()) selected_ = visible_.empty() ? 0 : visible_.size() - 1;
}

size_t SettingsPanel::max_visible(int height) const {
    // Without a height the whole list is shown.
    if (height <= 0) return settings_.size();

    // The reference keeps the pane of the tab as tall as it may be, and
    // reserves ten rows of it for the search, the gaps and the footer.
    const int pane = std::max(15, std::min(static_cast<int>(height * 0.8), 30));
    return std::max<size_t>(5, static_cast<size_t>(pane - 10));
}

void SettingsPanel::adjust_scroll() {
    if (max_visible_ == 0) return;
    if (selected_ < scroll_) {
        scroll_ = selected_;
    } else if (selected_ >= scroll_ + max_visible_) {
        scroll_ = selected_ - max_visible_ + 1;
    }
}

std::vector<std::string> SettingsPanel::render(int width, int height) const {
    const int w = width > 0 ? width : 1;

    std::vector<std::string> out;

    if (framed_) {
        // The rule across the top.
        std::string rule;
        rule.reserve(static_cast<size_t>(w) * 3);
        for (int i = 0; i < w; ++i) rule += "─";
        out.push_back(styled(rule, Style::fg(theme_.prompt_border)));

        if (!title_.empty()) {
            Style title_style;
            title_style.bold = true;
            out.push_back(styled(title_, title_style));
        }
    }

    // The rows sit at the indent of the panel when the panel draws its own
    // frame, and at the indent of the tab when it does not.
    const std::string indent = framed_ ? "  " : "";

    // The field of the search, in a box of its own, the way the reference
    // draws it.
    {
        Style border;
        border.foreground = theme_.suggestion;

        Style dim;
        dim.foreground = theme_.inactive;

        Style caret;
        caret.inverse = true;

        const std::string placeholder = "Search settings…";
        std::string content;
        if (filter_.empty()) {
            if (searching_) {
                // The first character of the placeholder is drawn in reverse
                // video: the caret is on it.
                content = styled("S", caret) + styled(placeholder.substr(1), dim);
            } else {
                content = styled(placeholder, dim);
            }
        } else {
            content = filter_;
        }

        std::string row = " ⌕ " + content;
        // The row of the field is as wide as the space inside the borders,
        // so that the three lines of the box have the same width.
        const size_t room = static_cast<size_t>(w) > 2 ? static_cast<size_t>(w) - 2 : 1;
        if (cc::display_width(strip(row)) < room) {
            row += std::string(room - cc::display_width(strip(row)), ' ');
        }
        std::string fill;
        const size_t count = static_cast<size_t>(w) > 2 ? static_cast<size_t>(w) - 2 : 1;
        for (size_t i = 0; i < count; ++i) fill += figures::kHorizontalRule;
        out.push_back(styled("╭" + fill + "╮", border));
        out.push_back(styled("│", border) + row + styled("│", border));
        out.push_back(styled("╰" + fill + "╯", border));
    }
    out.push_back("");

    if (visible_.empty()) {
        Style dim;
        dim.foreground = theme_.inactive;
        dim.italic = true;
        out.push_back(indent + styled("No settings match \"" + filter_ + "\"", dim));
        return out;
    }

    // The list of the settings is cut to the height of the pane: the rows
    // that are above and below the window are counted, the way the
    // reference counts them.
    max_visible_ = max_visible(height);
    const size_t first = std::min(scroll_, visible_.size());
    const size_t shown = std::min(max_visible_, visible_.size() - first);

    Style dim;
    dim.foreground = theme_.inactive;

    if (first > 0) {
        out.push_back(indent + styled(std::string(figures::kArrowUp) + " " + std::to_string(first) +
                                          " more above",
                                      dim));
    }

    // The settings. The reference gives the label a column of its own and
    // the value follows it; the row the cursor is on carries a pointer.
    for (size_t i = 0; i < shown; ++i) {
        const Setting& setting = visible_[first + i];
        // No row is marked while the search has the focus.
        const bool highlighted = !searching_ && first + i == selected_;

        Style style;
        if (highlighted) style.foreground = theme_.suggestion;

        // The reference paints the whole field of the label — the pointer
        // and the label itself — in the colour of the suggestion when the
        // row is the chosen one.
        const std::string pointer =
            highlighted ? std::string(figures::kPointer) + " " : "  ";
        const std::string plain = pointer + setting.label;

        // The column of the label is 44 wide, the way the reference makes
        // it. A label that does not fit is wrapped onto a second line, and
        // the value stays in its column.
        //
        // The label may carry the colours of its own: the escapes take no
        // room on the screen, so the width is measured without them.
        const size_t column = 44;
        std::vector<std::string> wrapped;
        if (cc::display_width(cc::strip_ansi(plain)) > column) {
            wrapped = cc::word_wrap(cc::strip_ansi(plain), column);
        } else {
            wrapped.push_back(plain);
        }
        if (wrapped.empty()) wrapped.emplace_back();

        std::string label = highlighted ? styled(wrapped.front(), style) : wrapped.front();
        const size_t used = cc::display_width(strip(label));
        if (used < column) label += std::string(column - used, ' ');

        out.push_back(indent + label + styled(setting.value, style));

        // The lines the label was wrapped onto.
        for (size_t i = 1; i < wrapped.size(); ++i) {
            out.push_back(indent + (highlighted ? styled(wrapped[i], style) : wrapped[i]));
        }
    }

    if (first + shown < visible_.size()) {
        const size_t below = visible_.size() - first - shown;
        out.push_back(indent + styled(std::string(figures::kArrowDown) + " " +
                                          std::to_string(below) + " more below",
                                      dim));
    }

    if (framed_) {
        out.push_back("");

        out.push_back(styled("↑↓ to move · Enter to change · Esc to close", dim));
    }

    return out;
}

std::string SettingsPanel::run(Terminal& terminal) {
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

        std::string picked;
        bool close = false;
        if (!handle(*key, picked, close)) continue;

        if (close) {
            terminal.clear_dynamic();
            return {};
        }
        if (!picked.empty()) {
            terminal.clear_dynamic();
            return picked;
        }
    }
}

bool SettingsPanel::handle(const Key& key, std::string& picked, bool& close) {
    if (searching_) {
        // The search has the focus: what is typed filters the list.
        switch (key.type) {
        case Key::Type::Up:
            // The up arrow gives the focus to the strip of the tabs; the
            // panel of the tabs sees it.
            return false;
        case Key::Type::Down:
        case Key::Type::Enter:
            // The search gives the focus to the list of the settings.
            searching_ = false;
            selected_ = 0;
            scroll_ = 0;
            return true;
        case Key::Type::Escape:
            // The escape key clears the search first, and only leaves it
            // when there is nothing to clear.
            if (!filter_.empty()) {
                filter_.clear();
                selected_ = 0;
                scroll_ = 0;
                filter();
            } else {
                searching_ = false;
            }
            return true;
        case Key::Type::Backspace:
            if (!filter_.empty()) {
                filter_.pop_back();
                selected_ = 0;
                scroll_ = 0;
                filter();
            }
            return true;
        case Key::Type::Character:
            filter_ += key.text;
            selected_ = 0;
            scroll_ = 0;
            filter();
            return true;
        default:
            return false;
        }
    }

    // The list of the settings has the focus.
    switch (key.type) {
    case Key::Type::Up:
        if (visible_.empty()) return true;
        if (selected_ == 0) {
            // At the top of the list the search takes over, the way the
            // reference does.
            searching_ = true;
            scroll_ = 0;
        } else {
            --selected_;
            adjust_scroll();
        }
        return true;
    case Key::Type::Down:
        if (!visible_.empty()) {
            selected_ = selected_ + 1 < visible_.size() ? selected_ + 1 : 0;
            adjust_scroll();
        }
        return true;
    case Key::Type::Enter:
        // The reference saves the settings and closes the panel.
        close = true;
        return true;
    case Key::Type::Escape:
    case Key::Type::CtrlC:
        close = true;
        return true;
    case Key::Type::Character:
        if (key.text == "/") {
            // The slash takes the panel back to its search.
            searching_ = true;
            filter_.clear();
            selected_ = 0;
            scroll_ = 0;
            filter();
            return true;
        }
        if (key.text == " ") {
            // The space changes the setting that is chosen.
            if (selected_ < visible_.size()) picked = visible_[selected_].id;
            return true;
        }
        // Any other character takes the panel to its search, the way the
        // reference does.
        searching_ = true;
        filter_ = key.text;
        selected_ = 0;
        scroll_ = 0;
        filter();
        return true;
    default:
        return false;
    }
}

} // namespace cc::ui
