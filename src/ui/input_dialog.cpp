#include "ui/input_dialog.hpp"

#include "ui/ansi.hpp"

namespace cc::ui {
namespace {

/// How long the first press of the Ctrl-C is remembered.
constexpr int kDoublePressTimeoutMs = 800;

} // namespace

InputDialog::InputDialog(Theme theme, std::string title, std::string subtitle, std::string value)
    : theme_(theme),
      title_(std::move(title)),
      subtitle_(std::move(subtitle)),
      input_(theme, "") {
    if (!value.empty()) input_.set_text(value);
}

void InputDialog::set_field(std::string label, std::string description, size_t index,
                             size_t count) {
    field_label_ = std::move(label);
    field_description_ = std::move(description);
    field_index_ = index;
    field_count_ = count;
}

void InputDialog::set_lead(std::string lead) { lead_ = std::move(lead); }

void InputDialog::set_hint(std::string hint) { hint_ = std::move(hint); }

std::vector<std::string> InputDialog::render(int width) const {
    const int w = width > 0 ? width : 1;

    std::vector<std::string> out;

    // The frame of the reference leaves an empty line above its rule, and
    // the rule is painted with the permission blue.
    out.push_back("");
    std::string rule;
    rule.reserve(static_cast<size_t>(w) * 3);
    for (int i = 0; i < w; ++i) rule += "─";
    out.push_back(styled(rule, Style::fg(theme_.permission)));

    // The content of the pane stands two columns in.
    const std::string indent = "  ";

    Style title_style;
    title_style.bold = true;
    title_style.foreground = theme_.permission;
    if (!title_.empty()) out.push_back(indent + styled(title_, title_style));

    // The subtitle of the dialog is dim: the ordinary colour of the text,
    // made faint with the attribute of dimness.
    Style dim;
    dim.dim = true;
    if (!subtitle_.empty()) out.push_back(indent + styled(subtitle_, dim));

    if (!title_.empty() || !subtitle_.empty()) out.push_back("");

    // The line that stands above the field.
    if (!lead_.empty()) {
        Style lead_style;
        lead_style.foreground = theme_.text;
        out.push_back(indent + styled(lead_, lead_style));
    }

    // The field has a title of its own and a description of its own.
    if (!field_label_.empty()) {
        Style label_style;
        label_style.bold = true;
        label_style.foreground = theme_.text;
        out.push_back(indent + styled(field_label_, label_style));
    }
    if (!field_description_.empty()) out.push_back(indent + styled(field_description_, dim));

    // The field is a single line: the small pointer, the text and the
    // block of the caret.
    out.push_back("");
    out.push_back(indent + styled(std::string("› "), Style{}) +
                  styled(input_.text(), Style{}) + styled("█", Style{}));

    // The number of the field, and the way it is saved.
    if (field_count_ > 0) {
        out.push_back("");
        out.push_back(indent + styled("Field " + std::to_string(field_index_ + 1) + " of " +
                                          std::to_string(field_count_),
                                      dim));
        if (field_index_ + 1 < field_count_) {
            out.push_back(indent + styled("Tab: Next field · Enter: Save and continue", dim));
        } else {
            out.push_back(indent + styled("Enter: Save configuration", dim));
        }
    }

    out.push_back("");

    // The hint of the reference is dim: the ordinary colour of the text,
    // made faint with the attribute of dimness.
    Style hint;
    hint.dim = true;
    hint.italic = true;
    out.push_back(indent + styled(ctrl_c_pending_ ? "Press Ctrl-C again to exit" : hint_, hint));

    return out;
}

InputDialogResult InputDialog::run(Terminal& terminal) {
    while (true) {
        // The first press of the Ctrl-C is forgotten by itself after the
        // time the reference waits, the way the reference forgets it.
        if (ctrl_c_pending_ && std::chrono::steady_clock::now() >= exit_deadline_) {
            ctrl_c_pending_ = false;
        }

        terminal.render_dynamic(render(terminal.width()));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return InputDialogResult{InputDialogOutcome::Cancelled, {}, false};
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Enter: {
            InputDialogResult result;
            result.value = input_.text();
            terminal.clear_dynamic();
            return result;
        }
        case Key::Type::Tab: {
            // The tab key asks for the next field; the reference keeps the
            // value of the field that is left.
            if (field_count_ == 0 || field_index_ + 1 >= field_count_) break;
            InputDialogResult result;
            result.value = input_.text();
            result.next_field = true;
            terminal.clear_dynamic();
            return result;
        }
        case Key::Type::Escape: {
            terminal.clear_dynamic();
            return InputDialogResult{InputDialogOutcome::Cancelled, {}, false};
        }
        case Key::Type::CtrlC: {
            if (!exit_on_ctrl_c_) {
                // The reference leaves the field to the Ctrl-C when a field
                // is being written in.
                terminal.clear_dynamic();
                return InputDialogResult{InputDialogOutcome::Interrupted, {}, false};
            }
            // The reference asks for the key twice: the first press only
            // rewrites the hint.
            if (ctrl_c_pending_) {
                terminal.clear_dynamic();
                return InputDialogResult{InputDialogOutcome::Cancelled, {}, false};
            }
            ctrl_c_pending_ = true;
            exit_deadline_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(kDoublePressTimeoutMs);
            break;
        }
        case Key::Type::Backspace:
            input_.backspace();
            break;
        case Key::Type::Delete:
            input_.delete_forward();
            break;
        case Key::Type::Left:
            input_.move_left();
            break;
        case Key::Type::Right:
            input_.move_right();
            break;
        case Key::Type::Home:
            input_.move_home();
            break;
        case Key::Type::End:
            input_.move_end();
            break;
        case Key::Type::CtrlU:
            input_.kill_to_start();
            break;
        case Key::Type::CtrlK:
            input_.kill_to_end();
            break;
        case Key::Type::Character:
            input_.insert(key->text);
            break;
        default:
            break;
        }
    }
}

} // namespace cc::ui
