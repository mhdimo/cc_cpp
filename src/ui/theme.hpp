#pragma once

#include "ui/ansi.hpp"

#include <string_view>

namespace cc::ui {

/// The colour palette. Mirrors Claude Code's theme so the port looks the
/// same as the original in both its dark and light variants.
struct Theme {
    Rgb auto_accept;
    Rgb bash_border;
    Rgb claude;
    Rgb claude_shimmer;
    Rgb claude_blue_for_system_spinner;
    Rgb claude_blue_shimmer_for_system_spinner;
    Rgb permission;
    Rgb permission_shimmer;
    Rgb plan_mode;
    Rgb ide;
    Rgb prompt_border;
    Rgb prompt_border_shimmer;
    Rgb text;
    Rgb inverse_text;
    Rgb inactive;
    Rgb inactive_shimmer;
    Rgb subtle;
    Rgb suggestion;
    Rgb remember;
    Rgb background;
    Rgb success;
    Rgb error;
    Rgb warning;
    Rgb merged;
    Rgb warning_shimmer;
    Rgb diff_added;
    Rgb diff_removed;
    Rgb diff_added_dimmed;
    Rgb diff_removed_dimmed;
    Rgb diff_added_word;
    Rgb diff_removed_word;
    Rgb red_for_subagents_only;
    Rgb blue_for_subagents_only;
    Rgb green_for_subagents_only;
    Rgb yellow_for_subagents_only;
    Rgb purple_for_subagents_only;
    Rgb orange_for_subagents_only;
    Rgb pink_for_subagents_only;
    Rgb cyan_for_subagents_only;
    Rgb professional_blue;
    Rgb chrome_yellow;
    Rgb user_message_background;
    Rgb selection_bg;
    Rgb bash_message_background;
    Rgb memory_background;
    Rgb rate_limit_fill;
    Rgb rate_limit_empty;
    Rgb fast_mode;
    Rgb brief_label_you;
    Rgb brief_label_claude;
    /// The mascot's body colour.
    Rgb clawd_body;
    /// The colour behind the mascot's eyes and body.
    Rgb clawd_background;

    /// The dark palette, used when the terminal reports a dark background.
    static Theme dark();

    /// The light palette.
    static Theme light();

    /// A palette that only uses the 16 ANSI colours, for terminals without
    /// true-colour support.
    static Theme ansi_dark();

    /// The 16-colour light palette.
    static Theme ansi_light();
};

/// The theme names Claude Code accepts in its settings.
enum class ThemeName { Dark, Light, LightDaltonized, DarkDaltonized, LightAnsi, DarkAnsi };

/// Parses a theme name, returning false when `name` is not recognised.
bool parse_theme_name(std::string_view name, ThemeName& out);

/// The canonical spelling of a theme name.
std::string_view to_string(ThemeName name);

/// Picks a palette for `name`.
Theme theme_for(ThemeName name);

} // namespace cc::ui
