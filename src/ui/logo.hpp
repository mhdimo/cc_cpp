#pragma once

#include "ui/feed.hpp"
#include "ui/theme.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cc::ui {

/// The pose the mascot is in.
enum class MascotPose {
    Default,   ///< The mascot as it is at rest.
    ArmsUp,    ///< Both arms raised, the way it looks when it springs up.
    LookLeft,  ///< The eyes to the left.
    LookRight, ///< The eyes to the right.
};

/// The mascot of Claude Code.
///
/// The mascot is three rows of block characters; its eyes and its body sit
/// on the background colour of the theme. The Terminal.app of Apple cannot
/// fill the background behind the eyes, so the mascot is drawn the other
/// way round there.
class Mascot {
public:
    Mascot(Theme theme, MascotPose pose = MascotPose::Default);

    /// The three rows the mascot occupies.
    ///
    /// The offset moves the mascot down. The row that hangs below is
    /// clipped, the way the reference clips it while the mascot crouches.
    std::vector<std::string> render(int offset = 0) const;

    /// The number of columns the mascot is wide.
    static constexpr int kWidth = 9;

private:
    Theme theme_;
    MascotPose pose_;
};

/// The layout the startup banner is drawn in.
enum class LogoLayout {
    /// The rounded panel: the name of the product in the frame, the mascot
    /// and the greeting on the left, the feeds on the right.
    Panel,
    /// The rounded box of a narrow terminal: the greeting, the mascot and
    /// the texts of the session, one above the other.
    Compact,
    /// The mascot and three lines of text, without a frame.
    Condensed,
};

/// The layout the banner is drawn in at `columns` columns.
///
/// The reference draws the panel when the terminal is at least seventy
/// columns wide, and the compact box otherwise.
LogoLayout logo_layout_for_width(int columns);

/// The layout the banner of the session is drawn in.
///
/// The reference draws the condensed banner when the notes of the release
/// have been seen already and the full logo is not asked for; the panel or
/// the compact box is drawn otherwise, in the layout the width of the
/// terminal asks for.
LogoLayout logo_layout_for_session(bool has_release_notes, bool force_full_logo, int columns);

/// Whether the banner of the session shows the notes of the release.
///
/// The notes are shown when they are new to the user. The first run of the
/// session is the one exception: the frame of the panel and the feeds are
/// left out then, and the notes are taken as seen all the same, so that
/// the panel does not come back at the next run. The full logo that is
/// asked for shows the notes all the same.
bool banner_shows_release_notes(bool has_notes, bool first_run, bool force_full_logo);

/// The greeting the banner shows.
///
/// The wording is "Welcome back <name>!" when the name of the account is
/// known and is not longer than twenty characters, and "Welcome back!"
/// otherwise.
std::string format_welcome_message(std::string_view display_name);

/// The suffix the name of the model carries when the user chose an effort.
///
/// The wording is " with high effort". The level the user chose is shown as
/// it was chosen: the banner never lowers it, and no model decides which
/// levels it takes.
std::string effort_suffix(std::string_view effort);

/// The effort the session runs with.
///
/// The level the user chose is used: the flag of the command line first,
/// the setting of the user second. The variable
/// `CLAUDE_CODE_EFFORT_LEVEL` overrides it when it holds a level, the way
/// the reference does it. The level is empty when the user chose none, and
/// when the variable asks for no effort at all.
std::string effective_effort_level(std::string_view flag_level, std::string_view setting);

/// The startup logo Claude Code shows.
///
/// The logo has three layouts. The panel is the frame the reference draws
/// around the mascot, the greeting and the feeds:
///
/// ```
/// ╭── Claude Code v2.0.0 ─────────────────╮
/// │                                       │
/// │            Welcome back!              │
/// │                                       │
/// │   ▐▛███▜▌          │ Recent activity  │
/// │  ▝▜█████▛▘         │ 3h ago Fix it    │
/// │   ▘▘ ▝▝           │ ──────────────── │
/// │                    │ What's new       │
/// │  Sonnet 4.5        │ A new thing      │
/// │  ~/my-project      │                  │
/// ╰───────────────────────────────────────╯
/// ```
///
/// The compact layout is the rounded box of a narrow terminal: the
/// greeting, the mascot and the texts of the session, one above the other.
///
/// ```
/// ╭─ Claude Code ─────────────────╮
/// │                                │
/// │         Welcome back!          │
/// │                                │
/// │        ▐▛███▜▌                 │
/// │       ▝▜█████▛▘                │
/// │        ▘▘ ▝▝                  │
/// │                                │
/// │         Sonnet 4.5             │
/// │         Claude Max             │
/// │         ~/my-project           │
/// │                                │
/// ╰────────────────────────────────╯
/// ```
///
/// The condensed layout is the mascot and three lines of text, and nothing
/// else:
///
/// ```
///  ▐▛███▜▌  Claude Code v2.0.0
/// ▝▜█████▛▘  Sonnet 4.5 · Claude Max
///   ▘▘ ▝▝    ~/my-project
/// ```
class Logo {
public:
    /// The information the logo shows.
    struct Info {
        std::string version;
        std::string model;
        std::string billing;
        std::string cwd;
        std::string agent_name;
        /// The name of the account of the user, for the greeting.
        std::string display_name;
        /// The organization the account belongs to.
        ///
        /// The organization is left out when it is empty.
        std::string organization_name;
        /// The effort the model spends on its answers, e.g. "high".
        ///
        /// The effort is left out when the user chose none.
        std::string effort;
    };

    Logo(Theme theme, Info info);

    /// The lines the condensed logo occupies.
    ///
    /// The width is the width of the terminal; the text of the logo takes
    /// what is left of it beside the mascot. The mascot is in the given
    /// pose; the full-screen layout animates it when the user clicks it.
    std::vector<std::string> render(int width, MascotPose pose = MascotPose::Default,
                                    int offset = 0) const;

    /// The lines the panel occupies.
    ///
    /// The width is the width of the terminal; the panel fills it. The
    /// feeds are drawn in the column on the right of the rule. Every line
    /// is cut to the width it is given, so that the panel never reaches
    /// past the edge of the terminal.
    std::vector<std::string> render_panel(int width, const std::vector<Feed>& feeds,
                                           MascotPose pose = MascotPose::Default,
                                           int offset = 0) const;

    /// The lines the compact box occupies.
    ///
    /// The box is as wide as the terminal is and holds the greeting, the
    /// mascot, the model, the billing and the working directory, one above
    /// the other. The reference draws it when the terminal is too narrow
    /// for the panel.
    std::vector<std::string> render_compact(int width, MascotPose pose = MascotPose::Default,
                                             int offset = 0) const;

    /// The lines the banner occupies, in the layout the width asks for.
    ///
    /// The width is the width of the terminal.
    std::vector<std::string> render_banner(int width, const std::vector<Feed>& feeds,
                                            MascotPose pose = MascotPose::Default,
                                            int offset = 0) const;

    /// The lines the banner of the session occupies.
    ///
    /// The banner is the condensed one when the notes of the release have
    /// been seen already and the full logo is not asked for; otherwise it
    /// is the panel or the compact box, in the layout the width of the
    /// terminal asks for. The feeds are only drawn in the panel.
    std::vector<std::string> render_session(int width, const std::vector<Feed>& feeds,
                                            bool has_release_notes, bool force_full_logo = false,
                                            MascotPose pose = MascotPose::Default,
                                            int offset = 0) const;

    /// The place the mascot stands in the banner.
    struct MascotPlace {
        /// The row of the mascot, counted from the top of the banner.
        int row = 0;
        /// The column of the mascot, counted from the left of the banner.
        int column = 0;
    };

    /// The place the mascot stands in the banner of `width` columns.
    ///
    /// The banner is the first thing the transcript holds, so the click of
    /// the mouse has to land on that place for the mascot to move.
    MascotPlace mascot_place(int width, const std::vector<Feed>& feeds) const;

    /// The place the mascot stands in the banner of the given layout.
    ///
    /// The layout is the one the banner is drawn in; the width alone does
    /// not tell the condensed banner from the panel.
    MascotPlace mascot_place(int width, const std::vector<Feed>& feeds, LogoLayout layout) const;

private:
    Theme theme_;
    Info info_;
};

} // namespace cc::ui
