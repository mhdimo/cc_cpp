#include "ui/plugin_configure.hpp"

#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace cc::ui {
namespace {

/// The name of the button that hands the values over.
constexpr std::string_view kSubmitLabel = "Save configuration";

/// The text a field of a secret shows in the place of the value that is
/// already saved.
constexpr std::string_view kUnchanged = "(unchanged)";

/// The triangles in front of and behind the value of a choice.
constexpr std::string_view kTriangleLeft = "◀";
constexpr std::string_view kTriangleRight = "▶";

/// The keys the dialog names at the bottom.
constexpr std::string_view kMoveHint = "↑/↓ to move";
constexpr std::string_view kChangeHint = "←/→ to change";
constexpr std::string_view kContinueHint = "Enter to continue";
constexpr std::string_view kCancelHint = "Esc to cancel";

/// The first line of the text, without the spaces around it.
///
/// The reference takes the first line of what the user wrote and trims it;
/// the rest of the text is left out.
std::string first_line(const std::string& text) {
    size_t end = text.find('\n');
    std::string line = end == std::string::npos ? text : text.substr(0, end);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return cc::trim(line);
}

/// The number the text holds, when it holds one.
///
/// The reference reads the number with `Number()`, which takes a number
/// with a fraction as well; a text that holds no number at all is no
/// number.
std::optional<double> parse_number(const std::string& text) {
    const std::string trimmed = cc::trim(text);
    if (trimmed.empty()) return std::nullopt;

    char* end = nullptr;
    const double number = std::strtod(trimmed.c_str(), &end);
    if (end == nullptr || *end != '\0') return std::nullopt;
    if (!std::isfinite(number)) return std::nullopt;

    return number;
}

/// The text of a value that is saved.
///
/// The reference turns the value into a string; a value that is nothing
/// the dialog can show is no text at all.
std::string value_text(const boost::json::value& value) {
    if (value.is_string()) return std::string(value.as_string().c_str());
    if (value.is_bool()) return value.as_bool() ? "true" : "false";
    if (value.is_int64()) return std::to_string(value.as_int64());
    if (value.is_double()) return std::to_string(value.as_double());
    return {};
}

/// True when the value is the truthy one.
///
/// The reference holds a value when it is `true` or the text "true"; every
/// other value is not held.
bool held_true(const boost::json::value& value) {
    if (value.is_bool()) return value.as_bool();
    if (value.is_string()) return value.as_string() == "true";
    return false;
}

} // namespace

PluginConfigure::PluginConfigure(Theme theme, std::string title, std::string subtitle)
    : theme_(theme), title_(std::move(title)), subtitle_(std::move(subtitle)) {}

void PluginConfigure::set_options(std::vector<plugins::PluginOption> options,
                                  boost::json::object values) {
    fields_.clear();
    inputs_.clear();
    saved_ = std::move(values);
    values_.clear();
    current_ = 0;
    cancelled_ = false;
    label_width_ = 0;

    for (const plugins::PluginOption& option : options) {
        Field field;
        field.key = option.key;
        field.label = option.title.empty() ? option.key : option.title;
        field.description = option.description;
        field.type = option.type;
        field.sensitive = option.sensitive;
        field.choice = option.type == "boolean";

        label_width_ = std::max(label_width_, cc::display_width(field.label));

        // The value the field is filled in with.
        //
        // The value that is saved wins over the default of the option; a
        // value that is secret is never filled in, the way the reference
        // leaves it: the secret that is saved is not shown again.
        std::string text;
        const auto stored = saved_.find(field.key);
        const bool saved = stored != saved_.end();

        // A secret that is already saved is not asked for again: the
        // reference leaves the field of it out of the ones that must be
        // given, so that the secret that is saved is kept.
        field.required = option.required && !(field.sensitive && saved);

        if (field.choice) {
            const boost::json::value value =
                saved ? stored->value() : boost::json::value(option.default_value);
            text = held_true(value) ? "true" : "false";
        } else if (!field.sensitive) {
            if (saved) {
                text = value_text(stored->value());
            } else if (!option.default_value.empty()) {
                text = option.default_value;
            }
        }

        fields_.push_back(std::move(field));
        inputs_.emplace_back(theme_, "");
        inputs_.back().set_text(std::move(text));
    }
}

const std::string& PluginConfigure::field_text(size_t index) const {
    static const std::string empty;
    if (index >= inputs_.size()) return empty;
    return inputs_[index].text();
}

const PluginConfigure::Field* PluginConfigure::current_field() const {
    if (current_ >= fields_.size()) return nullptr;
    return &fields_[current_];
}

std::vector<std::string> PluginConfigure::render(int width) const {
    const int w = width > 0 ? width : 1;

    std::vector<std::string> out;

    // The frame of the reference leaves an empty line above its rule, and
    // the rule is painted with the permission blue.
    out.push_back("");
    std::string rule;
    rule.reserve(static_cast<size_t>(w) * 3);
    for (int i = 0; i < w; ++i) rule += figures::kHorizontalRule;
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

    // The gap between the title of the dialog and the options.
    out.push_back("");

    for (size_t i = 0; i < fields_.size(); ++i) out.push_back(indent + render_field(i));

    // The button that hands the values over.
    out.push_back("");
    out.push_back(indent + render_submit());

    // The error of the option that is asked, or its explanation.
    out.push_back("");
    out.push_back(indent + render_note());

    // The keys.
    out.push_back("");
    out.push_back(indent + styled(render_keys(), dim));

    return out;
}

std::string PluginConfigure::render_field(size_t index) const {
    const Field& field = fields_[index];
    const bool focused = current_ == index;

    Style pointer_style;
    pointer_style.foreground = theme_.suggestion;

    // The pointer stands in front of the option that is asked; the others
    // keep the space of the pointer.
    std::string line =
        focused ? styled(std::string(figures::kPointer) + " ", pointer_style) : std::string("  ");

    // The name of the option is faint when it is not the one that is asked.
    Style label_style;
    label_style.dim = !focused;

    Style error_style;
    error_style.foreground = theme_.error;

    // The name of the option, the mark of the ones that must be given and
    // the padding that lines the values up.
    line += styled(field.label, label_style);
    const bool missing = field.required && !field.choice && field_text(index).empty();
    line += missing ? styled("*", error_style) : " ";
    const size_t width_of_label = cc::display_width(field.label);
    if (label_width_ > width_of_label) {
        line += styled(std::string(label_width_ - width_of_label, ' '), label_style);
    }
    line += " ";

    Style dim;
    dim.dim = true;

    if (field.choice) {
        // The choice stands between two triangles when it is the one that
        // is asked.
        if (focused) {
            line += styled(std::string(kTriangleLeft) + " ", dim);
            line += field_text(index);
            line += styled(" " + std::string(kTriangleRight), dim);
        } else {
            line += field_text(index);
        }
        return line;
    }

    // The value of the option.
    const std::string& text = field_text(index);
    if (text.empty()) {
        // The field of a secret that is already saved says that the value
        // is unchanged.
        const bool unchanged = field.sensitive && saved_.find(field.key) != saved_.end();
        if (unchanged) line += styled(std::string(kUnchanged), dim);
    } else if (field.sensitive) {
        // The value of a secret is masked.
        line += styled(std::string(cc::display_width(text), '*'), Style{});
    } else {
        line += styled(text, Style{});
    }

    // The block of the caret stands in the field that is asked.
    if (focused) line += styled("█", Style{});

    return line;
}

std::string PluginConfigure::render_submit() const {
    const bool focused = current_ == submit_row();

    Style pointer_style;
    pointer_style.foreground = theme_.suggestion;

    std::string line =
        focused ? styled(std::string(figures::kPointer) + " ", pointer_style) : std::string("  ");

    // The name of the button is in bold when the pointer is on it, and it
    // is faint while a value is still missing.
    Style label_style;
    label_style.bold = focused;
    const std::string error = first_error();
    if (!error.empty()) label_style.dim = true;
    line += styled(std::string(kSubmitLabel), label_style);

    // The error stands beside the button when the pointer is on it.
    if (focused && !error.empty()) {
        Style error_style;
        error_style.foreground = theme_.error;
        line += styled(" · " + error, error_style);
    }

    return line;
}

std::string PluginConfigure::render_note() const {
    const Field* field = current_field();
    if (field != nullptr) {
        const std::string error = error_of(current_);
        if (!error.empty()) {
            Style error_style;
            error_style.foreground = theme_.error;
            return styled(error, error_style);
        }

        if (!field->description.empty()) {
            Style dim;
            dim.dim = true;
            return styled(field->description, dim);
        }
    }

    return " ";
}

std::string PluginConfigure::render_keys() const {
    std::string keys = std::string(kMoveHint);

    // The choice of a value of the kind `boolean` is changed with the
    // arrows that stand on their sides.
    const Field* field = current_field();
    if (field != nullptr && field->choice) keys += " · " + std::string(kChangeHint);

    keys += " · " + std::string(kContinueHint);
    keys += " · " + std::string(kCancelHint);

    return keys;
}

std::string PluginConfigure::error_of(size_t index) const {
    const Field& field = fields_[index];
    const std::string text = field_text(index);

    // A number that is not a number.
    //
    // The reference names this error under the option that is asked; the
    // error of an option that must be given and is not there stands beside
    // the button that hands the values over.
    if (field.type == "number" && !text.empty() && !parse_number(text)) return "It takes a number.";

    return {};
}

std::string PluginConfigure::first_error() const {
    for (size_t i = 0; i < fields_.size(); ++i) {
        const Field& field = fields_[i];
        const std::string text = field_text(i);

        // A value that must be given and is not there.
        if (field.required && !field.choice && text.empty()) return field.label + " is required";

        const std::string error = error_of(i);
        if (!error.empty()) return error;
    }
    return {};
}

boost::json::object PluginConfigure::build_values() const {
    boost::json::object out;

    for (size_t i = 0; i < fields_.size(); ++i) {
        const Field& field = fields_[i];
        const std::string text = first_line(field_text(i));
        const bool saved = saved_.find(field.key) != saved_.end();

        if (text.empty()) {
            // A secret that is already saved is kept: the field was left
            // empty on purpose.
            if (field.sensitive && saved) continue;
            // A number that was left out is left out of the values.
            if (field.type == "number") continue;
            // An option that is not needed and was never given is left out.
            if (!field.required && !saved) continue;
        }

        if (field.type == "number") {
            const std::optional<double> number = parse_number(text);
            if (!number) {
                out[field.key] = boost::json::value(text);
            } else if (*number == std::floor(*number) && std::abs(*number) < 9.0e18) {
                out[field.key] = boost::json::value(static_cast<int64_t>(*number));
            } else {
                out[field.key] = boost::json::value(*number);
            }
        } else if (field.type == "boolean") {
            const std::optional<bool> yes = cc::parse_bool(text);
            if (yes) out[field.key] = boost::json::value(*yes);
        } else {
            out[field.key] = boost::json::value(text);
        }
    }

    return out;
}

void PluginConfigure::move_to(size_t row) { current_ = std::min(row, submit_row()); }

bool PluginConfigure::submit() {
    // The values are not handed over while one of them is missing.
    if (!first_error().empty()) return false;

    values_ = build_values();
    return true;
}

bool PluginConfigure::handle_key(const Key& key) {
    switch (key.type) {
    case Key::Type::Enter: {
        // The last field of the text hands the values over; every other
        // one moves the pointer on.
        if (current_ == submit_row()) return submit();

        const Field* field = current_field();
        if (field != nullptr && !field->choice && current_ + 1 == fields_.size()) return submit();

        move_to(current_ + 1);
        return false;
    }
    case Key::Type::Tab:
        move_to(current_ + 1);
        return false;
    case Key::Type::Escape:
    case Key::Type::CtrlC:
        cancelled_ = true;
        return true;
    case Key::Type::Up:
        move_to(current_ == 0 ? 0 : current_ - 1);
        return false;
    case Key::Type::Down:
        move_to(current_ + 1);
        return false;
    case Key::Type::Left:
    case Key::Type::Right: {
        const Field* field = current_field();
        if (field == nullptr) return false;

        if (field->choice) {
            // The choice changes to the one beside it.
            if (current_ >= inputs_.size()) return false;
            const std::string text = field_text(current_);
            inputs_[current_].set_text(text == "true" ? "false" : "true");
            return false;
        }

        if (current_ >= inputs_.size()) return false;
        if (key.type == Key::Type::Left) {
            inputs_[current_].move_left();
        } else {
            inputs_[current_].move_right();
        }
        return false;
    }
    case Key::Type::Backspace:
        if (current_ < inputs_.size() && current_field() != nullptr && !current_field()->choice) {
            inputs_[current_].backspace();
        }
        return false;
    case Key::Type::Delete:
        if (current_ < inputs_.size() && current_field() != nullptr && !current_field()->choice) {
            inputs_[current_].delete_forward();
        }
        return false;
    case Key::Type::Home:
        if (current_ < inputs_.size() && current_field() != nullptr && !current_field()->choice) {
            inputs_[current_].move_home();
        }
        return false;
    case Key::Type::End:
        if (current_ < inputs_.size() && current_field() != nullptr && !current_field()->choice) {
            inputs_[current_].move_end();
        }
        return false;
    case Key::Type::CtrlU:
        if (current_ < inputs_.size() && current_field() != nullptr && !current_field()->choice) {
            inputs_[current_].kill_to_start();
        }
        return false;
    case Key::Type::CtrlK:
        if (current_ < inputs_.size() && current_field() != nullptr && !current_field()->choice) {
            inputs_[current_].kill_to_end();
        }
        return false;
    case Key::Type::Character:
    case Key::Type::Paste: {
        const Field* field = current_field();
        if (field == nullptr || field->choice) return false;
        if (current_ >= inputs_.size()) return false;

        // A pasted block is one piece of text; the newline in it never
        // commits the field.
        inputs_[current_].insert(key.text);
        return false;
    }
    default:
        return false;
    }
}

std::optional<boost::json::object> PluginConfigure::run(std::vector<plugins::PluginOption> options,
                                                         boost::json::object values,
                                                         Terminal& terminal) {
    TerminalScreen screen(terminal);
    return run(std::move(options), std::move(values), screen);
}

std::optional<boost::json::object> PluginConfigure::run(std::vector<plugins::PluginOption> options,
                                                         boost::json::object values,
                                                         Screen& screen) {
    set_options(std::move(options), std::move(values));

    while (true) {
        screen.draw(render(screen.width()));

        const auto key = screen.read_key(100);
        if (!key) {
            // The terminal closed: the dialog ends rather than wait for a
            // key that will never come.
            if (screen.closed()) {
                screen.clear();
                return std::nullopt;
            }
            continue;
        }

        if (!handle_key(*key)) continue;

        screen.clear();
        if (cancelled_) return std::nullopt;
        return values_;
    }
}

boost::json::object PluginConfigure::secrets_of(const std::vector<plugins::PluginOption>& options,
                                                 const boost::json::object& values) {
    boost::json::object out;

    for (const plugins::PluginOption& option : options) {
        if (!option.sensitive) continue;

        const auto found = values.find(option.key);
        if (found == values.end()) continue;

        out[option.key] = found->value();
    }

    return out;
}

} // namespace cc::ui
