#include "ui/mobile_view.hpp"

#include "ui/figures.hpp"
#include "ui/qr.hpp"


namespace cc::ui {
namespace {

/// The address of the application of the iPhone.
constexpr std::string_view kIosUrl = "https://apps.apple.com/app/claude-by-anthropic/id6473753684";

/// The address of the application of the Android.
constexpr std::string_view kAndroidUrl =
    "https://play.google.com/store/apps/details?id=com.anthropic.claude";

/// The padding of the pane.
///
/// The reference's Pane puts two columns of padding on the left of its
/// content and a blank line above the rule that tops it.
constexpr size_t kPanePadding = 2;

/// The padding of the pane of the modal slot.
///
/// The pane of the modal slot has one column of padding of its own, so that
/// its contents sit three columns in.
constexpr size_t kModalPanePadding = 3;

/// A string of `count` copies of `character`.
std::string repeat(std::string_view character, size_t count) {
    std::string out;
    out.reserve(character.size() * count);
    for (size_t i = 0; i < count; ++i) out += character;
    return out;
}

} // namespace

MobileView::MobileView(Theme theme) : theme_(std::move(theme)) {}

void MobileView::set_modal(bool modal) {
    modal_ = modal;
}

std::string MobileView::url() const {
    return ios_ ? std::string(kIosUrl) : std::string(kAndroidUrl);
}

std::vector<std::string> MobileView::code() const {
    const std::optional<QrCode> code = QrCode::encode(url());
    if (!code) return {};
    return code->lines();
}

std::vector<std::string> MobileView::render(int width, int height) const {
    const size_t w = width > 1 ? static_cast<size_t>(width) : 1;

    Style dim;
    dim.foreground = theme_.inactive;

    // The contents of the pane sit three columns in when the pane is drawn
    // in the modal slot, and two columns in otherwise.
    const std::string indent(modal_ ? kModalPanePadding : kPanePadding, ' ');

    std::vector<std::string> lines;

    if (modal_) {
        // The modal slot of the full-screen layout is topped by a rule of its
        // own, in the colour of the permission.
        Style slot;
        slot.foreground = theme_.permission;
        lines.push_back(styled(repeat(figures::kTopRule, w), slot));
    } else {
        // The pane starts with a blank line above the rule that tops it.
        lines.push_back("");

        // The rule that tops the pane.
        lines.push_back(styled(repeat(figures::kHorizontalRule, w), dim));
    }

    // The pane holds the code, with a blank line above and below it.
    lines.push_back("");
    lines.push_back("");

    for (const auto& line : code()) {
        lines.push_back(indent + line);
    }

    lines.push_back("");
    lines.push_back("");

    // The platforms, with the one that is showing in bold.
    Style bold;
    bold.bold = true;
    bold.underline = true;

    std::string platforms = indent;
    platforms += ios_ ? styled("iOS", bold) : styled("iOS", Style{});
    platforms += styled(" / ", dim);
    platforms += ios_ ? styled("Android", Style{}) : styled("Android", bold);
    platforms += "  " + styled("(tab to switch, esc to close)", dim);
    lines.push_back(platforms);

    // The address of the application.
    lines.push_back(indent + styled(url(), dim));

    return lines;
}

void MobileView::run(Terminal& terminal) {
    // Without a terminal there are no keys to read, and the loop below
    // would spin.
    if (!terminal.interactive()) return;

    // The reference floats the panes of the slash commands in the modal slot
    // of the full-screen layout.
    modal_ = terminal.fullscreen();

    while (true) {
        const int width = terminal.width();
        const int height = terminal.height();
        terminal.render_dynamic(render(width, height));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return;
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Tab:
        case Key::Type::Left:
        case Key::Type::Right:
            ios_ = !ios_;
            break;

        case Key::Type::Escape:
        case Key::Type::CtrlC:
            terminal.clear_dynamic();
            return;

        case Key::Type::Character:
            if (key->text == "q") {
                terminal.clear_dynamic();
                return;
            }
            break;

        default:
            break;
        }
    }
}

} // namespace cc::ui
