#include "ui/session_picker.hpp"

#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>

namespace cc::ui {
namespace {

/// The number of the conversations that are shown at once.
constexpr size_t kVisible = 10;

/// The name of the field the conversations are searched in.
constexpr std::string_view kSearchPlaceholder = "Search…";

/// The sign in front of the field the conversations are searched in.
constexpr std::string_view kSearchSign = "⌕";

/// A run of the horizontal rule.
std::string rule(size_t count) {
    std::string out;
    out.reserve(count * figures::kHorizontalRule.size());
    for (size_t i = 0; i < count; ++i) out += figures::kHorizontalRule;
    return out;
}

/// The field the conversations are searched in.
///
/// The reference draws it in a box with rounded corners, the colour of the
/// suggestion: the sign of the field and the name of it are dim, and the
/// first letter of the name is the one the cursor stands on.
std::vector<std::string> search_field(const Theme& theme, const std::string& query,
                                      size_t width) {
    Style border;
    border.foreground = theme.suggestion;

    Style dim;
    dim.dim = true;

    Style inverse;
    inverse.inverse = true;

    const size_t inner = width > 4 ? width - 4 : 1;

    std::string content = styled(std::string(kSearchSign) + " ", dim);
    if (query.empty()) {
        content += styled(std::string(kSearchPlaceholder).substr(0, 1), inverse);
        content += styled(std::string(kSearchPlaceholder).substr(1), dim);
    } else {
        content += styled(query, Style{});
        content += styled("█", inverse);
    }

    // The text of the field is cut when it does not fit.
    const size_t used = kSearchSign.size() + 1 + (query.empty() ? kSearchPlaceholder.size()
                                                                : query.size() + 1);
    if (used > inner) {
        content = styled(std::string(kSearchSign) + " ", dim) +
                  styled(cc::truncate(query, inner, "…"), Style{});
    }

    std::vector<std::string> lines;
    lines.push_back(styled("╭" + rule(inner + 2) + "╮", border));
    lines.push_back(styled("│", border) + " " + content + " " + styled("│", border));
    lines.push_back(styled("╰" + rule(inner + 2) + "╯", border));
    return lines;
}

/// The number of the messages of a conversation, the way the reference
/// writes it.
std::string message_count(size_t count) {
    return std::to_string(count) + (count == 1 ? " message" : " messages");
}

} // namespace

SessionPicker::SessionPicker(Theme theme, std::vector<Entry> entries,
                             std::vector<Entry> all_entries, std::string current_branch)
    : theme_(std::move(theme)),
      entries_(std::move(entries)),
      all_entries_(std::move(all_entries)),
      current_branch_(std::move(current_branch)) {}

const std::vector<SessionPicker::Entry>& SessionPicker::shown() const {
    return all_ ? all_entries_ : entries_;
}

std::vector<size_t> SessionPicker::matching() const {
    const std::vector<Entry>& entries = shown();

    std::vector<size_t> found;
    for (size_t i = 0; i < entries.size(); ++i) {
        // The branch filter hides the conversations of the other branches.
        if (branch_only_ && !current_branch_.empty() && !entries[i].branch.empty() &&
            entries[i].branch != current_branch_) {
            continue;
        }
        if (query_.empty() || cc::contains(cc::to_lower(entries[i].title), cc::to_lower(query_))) {
            found.push_back(i);
        }
    }
    return found;
}

int SessionPicker::row_of(size_t index) const {
    const std::vector<size_t> found = matching();
    for (size_t i = 0; i < found.size(); ++i) {
        if (found[i] != index) continue;
        if (i < first_) return -1;
        // Every conversation takes two rows: its title and the line under
        // it.
        return kFirstRow + static_cast<int>(i - first_) * 2;
    }
    return -1;
}

void SessionPicker::scroll_to_selection() {
    const std::vector<size_t> found = matching();
    if (found.empty()) {
        first_ = 0;
        return;
    }

    // The window follows the highlight, the way the reference shows it: the
    // chosen conversation is in the middle of it, and the window never
    // reaches past the end of the list.
    const size_t visible = std::min(kVisible, found.size());
    size_t start = first_;
    if (selected_ > visible / 2) start = selected_ - visible / 2;
    start = std::min(start, found.size() > visible ? found.size() - visible : 0);
    first_ = start;
}

std::string SessionPicker::hints() const {
    std::vector<std::string> parts;

    // The key that shows the conversations of every folder, and the ones of
    // the folder of the session.
    if (!all_entries_.empty()) {
        parts.push_back(all_ ? "Ctrl+A to show current dir" : "Ctrl+A to show all projects");
    }

    // The key that shows the conversations of the branch of the session.
    if (!current_branch_.empty()) {
        parts.push_back(branch_only_ ? "Ctrl+B to show all branches" : "Ctrl+B to toggle branch");
    }

    parts.push_back("Type to search");
    parts.push_back("Esc to cancel");

    return cc::join(parts, " · ");
}

std::vector<std::string> SessionPicker::render(int width) const {
    const size_t w = width > 0 ? static_cast<size_t>(width) : 1;

    Style rule_style;
    rule_style.foreground = theme_.suggestion;

    Style title;
    title.bold = true;
    title.foreground = theme_.suggestion;

    Style dim;
    dim.dim = true;

    Style normal;
    normal.foreground = theme_.text;

    Style focused;
    focused.foreground = theme_.suggestion;

    std::vector<std::string> lines;

    // The rule the reference opens the picker with.
    lines.push_back(styled(rule(w), rule_style));
    lines.push_back("");

    // The heading, with the number of the conversation that is highlighted
    // when the list is longer than what is shown.
    const std::vector<size_t> found = matching();
    std::string heading = styled(" Resume Session", title);
    if (found.size() > kVisible) {
        heading += styled(" (" + std::to_string(selected_ + 1) + " of " +
                               std::to_string(found.size()) + ")",
                           dim);
    }
    lines.push_back(heading);

    // The field the conversations are searched in.
    for (auto& line : search_field(theme_, query_, w)) lines.push_back(std::move(line));

    lines.push_back("");

    if (found.empty()) {
        lines.push_back(styled("  No matching sessions found.", dim));
        lines.push_back("");
        lines.push_back("  " + styled(hints(), dim));
        return lines;
    }

    // The conversations: the title of every one of them, and the time it
    // was written on under it.
    const size_t visible = std::min(kVisible, found.size());
    for (size_t i = first_; i < first_ + visible && i < found.size(); ++i) {
        const Entry& entry = shown()[found[i]];
        const bool is_selected = i == selected_;

        lines.push_back(std::string(is_selected ? std::string(figures::kPointer) + " "
                                               : "  ") +
                        styled(entry.title, is_selected ? focused : normal));

        std::string metadata = entry.when;
        if (!entry.branch.empty()) metadata += " · " + entry.branch;
        // The size of the transcript is shown when it is known; the number
        // of the messages is the other half of the choice.
        if (!entry.size.empty()) {
            metadata += " · " + entry.size;
        } else {
            metadata += " · " + message_count(entry.messages);
        }
        // The folder of the conversation is named when the list holds the
        // conversations of every folder.
        if (all_ && !entry.path.empty()) metadata += " · " + entry.path;
        lines.push_back(styled("  " + metadata, dim));
    }

    lines.push_back("");
    lines.push_back("  " + styled(hints(), dim));

    return lines;
}

std::string SessionPicker::run(Terminal& terminal) {
    const int width = terminal.width();

    while (true) {
        terminal.render_dynamic(render(width));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return {};
            }
            continue;
        }

        const std::vector<size_t> found = matching();

        switch (key->type) {
        case Key::Type::Up:
            if (!found.empty()) {
                selected_ = selected_ == 0 ? found.size() - 1 : selected_ - 1;
                scroll_to_selection();
            }
            break;
        case Key::Type::Down:
            if (!found.empty()) {
                selected_ = selected_ + 1 < found.size() ? selected_ + 1 : 0;
                scroll_to_selection();
            }
            break;
        case Key::Type::Enter:
            terminal.clear_dynamic();
            if (selected_ < found.size()) return shown()[found[selected_]].id;
            return {};
        case Key::Type::CtrlA:
            // The reference switches between the conversations of the
            // folder and the conversations of every folder.
            if (!all_entries_.empty()) {
                all_ = !all_;
                selected_ = 0;
                first_ = 0;
            }
            break;
        case Key::Type::CtrlB:
            // The reference shows the conversations of the branch of the
            // session only, and then all of them again.
            if (!current_branch_.empty()) {
                branch_only_ = !branch_only_;
                selected_ = 0;
                first_ = 0;
            }
            break;
        case Key::Type::Backspace:
            if (!query_.empty()) query_.pop_back();
            selected_ = 0;
            first_ = 0;
            break;
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            terminal.clear_dynamic();
            return {};
        case Key::Type::MouseClick: {
            // A click on a conversation picks it, the way the reference
            // picks it.
            const int row = key->mouse_y - terminal.dynamic_row();
            for (size_t i = first_; i < found.size(); ++i) {
                if (row_of(found[i]) != row) continue;
                selected_ = i;
                terminal.clear_dynamic();
                return shown()[found[selected_]].id;
            }
            break;
        }
        case Key::Type::Character:
            query_ += key->text;
            selected_ = 0;
            first_ = 0;
            break;
        default:
            break;
        }
    }
}

} // namespace cc::ui
