#pragma once

#include "ui/ansi.hpp"
#include "ui/terminal.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// The questions the session asks when it is set up for the first time.
///
/// The reference asks the questions of a user who has not been here
/// before: the theme of the session, the key of the API that was found in
/// the environment, the notes of the safety and the settings of the
/// terminal.
///
/// The welcome stands above the questions: it is written to the transcript
/// and stays there while they are asked.
class Onboarding {
public:
    /// The answers of the user.
    struct Answers {
        /// The theme the user chose.
        ///
        /// The string is empty when the theme was not changed.
        std::string theme;

        /// True when the user approved the key of the API.
        bool api_key_approved = false;

        /// True when the user asked for the settings of the terminal.
        bool terminal_setup = false;
    };

    /// The questions of the setup.
    ///
    /// The theme that is set to begin with is the one the pointer of the
    /// picker starts on; the version of the session is written into the
    /// welcome. The name of the theme tells whether the welcome is the
    /// picture of the dark theme or the picture of the light ones.
    Onboarding(Theme theme, ThemeName name, std::string current_theme, std::string version);

    /// Asks the questions.
    Answers run(Terminal& terminal);

private:
    /// The key of the API that waits for the approval of the user.
    ///
    /// The string is empty when there is none, or when the user has
    /// answered for it already.
    static std::string api_key_to_approve();

    /// Asks whether the key of the API is to be used.
    ///
    /// Returns true when the user approved it.
    bool ask_for_api_key(Terminal& terminal, const std::string& key);

    /// Shows the notes of the safety and waits for the Enter.
    void show_security_notes(Terminal& terminal);

    /// Asks for the settings of the terminal.
    ///
    /// Returns true when the user asked for them.
    bool ask_for_terminal_setup(Terminal& terminal);

    /// The lines of the welcome of the session.
    std::vector<std::string> welcome() const;

    Theme theme_;

    /// The name of the theme of the session.
    ThemeName name_ = ThemeName::Dark;

    /// The theme that is set to begin with.
    std::string current_theme_;

    /// The version of the session.
    std::string version_;
};

} // namespace cc::ui
