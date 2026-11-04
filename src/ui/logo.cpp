#include "ui/logo.hpp"

#include "utils/path.hpp"
#include "utils/string.hpp"

#include <algorithm>
#include <cstdlib>

namespace cc::ui {
namespace {

/// The parts of the mascot in the poses it can be in.
///
/// The rows are split into the segments that change, so that only the eyes
/// and the arms move. Every pose is nine columns wide.
struct MascotSegments {
    /// The left arm of the first row, and the side of the body.
    std::string_view row1_left;
    /// The eyes of the first row.
    std::string_view row1_eyes;
    /// The right arm of the first row, and the side of the body.
    std::string_view row1_right;
    /// The left arm of the second row, and the curve of the body.
    std::string_view row2_left;
    /// The right arm of the second row, and the curve of the body.
    std::string_view row2_right;
};

/// The parts of the mascot in every pose.
constexpr MascotSegments kPoses[] = {
    // The mascot as it is at rest.
    {" ▐", "▛███▜", "▌", "▝▜", "▛▘"},
    // Both arms raised, the way it looks when it springs up.
    {"▗▟", "▛███▜", "▙▖", " ▜", "▛ "},
    // The eyes to the left.
    {" ▐", "▟███▟", "▌", "▝▜", "▛▘"},
    // The eyes to the right.
    {" ▐", "▙███▙", "▌", "▝▜", "▛▘"},
};

/// The feet of the mascot, which never move.
constexpr std::string_view kFeet = "  ▘▘ ▝▝  ";

/// The eyes of the mascot on the Terminal.app of Apple.
///
/// The Terminal.app of Apple cannot fill the background behind the eyes the
/// way the other terminals do, so the mascot is drawn the other way round
/// there: the eyes are the background of the body. The arms are left out,
/// so the pose with the arms raised looks like the mascot at rest.
constexpr std::string_view kAppleEyes[] = {
    // The mascot as it is at rest.
    " ▗   ▖ ",
    // Both arms raised: the arms are not drawn there.
    " ▗   ▖ ",
    // The eyes to the left.
    " ▘   ▘ ",
    // The eyes to the right.
    " ▝   ▝ ",
};

/// The number of columns between the mascot and the text.
constexpr int kGap = 2;

/// The width of the panel's left column, at most.
constexpr int kMaxLeftWidth = 50;

/// The number of columns the panel spends on its frame, on the padding
/// around its contents and on the rule that stands between the two
/// columns.
constexpr int kPanelChrome = 7;

/// The number of columns of a row of the panel that are not its contents.
///
/// The row is one column short of the panel, because the padding on the
/// right of the contents is filled with spaces.
constexpr int kPanelRowChrome = kPanelChrome - 1;

/// The number of columns the name of the model has in the panel.
constexpr int kPanelModelWidth = kMaxLeftWidth - 20;

/// The number of columns the frame of the compact box and the padding
/// around its contents take.
constexpr int kCompactChrome = 4;

/// The number of columns between the corner of the compact box and the name
/// of the product in it.
constexpr int kCompactTitleOffset = 1;

/// The number of characters of the name of the account that are shown in
/// the greeting.
constexpr size_t kMaxUsernameLength = 20;

/// A run of `count` horizontal rules.
std::string rule_run(size_t count) {
    std::string out;
    out.reserve(count * 3);
    for (size_t i = 0; i < count; ++i) out += "─";
    return out;
}

/// True when the CLI runs in the Terminal.app of Apple.
///
/// The reference asks the terminal with the same variable, the way
/// `src/ui/ansi.cpp` does.
bool is_apple_terminal() {
    const char* program = std::getenv("TERM_PROGRAM");
    return program != nullptr && std::string(program) == "Apple_Terminal";
}

/// Centres `text` in `width` columns.
std::string centre(std::string_view text, size_t width) {
    const size_t used = cc::display_width(text);
    if (used >= width) return std::string(text);
    const size_t left = (width - used) / 2;
    return std::string(left, ' ') + std::string(text) + std::string(width - used - left, ' ');
}

/// The column the content of a box stands at in a box of `room` columns.
///
/// The reference rounds the place of a box half up, so that the content
/// stands one column further to the right when the leftover is an odd
/// number of columns.
size_t centre_offset(size_t room, size_t width) {
    if (width >= room) return 0;
    return (room - width + 1) / 2;
}

/// Centres `text` in `width` columns, the leftover column to the left.
///
/// The reference rounds the place of a box half up, so that the text of a
/// box stands one column further to the right when the leftover is an odd
/// number of columns.
std::string centre_half_up(std::string_view text, size_t width) {
    const size_t used = cc::display_width(text);
    if (used >= width) return std::string(text);
    const size_t left = centre_offset(width, used);
    return std::string(left, ' ') + std::string(text) + std::string(width - used - left, ' ');
}

/// Pads `text` on the right, so that it fills `width` columns.
std::string pad_right(std::string_view text, size_t width) {
    const size_t used = cc::display_width(text);
    if (used >= width) return std::string(text);
    return std::string(text) + std::string(width - used, ' ');
}

/// The number of columns a feed needs.
///
/// The feed is as wide as its widest line, the times of the lines counted
/// with the two columns that stand between them.
size_t feed_width(const Feed& feed) {
    size_t width = cc::display_width(feed.title);

    if (feed.lines.empty()) {
        width = std::max(width, cc::display_width(feed.empty_message));
    } else {
        size_t stamps = 0;
        for (const auto& line : feed.lines) {
            stamps = std::max(stamps, cc::display_width(line.timestamp));
        }
        for (const auto& line : feed.lines) {
            const size_t line_width =
                cc::display_width(line.text) + (stamps > 0 ? stamps + 2 : 0);
            width = std::max(width, line_width);
        }
    }

    return std::max(width, cc::display_width(feed.footer));
}

/// The rows of the column of the feeds.
///
/// Every feed is as wide as it needs to be, but no wider than the room
/// there is; the lines that do not fit are cut, so that the column never
/// reaches past the edge of the terminal.
std::vector<std::string> render_feeds(const Theme& theme, const std::vector<Feed>& feeds,
                                       size_t width) {
    // There is no room for the feeds at all.
    if (width == 0) return {};

    Style title_style;
    title_style.bold = true;
    title_style.foreground = theme.claude;

    Style dim;
    dim.foreground = theme.inactive;

    Style footer_style;
    footer_style.dim = true;
    footer_style.italic = true;

    Style rule_style;
    rule_style.foreground = theme.claude;

    std::vector<std::string> rows;

    for (size_t i = 0; i < feeds.size(); ++i) {
        if (i > 0) {
            // The rule that stands between the feeds.
            rows.push_back(styled(rule_run(width), rule_style));
        }

        const Feed& feed = feeds[i];
        rows.push_back(styled(truncate(feed.title, width), title_style));

        if (feed.lines.empty()) {
            if (!feed.empty_message.empty()) {
                rows.push_back(styled(truncate(feed.empty_message, width), dim));
            }
        } else {
            // The width of the widest time of the feed; the times are cut
            // when the column is too narrow for them.
            size_t stamps = 0;
            for (const auto& line : feed.lines) {
                stamps = std::max(stamps, cc::display_width(line.timestamp));
            }
            stamps = std::min(stamps, width);

            // The room the text of a line has, once the time and the two
            // columns beside it are taken.
            const size_t room = width > stamps + 2 ? width - stamps - 2 : 0;

            for (const auto& line : feed.lines) {
                // The column is too narrow for the time and the text
                // together; the time is left out then.
                if (stamps == 0 || room == 0) {
                    rows.push_back(truncate(line.text, std::max<size_t>(1, width)));
                    continue;
                }
                const std::string stamp = truncate(line.timestamp, stamps);
                rows.push_back(styled(stamp, dim) +
                               std::string(stamps - cc::display_width(stamp), ' ') + "  " +
                               truncate(line.text, room));
            }
        }

        if (!feed.footer.empty()) {
            rows.push_back(styled(truncate(feed.footer, width), footer_style));
        }
    }

    return rows;
}

/// True when the level is one of the levels of the effort.
///
/// The reference knows four of them: "low", "medium", "high" and "max".
bool is_effort_level(std::string_view level) {
    return level == "low" || level == "medium" || level == "high" || level == "max";
}

/// True when the value is a whole number.
///
/// The reference takes a number as the effort of a model, and shows it as
/// "high" for everybody but the users of the inside build.
bool is_numeric_effort(std::string_view value) {
    if (value.empty()) return false;
    const size_t start = value[0] == '-' || value[0] == '+' ? 1 : 0;
    if (start >= value.size()) return false;
    for (size_t i = start; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

/// The level of the effort a value asks for.
///
/// The four levels are named; a whole number is an effort of its own,
/// which the reference shows as "high" for everybody but the users of the
/// inside build. A value that is neither asks for no level at all.
std::string effort_level_of(std::string_view value) {
    const std::string lower = cc::to_lower(value);
    if (is_effort_level(lower)) return lower;
    if (is_numeric_effort(lower)) return "high";
    return {};
}

/// The shape of the panel: the columns it is made of, the rows that fill
/// them, and the place of the mascot.
struct Panel {
    /// The width of the left column.
    int left_width = 0;
    /// The width of the column of the feeds.
    size_t feed_columns = 0;
    /// The number of rows the panel has, the frame apart.
    size_t height = 0;
    /// The rows of the left column.
    std::vector<std::string> left;
    /// The rows of the column of the feeds.
    std::vector<std::string> feeds;
    /// The row of the mascot, counted from the top of the panel.
    int mascot_row = 0;
    /// The column of the mascot, counted from the left of the panel.
    int mascot_column = 0;
};

/// The panel of the banner: the left column, which holds the greeting and
/// the mascot, and the column of the feeds.
///
/// The width of the left column follows the texts it holds, and the column
/// of the feeds takes what is left of the panel.
Panel build_panel(const Theme& theme, const Logo::Info& info, int width,
                  const std::vector<Feed>& feeds, MascotPose pose, int offset) {
    const int columns = std::max(width, 20);

    Style bold;
    bold.bold = true;

    Style dim;
    dim.foreground = theme.inactive;

    // The greeting, at the top of the left column.
    const std::string welcome = format_welcome_message(info.display_name);

    // The model, with the effort the user chose.
    const std::string model = truncate(info.model + effort_suffix(info.effort),
                                       static_cast<size_t>(kPanelModelWidth));
    std::string model_line = model;
    if (!info.billing.empty()) model_line += " · " + info.billing;
    if (!info.organization_name.empty()) model_line += " · " + info.organization_name;

    // The working directory, cut in the middle so that the name of the
    // folder stays visible.
    const size_t agent_room = info.agent_name.empty() ? 0 : cc::display_width(info.agent_name) + 4;
    const std::string path = truncate_path_middle(
        tilde_path(info.cwd),
        std::max<size_t>(static_cast<size_t>(kMaxLeftWidth) - agent_room, 10));
    std::string cwd_line = info.agent_name.empty() ? path : "@" + info.agent_name + " · " + path;

    // The left column is as wide as its widest text, and never wider than
    // the reference makes it.
    const size_t content =
        std::max({cc::display_width(welcome), cc::display_width(cwd_line),
                  cc::display_width(model_line), static_cast<size_t>(20)});

    // The room the two columns share.
    const int room = std::max(0, columns - kPanelChrome);

    // The left column takes what its texts need, and never more than the
    // panel has.
    int left_width = std::min(static_cast<int>(content) + 4, kMaxLeftWidth);
    left_width = std::min(left_width, room);

    // The texts are cut to the column, so that they never reach past it.
    const std::string greeting = truncate(welcome, static_cast<size_t>(left_width));
    model_line = truncate(model_line, static_cast<size_t>(left_width));
    cwd_line = truncate(cwd_line, static_cast<size_t>(left_width));

    Panel panel;
    panel.left_width = left_width;

    // The column of the feeds is as wide as the feeds need, and never
    // wider than the room the panel has left.
    const int feed_room = room - left_width;
    size_t wanted = 0;
    for (const auto& feed : feeds) wanted = std::max(wanted, feed_width(feed));
    panel.feed_columns = std::min(static_cast<size_t>(feed_room), wanted);
    panel.feeds = render_feeds(theme, feeds, panel.feed_columns);

    // The left column: the greeting, the mascot and the two texts. The
    // reference spreads them over the height of the panel, the greeting at
    // the top and the texts at the bottom.
    //
    // The three of them sit in a box of their own, and every box is
    // centred in the column with the leftover column given to the left, the
    // way the reference rounds the place of a box half up.
    const Mascot mascot(theme, pose);

    std::vector<std::string> greeting_rows = {
        std::string(), centre_half_up(styled(greeting, bold), static_cast<size_t>(left_width))};

    // The mascot is a box of nine columns, and its three rows stand at the
    // left edge of it.
    const size_t mascot_pad = centre_offset(static_cast<size_t>(left_width), Mascot::kWidth);
    std::vector<std::string> mascot_rows = mascot.render(offset);
    for (std::string& row : mascot_rows) row = std::string(mascot_pad, ' ') + row;

    // The two texts sit in a box of their own, which is centred half up;
    // the texts are centred in the box with the leftover column to the
    // left.
    const size_t texts_width =
        std::max(cc::display_width(model_line), cc::display_width(cwd_line));
    const size_t texts_pad = centre_offset(static_cast<size_t>(left_width), texts_width);
    std::vector<std::string> text_rows = {
        std::string(texts_pad + (texts_width - cc::display_width(model_line)) / 2, ' ') +
            styled(model_line, dim),
        std::string(texts_pad + (texts_width - cc::display_width(cwd_line)) / 2, ' ') +
            styled(cwd_line, dim)};

    panel.height = std::max({greeting_rows.size() + mascot_rows.size() + text_rows.size(),
                             panel.feeds.size(), static_cast<size_t>(9)});
    const size_t free_rows =
        panel.height - greeting_rows.size() - mascot_rows.size() - text_rows.size();
    const size_t above = free_rows / 2;
    const size_t below = free_rows - above;

    panel.left = greeting_rows;
    panel.left.insert(panel.left.end(), above, std::string());
    panel.left.insert(panel.left.end(), mascot_rows.begin(), mascot_rows.end());
    panel.left.insert(panel.left.end(), below, std::string());
    panel.left.insert(panel.left.end(), text_rows.begin(), text_rows.end());

    // The mascot stands in the middle of the left column.
    panel.mascot_row = static_cast<int>(greeting_rows.size() + above);
    panel.mascot_column = 2 + static_cast<int>(mascot_pad);
    return panel;
}

} // namespace

Mascot::Mascot(Theme theme, MascotPose pose) : theme_(std::move(theme)), pose_(pose) {}

std::vector<std::string> Mascot::render(int offset) const {
    Style body;
    body.foreground = theme_.clawd_body;

    Style body_on_background;
    body_on_background.foreground = theme_.clawd_body;
    body_on_background.background = theme_.clawd_background;

    std::vector<std::string> rows;

    if (is_apple_terminal()) {
        // The Terminal.app of Apple draws the mascot the other way round:
        // the eyes are the background of the body and the arms are left
        // out. The rows are centred in the width of the mascot, the way
        // the reference centres them.
        Style eyes;
        eyes.foreground = theme_.clawd_background;
        eyes.background = theme_.clawd_body;

        Style background;
        background.background = theme_.clawd_body;

        const std::string_view eyes_of_pose = kAppleEyes[static_cast<size_t>(pose_)];
        rows = {
            styled("▗", body) + styled(std::string(eyes_of_pose), eyes) + styled("▖", body),
            std::string(1, ' ') + styled(std::string(7, ' '), background) + std::string(1, ' '),
            std::string(2, ' ') + styled("▘▘ ▝▝", body) + std::string(2, ' '),
        };
    } else {
        const MascotSegments& parts = kPoses[static_cast<size_t>(pose_)];

        rows = {
            styled(std::string(parts.row1_left), body) +
                styled(std::string(parts.row1_eyes), body_on_background) +
                styled(std::string(parts.row1_right), body),
            styled(std::string(parts.row2_left), body) + styled("█████", body_on_background) +
                styled(std::string(parts.row2_right), body),
            styled(std::string(kFeet), body),
        };
    }

    // The mascot crouches by moving down; the row that hangs below is
    // clipped, the way the reference clips it.
    if (offset <= 0) return rows;

    std::vector<std::string> shifted;
    for (int i = 0; i < offset; ++i) {
        shifted.push_back(std::string(static_cast<size_t>(kWidth), ' '));
    }
    for (size_t i = 0; i + static_cast<size_t>(offset) < rows.size(); ++i) {
        shifted.push_back(rows[i + static_cast<size_t>(offset)]);
    }
    return shifted;
}

LogoLayout logo_layout_for_width(int columns) {
    // The reference draws the panel when the terminal is wide enough for
    // the two columns, and the compact box when it is not.
    return columns >= 70 ? LogoLayout::Panel : LogoLayout::Compact;
}

LogoLayout logo_layout_for_session(bool has_release_notes, bool force_full_logo, int columns) {
    // The reference draws the condensed banner when the notes of the
    // release have been seen already and the full logo is not asked for.
    if (!has_release_notes && !force_full_logo) return LogoLayout::Condensed;
    return logo_layout_for_width(columns);
}

std::string format_welcome_message(std::string_view display_name) {
    if (display_name.empty() || cc::utf8_length(display_name) > kMaxUsernameLength) {
        return "Welcome back!";
    }
    return "Welcome back " + std::string(display_name) + "!";
}

std::string effort_suffix(std::string_view effort) {
    if (effort.empty()) return {};

    std::string level(effort);
    // A level that is not one the reference knows is shown as "high", the
    // way the reference shows it.
    if (!is_effort_level(level)) level = "high";
    return " with " + level + " effort";
}

std::string effective_effort_level(std::string_view flag_level, std::string_view setting) {
    // The level the user chose: the flag of the command line first, the
    // setting of the user second. A flag that holds no level is ignored.
    std::string chosen = effort_level_of(flag_level);
    if (chosen.empty()) {
        // The setting of the user holds the levels the reference keeps; a
        // number in it is not one of them.
        const std::string level = cc::to_lower(setting);
        if (is_effort_level(level)) chosen = level;
    }

    // The user chose no effort at all: the banner says nothing about it.
    if (chosen.empty()) return {};

    // The variable overrides the level the user chose.
    const char* env = std::getenv("CLAUDE_CODE_EFFORT_LEVEL");
    if (env == nullptr || *env == '\0') return chosen;

    const std::string lower = cc::to_lower(env);
    // The variable asks for no effort at all.
    if (lower == "unset" || lower == "auto") return {};
    // A variable that holds no level is ignored, the way the reference
    // ignores it: the level the user chose is used.
    const std::string level = effort_level_of(lower);
    if (level.empty()) return chosen;
    return level;
}

Logo::Logo(Theme theme, Info info) : theme_(std::move(theme)), info_(std::move(info)) {}

std::vector<std::string> Logo::render(int width, MascotPose pose, int offset) const {
    // The mascot and the gap beside it take eleven columns of the
    // terminal, and four columns are kept free; the text gets what is left
    // of it, but never less than twenty columns.
    const int text_width = std::max(width - 15, 20);

    Style name_style;
    name_style.bold = true;

    Style dim;
    dim.foreground = theme_.inactive;

    // The product name and the version. The version is cut when the two
    // do not fit together, and the name itself is cut when even it does
    // not fit.
    const int version_room = std::max(text_width - 13, 6);
    std::string title = styled("Claude Code", name_style);
    title += " " + styled("v" + truncate(info_.version, static_cast<size_t>(version_room)), dim);
    if (cc::display_width(title) > static_cast<size_t>(text_width)) {
        title = styled(truncate("Claude Code", static_cast<size_t>(text_width)), name_style);
    }

    // The model and the billing type. The reference cuts the model on its
    // own and always keeps the billing, and it puts the billing on a line
    // of its own when the two do not fit together.
    const std::string model = info_.model + effort_suffix(info_.effort);
    std::vector<std::string> model_lines;
    if (info_.billing.empty()) {
        model_lines.push_back(styled(truncate(model, text_width), dim));
    } else {
        const size_t separator = 3;  // " · "
        const size_t billing_width = cc::display_width(info_.billing);
        if (cc::display_width(model) + separator + billing_width >
            static_cast<size_t>(std::max(text_width, 0))) {
            // The two do not fit together: the billing goes on a line of
            // its own.
            model_lines.push_back(styled(truncate(model, text_width), dim));
            model_lines.push_back(styled(truncate(info_.billing, text_width), dim));
        } else {
            const size_t room = static_cast<size_t>(text_width) > billing_width + separator
                                    ? static_cast<size_t>(text_width) - billing_width - separator
                                    : 10;
            model_lines.push_back(styled(truncate(model, std::max<size_t>(room, 10)), dim) +
                                  " · " + styled(info_.billing, dim));
        }
    }

    // The working directory, below the model. The path is cut on its own,
    // so that the name of the folder stays visible; the name of the agent
    // takes four columns of the room of the path.
    const size_t agent_room =
        info_.agent_name.empty() ? 0 : cc::display_width(info_.agent_name) + 4;
    const std::string path = truncate_path_middle(
        tilde_path(info_.cwd),
        std::max<size_t>(static_cast<size_t>(text_width) - agent_room, 10));
    const std::string location =
        info_.agent_name.empty() ? path : "@" + info_.agent_name + " · " + path;

    std::vector<std::string> text = {title};
    for (const auto& line : model_lines) text.push_back(line);
    text.push_back(styled(location, dim));

    // The mascot stands beside the text; the two are side by side, and the
    // text always stands at the column the mascot ends at.
    const Mascot mascot(theme_, pose);
    const std::vector<std::string> mascot_rows = mascot.render(offset);

    // The mascot is centred against the text, the way the reference centres
    // it: it stands one row lower when the text is four rows tall.
    const size_t mascot_top = centre_offset(text.size(), mascot_rows.size());

    const std::string padding(kGap, ' ');

    std::vector<std::string> lines;
    for (size_t i = 0; i < text.size(); ++i) {
        // The mascot is a box of nine columns, and the rows of it stand at
        // its left edge.
        std::string body(static_cast<size_t>(Mascot::kWidth), ' ');
        if (i >= mascot_top && i - mascot_top < mascot_rows.size()) {
            body = pad_right(mascot_rows[i - mascot_top], static_cast<size_t>(Mascot::kWidth));
        }
        lines.push_back(body + padding + text[i]);
    }
    return lines;
}

std::vector<std::string> Logo::render_panel(int width, const std::vector<Feed>& feeds,
                                             MascotPose pose, int offset) const {
    const int columns = std::max(width, 20);
    const Panel panel = build_panel(theme_, info_, columns, feeds, pose, offset);

    Style border_style;
    border_style.foreground = theme_.claude;

    Style rule_style;
    rule_style.foreground = theme_.claude;
    rule_style.dim = true;

    // The title of the frame: the name of the product and the version.
    // The reference cuts the version when the frame is too narrow for it.
    std::string version = "v" + info_.version;
    const size_t version_room =
        static_cast<size_t>(columns) > 18 ? static_cast<size_t>(columns) - 18 : 1;
    if (cc::display_width(version) > version_room) version = truncate(version, version_room);

    Style name_style;
    name_style.foreground = theme_.claude;

    Style version_style;
    version_style.foreground = theme_.inactive;

    const std::string title = " " + styled("Claude Code", name_style) + " " +
                              styled(version, version_style) + " ";
    const size_t title_width = cc::display_width(title);

    // The top of the frame, with the title three columns from the left.
    const size_t fill = title_width + 5 <= static_cast<size_t>(columns)
                            ? static_cast<size_t>(columns) - 5 - title_width
                            : 0;
    std::string top = styled("╭───", border_style) + title +
                      styled(rule_run(fill), border_style) + styled("╮", border_style);

    std::vector<std::string> lines;
    lines.push_back(top);

    const std::string edge = styled("│", border_style);
    const std::string rule = styled("│", rule_style);
    const size_t pad = static_cast<size_t>(columns) - static_cast<size_t>(panel.left_width) -
                       panel.feed_columns - kPanelRowChrome;

    for (size_t i = 0; i < panel.height; ++i) {
        const std::string left = i < panel.left.size()
                                     ? pad_right(panel.left[i], static_cast<size_t>(panel.left_width))
                                     : std::string(static_cast<size_t>(panel.left_width), ' ');
        const std::string feed = i < panel.feeds.size()
                                     ? pad_right(panel.feeds[i], panel.feed_columns)
                                     : std::string(panel.feed_columns, ' ');
        lines.push_back(edge + " " + left + " " + rule + " " + feed + std::string(pad, ' ') +
                         edge);
    }

    lines.push_back(styled("╰" + rule_run(static_cast<size_t>(columns) - 2) + "╯", border_style));
    return lines;
}

Logo::MascotPlace Logo::mascot_place(int width, const std::vector<Feed>& feeds) const {
    return mascot_place(width, feeds, logo_layout_for_width(width));
}

Logo::MascotPlace Logo::mascot_place(int width, const std::vector<Feed>& feeds,
                                      LogoLayout layout) const {
    MascotPlace place;
    if (layout == LogoLayout::Compact) {
        // The mascot stands in the compact box: under the frame, the
        // padding, the greeting and the margin above it, and centred in
        // the contents of the box. The frame of the box and the padding
        // around its contents take two columns, so the contents start at
        // the third one.
        const int columns = std::max(width, 20);
        const int room = columns - kCompactChrome;
        place.row = 4;
        place.column = 2 + static_cast<int>(centre_offset(static_cast<size_t>(room), Mascot::kWidth));
        return place;
    }

    if (layout != LogoLayout::Panel) {
        // The condensed banner starts with the mascot, in the first column.
        place.row = 0;
        place.column = 0;
        return place;
    }

    const Panel panel = build_panel(theme_, info_, std::max(width, 20), feeds,
                                    MascotPose::Default, 0);
    place.row = panel.mascot_row + 1;  // the frame of the panel is above it
    place.column = panel.mascot_column;
    return place;
}

std::vector<std::string> Logo::render_compact(int width, MascotPose pose, int offset) const {
    const int columns = std::max(width, 20);

    // The frame of the box and the padding around its contents take four
    // columns; the contents have the rest of the width.
    const int room = columns - kCompactChrome;

    Style border_style;
    border_style.foreground = theme_.claude;

    Style name_style;
    name_style.foreground = theme_.claude;

    Style bold;
    bold.bold = true;

    Style dim;
    dim.foreground = theme_.inactive;

    // The greeting. The name of the account is left out when the greeting
    // does not fit in the box.
    std::string welcome = format_welcome_message(info_.display_name);
    if (cc::display_width(welcome) > static_cast<size_t>(room)) {
        welcome = format_welcome_message("");
    }

    // The model, with the effort the user chose.
    const std::string model =
        truncate(info_.model + effort_suffix(info_.effort),
                 std::min<size_t>(static_cast<size_t>(kPanelModelWidth), static_cast<size_t>(room)));

    // The working directory, cut in the middle so that the name of the
    // folder stays visible.
    const size_t agent_room = info_.agent_name.empty() ? 0 : cc::display_width(info_.agent_name) + 4;
    const std::string path = truncate_path_middle(
        tilde_path(info_.cwd),
        std::max<size_t>(static_cast<size_t>(room) - agent_room, 10));
    std::string cwd_line = info_.agent_name.empty() ? path : "@" + info_.agent_name + " · " + path;

    // The contents of the box: the greeting, the mascot and the three
    // texts, one above the other. The mascot has a row of margin above
    // and below it.
    const Mascot mascot(theme_, pose);
    const std::vector<std::string> mascot_rows = mascot.render(offset);

    std::vector<std::string> contents;
    contents.push_back(centre(styled(welcome, bold), static_cast<size_t>(room)));
    contents.push_back(std::string());

    // The mascot is a box of nine columns, and its three rows stand at the
    // left edge of it; the box is centred with the leftover column given to
    // the left, the way the reference rounds the place of a box half up.
    const size_t mascot_pad = centre_offset(static_cast<size_t>(room), Mascot::kWidth);
    for (const auto& row : mascot_rows) {
        contents.push_back(pad_right(std::string(mascot_pad, ' ') + row, static_cast<size_t>(room)));
    }
    contents.push_back(std::string());
    contents.push_back(centre(styled(truncate(model, static_cast<size_t>(room)), dim),
                              static_cast<size_t>(room)));
    contents.push_back(centre(styled(truncate(info_.billing, static_cast<size_t>(room)), dim),
                              static_cast<size_t>(room)));
    contents.push_back(centre(styled(truncate(cwd_line, static_cast<size_t>(room)), dim),
                              static_cast<size_t>(room)));

    // The frame of the box, with the name of the product in it.
    const std::string title = " " + styled("Claude Code", name_style) + " ";
    const size_t title_width = cc::display_width(title);
    const size_t fill = static_cast<size_t>(columns) > title_width + static_cast<size_t>(kCompactTitleOffset) + 2
                            ? static_cast<size_t>(columns) - title_width -
                                  static_cast<size_t>(kCompactTitleOffset) - 2
                            : 0;
    std::string top = styled("╭", border_style) +
                      styled(rule_run(static_cast<size_t>(kCompactTitleOffset)), border_style) + title +
                      styled(rule_run(fill), border_style) + styled("╮", border_style);

    // The rows of the box: the frame, the padding, the contents and the
    // padding again.
    const std::string edge = styled("│", border_style);
    const std::string blank = std::string(static_cast<size_t>(room), ' ');

    std::vector<std::string> lines;
    lines.push_back(top);
    lines.push_back(edge + " " + blank + " " + edge);
    for (const auto& row : contents) {
        lines.push_back(edge + " " + row + " " + edge);
    }
    lines.push_back(edge + " " + blank + " " + edge);
    lines.push_back(styled("╰" + rule_run(static_cast<size_t>(columns) - 2) + "╯", border_style));
    return lines;
}

std::vector<std::string> Logo::render_banner(int width, const std::vector<Feed>& feeds,
                                              MascotPose pose, int offset) const {
    if (logo_layout_for_width(width) == LogoLayout::Panel) {
        return render_panel(width, feeds, pose, offset);
    }
    return render_compact(width, pose, offset);
}

std::vector<std::string> Logo::render_session(int width, const std::vector<Feed>& feeds,
                                              bool has_release_notes, bool force_full_logo,
                                              MascotPose pose, int offset) const {
    switch (logo_layout_for_session(has_release_notes, force_full_logo, width)) {
        case LogoLayout::Condensed:
            return render(width, pose, offset);
        case LogoLayout::Compact:
            return render_compact(width, pose, offset);
        case LogoLayout::Panel:
            break;
    }
    return render_panel(width, feeds, pose, offset);
}

} // namespace cc::ui
