#include "ui/trust_dialog.hpp"

#include "ui/select.hpp"
#include "utils/string.hpp"

namespace cc::ui {
namespace {

/// The warning the reference shows above the choices.
constexpr std::string_view kSafetyCheck =
    "Quick safety check: Is this a project you created or one you trust? (Like your own code, a "
    "well-known open source project, or work from your team). If not, take a moment to review "
    "what's in this folder first.";

/// What the user is told the session may do in the folder.
constexpr std::string_view kCanDo = "Claude Code'll be able to read, edit, and execute files here.";

/// The link to the security guide.
constexpr std::string_view kSecurityGuide = "Security guide";

/// Where the security guide lives.
constexpr std::string_view kSecurityGuideUrl = "https://code.claude.com/docs/en/security";

/// How long the first press of the exit key is remembered.
constexpr int kDoublePressTimeoutMs = 800;

} // namespace

TrustDialog::TrustDialog(Theme theme, std::string cwd)
    : theme_(std::move(theme)), cwd_(std::move(cwd)) {}

std::vector<std::string> TrustDialog::render(int width) const {
    const size_t w = width > 3 ? static_cast<size_t>(width) : 3;

    Style rule;
    rule.foreground = theme_.warning;

    Style title_style;
    title_style.bold = true;
    title_style.foreground = theme_.warning;

    Style bold;
    bold.bold = true;

    // The link to the guide is dim: the ordinary colour of the text, made
    // faint with the attribute of dimness.
    Style dim;
    dim.dim = true;

    std::vector<std::string> lines;

    // The frame of the reference leaves an empty line above the cap, and
    // the cap is the top border of a rounded box without its corners.
    lines.push_back("");
    std::string rule_text;
    rule_text.reserve(w * 3);
    for (size_t i = 0; i < w; ++i) rule_text += "─";
    lines.push_back(styled(rule_text, rule));

    lines.push_back(" " + styled("Accessing workspace:", title_style));
    lines.push_back("");

    // The folder the session is about to work in.
    lines.push_back(" " + styled(cwd_, bold));
    lines.push_back("");

    // The safety check. The reference leaves a blank line between every
    // part of it.
    for (const auto& line : cc::word_wrap(kSafetyCheck, w > 2 ? w - 2 : 1)) {
        lines.push_back(" " + line);
    }
    lines.push_back("");
    lines.push_back(" " + std::string(kCanDo));
    lines.push_back("");

    // The guide is a link: a terminal that cannot follow one is shown the
    // word alone, the way the reference shows it.
    const std::string guide =
        supports_hyperlinks() ? hyperlink(kSecurityGuide, kSecurityGuideUrl)
                              : std::string(kSecurityGuide);
    lines.push_back(" " + styled(guide, dim));
    lines.push_back("");

    // The choices. The first one is highlighted until the user moves.
    const std::vector<std::string> choices = {"Yes, I trust this folder", "No, exit"};
    for (size_t i = 0; i < choices.size(); ++i) {
        const bool is_selected = i == selected_;
        const std::string marker = is_selected ? "❯ " : "  ";
        const std::string number = std::to_string(i + 1) + ".";

        Style label;
        label.foreground = is_selected ? theme_.suggestion : theme_.text;
        lines.push_back(" " + marker + styled(number, dim) + " " + styled(choices[i], label));
    }

    lines.push_back("");
    // The hint of the reference is dim as well, and it sits one column in,
    // the way the choices do.
    Style hint;
    hint.dim = true;
    lines.push_back(" " + styled(ctrl_c_pending_ ? "Press " + exit_key_ + " again to exit"
                                                : "Enter to confirm · Esc to cancel",
                                   hint));

    return lines;
}

bool TrustDialog::run(Terminal& terminal) {
    const int width = terminal.width();

    while (true) {
        // The first press of the exit key is forgotten by itself after the
        // time the reference waits, the way the reference forgets it.
        if (ctrl_c_pending_ && std::chrono::steady_clock::now() >= exit_deadline_) {
            ctrl_c_pending_ = false;
        }

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
        case Key::Type::Enter: {
            // The highlighted choice is picked.
            terminal.clear_dynamic();
            return selected_ == 0;
        }
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
        case Key::Type::CtrlC:
        case Key::Type::CtrlD: {
            // The reference asks for the key twice: the first press only
            // rewrites the hint.
            if (ctrl_c_pending_) {
                terminal.clear_dynamic();
                return false;
            }
            ctrl_c_pending_ = true;
            exit_key_ = key->type == Key::Type::CtrlD ? "Ctrl-D" : "Ctrl-C";
            exit_deadline_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(kDoublePressTimeoutMs);
            break;
        }
        case Key::Type::Escape:
            terminal.clear_dynamic();
            return false;
        default:
            break;
        }
    }
}

} // namespace cc::ui
