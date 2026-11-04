#include "ui/theme.hpp"

namespace cc::ui {
namespace {

constexpr Rgb kDarkAutoAccept{175, 135, 255};
constexpr Rgb kDarkBashBorder{253, 93, 177};
constexpr Rgb kDarkClaude{215, 119, 87};
constexpr Rgb kDarkClaudeShimmer{235, 159, 127};
constexpr Rgb kDarkClaudeBlue{147, 165, 255};
constexpr Rgb kDarkClaudeBlueShimmer{177, 195, 255};
constexpr Rgb kDarkPermission{177, 185, 249};
constexpr Rgb kDarkPermissionShimmer{207, 215, 255};
constexpr Rgb kDarkPlanMode{72, 150, 140};
constexpr Rgb kDarkIde{71, 130, 200};
constexpr Rgb kDarkPromptBorder{136, 136, 136};
constexpr Rgb kDarkPromptBorderShimmer{166, 166, 166};
constexpr Rgb kDarkText{255, 255, 255};
constexpr Rgb kDarkInverseText{0, 0, 0};
constexpr Rgb kDarkInactive{153, 153, 153};
constexpr Rgb kDarkInactiveShimmer{193, 193, 193};
constexpr Rgb kDarkSubtle{80, 80, 80};
constexpr Rgb kDarkSuggestion{177, 185, 249};
constexpr Rgb kDarkRemember{177, 185, 249};
constexpr Rgb kDarkBackground{0, 204, 204};
constexpr Rgb kDarkSuccess{78, 186, 101};
constexpr Rgb kDarkError{255, 107, 128};
constexpr Rgb kDarkWarning{255, 193, 7};
constexpr Rgb kDarkMerged{175, 135, 255};
constexpr Rgb kDarkWarningShimmer{255, 223, 57};
constexpr Rgb kDarkDiffAdded{34, 92, 43};
constexpr Rgb kDarkDiffRemoved{122, 41, 54};
constexpr Rgb kDarkDiffAddedDimmed{71, 88, 74};
constexpr Rgb kDarkDiffRemovedDimmed{105, 72, 77};
constexpr Rgb kDarkDiffAddedWord{56, 166, 96};
constexpr Rgb kDarkDiffRemovedWord{179, 89, 107};
constexpr Rgb kRed600{220, 38, 38};
constexpr Rgb kBlue600{37, 99, 235};
constexpr Rgb kGreen600{22, 163, 74};
constexpr Rgb kYellow600{202, 138, 4};
constexpr Rgb kPurple600{147, 51, 234};
constexpr Rgb kOrange600{234, 88, 12};
constexpr Rgb kPink600{219, 39, 119};
constexpr Rgb kCyan600{8, 145, 178};
constexpr Rgb kProfessionalBlue{106, 155, 204};
constexpr Rgb kChromeYellow{251, 188, 4};
constexpr Rgb kDarkUserMessageBackground{55, 55, 55};
constexpr Rgb kDarkSelectionBg{38, 79, 120};
constexpr Rgb kDarkBashMessageBackground{65, 60, 65};
constexpr Rgb kDarkMemoryBackground{55, 65, 70};
constexpr Rgb kDarkRateLimitFill{177, 185, 249};
constexpr Rgb kDarkRateLimitEmpty{80, 83, 112};
constexpr Rgb kDarkFastMode{255, 120, 20};
constexpr Rgb kDarkBriefLabelYou{122, 180, 232};
constexpr Rgb kDarkBriefLabelClaude{215, 119, 87};
constexpr Rgb kDarkClawdBody{215, 119, 87};
constexpr Rgb kDarkClawdBackground{0, 0, 0};

constexpr Rgb kLightAutoAccept{135, 0, 255};
constexpr Rgb kLightBashBorder{255, 0, 135};
constexpr Rgb kLightClaude{215, 119, 87};
constexpr Rgb kLightClaudeShimmer{245, 149, 117};
constexpr Rgb kLightClaudeBlue{87, 105, 247};
constexpr Rgb kLightClaudeBlueShimmer{117, 135, 255};
constexpr Rgb kLightPermission{87, 105, 247};
constexpr Rgb kLightPermissionShimmer{137, 155, 255};
constexpr Rgb kLightPlanMode{0, 102, 102};
constexpr Rgb kLightIde{71, 130, 200};
constexpr Rgb kLightPromptBorder{153, 153, 153};
constexpr Rgb kLightPromptBorderShimmer{183, 183, 183};
constexpr Rgb kLightText{0, 0, 0};
constexpr Rgb kLightInverseText{255, 255, 255};
constexpr Rgb kLightInactive{102, 102, 102};
constexpr Rgb kLightInactiveShimmer{142, 142, 142};
constexpr Rgb kLightSubtle{175, 175, 175};
constexpr Rgb kLightSuggestion{87, 105, 247};
constexpr Rgb kLightRemember{0, 0, 255};
constexpr Rgb kLightBackground{0, 153, 153};
constexpr Rgb kLightSuccess{44, 122, 57};
constexpr Rgb kLightError{171, 43, 63};
constexpr Rgb kLightWarning{150, 108, 30};
constexpr Rgb kLightMerged{135, 0, 255};
constexpr Rgb kLightWarningShimmer{200, 158, 80};
constexpr Rgb kLightDiffAdded{105, 219, 124};
constexpr Rgb kLightDiffRemoved{255, 168, 180};
constexpr Rgb kLightDiffAddedDimmed{199, 225, 203};
constexpr Rgb kLightDiffRemovedDimmed{253, 210, 216};
constexpr Rgb kLightDiffAddedWord{47, 157, 68};
constexpr Rgb kLightDiffRemovedWord{209, 69, 75};
constexpr Rgb kLightUserMessageBackground{240, 240, 240};
constexpr Rgb kLightSelectionBg{173, 214, 255};
constexpr Rgb kLightBashMessageBackground{245, 240, 245};
constexpr Rgb kLightMemoryBackground{235, 240, 245};
constexpr Rgb kLightRateLimitFill{87, 105, 247};
constexpr Rgb kLightRateLimitEmpty{180, 183, 212};
constexpr Rgb kLightFastMode{230, 100, 10};
constexpr Rgb kLightBriefLabelYou{20, 100, 175};
constexpr Rgb kLightBriefLabelClaude{215, 119, 87};
constexpr Rgb kLightClawdBody{215, 119, 87};
constexpr Rgb kLightClawdBackground{0, 0, 0};

/// The 16-colour palette, shared by the `ansi` themes.
///
/// The reference names the colours of these themes after the palette of the
/// terminal, so the colour that is drawn is the one of the palette and not a
/// twenty-four bit one.
constexpr Rgb ansi(BasicColor colour) {
    Rgb out{};
    out.basic = colour;
    return out;
}

} // namespace

Theme Theme::dark() {
    Theme t{};
    t.auto_accept = kDarkAutoAccept;
    t.bash_border = kDarkBashBorder;
    t.claude = kDarkClaude;
    t.claude_shimmer = kDarkClaudeShimmer;
    t.claude_blue_for_system_spinner = kDarkClaudeBlue;
    t.claude_blue_shimmer_for_system_spinner = kDarkClaudeBlueShimmer;
    t.permission = kDarkPermission;
    t.permission_shimmer = kDarkPermissionShimmer;
    t.plan_mode = kDarkPlanMode;
    t.ide = kDarkIde;
    t.prompt_border = kDarkPromptBorder;
    t.prompt_border_shimmer = kDarkPromptBorderShimmer;
    t.text = kDarkText;
    t.inverse_text = kDarkInverseText;
    t.inactive = kDarkInactive;
    t.inactive_shimmer = kDarkInactiveShimmer;
    t.subtle = kDarkSubtle;
    t.suggestion = kDarkSuggestion;
    t.remember = kDarkRemember;
    t.background = kDarkBackground;
    t.success = kDarkSuccess;
    t.error = kDarkError;
    t.warning = kDarkWarning;
    t.merged = kDarkMerged;
    t.warning_shimmer = kDarkWarningShimmer;
    t.diff_added = kDarkDiffAdded;
    t.diff_removed = kDarkDiffRemoved;
    t.diff_added_dimmed = kDarkDiffAddedDimmed;
    t.diff_removed_dimmed = kDarkDiffRemovedDimmed;
    t.diff_added_word = kDarkDiffAddedWord;
    t.diff_removed_word = kDarkDiffRemovedWord;
    t.red_for_subagents_only = kRed600;
    t.blue_for_subagents_only = kBlue600;
    t.green_for_subagents_only = kGreen600;
    t.yellow_for_subagents_only = kYellow600;
    t.purple_for_subagents_only = kPurple600;
    t.orange_for_subagents_only = kOrange600;
    t.pink_for_subagents_only = kPink600;
    t.cyan_for_subagents_only = kCyan600;
    t.professional_blue = kProfessionalBlue;
    t.chrome_yellow = kChromeYellow;
    t.user_message_background = kDarkUserMessageBackground;
    t.selection_bg = kDarkSelectionBg;
    t.bash_message_background = kDarkBashMessageBackground;
    t.memory_background = kDarkMemoryBackground;
    t.rate_limit_fill = kDarkRateLimitFill;
    t.rate_limit_empty = kDarkRateLimitEmpty;
    t.fast_mode = kDarkFastMode;
    t.brief_label_you = kDarkBriefLabelYou;
    t.brief_label_claude = kDarkBriefLabelClaude;
    t.clawd_body = kDarkClawdBody;
    t.clawd_background = kDarkClawdBackground;
    return t;
}

Theme Theme::light() {
    Theme t{};
    t.auto_accept = kLightAutoAccept;
    t.bash_border = kLightBashBorder;
    t.claude = kLightClaude;
    t.claude_shimmer = kLightClaudeShimmer;
    t.claude_blue_for_system_spinner = kLightClaudeBlue;
    t.claude_blue_shimmer_for_system_spinner = kLightClaudeBlueShimmer;
    t.permission = kLightPermission;
    t.permission_shimmer = kLightPermissionShimmer;
    t.plan_mode = kLightPlanMode;
    t.ide = kLightIde;
    t.prompt_border = kLightPromptBorder;
    t.prompt_border_shimmer = kLightPromptBorderShimmer;
    t.text = kLightText;
    t.inverse_text = kLightInverseText;
    t.inactive = kLightInactive;
    t.inactive_shimmer = kLightInactiveShimmer;
    t.subtle = kLightSubtle;
    t.suggestion = kLightSuggestion;
    t.remember = kLightRemember;
    t.background = kLightBackground;
    t.success = kLightSuccess;
    t.error = kLightError;
    t.warning = kLightWarning;
    t.merged = kLightMerged;
    t.warning_shimmer = kLightWarningShimmer;
    t.diff_added = kLightDiffAdded;
    t.diff_removed = kLightDiffRemoved;
    t.diff_added_dimmed = kLightDiffAddedDimmed;
    t.diff_removed_dimmed = kLightDiffRemovedDimmed;
    t.diff_added_word = kLightDiffAddedWord;
    t.diff_removed_word = kLightDiffRemovedWord;
    t.red_for_subagents_only = kRed600;
    t.blue_for_subagents_only = kBlue600;
    t.green_for_subagents_only = kGreen600;
    t.yellow_for_subagents_only = kYellow600;
    t.purple_for_subagents_only = kPurple600;
    t.orange_for_subagents_only = kOrange600;
    t.pink_for_subagents_only = kPink600;
    t.cyan_for_subagents_only = kCyan600;
    t.professional_blue = kProfessionalBlue;
    t.chrome_yellow = kChromeYellow;
    t.user_message_background = kLightUserMessageBackground;
    t.selection_bg = kLightSelectionBg;
    t.bash_message_background = kLightBashMessageBackground;
    t.memory_background = kLightMemoryBackground;
    t.rate_limit_fill = kLightRateLimitFill;
    t.rate_limit_empty = kLightRateLimitEmpty;
    t.fast_mode = kLightFastMode;
    t.brief_label_you = kLightBriefLabelYou;
    t.brief_label_claude = kLightBriefLabelClaude;
    t.clawd_body = kLightClawdBody;
    t.clawd_background = kLightClawdBackground;
    return t;
}

Theme Theme::ansi_dark() {
    Theme t = dark();
    t.auto_accept = ansi(BasicColor::Magenta);
    t.bash_border = ansi(BasicColor::Magenta);
    t.claude = ansi(BasicColor::BrightRed);
    t.claude_shimmer = ansi(BasicColor::BrightYellow);
    t.claude_blue_for_system_spinner = ansi(BasicColor::Blue);
    t.claude_blue_shimmer_for_system_spinner = ansi(BasicColor::BrightBlue);
    t.permission = ansi(BasicColor::Blue);
    t.permission_shimmer = ansi(BasicColor::BrightBlue);
    t.plan_mode = ansi(BasicColor::Cyan);
    t.ide = ansi(BasicColor::BrightBlue);
    t.prompt_border = ansi(BasicColor::White);
    t.prompt_border_shimmer = ansi(BasicColor::BrightWhite);
    t.text = ansi(BasicColor::White);
    t.inverse_text = ansi(BasicColor::White);
    t.inactive = ansi(BasicColor::BrightBlack);
    t.inactive_shimmer = ansi(BasicColor::White);
    t.subtle = ansi(BasicColor::BrightBlack);
    t.suggestion = ansi(BasicColor::Blue);
    t.remember = ansi(BasicColor::Blue);
    t.background = ansi(BasicColor::Cyan);
    t.success = ansi(BasicColor::Green);
    t.error = ansi(BasicColor::Red);
    t.warning = ansi(BasicColor::Yellow);
    t.merged = ansi(BasicColor::Magenta);
    t.warning_shimmer = ansi(BasicColor::BrightYellow);
    t.diff_added = ansi(BasicColor::Green);
    t.diff_removed = ansi(BasicColor::Red);
    t.diff_added_dimmed = ansi(BasicColor::Green);
    t.diff_removed_dimmed = ansi(BasicColor::Red);
    t.diff_added_word = ansi(BasicColor::BrightGreen);
    t.diff_removed_word = ansi(BasicColor::BrightRed);
    t.red_for_subagents_only = ansi(BasicColor::Red);
    t.blue_for_subagents_only = ansi(BasicColor::Blue);
    t.green_for_subagents_only = ansi(BasicColor::Green);
    t.yellow_for_subagents_only = ansi(BasicColor::Yellow);
    t.purple_for_subagents_only = ansi(BasicColor::Magenta);
    t.orange_for_subagents_only = ansi(BasicColor::BrightRed);
    t.pink_for_subagents_only = ansi(BasicColor::BrightMagenta);
    t.cyan_for_subagents_only = ansi(BasicColor::Cyan);
    t.professional_blue = ansi(BasicColor::BrightBlue);
    t.chrome_yellow = ansi(BasicColor::Yellow);
    t.user_message_background = ansi(BasicColor::White);
    t.selection_bg = ansi(BasicColor::Cyan);
    t.bash_message_background = ansi(BasicColor::BrightWhite);
    t.memory_background = ansi(BasicColor::White);
    t.clawd_body = ansi(BasicColor::BrightRed);
    t.clawd_background = ansi(BasicColor::Black);
    return t;
}

Theme Theme::ansi_light() {
    Theme t = ansi_dark();
    t.text = ansi(BasicColor::Black);
    t.inactive = ansi(BasicColor::BrightBlack);
    t.subtle = ansi(BasicColor::BrightBlack);
    t.background = ansi(BasicColor::Cyan);
    return t;
}

bool parse_theme_name(std::string_view name, ThemeName& out) {
    if (name == "dark") {
        out = ThemeName::Dark;
    } else if (name == "light") {
        out = ThemeName::Light;
    } else if (name == "light-daltonized") {
        out = ThemeName::LightDaltonized;
    } else if (name == "dark-daltonized") {
        out = ThemeName::DarkDaltonized;
    } else if (name == "light-ansi") {
        out = ThemeName::LightAnsi;
    } else if (name == "dark-ansi") {
        out = ThemeName::DarkAnsi;
    } else {
        return false;
    }
    return true;
}

std::string_view to_string(ThemeName name) {
    switch (name) {
    case ThemeName::Dark:
        return "dark";
    case ThemeName::Light:
        return "light";
    case ThemeName::LightDaltonized:
        return "light-daltonized";
    case ThemeName::DarkDaltonized:
        return "dark-daltonized";
    case ThemeName::LightAnsi:
        return "light-ansi";
    case ThemeName::DarkAnsi:
        return "dark-ansi";
    }
    return "dark";
}

Theme theme_for(ThemeName name) {
    switch (name) {
    case ThemeName::Dark:
        return Theme::dark();
    case ThemeName::Light:
        return Theme::light();
    case ThemeName::LightDaltonized:
        return Theme::light();
    case ThemeName::DarkDaltonized:
        return Theme::dark();
    case ThemeName::LightAnsi:
        return Theme::ansi_light();
    case ThemeName::DarkAnsi:
        return Theme::ansi_dark();
    }
    return Theme::dark();
}

} // namespace cc::ui
