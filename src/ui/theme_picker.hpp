#pragma once

#include "ui/select.hpp"
#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

class Terminal;

/// How the picker of the theme is drawn.
struct ThemePickerOptions {
    /// True when the picker says "Let's get started." instead of the name
    /// of the picker.
    ///
    /// The reference asks the question this way when the picker is part of
    /// the setup of the session.
    bool intro = false;

    /// The line under the question.
    ///
    /// The reference writes the way to change the theme later under the
    /// question of the setup of the session.
    std::string help_text;

    /// True when the hint under the picker is left out.
    ///
    /// The reference leaves the hint out when the picker is part of the
    /// setup of the session: the escape does not end the setup.
    bool hide_hint = false;
};

/// The picker of the theme.
///
/// The reference shows the themes in a list and, under it, the preview of
/// the one the pointer is on, drawn with the colours of that theme, so that
/// the user sees what the theme looks like before it is chosen. The pointer
/// changes the preview; Enter chooses the theme.
class ThemePicker {
public:
    /// The picker, with the theme that is set to begin with.
    ///
    /// The name of the theme is the one the reference writes in the
    /// settings: "dark", "light", "dark-daltonized" and so on.
    ThemePicker(Theme theme, std::string selected, ThemePickerOptions options = {});

    /// The lines of the picker.
    std::vector<std::string> render(int width) const;

    /// Runs the picker until the user picks a theme.
    ///
    /// The name of the theme is returned; the string is empty when the
    /// picker was dismissed.
    std::string run(Terminal& terminal);

private:
    /// The theme the picker is drawn with.
    Theme theme_;

    /// The list of the themes.
    Select select_;

    /// The theme that is previewed, the one the pointer is on.
    Theme preview_;

    /// How the picker is drawn.
    ThemePickerOptions options_;
};

} // namespace cc::ui
