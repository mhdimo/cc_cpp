#include "ui/external_includes_dialog.hpp"

#include "ui/ansi.hpp"
#include "ui/figures.hpp"
#include "utils/string.hpp"

namespace cc::ui {
namespace {

/// The title of the dialog.
constexpr std::string_view kTitle = "Allow external CLAUDE.md file imports?";

/// What the dialog tells the user about the files of the project.
constexpr std::string_view kBody =
    "This project's CLAUDE.md imports files outside the current working directory. Never allow this "
    "for third-party repositories.";

/// The heading of the list of the files.
constexpr std::string_view kExternalImports = "External imports:";

/// The note about the files of a project the user does not know.
constexpr std::string_view kSecurityNote =
    "Important: Only use Claude Code with files you trust. Accessing untrusted files may pose security "
    "risks";

/// Where the security guide lives.
constexpr std::string_view kSecurityGuideUrl = "https://code.claude.com/docs/en/security";

/// The choices of the dialog.
constexpr const char* kChoices[] = {"Yes, allow external imports", "No, disable external imports"};

} // namespace

ExternalIncludesDialog::ExternalIncludesDialog(Theme theme, std::vector<std::string> includes,
                                               bool standalone)
    : theme_(std::move(theme)),
      includes_(std::move(includes)),
      standalone_(standalone) {}

std::vector<std::string> ExternalIncludesDialog::render(int width) const {
    const size_t w = width > 3 ? static_cast<size_t>(width) : 3;

    Style rule;
    rule.foreground = theme_.warning;

    Style title_style;
    title_style.bold = true;
    title_style.foreground = theme_.warning;

    Style normal;
    normal.foreground = theme_.text;

    // The numbers are dim the way `<Text dimColor>` is: the ordinary colour
    // of the text, made faint with the attribute of dimness.
    Style dim;
    dim.dim = true;

    Style focused;
    focused.foreground = theme_.suggestion;

    Style hint_style;
    hint_style.dim = true;
    hint_style.italic = true;

    std::vector<std::string> lines;

    // The frame of the reference leaves an empty line above the rule, and
    // the rule is painted with the colour of the warning.
    if (standalone_) {
        lines.push_back("");
        std::string rule_text;
        rule_text.reserve(w * 3);
        for (size_t i = 0; i < w; ++i) rule_text += figures::kHorizontalRule;
        lines.push_back(styled(rule_text, rule));
    }

    // The content of the pane stands two columns in.
    const std::string indent = "  ";
    const size_t text_width = w > 2 ? w - 2 : 1;

    lines.push_back(indent + styled(std::string(kTitle), title_style));
    lines.push_back("");

    for (const auto& line : cc::word_wrap(std::string(kBody), text_width)) {
        lines.push_back(indent + styled(line, normal));
    }
    lines.push_back("");

    // The files the project pulls in. The reference lists them dim, one
    // under the other.
    if (!includes_.empty()) {
        lines.push_back(indent + styled(std::string(kExternalImports), dim));
        for (const auto& path : includes_) {
            lines.push_back(indent + "  " + styled(path, dim));
        }
        lines.push_back("");
    }

    // The note about the files of a project the user does not know. The
    // guide is a link: a terminal that cannot follow one is shown the
    // address alone, the way the reference shows it.
    const std::string guide = hyperlink(std::string(kSecurityGuideUrl), kSecurityGuideUrl);
    for (const auto& line :
         cc::word_wrap(std::string(kSecurityNote) + " " + guide, text_width)) {
        lines.push_back(indent + styled(line, dim));
    }
    lines.push_back("");

    // The choices.
    for (size_t i = 0; i < std::size(kChoices); ++i) {
        const bool is_selected = i == selected_;
        const std::string marker = is_selected ? std::string(figures::kPointer) + " " : "  ";
        const std::string number = std::to_string(i + 1) + ".";
        lines.push_back(indent + marker + styled(number, dim) + " " +
                        styled(kChoices[i], is_selected ? focused : normal));
    }

    if (standalone_) {
        lines.push_back("");
        lines.push_back(indent + styled("Enter to confirm · Esc to cancel", hint_style));
    }

    return lines;
}

bool ExternalIncludesDialog::run(Terminal& terminal) {
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
            selected_ = selected_ == 0 ? 1 : 0;
            break;
        case Key::Type::Down:
            selected_ = selected_ + 1 < 2 ? selected_ + 1 : 0;
            break;
        case Key::Type::Enter:
            terminal.clear_dynamic();
            return selected_ == 0;
        case Key::Type::Character: {
            if (key->text == "1") {
                terminal.clear_dynamic();
                return true;
            }
            if (key->text == "2") {
                terminal.clear_dynamic();
                return false;
            }
            break;
        }
        case Key::Type::Escape:
        case Key::Type::CtrlC:
        case Key::Type::CtrlD:
            // The reference treats the escape as a refusal: the files
            // outside the folder are not read.
            terminal.clear_dynamic();
            return false;
        default:
            break;
        }
    }
}

} // namespace cc::ui
