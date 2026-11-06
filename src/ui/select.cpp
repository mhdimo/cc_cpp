#include "ui/select.hpp"

#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace cc::ui {
namespace {

/// The number of columns the option numbers take up.
size_t index_width(size_t count) {
    return std::to_string(count).size();
}

/// The option a number key names, or -1 when the key is not a number.
long number_index(const std::string& text) {
    if (text.empty()) return -1;

    long value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
        if (value > 100000) return -1;
    }
    return value - 1;
}

} // namespace

Select::Select(Theme theme, std::vector<SelectOption> options, std::string title,
               size_t visible_count)
    : theme_(std::move(theme)),
      title_(std::move(title)),
      options_(std::move(options)),
      visible_count_(visible_count == 0 ? 1 : visible_count) {}

void Select::set_selected(size_t index) {
    if (options_.empty()) {
        selected_ = 0;
        return;
    }
    selected_ = std::min(index, options_.size() - 1);

    // The list follows the highlight.
    if (selected_ < first_) {
        first_ = selected_;
    } else if (selected_ >= first_ + visible_count_) {
        first_ = selected_ - visible_count_ + 1;
    }
}

std::vector<std::string> Select::render(int width) const {
    // The layout does not depend on how wide the terminal is.
    (void)width;

    // The numbers are padded to the width of the largest one, so that every
    // label starts in the same column.
    const size_t width_of_index = index_width(options_.size());

    // The pointer, a space, the number and a space: the label starts four
    // columns past the number.
    const size_t label_column = width_of_index + 4;

    Style pointer;
    pointer.foreground = theme_.suggestion;

    Style dim;
    dim.foreground = theme_.inactive;

    Style normal;
    normal.foreground = theme_.text;

    std::vector<std::string> lines;

    // The title of the picker, the way the reference names it.
    if (!title_.empty()) {
        Style title_style;
        title_style.bold = true;
        title_style.foreground = theme_.remember;
        lines.push_back(" " + styled(title_, title_style));
        lines.push_back("");
    }

    // The reference puts the descriptions in a second column as soon as
    // there are any.
    bool two_columns = false;
    size_t label_width = 0;
    for (const auto& option : options_) {
        if (!option.description.empty()) two_columns = true;
        label_width = std::max(label_width, cc::display_width(option.label));
    }

    // Only as many options as fit are shown; the rest is reached with the
    // arrow keys.
    const size_t visible = std::min(visible_count_, options_.size());
    const bool more_above = first_ > 0;
    const bool more_below = first_ + visible < options_.size();

    for (size_t i = first_; i < first_ + visible && i < options_.size(); ++i) {
        const bool is_focused = i == selected_;

        std::string number = std::to_string(i + 1) + ".";
        number.resize(width_of_index + 1, ' ');

        // The focused entry is marked with the pointer; the ends of the
        // list show an arrow when there is more to see.
        std::string marker = " ";
        if (is_focused) {
            marker = styled(figures::kPointer, pointer);
        } else if (i == first_ && more_above) {
            marker = styled(figures::kArrowUp, dim);
        } else if (i + 1 == first_ + visible && more_below) {
            marker = styled(figures::kArrowDown, dim);
        }

        std::string line = marker + " ";
        line += styled(number, dim);
        line += " ";
        line += styled(options_[i].label, is_focused ? pointer : normal);

        if (two_columns) {
            // The description sits in a column of its own, two columns
            // past the widest label.
            const size_t used = label_column - 1 + cc::display_width(options_[i].label);
            const size_t padding = label_width + 2 > used ? label_width + 2 - used : 1;
            line += std::string(padding, ' ');
            line += styled(options_[i].description.empty() ? " " : options_[i].description, dim);
            lines.push_back(std::move(line));
            continue;
        }

        lines.push_back(std::move(line));
    }

    return lines;
}

std::string Select::selected_label() const {
    if (selected_ >= options_.size()) return {};
    return options_[selected_].label;
}

bool Select::run(Terminal& terminal) {
    const int width = terminal.width();

    while (true) {
        terminal.render_dynamic(render(width));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return false;
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Up:
            // The reference wraps around at the ends of the list.
            if (!options_.empty()) {
                set_selected(selected_ == 0 ? options_.size() - 1 : selected_ - 1);
            }
            break;
        case Key::Type::Down:
            if (!options_.empty()) {
                set_selected(selected_ + 1 < options_.size() ? selected_ + 1 : 0);
            }
            break;
        case Key::Type::Enter:
            terminal.clear_dynamic();
            return selected_ < options_.size();
        case Key::Type::Character: {
            // A number picks the entry with that number.
            const long index = number_index(key->text);
            if (index < 0 || static_cast<size_t>(index) >= options_.size()) break;
            set_selected(static_cast<size_t>(index));
            terminal.clear_dynamic();
            return true;
        }
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            terminal.clear_dynamic();
            return false;
        default:
            break;
        }
    }
}

} // namespace cc::ui
