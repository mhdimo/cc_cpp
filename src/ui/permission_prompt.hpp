#pragma once

#include "ui/ansi.hpp"
#include "ui/prompt_input.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cc::ui {

/// One choice in the permission dialog.
struct PermissionOption {
    /// The text shown for the choice.
    std::string label;
    /// The value the choice carries: "yes", "yes-session",
    /// "yes-dont-ask-again" or "no".
    std::string value;
};

/// What a permission request is about.
///
/// The reference words the choices of a file differently from the ones of a
/// shell command, and only the shell command may be allowed for good.
enum class PermissionKind {
    /// A shell command: the choice may be saved as a rule of the settings.
    Shell,
    /// A file the model wants to read.
    Read,
    /// A file the model wants to write.
    Write,
};

/// The dialog Claude Code shows when a tool needs the user's approval.
///
/// The dialog is a cap: a rounded rule across the top, the title of the
/// request, what is being approved, the question and the list of choices.
/// The selected choice is marked with `❯`; the others are numbered.
class PermissionPrompt {
public:
    PermissionPrompt(Theme theme, std::string title, std::string question,
                     std::vector<PermissionOption> options, std::string body = {},
                     std::string description = {}, PermissionKind kind = PermissionKind::Shell);

    /// Renders the dialog.
    std::vector<std::string> render(int width) const;

    /// Runs the dialog until the user chooses.
    ///
    /// Returns the value of the chosen option, or an empty string when the
    /// user pressed Escape to reject. The instructions the user typed while
    /// amending the choice are left in `feedback`.
    std::string run(Terminal& terminal, std::string* feedback = nullptr);

    /// The message the terminal is given when the user has been away.
    ///
    /// The reference tells the terminal that the session wants the user
    /// when nobody has touched the keyboard for a while.
    void set_notification(const std::string& message) { notification_ = message; }

    /// A function that gives the answer of the IDE, when there is one.
    ///
    /// The reference waits for the answer of the IDE: the change may be
    /// saved there, which accepts it, or it may be rejected there, which
    /// refuses it. The function gives the value of the choice that was made
    /// in the IDE, and an empty string while the IDE has not answered.
    void set_answer_provider(std::function<std::string()> provider) {
        answer_provider_ = std::move(provider);
    }

    /// The index of the highlighted choice.
    size_t selected() const { return selected_; }

private:
    Theme theme_;
    std::string title_;
    std::string question_;

    /// What is being approved, the way the reference shows it.
    std::string body_;

    /// What the tool does, in the words of the model.
    std::string description_;

    /// What the request is about: the body of a file is drawn in another
    /// shape than the body of a shell command.
    PermissionKind kind_;

    std::vector<PermissionOption> options_;
    size_t selected_ = 0;

    /// True when the user is typing the instructions that go with the choice.
    bool amending_ = false;

    /// The message the terminal is given when the user has been away; the
    /// string is empty when the terminal is not to be told.
    std::string notification_;

    /// The moment the user last touched the keyboard.
    int64_t last_interaction_ms_ = 0;

    /// True when the terminal has already been told.
    bool notified_ = false;

    /// The function that gives the answer of the IDE.
    std::function<std::string()> answer_provider_;

    /// The instructions the user typed.
    PromptInput amend_;
};

/// The choices Claude Code offers for a tool.
///
/// The reference offers a file the permission of the session alone, and a
/// shell command a rule that is written to the settings.
std::vector<PermissionOption> permission_options(const std::string& user_facing_name,
                                                  PermissionKind kind, const std::string& path,
                                                  const std::string& scope, const std::string& cwd,
                                                  bool always_allow, const Theme& theme = Theme::dark());

/// The rule a session-scoped choice adds.
///
/// The reference keeps the rule of a file in the memory of the session: it
/// is never written to the settings.
std::string permission_session_rule(PermissionKind kind, const std::string& path,
                                    const std::string& cwd);

} // namespace cc::ui
