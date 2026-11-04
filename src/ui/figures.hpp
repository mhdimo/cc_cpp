#pragma once

#include <string_view>

namespace cc::ui {

/// The glyphs Claude Code draws with.
///
/// These mirror the reference's `src/constants/figures.ts`.
namespace figures {

/// The pointer in front of the user's prompt.
inline constexpr std::string_view kPointer = "❯";

/// The gutter in front of a tool's result.
inline constexpr std::string_view kMessageResponse = "  ⎿  ";

/// The dot in front of a tool call.
///
/// The reference uses the character that is better aligned on macOS and the
/// plain one on the other systems, because the first is not usually
/// supported there.
#if defined(__APPLE__)
inline constexpr std::string_view kBlackCircle = "⏺";
#else
inline constexpr std::string_view kBlackCircle = "●";
#endif

/// The bar in front of a blockquote.
inline constexpr std::string_view kBlockquoteBar = "▎";

/// The horizontal rule the prompt bar and the dialogs are drawn with.
inline constexpr std::string_view kHorizontalRule = "─";

/// The rule the modal slot of the full-screen layout is topped with.
inline constexpr std::string_view kTopRule = "▔";

/// The rule the diff gutter is drawn with.
inline constexpr std::string_view kVerticalRule = "│";

/// The arrow that says that a list continues above.
inline constexpr std::string_view kArrowUp = "↑";

/// The arrow that says that a list continues below.
inline constexpr std::string_view kArrowDown = "↓";

/// The tick in front of something that worked.
inline constexpr std::string_view kTick = "✔";

/// The cross in front of something that failed.
inline constexpr std::string_view kCross = "✘";

/// The empty square in front of something that is not connected yet.
///
/// The reference takes it from the `figures` package, which draws it as
/// `◻`.
inline constexpr std::string_view kRadioOff = "◻";

/// The circle of a plugin that is chosen for the installation.
///
/// The reference takes it from the `figures` package, which draws it as
/// `◉`.
inline constexpr std::string_view kRadioOn = "◉";

/// The circle of a plugin that is not chosen for the installation.
///
/// The reference takes it from the `figures` package, which draws it as
/// `◯`.
inline constexpr std::string_view kRadioCircle = "◯";

/// The square of a choice that is not picked.
inline constexpr std::string_view kCheckboxOff = "☐";

/// The square of a choice that is picked.
inline constexpr std::string_view kCheckboxOn = "☑";

/// The arrow that points to the left.
inline constexpr std::string_view kArrowLeft = "←";

/// The arrow that points to the right.
inline constexpr std::string_view kArrowRight = "→";

/// The dot in front of an item of a list.
inline constexpr std::string_view kBullet = "•";

/// The sign in front of a warning.
inline constexpr std::string_view kWarning = "⚠";

} // namespace figures

} // namespace cc::ui
