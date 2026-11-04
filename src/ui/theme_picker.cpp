#include "ui/theme_picker.hpp"

#include "commands/registry.hpp"
#include "ui/ansi.hpp"
#include "ui/diff.hpp"

#include <utility>

namespace cc::ui {
namespace {

/// The line of the dashes that frame the preview.
///
/// The reference draws the frame of the preview with a dashed border, and
/// only the top and the bottom of it.
constexpr std::string_view kDashedRule = "╌";

/// The file the preview shows.
///
/// The name of the file is what the reference gives to the highlighter, so
/// that the colours of the preview are the ones of the language of the
/// file. The highlighter is not wired to the diffs yet, so the name is not
/// used for now.
[[maybe_unused]] constexpr std::string_view kPreviewFile = "demo.js";

/// The patch the preview shows.
///
/// It is the patch of the reference: a function whose line is changed.
std::vector<DiffLine> preview_patch() {
    std::vector<DiffLine> lines;

    DiffLine opening;
    opening.kind = DiffLine::Kind::Context;
    opening.text = " function greet() {";
    opening.old_line = 1;
    opening.new_line = 1;
    lines.push_back(std::move(opening));

    DiffLine removed;
    removed.kind = DiffLine::Kind::Removed;
    removed.text = "  console.log(\"Hello, World!\");";
    removed.old_line = 2;
    lines.push_back(std::move(removed));

    DiffLine added;
    added.kind = DiffLine::Kind::Added;
    added.text = "  console.log(\"Hello, Claude!\");";
    added.new_line = 2;
    lines.push_back(std::move(added));

    DiffLine closing;
    closing.kind = DiffLine::Kind::Context;
    closing.text = " }";
    closing.old_line = 3;
    closing.new_line = 3;
    lines.push_back(std::move(closing));

    return lines;
}

/// The list of the themes, the way the picker shows it.
std::vector<SelectOption> select_options() {
    std::vector<SelectOption> options;
    for (const auto& option : commands::theme_options()) {
        // The reference shows the names of the themes alone, without an
        // explanation under them.
        options.push_back({option.label, ""});
    }
    return options;
}

/// The rule of the dashes that frame the preview.
std::string dashed_rule(size_t width) {
    std::string rule;
    rule.reserve(width * kDashedRule.size());
    for (size_t i = 0; i < width; ++i) rule += kDashedRule;
    return rule;
}

} // namespace

ThemePicker::ThemePicker(Theme theme, std::string selected, ThemePickerOptions options)
    : theme_(theme), select_(theme, select_options()), preview_(theme), options_(std::move(options)) {
    const auto themes = commands::theme_options();

    // The pointer starts on the theme that is set.
    for (size_t i = 0; i < themes.size(); ++i) {
        if (themes[i].value == selected) {
            select_.set_selected(i);
            break;
        }
    }

    // The preview shows the theme the pointer is on.
    ThemeName name = ThemeName::Dark;
    if (parse_theme_name(themes[select_.selected()].value, name)) preview_ = theme_for(name);
}

std::vector<std::string> ThemePicker::render(int width) const {
    const size_t w = width > 1 ? static_cast<size_t>(width) : 1;

    Style title;
    title.bold = true;
    title.foreground = theme_.permission;

    Style bold;
    bold.bold = true;

    Style dim;
    dim.foreground = theme_.inactive;

    Style rule;
    rule.foreground = theme_.subtle;

    std::vector<std::string> lines;

    // The name of the picker. The setup of the session asks the question
    // another way, the way the reference asks it there.
    if (options_.intro) {
        lines.push_back("Let's get started.");
    } else {
        lines.push_back(styled("Theme", title));
    }

    // The line that asks the question.
    lines.push_back(styled("Choose the text style that looks best with your terminal", bold));

    // The line under the question.
    if (!options_.help_text.empty()) {
        lines.push_back(styled(options_.help_text, dim));
    }

    // The list of the themes.
    for (const auto& line : select_.render(static_cast<int>(w))) {
        lines.push_back(line);
    }

    // A blank line between the list and the preview.
    lines.push_back("");

    // The preview: the frame of dashes and the patch, in the colours of the
    // theme the pointer is on.
    lines.push_back(styled(dashed_rule(w), rule));
    for (const auto& line : render_diff(preview_patch(), preview_, static_cast<int>(w))) {
        lines.push_back(line);
    }
    lines.push_back(styled(dashed_rule(w), rule));

    // The line that says whether the colours of the code are on.
    lines.push_back(styled(" Syntax highlighting enabled (ctrl+t to disable)", dim));

    // The hint under the picker. The setup of the session leaves it out:
    // the escape does not end the setup there.
    if (!options_.hide_hint) {
        lines.push_back("");
        lines.push_back(styled("Enter to select · Esc to cancel", dim));
    }

    return lines;
}

std::string ThemePicker::run(Terminal& terminal) {
    // The picker is drawn in the region of the terminal, and the region is
    // taken down when it is over.
    while (true) {
        const int width = terminal.width();
        terminal.render_dynamic(render(width), /*pinned=*/true);

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the picker ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return {};
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Up:
            select_.set_selected(select_.selected() == 0 ? select_.size() - 1 : select_.selected() - 1);
            break;
        case Key::Type::Down:
            select_.set_selected(select_.selected() + 1 >= select_.size() ? 0 : select_.selected() + 1);
            break;
        case Key::Type::Enter: {
            terminal.clear_dynamic();
            const auto themes = commands::theme_options();
            return themes[select_.selected()].value;
        }
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            terminal.clear_dynamic();
            return {};
        default:
            break;
        }

        // The preview shows the theme the pointer is on.
        const auto themes = commands::theme_options();
        ThemeName name = ThemeName::Dark;
        if (parse_theme_name(themes[select_.selected()].value, name)) preview_ = theme_for(name);
    }
}

} // namespace cc::ui
