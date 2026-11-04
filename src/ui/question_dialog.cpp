#include "ui/question_dialog.hpp"

#include "ui/figures.hpp"
#include "utils/string.hpp"

#include <algorithm>

namespace cc::ui {
namespace {

/// The number of columns the choice index takes up.
size_t index_width(size_t count) {
    return std::to_string(count).size();
}

/// The tick in the square of a choice that is picked.
constexpr std::string_view kChecked = "✔";

/// What the reference tells the user about the servers of a project.
constexpr std::string_view kMcpCopy =
    "MCP servers may execute code or access system resources. All tool calls require approval. "
    "Learn more in the ";

/// Where the documentation of the servers lives.
constexpr std::string_view kMcpDocumentationUrl = "https://code.claude.com/docs/en/mcp";

/// The name of the documentation of the servers.
constexpr std::string_view kMcpDocumentation = "MCP documentation";

/// The title of the view that reviews the answers.
constexpr std::string_view kReviewTitle = "Review your answers";

/// The warning of the view that reviews the answers.
constexpr std::string_view kReviewWarning = "You have not answered all questions";

/// The line the review asks before the answers are handed over.
constexpr std::string_view kReviewQuestion = "Ready to submit your answers?";

} // namespace

QuestionDialog::QuestionDialog(Theme theme, UserQuestion question)
    : theme_(std::move(theme)),
      questions_{},
      states_{},
      other_(theme_, "") {
    questions_.push_back(std::move(question));
    states_.emplace_back();
}

void QuestionDialog::set_questions(std::vector<UserQuestion> questions, size_t current) {
    questions_ = std::move(questions);
    states_.assign(questions_.size(), State{});
    current_ = std::min(current, questions_.size());
}

size_t QuestionDialog::selected() const {
    if (in_review() || states_.empty()) return 0;
    return states_[current_].selected;
}

std::string QuestionDialog::navigation_bar() const {
    // The arrows are dim when there is nothing on that side.
    Style inactive;
    inactive.foreground = theme_.inactive;

    Style plain;
    plain.foreground = theme_.text;

    // The chip of the question that is asked stands out: it is written on
    // the colour of the permission, in the colour that stands out on it.
    Style current;
    current.background = theme_.permission;
    current.foreground = theme_.inverse_text;

    std::string bar;

    // The arrows are only drawn when there is more than one tab: the
    // reference hides them when the call holds a single question.
    if (!submit_hidden()) {
        bar += styled(std::string(figures::kArrowLeft) + " ", current_ == 0 ? inactive : plain);
    }

    for (size_t i = 0; i < questions_.size(); ++i) {
        const UserQuestion& question = questions_[i];
        const std::string text =
            question.header.empty() ? "Q" + std::to_string(i + 1) : question.header;
        // The square of the chip says whether the question has an answer.
        const std::string box =
            answered(i) ? std::string(figures::kCheckboxOn) : std::string(figures::kCheckboxOff);
        const std::string chip = " " + box + " " + text + " ";
        bar += styled(chip, i == current_ ? current : plain);
    }

    // The chip that hands the answers over.
    const bool on_review = in_review();
    if (!submit_hidden()) {
        const std::string submit = " " + std::string(figures::kTick) + " Submit ";
        bar += styled(submit, on_review ? current : plain);
        bar += styled(" " + std::string(figures::kArrowRight), on_review ? inactive : plain);
    }

    return bar;
}

UserAnswers QuestionDialog::answers() const {
    UserAnswers answers;
    for (size_t i = 0; i < questions_.size(); ++i) {
        if (!answered(i)) continue;
        answers[questions_[i].text] = states_[i].answer;
    }
    return answers;
}

std::string QuestionDialog::answer_of(size_t index) const {
    const UserQuestion& question = questions_[index];
    const size_t count = option_count(index);

    std::string answer;
    for (size_t choice : states_[index].checked) {
        if (!answer.empty()) answer += ", ";
        answer += choice < count ? question.options[choice].label : "";
    }
    if (!states_[index].other.empty()) {
        if (!answer.empty()) answer += ", ";
        answer += states_[index].other;
    }
    return answer;
}

void QuestionDialog::remember_answer(size_t index) {
    states_[index].answer = answer_of(index);
}

void QuestionDialog::toggle_choice(size_t choice) {
    std::vector<size_t>& checked = states_[current_].checked;
    const auto it = std::find(checked.begin(), checked.end(), choice);
    if (it == checked.end()) {
        checked.push_back(choice);
    } else {
        checked.erase(it);
    }
}

void QuestionDialog::go_to(size_t tab) {
    // The text of the field belongs to the question that is left.
    if (current_ < states_.size()) states_[current_].other = other_.text();

    current_ = std::min(tab, max_tab());
    footer_focused_ = false;

    // The field of the question that is asked shows the answer of the user
    // that was written there.
    if (current_ < states_.size()) {
        other_.set_text(states_[current_].other);
    } else {
        other_.clear();
    }
}

std::vector<std::string> QuestionDialog::render_question(int width) const {
    // The reference draws the rule in the dim colour.
    Style rule;
    rule.foreground = theme_.inactive;

    // The numbers are dim the way `<Text dimColor>` is: the ordinary
    // colour of the text, made faint with the attribute of dimness.
    Style dim;
    dim.dim = true;

    // The hint of the reference is dim as well.
    Style hint_style;
    hint_style.foreground = theme_.inactive;
    hint_style.dim = true;

    Style question_style;
    question_style.bold = true;
    question_style.foreground = theme_.text;

    Style focused;
    focused.foreground = theme_.suggestion;

    Style normal;
    normal.foreground = theme_.text;

    const UserQuestion& question = questions_[current_];
    const State& state = states_[current_];

    std::vector<std::string> lines;

    // The question, in bold. The reference draws the whole view without an
    // indent of its own.
    for (const auto& line : cc::word_wrap(question.text, static_cast<size_t>(std::max(width, 1)))) {
        lines.push_back(styled(line, question_style));
    }
    lines.push_back("");

    // The choices, numbered, with the highlighted one marked. The number of
    // the "Other" choice counts, the way it counts in the reference.
    const size_t count = option_count(current_);
    const size_t width_of_index = index_width(count + 1);

    for (size_t i = 0; i < count; ++i) {
        const bool is_selected = i == state.selected;
        const std::string pointer = is_selected ? std::string(figures::kPointer) + " " : "  ";
        std::string number = std::to_string(i + 1) + ".";
        number.resize(width_of_index + 1, ' ');

        const UserQuestionOption& option = question.options[i];
        Style label_style = is_selected ? focused : normal;

        std::string line = pointer + styled(number, dim);
        if (question.multi_select) {
            const bool checked =
                std::find(state.checked.begin(), state.checked.end(), i) != state.checked.end();
            line += styled(std::string("[") + (checked ? std::string(kChecked) : " ") + "]",
                          checked ? focused : normal);
        }
        line += styled(option.label, label_style);
        lines.push_back(std::move(line));

        if (!option.description.empty()) {
            const std::string indent(width_of_index + 4, ' ');
            for (const auto& wrapped :
                 cc::word_wrap(option.description,
                               static_cast<size_t>(std::max(width, 1)) - indent.size())) {
                lines.push_back(indent + styled(wrapped, dim));
            }
        }
    }

    // The choice that lets the user answer in their own words.
    {
        const bool is_selected = state.selected == other_index(current_);
        const std::string pointer = is_selected ? std::string(figures::kPointer) + " " : "  ";
        std::string number = std::to_string(count + 1) + ".";
        number.resize(width_of_index + 1, ' ');

        std::string line = pointer + styled(number, dim);
        if (question.multi_select) {
            const bool checked =
                std::find(state.checked.begin(), state.checked.end(), count) != state.checked.end();
            line += styled(std::string("[") + (checked ? std::string(kChecked) : " ") + "]",
                          checked ? focused : normal);
        }
        line += styled("Other", is_selected ? focused : normal);
        lines.push_back(std::move(line));

        // The field the answer is typed in.
        if (state.other_active || question.multi_select) {
            const std::string indent(width_of_index + 4, ' ');
            const std::string text = other_.text();
            const std::string placeholder =
                question.multi_select ? "Type something" : "Type something.";
            lines.push_back(indent + styled(text.empty() ? placeholder : text,
                                           text.empty() ? dim : normal) +
                              styled("█", Style{}));
        }
    }

    // The button that hands the answers of the question over, when the
    // question may have more than one answer.
    if (question.multi_select) {
        const bool is_selected = state.selected == submit_index(current_);
        const std::string pointer = is_selected ? std::string(figures::kPointer) : " ";
        Style submit_style;
        submit_style.bold = true;
        if (is_selected) submit_style.foreground = theme_.suggestion;
        lines.push_back(pointer + "   " + styled("Submit", submit_style));
    }

    // The preview of the choice that is highlighted, when the model wrote
    // one: the reference puts it beside the list.
    if (state.selected < count && !question.options[state.selected].preview.empty()) {
        lines.push_back("");
        for (const auto& line :
             cc::word_wrap(question.options[state.selected].preview,
                           static_cast<size_t>(std::max(width, 1)))) {
            lines.push_back(styled(line, dim));
        }
    }

    // The rule that separates the list from the way out, and the way out
    // itself.
    const size_t rule_width = static_cast<size_t>(std::max(width, 1));
    std::string rule_text;
    rule_text.reserve(rule_width * 3);
    for (size_t i = 0; i < rule_width; ++i) rule_text += figures::kHorizontalRule;
    lines.push_back(styled(rule_text, rule));
    {
        const std::string pointer = footer_focused_ ? std::string(figures::kPointer) : " ";
        Style footer_style;
        if (footer_focused_) footer_style.foreground = theme_.suggestion;
        lines.push_back(pointer + " " +
                        styled(std::to_string(count + 2) + ". Chat about this", footer_style));
    }

    lines.push_back("");
    lines.push_back(styled("Enter to select · ↑/↓ to navigate · Esc to cancel", hint_style));

    return lines;
}

std::vector<std::string> QuestionDialog::render_review(int width) const {
    Style dim;
    dim.dim = true;

    Style hint_style;
    hint_style.foreground = theme_.inactive;
    hint_style.dim = true;

    Style title_style;
    title_style.bold = true;
    title_style.foreground = theme_.text;

    Style warning_style;
    warning_style.foreground = theme_.warning;

    Style success_style;
    success_style.foreground = theme_.success;

    Style normal;
    normal.foreground = theme_.text;

    Style focused;
    focused.foreground = theme_.suggestion;

    const size_t w = static_cast<size_t>(std::max(width, 1));

    std::vector<std::string> lines;

    lines.push_back(styled(std::string(kReviewTitle), title_style));
    lines.push_back("");

    // The warning of the reference, when a question has no answer.
    const bool all_answered = std::all_of(states_.begin(), states_.end(),
                                          [](const State& state) { return !state.answer.empty(); });
    if (!all_answered) {
        lines.push_back(styled(std::string(figures::kWarning) + " " + std::string(kReviewWarning),
                               warning_style));
        lines.push_back("");
    }

    // The answers the user gave, one under the question they belong to.
    for (size_t i = 0; i < questions_.size(); ++i) {
        if (!answered(i)) continue;
        for (const auto& line : cc::word_wrap(questions_[i].text, w > 2 ? w - 2 : 1)) {
            lines.push_back(std::string(figures::kBullet) + " " + styled(line, normal));
        }
        for (const auto& line : cc::word_wrap(states_[i].answer, w > 4 ? w - 4 : 1)) {
            lines.push_back("  " + styled(std::string(figures::kArrowRight) + " " + line, success_style));
        }
    }

    lines.push_back("");
    lines.push_back(styled(std::string(kReviewQuestion), dim));
    lines.push_back("");

    // The two choices of the review.
    const std::vector<std::string> choices = {"Submit answers", "Cancel"};
    for (size_t i = 0; i < choices.size(); ++i) {
        const bool is_selected = i == review_selected_;
        const std::string pointer = is_selected ? std::string(figures::kPointer) + " " : "  ";
        std::string number = std::to_string(i + 1) + ".";
        number.resize(2, ' ');
        lines.push_back(pointer + styled(number, dim) + " " +
                        styled(choices[i], is_selected ? focused : normal));
    }

    lines.push_back("");
    lines.push_back(styled("Enter to select · ↑/↓ to navigate · Esc to cancel", hint_style));

    return lines;
}

std::vector<std::string> QuestionDialog::render(int width) const {
    // The reference draws the rule in the dim colour.
    Style rule;
    rule.foreground = theme_.inactive;

    std::vector<std::string> lines;

    // The cap: a rule across the top, without corners.
    const size_t rule_width = static_cast<size_t>(std::max(width, 1));
    std::string rule_text;
    rule_text.reserve(rule_width * 3);
    for (size_t i = 0; i < rule_width; ++i) rule_text += figures::kHorizontalRule;
    lines.push_back(styled(rule_text, rule));

    // The bar of the questions, and the empty line below it.
    lines.push_back(navigation_bar());
    lines.push_back("");

    if (in_review()) {
        for (auto& line : render_review(width)) lines.push_back(std::move(line));
    } else {
        for (auto& line : render_question(width)) lines.push_back(std::move(line));
    }

    return lines;
}

UserAnswers QuestionDialog::run(Terminal& terminal) {
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

        // The review of the answers has its own keys.
        if (in_review()) {
            switch (key->type) {
            case Key::Type::Up:
                review_selected_ = review_selected_ == 0 ? 1 : review_selected_ - 1;
                break;
            case Key::Type::Down:
                review_selected_ = review_selected_ + 1 < 2 ? review_selected_ + 1 : 0;
                break;
            case Key::Type::Left:
                // The arrow keys move from one tab of the bar to the next.
                if (current_ > 0) go_to(current_ - 1);
                break;
            case Key::Type::Right:
                // The review is the last tab of the bar.
                break;
            case Key::Type::Enter: {
                terminal.clear_dynamic();
                if (review_selected_ != 0) return {};

                // The answers go back to the model, the way the reference
                // hands them over.
                return answers();
            }
            case Key::Type::Escape:
            case Key::Type::CtrlC:
                terminal.clear_dynamic();
                return {};
            default:
                break;
            }
            continue;
        }

        // While the user types the answer, the keys go to the field.
        if (states_[current_].other_active) {
            switch (key->type) {
            case Key::Type::Enter:
                if (questions_[current_].multi_select) {
                    // The field keeps its text; the answers are handed
                    // over with the button below the list.
                    states_[current_].other_active = false;
                    break;
                }
                // The answer of the user's own is the answer of the
                // question, and the dialog moves on to the next question.
                states_[current_].other = other_.text();
                remember_answer(current_);
                // A call that holds a single question goes straight back to
                // the model.
                if (submit_hidden()) {
                    terminal.clear_dynamic();
                    return answers();
                }
                go_to(current_ + 1);
                break;
            case Key::Type::Escape:
                // The reference leaves the dialog when the field is left.
                terminal.clear_dynamic();
                return {};
            case Key::Type::Backspace:
                other_.backspace();
                break;
            case Key::Type::Delete:
                other_.delete_forward();
                break;
            case Key::Type::Left:
                other_.move_left();
                break;
            case Key::Type::Right:
                other_.move_right();
                break;
            case Key::Type::Home:
                other_.move_home();
                break;
            case Key::Type::End:
                other_.move_end();
                break;
            case Key::Type::Character:
                other_.insert(key->text);
                break;
            default:
                break;
            }
            continue;
        }

        const size_t last = footer_index(current_);

        switch (key->type) {
        case Key::Type::Left:
            // The arrow keys move from one tab of the bar to the next.
            if (current_ > 0) go_to(current_ - 1);
            break;
        case Key::Type::Right:
            // The last tab of the bar is the review of the answers.
            go_to(current_ + 1);
            break;
        case Key::Type::Up:
            // The reference wraps around at the top of the list.
            if (footer_focused_) {
                footer_focused_ = false;
                states_[current_].selected =
                    questions_[current_].multi_select ? submit_index(current_) : other_index(current_);
            } else if (states_[current_].selected == 0) {
                states_[current_].selected =
                    questions_[current_].multi_select ? submit_index(current_) : other_index(current_);
            } else {
                --states_[current_].selected;
            }
            break;
        case Key::Type::Down:
            // The way out of the dialog is below the last choice; the
            // reference does not wrap there.
            if (footer_focused_) break;
            if (states_[current_].selected + 1 <= other_index(current_)) {
                ++states_[current_].selected;
            } else if (states_[current_].selected == other_index(current_)) {
                if (questions_[current_].multi_select) {
                    ++states_[current_].selected;
                } else {
                    states_[current_].selected = last;
                }
            } else if (states_[current_].selected < last) {
                ++states_[current_].selected;
            }
            if (states_[current_].selected == last) footer_focused_ = true;
            break;
        case Key::Type::Enter: {
            if (footer_focused_) {
                // The way out of the dialog: the question is not answered.
                terminal.clear_dynamic();
                return {};
            }
            if (states_[current_].selected == other_index(current_)) {
                // The field of the answer of the user's own.
                states_[current_].other_active = true;
                break;
            }
            if (questions_[current_].multi_select) {
                if (states_[current_].selected == submit_index(current_)) {
                    // The answers of the question are handed over, and the
                    // dialog moves on to the next question.
                    remember_answer(current_);
                    // A call that holds a single question goes straight
                    // back to the model.
                    if (submit_hidden()) {
                        terminal.clear_dynamic();
                        return answers();
                    }
                    go_to(current_ + 1);
                    break;
                }
                // The choice of a question that may have more than one
                // answer is picked with the space bar.
                toggle_choice(states_[current_].selected);
                break;
            }
            // The answer of the question is the choice that is highlighted,
            // and the dialog moves on to the next question.
            {
                const size_t choice = states_[current_].selected;
                states_[current_].checked.assign(1, choice);
                remember_answer(current_);
                // A call that holds a single question goes straight back to
                // the model.
                if (submit_hidden()) {
                    terminal.clear_dynamic();
                    return answers();
                }
                go_to(current_ + 1);
            }
            break;
        }
        case Key::Type::Character: {
            // The space bar picks a choice of a question that may have more
            // than one answer.
            if (key->text == " ") {
                if (questions_[current_].multi_select &&
                    states_[current_].selected <= other_index(current_)) {
                    toggle_choice(states_[current_].selected);
                }
                break;
            }
            // A digit picks the choice with that number.
            if (key->text.size() != 1 || key->text[0] < '1' || key->text[0] > '9') break;
            const size_t index = static_cast<size_t>(key->text[0] - '1');
            if (index >= option_count(current_)) break;
            states_[current_].selected = index;
            states_[current_].checked.assign(1, index);
            remember_answer(current_);
            // A call that holds a single question goes straight back to the
            // model.
            if (submit_hidden()) {
                terminal.clear_dynamic();
                return answers();
            }
            go_to(current_ + 1);
            break;
        }
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            terminal.clear_dynamic();
            return {};
        default:
            break;
        }
    }
}

McpServerDialog::McpServerDialog(Theme theme, std::string server_name)
    : theme_(std::move(theme)), server_name_(std::move(server_name)) {}

std::vector<std::string> McpServerDialog::render(int width) const {
    const size_t w = width > 0 ? static_cast<size_t>(width) : 1;

    Style rule;
    rule.foreground = theme_.warning;

    Style title_style;
    title_style.bold = true;
    title_style.foreground = theme_.warning;

    Style normal;
    normal.foreground = theme_.text;

    // The numbers are dim the way `<Text dimColor>` is.
    Style dim;
    dim.dim = true;

    Style focused;
    focused.foreground = theme_.suggestion;

    Style hint_style;
    hint_style.dim = true;
    hint_style.italic = true;

    std::vector<std::string> lines;

    // The frame of the reference leaves an empty line above its rule, and
    // the rule is painted with the colour of the warning.
    lines.push_back("");
    std::string rule_text;
    rule_text.reserve(w * 3);
    for (size_t i = 0; i < w; ++i) rule_text += figures::kHorizontalRule;
    lines.push_back(styled(rule_text, rule));

    // The content of the pane stands two columns in.
    const std::string indent = "  ";

    lines.push_back(indent + styled("New MCP server found in .mcp.json: " + server_name_,
                                    title_style));
    lines.push_back("");

    // The copy of the dialog. The name of the documentation is a link.
    const std::string documentation =
        supports_hyperlinks() ? hyperlink(kMcpDocumentation, kMcpDocumentationUrl)
                              : std::string(kMcpDocumentation);
    for (const auto& line :
         cc::word_wrap(std::string(kMcpCopy) + documentation + ".", w > 2 ? w - 2 : 1)) {
        lines.push_back(indent + styled(line, normal));
    }

    // The choices.
    const std::vector<std::string> choices = {"Use this and all future MCP servers in this project",
                                               "Use this MCP server",
                                               "Continue without using this MCP server"};
    for (size_t i = 0; i < choices.size(); ++i) {
        const bool is_selected = i == selected_;
        const std::string marker = is_selected ? std::string(figures::kPointer) + " " : "  ";
        const std::string number = std::to_string(i + 1) + ".";
        lines.push_back(indent + marker + styled(number, dim) + " " +
                        styled(choices[i], is_selected ? focused : normal));
    }

    lines.push_back("");
    lines.push_back(indent + styled("Enter to confirm · Esc to cancel", hint_style));

    return lines;
}

std::string McpServerDialog::run(Terminal& terminal) {
    const int width = terminal.width();

    while (true) {
        terminal.render_dynamic(render(width));

        const auto key = terminal.read_key(100);
        if (!key) {
            // The terminal closed: the view ends rather than wait for a
            // key that will never come.
            if (terminal.closed()) {
                terminal.clear_dynamic();
                return "no";
            }
            continue;
        }

        switch (key->type) {
        case Key::Type::Up:
            // The reference wraps around at the ends of the list.
            selected_ = selected_ == 0 ? 2 : selected_ - 1;
            break;
        case Key::Type::Down:
            selected_ = selected_ + 1 < 3 ? selected_ + 1 : 0;
            break;
        case Key::Type::Enter: {
            static const char* const kValues[] = {"yes-all", "yes", "no"};
            terminal.clear_dynamic();
            return kValues[selected_];
        }
        case Key::Type::Character: {
            if (key->text.size() != 1 || key->text[0] < '1' || key->text[0] > '3') break;
            selected_ = static_cast<size_t>(key->text[0] - '1');
            terminal.clear_dynamic();
            static const char* const kValues[] = {"yes-all", "yes", "no"};
            return kValues[selected_];
        }
        case Key::Type::Escape:
        case Key::Type::CtrlC:
            // The reference refuses the server when the dialog is left.
            terminal.clear_dynamic();
            return "no";
        default:
            break;
        }
    }
}

} // namespace cc::ui
