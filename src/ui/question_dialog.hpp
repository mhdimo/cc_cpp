#pragma once

#include "tools/tool_use_context.hpp"
#include "ui/ansi.hpp"
#include "ui/prompt_input.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// The dialog Claude Code shows when the model asks the user questions.
///
/// The dialog is a cap: a rule across the top, the bar of the questions,
/// the question that is asked, the numbered choices with the highlighted
/// one marked, and the way out of the dialog.
///
/// Every question of the call is in the bar at the top, the way the
/// reference shows them: the arrow keys move from one question to the
/// next, and the last tab of the bar is the one that reviews the answers
/// before they are handed over.
class QuestionDialog {
public:
    QuestionDialog(Theme theme, UserQuestion question);

    /// Sets the questions of the call.
    ///
    /// The reference shows every question of the call in the bar, with the
    /// one that is asked standing out.
    void set_questions(std::vector<UserQuestion> questions, size_t current);

    /// Renders the dialog.
    std::vector<std::string> render(int width) const;

    /// Runs the dialog until the user has answered.
    ///
    /// Returns the answers the user gave, keyed by the text of the question.
    /// An empty map means that the user gave the dialog up.
    UserAnswers run(Terminal& terminal);

    /// The index of the highlighted choice of the question that is asked.
    size_t selected() const;

private:
    /// What the user did with one question of the call.
    struct State {
        /// The index of the highlighted choice.
        size_t selected = 0;
        /// The choices the user picked, when the question may have more than
        /// one answer.
        std::vector<size_t> checked;
        /// The answer the user typed in.
        std::string other;
        /// True when the user is typing the answer of their own.
        bool other_active = false;
        /// The answer of the question; empty when it is not answered.
        std::string answer;
    };

    /// The number of the choices the model wrote.
    size_t option_count(size_t index) const { return questions_[index].options.size(); }

    /// The number of the entry that lets the user answer in their own words.
    size_t other_index(size_t index) const { return option_count(index); }

    /// The number of the entry that hands the answers of the question over,
    /// when the question may have more than one answer.
    size_t submit_index(size_t index) const { return option_count(index) + 1; }

    /// The number of the entry that leaves the dialog.
    size_t footer_index(size_t index) const {
        return option_count(index) + (questions_[index].multi_select ? 2 : 1);
    }

    /// The bar of the questions, the way the reference draws it.
    std::string navigation_bar() const;

    /// The view of the question that is asked.
    std::vector<std::string> render_question(int width) const;

    /// The view that reviews the answers before they are handed over.
    std::vector<std::string> render_review(int width) const;

    /// True when the question has an answer.
    bool answered(size_t index) const { return !states_[index].answer.empty(); }

    /// True when the bar of the questions hides the tab of the review.
    ///
    /// The reference hides it when the call holds a single question that
    /// may have one answer only: the answer goes straight back to the
    /// model, and there is nothing to review.
    bool submit_hidden() const {
        return questions_.size() == 1 && !questions_.front().multi_select;
    }

    /// The last tab of the bar.
    size_t max_tab() const { return submit_hidden() ? questions_.size() - 1 : questions_.size(); }

    /// True when the tab of the review is the one that is asked.
    bool in_review() const { return !submit_hidden() && current_ >= questions_.size(); }

    /// The answer of the question, made of the choices that were picked.
    std::string answer_of(size_t index) const;

    /// The answers the user gave, keyed by the text of the question.
    UserAnswers answers() const;

    /// Records the answer of the question that is asked.
    void remember_answer(size_t index);

    /// Picks the choice of the question that is asked, or lets it go when
    /// it is already picked.
    void toggle_choice(size_t choice);

    /// Moves to another tab of the bar.
    void go_to(size_t tab);

    Theme theme_;

    /// The questions of the call.
    std::vector<UserQuestion> questions_;

    /// What the user did with each of the questions.
    std::vector<State> states_;

    /// The question that is asked; the count of the questions is the tab of
    /// the review of the answers.
    size_t current_ = 0;

    /// The field the answer of the user is typed in.
    PromptInput other_;

    /// True when the pointer is on the way out of the dialog.
    bool footer_focused_ = false;

    /// The choice of the review that is highlighted.
    size_t review_selected_ = 0;
};

/// The dialog Claude Code opens when it meets a server of the project.
///
/// The reference opens a dialog of the design system: the title names the
/// server, the copy warns of what the servers may do, and the three
/// choices are worded the way the reference words them.
class McpServerDialog {
public:
    McpServerDialog(Theme theme, std::string server_name);

    /// Renders the dialog.
    std::vector<std::string> render(int width) const;

    /// Runs the dialog until the user chooses.
    ///
    /// Returns "yes-all", "yes" or "no".
    std::string run(Terminal& terminal);

private:
    Theme theme_;

    /// The name of the server the dialog is about.
    std::string server_name_;

    /// The index of the highlighted choice.
    size_t selected_ = 0;
};

} // namespace cc::ui
