#include "ui/transcript_view.hpp"

#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace cc::ui {
namespace {

/// The number of lines the footer takes up: a blank line, the rule and the
/// footer text.
constexpr int kFooterLines = 3;

/// The number of messages the transcript shows.
///
/// The reference shows the last thirty messages of the conversation and
/// hides the rest behind a rule that names their number; Ctrl+E brings
/// them back.
constexpr size_t kMaxMessages = 30;

/// A string of `count` copies of `character`.
std::string repeat(std::string_view character, size_t count) {
    std::string out;
    out.reserve(character.size() * count);
    for (size_t i = 0; i < count; ++i) out += character;
    return out;
}

/// The text of the footer under the transcript.
///
/// The reference names either the shortcut that shows everything or the keys
/// that scroll the transcript, and never both: the view here is the one that
/// scrolls, so the keys are the ones it names. The rule in the body names
/// the shortcut that shows everything.
std::string footer_text(bool show_all) {
    (void)show_all;
    return "Showing detailed transcript · ctrl+o to toggle · " +
           std::string(figures::kArrowUp) + std::string(figures::kArrowDown) +
           " scroll · home/end top/bottom";
}

/// The number of lines a page key scrolls.
///
/// The reference's page keys move by half a viewport.
int page_size(int height) {
    return std::max(1, (height - kFooterLines) / 2);
}

} // namespace

TranscriptView::TranscriptView(Theme theme, std::vector<TranscriptEntry> entries)
    : theme_(std::move(theme)), entries_(std::move(entries)) {}

std::string TranscriptView::truncation_rule(size_t hidden_count, bool show_all, int width) const {
    Style dim;
    dim.foreground = theme_.inactive;
    Style bold;
    bold.bold = true;
    // The reference draws the number of the rule in the grey of the rule
    // itself, and in bold.
    bold.foreground = theme_.inactive;

    const std::string action = show_all ? "hide " : "show ";
    const std::string plain = "ctrl+e to " + action + std::to_string(hidden_count) +
                              " previous messages";

    // The title sits in the middle of the rule, with the rule on both sides
    // of it, the way the reference draws it.
    const int side = std::max(0, width - (static_cast<int>(cc::display_width(plain)) + 2));
    const size_t left = static_cast<size_t>(side) / 2;
    const size_t right = static_cast<size_t>(side) - left;

    // The pieces carry their own style, so that the number keeps its bold
    // inside the dim rule.
    std::string out = styled(repeat(figures::kHorizontalRule, left) + " ", dim);
    out += styled("ctrl+e to " + action, dim);
    out += styled(std::to_string(hidden_count), bold);
    out += styled(" previous messages", dim);
    out += styled(" " + repeat(figures::kHorizontalRule, right), dim);
    return out;
}

const std::vector<std::string>& TranscriptView::body_lines(int width) const {
    // The conversation is rendered once per width and state; the view
    // redraws itself on every tick, so rendering the markdown each time
    // would be a waste.
    if (body_width_ == width && body_show_all_ == show_all_) return body_;

    body_.clear();
    body_width_ = width;
    body_show_all_ = show_all_;

    // Only the last messages of the conversation are shown; the older ones
    // are hidden behind the rule that names their number.
    const size_t total = entries_.size();
    const size_t shown = std::min(total, kMaxMessages);
    const size_t hidden_count = total - shown;

    if (hidden_count > 0) {
        body_.push_back(truncation_rule(hidden_count, show_all_, width));
    }

    for (size_t i = 0; i < total; ++i) {
        const auto& entry = entries_[i];

        // The entries of the older messages and the ones that are marked as
        // hidden only join the transcript when everything is shown.
        if ((entry.hidden || i < total - shown) && !show_all_) continue;

        // A blank line between the entries, the way the reference separates
        // the messages.
        if (!body_.empty()) body_.push_back("");

        for (const auto& line : entry.lines) {
            body_.push_back(line);
        }
    }

    return body_;
}

int TranscriptView::max_scroll(int width, int height) const {
    const int body_height = std::max(height - kFooterLines, 1);
    const int lines = static_cast<int>(body_lines(width).size());
    return std::max(0, lines - body_height);
}

std::vector<std::string> TranscriptView::render(int width, int height) const {
    Style dim;
    dim.foreground = theme_.inactive;

    // A terminal that is too short for the footer only gets the footer.
    if (height <= kFooterLines) {
        return {"  " + styled(footer_text(show_all_), dim)};
    }

    const auto& body = body_lines(width);

    const int body_height = height - kFooterLines;
    const int max_offset = std::max(0, static_cast<int>(body.size()) - body_height);
    const int offset = std::clamp(scroll_, 0, max_offset);

    std::vector<std::string> lines;
    lines.reserve(static_cast<size_t>(height));

    // The window of the conversation that is on screen.
    for (int i = offset; i < static_cast<int>(body.size()) &&
                         static_cast<int>(lines.size()) < body_height;
         ++i) {
        lines.push_back(body[static_cast<size_t>(i)]);
    }

    // The rest of the screen stays empty, so that the footer sits at the
    // bottom of it.
    while (static_cast<int>(lines.size()) < body_height) lines.push_back("");

    // The footer: a blank line, a rule and the text under it.
    lines.push_back("");

    Style rule;
    rule.foreground = theme_.inactive;
    lines.push_back(styled(repeat(figures::kHorizontalRule, static_cast<size_t>(std::max(width, 1))),
                            rule));

    lines.push_back("  " + styled(footer_text(show_all_), dim));

    return lines;
}

void TranscriptView::run(Terminal& terminal, bool& show_all) {
    show_all_ = show_all;

    // The transcript opens at the bottom, the way the reference's sticky
    // scroll does.
    scroll_ = max_scroll(terminal.width(), terminal.height());

    // The view has no text field, so the cursor is of no use.
    terminal.hide_cursor();

    const auto leave = [&terminal]() {
        terminal.clear_dynamic();
        // The cursor of the terminal stays hidden: the prompt draws its own
        // caret, and the cursor of the terminal would sit at the end of the
        // box of the prompt for the rest of the session.
        terminal.hide_cursor();
    };

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
        case Key::Type::CtrlE:
            // Everything is shown now: the raw tool results and the thinking
            // blocks join the transcript.
            show_all_ = !show_all_;
            show_all = show_all_;
            break;
        case Key::Type::CtrlO:
            // Ctrl+O toggles the transcript: the view closes and the
            // session comes back, the way the reference does it.
            leave();
            return;
        case Key::Type::CtrlC:
        case Key::Type::Escape:
            leave();
            return;
        case Key::Type::Up:
            --scroll_;
            break;
        case Key::Type::Down:
            ++scroll_;
            break;
        case Key::Type::MouseScrollUp:
            // The wheel of the mouse scrolls the conversation.
            scroll_ -= 3;
            break;
        case Key::Type::MouseScrollDown:
            scroll_ += 3;
            break;
        case Key::Type::PageUp:
            scroll_ -= page_size(height);
            break;
        case Key::Type::PageDown:
            scroll_ += page_size(height);
            break;
        case Key::Type::Home:
            scroll_ = 0;
            break;
        case Key::Type::End:
            scroll_ = max_scroll(width, height);
            break;
        case Key::Type::Character:
            if (key->text == "q") {
                leave();
                return;
            }
            break;
        default:
            break;
        }

        // The conversation may have grown or shrunk, so the offset is
        // brought back into range after every key.
        scroll_ = std::clamp(scroll_, 0, max_scroll(width, height));
    }
}

} // namespace cc::ui
