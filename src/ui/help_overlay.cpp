#include "ui/help_overlay.hpp"

#include "core/version.hpp"
#include "ui/figures.hpp"
#include "ui/prompt_input.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <chrono>

namespace cc::ui {
namespace {

/// The sentence the general tab opens with.
constexpr std::string_view kIntro =
    "Claude understands your codebase, makes edits with your permission, and executes commands "
    "— right from your terminal.";

/// The titles of the tabs, in the order they are shown.
struct TabTitle {
    HelpTab tab;
    std::string_view title;
};

constexpr TabTitle kTabs[] = {
    {HelpTab::General, "general"},
    {HelpTab::Commands, "commands"},
    {HelpTab::CustomCommands, "custom-commands"},
};

/// A string of `count` copies of `character`.
std::string repeat(std::string_view character, size_t count) {
    std::string out;
    out.reserve(character.size() * count);
    for (size_t i = 0; i < count; ++i) out += character;
    return out;
}

/// Cuts the text to `width` columns, with an ellipsis in place of what is
/// left over.
std::string truncate_to_width(std::string_view text, size_t width) {
    if (cc::display_width(text) <= width) return std::string(text);
    if (width <= 1) return "…";

    size_t used = 0;
    std::string out;
    for (const auto& cluster : cc::graphemes(text)) {
        const size_t columns = cc::display_width(cluster);
        if (used + columns > width - 1) break;
        out += cluster;
        used += columns;
    }
    return out + "…";
}

/// Cuts the text to one line of `width` columns.
///
/// The reference keeps the first line of the text and puts an ellipsis in
/// its place.
std::string truncate_line(std::string_view text, size_t width) {
    const size_t newline = text.find('\n');
    if (newline != std::string::npos) {
        const std::string head(text.substr(0, newline));
        if (cc::display_width(head) + 1 > width) return truncate_to_width(head, width);
        return head + "…";
    }
    return truncate_to_width(text, width);
}

/// The time the second Ctrl-C has to come in.
constexpr auto kDoublePressTimeout = std::chrono::milliseconds(800);

} // namespace

HelpOverlay::HelpOverlay(Theme theme, std::vector<HelpCommand> builtin,
                         std::vector<HelpCommand> custom)
    : theme_(std::move(theme)), builtin_(std::move(builtin)), custom_(std::move(custom)) {
    // The command lists are sorted by name, the way the reference sorts them.
    std::sort(builtin_.begin(), builtin_.end(),
              [](const HelpCommand& a, const HelpCommand& b) { return a.name < b.name; });
    std::sort(custom_.begin(), custom_.end(),
              [](const HelpCommand& a, const HelpCommand& b) { return a.name < b.name; });
}

const std::vector<HelpCommand>& HelpOverlay::current_commands() const {
    return tab_ == HelpTab::CustomCommands ? custom_ : builtin_;
}

std::vector<std::string> HelpOverlay::render_general(int width) const {
    std::vector<std::string> lines;

    // The tab opens with a blank line, the way the reference opens it.
    lines.push_back("");

    // The pane of the reference has a padding of two columns on both sides,
    // so the text is wrapped four columns narrower than the pane: the two
    // columns of the left margin are written here, the two of the right one
    // are left empty.
    for (const auto& line :
         cc::word_wrap(kIntro, width > 4 ? static_cast<size_t>(width) - 4 : 1)) {
        lines.push_back("  " + line);
    }

    lines.push_back("");
    lines.push_back("  " + styled("Shortcuts", Style{.bold = true}));

    // The menu of the help pane has a gap of two between its columns.
    for (const auto& line : PromptInput::shortcuts_menu(width, /*dim=*/false, /*gap=*/2)) {
        lines.push_back("  " + line);
    }

    lines.push_back("");
    return lines;
}

std::vector<std::string> HelpOverlay::render_commands(const std::vector<HelpCommand>& commands,
                                                      const std::string& title, int width,
                                                      int height, bool show_empty_message) const {
    std::vector<std::string> lines;
    lines.push_back("");

    if (commands.empty() && show_empty_message) {
        // The reference shows the message on the tab of the commands of the
        // user only; the tab of the built-in commands keeps its title above
        // the empty list. The message is dimmed, the way the reference dims
        // it.
        lines.push_back("  " + styled("No custom commands found", Style{.dim = true}));
        lines.push_back("");
        return lines;
    }

    lines.push_back("  " + title);
    lines.push_back("");

    // Every command takes two lines: the name and the description, so the
    // list is half as long as the pane allows. The height that is given is
    // the height of the pane, which is half the screen, the way the
    // reference keeps it.
    const size_t pane_height = height > 0 ? static_cast<size_t>(height) : 1;
    const size_t visible =
        std::max<size_t>(1, (pane_height > 10 ? pane_height - 10 : 0) / 2);

    // The window of the list is sticky: it stands where it is and moves by
    // one row only when the highlight passes its edge.
    const size_t first = first_;

    // The description is cut to one line rather than wrapped.
    const size_t description_width = width > 10 ? static_cast<size_t>(width) - 10 : 1;

    const bool more_above = first > 0;
    const bool more_below = first + visible < commands.size();

    for (size_t i = first; i < first + visible && i < commands.size(); ++i) {
        const bool is_selected = i == selected_;
        const bool is_first = i == first;
        const bool is_last = i + 1 == first + visible;

        Style label;
        label.foreground = is_selected ? theme_.suggestion : theme_.text;

        // The pointer marks the focused command; the arrows at the ends of
        // the list say that there is more to see. The arrows are dimmed,
        // the way the reference dims them.
        std::string indicator = " ";
        if (is_selected) {
            Style pointer;
            pointer.foreground = theme_.suggestion;
            indicator = styled(std::string(figures::kPointer), pointer);
        } else if (is_first && more_above) {
            Style dim;
            dim.dim = true;
            indicator = styled(std::string(figures::kArrowUp), dim);
        } else if (is_last && more_below) {
            Style dim;
            dim.dim = true;
            indicator = styled(std::string(figures::kArrowDown), dim);
        }

        lines.push_back("  " + indicator + " " + styled(commands[i].name, label));

        // The description is dimmed, and the one of the focused command
        // takes the colour of the highlight, the way the reference draws
        // it.
        Style description;
        description.dim = true;
        if (is_selected) description.foreground = theme_.suggestion;
        const std::string text = truncate_line(commands[i].description, description_width);
        lines.push_back("      " + styled(text, description));
    }

    lines.push_back("");
    return lines;
}

std::vector<std::string> HelpOverlay::render(int width, int height) const {
    const size_t w = width > 1 ? static_cast<size_t>(width) : 1;

    // The reference wraps the pane in a box of half the screen, so the pane
    // is never taller than that and the rows that do not fit are cut off.
    const size_t max_height = height > 0 ? static_cast<size_t>(height) / 2 : 0;
    pane_height_ = max_height;

    std::vector<std::string> lines;

    // The pane starts with a blank line above the rule that tops it.
    lines.push_back("");

    // The rule that tops the pane.
    Style rule;
    rule.foreground = theme_.professional_blue;
    lines.push_back(styled(repeat("─", w), rule));

    // The header: the title and the tabs. The tab that is showing is
    // inverted.
    Style title_style;
    title_style.bold = true;
    title_style.foreground = theme_.professional_blue;

    std::string header = "  " + styled("Claude Code v" + std::string(version()), title_style);
    for (const auto& tab : kTabs) {
        header += " ";
        if (tab.tab == tab_) {
            // The tab that is showing is bold, the way the reference draws
            // it. While the header holds the focus it is painted with the
            // colour of the pane and its text with the colour that goes
            // with it; once the list of the commands has the focus the text
            // is inverted instead.
            Style active;
            active.bold = true;
            if (header_focused_) {
                active.background = theme_.professional_blue;
                active.foreground = theme_.inverse_text;
            } else {
                active.inverse = true;
            }
            header += styled(" " + std::string(tab.title) + " ", active);
        } else {
            header += styled(" " + std::string(tab.title) + " ", Style{});
        }
    }
    lines.push_back(header);
    lines.push_back("");

    if (tab_ == HelpTab::General) {
        for (const auto& line : render_general(static_cast<int>(w))) {
            lines.push_back(line);
        }
    } else {
        const bool custom = tab_ == HelpTab::CustomCommands;
        for (const auto& line :
             render_commands(custom ? custom_ : builtin_,
                             custom ? "Browse custom commands:" : "Browse default commands:",
                             static_cast<int>(w), static_cast<int>(max_height), custom)) {
            lines.push_back(line);
        }
    }

    lines.push_back("");
    // The address of the help is a link, the way the reference writes it.
    // A terminal that cannot follow one is shown the address alone.
    lines.push_back("  For more help: " +
                    hyperlink("https://code.claude.com/docs/en/overview",
                              "https://code.claude.com/docs/en/overview"));
    lines.push_back("");

    // The last line is dimmed. While the exit waits for its second Ctrl-C
    // it asks for it, the way the reference asks for it.
    Style dim;
    dim.dim = true;
    if (exit_pending()) {
        lines.push_back("  " + styled("Press Ctrl-C again to exit", dim));
    } else {
        dim.italic = true;
        // The name of the key that dismisses the pane is written the way
        // the reference writes it.
        lines.push_back("  " + styled("Esc to cancel", dim));
    }

    // The pane is at most half the screen tall, the way the reference keeps
    // it in a box of that height.
    if (max_height > 0 && lines.size() > max_height) lines.resize(max_height);

    return lines;
}

bool HelpOverlay::exit_pending() const {
    if (!exit_armed_) return false;
    return std::chrono::steady_clock::now() - exit_pressed_at_ <= kDoublePressTimeout;
}

bool HelpOverlay::handle_key(const Key& key) {
    switch (key.type) {
    case Key::Type::Escape:
        return false;

    case Key::Type::CtrlC: {
        // The first Ctrl-C only arms the exit; the second one closes the
        // pane, the way the reference closes it.
        if (exit_pending()) {
            exit_armed_ = false;
            return false;
        }
        exit_armed_ = true;
        exit_pressed_at_ = std::chrono::steady_clock::now();
        return true;
    }

    case Key::Type::Enter:
        // The reference does nothing when the return key is pressed in the
        // pane: the list cannot be accepted.
        return true;

    case Key::Type::Left:
    case Key::Type::ShiftTab: {
        // The keys of the tabs only work while the header holds the focus,
        // the way the reference binds them.
        if (!header_focused_) return true;
        const size_t count = sizeof(kTabs) / sizeof(kTabs[0]);
        size_t index = 0;
        for (size_t i = 0; i < count; ++i) {
            if (kTabs[i].tab == tab_) index = i;
        }
        tab_ = kTabs[(index + count - 1) % count].tab;
        selected_ = 0;
        first_ = 0;
        return true;
    }

    case Key::Type::Right:
    case Key::Type::Tab: {
        if (!header_focused_) return true;
        const size_t count = sizeof(kTabs) / sizeof(kTabs[0]);
        size_t index = 0;
        for (size_t i = 0; i < count; ++i) {
            if (kTabs[i].tab == tab_) index = i;
        }
        tab_ = kTabs[(index + 1) % count].tab;
        selected_ = 0;
        first_ = 0;
        return true;
    }

    case Key::Type::Up: {
        // While the header holds the focus the list is disabled, the way
        // the reference disables it.
        if (header_focused_) return true;
        // The general tab lists no command.
        if (tab_ == HelpTab::General) return true;
        const auto& commands = current_commands();
        if (commands.empty()) return true;
        if (selected_ == 0) {
            // The ↑ on the first entry goes back to the header.
            header_focused_ = true;
            return true;
        }
        --selected_;
        // The window of the list moves up by one row only when the
        // highlight passes its top edge.
        if (selected_ <= first_ && first_ > 0) --first_;
        return true;
    }

    case Key::Type::Down: {
        if (header_focused_) {
            // The first ↓ leaves the header and hands the focus to the
            // list; the highlight stays where it is.
            header_focused_ = false;
            return true;
        }
        // The general tab lists no command.
        if (tab_ == HelpTab::General) return true;
        const auto& commands = current_commands();
        if (commands.empty()) return true;
        if (selected_ + 1 >= commands.size()) return true;
        ++selected_;
        // The window of the list moves down by one row only when the
        // highlight passes its bottom edge.
        const size_t visible =
            std::max<size_t>(1, (pane_height_ > 10 ? pane_height_ - 10 : 0) / 2);
        if (selected_ >= first_ + visible) ++first_;
        return true;
    }

    default:
        return true;
    }
}

} // namespace cc::ui
