#include "ui/spinner.hpp"

#include "utils/string.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace cc::ui {
namespace {

/// The verbs Claude Code picks the spinner text from.
const std::vector<std::string>& spinner_verbs() {
    static const std::vector<std::string> kVerbs = {
        "Accomplishing",
        "Actioning",
        "Actualizing",
        "Architecting",
        "Baking",
        "Beaming",
        "Beboppin'",
        "Befuddling",
        "Billowing",
        "Blanching",
        "Bloviating",
        "Boogieing",
        "Boondoggling",
        "Booping",
        "Bootstrapping",
        "Brewing",
        "Bunning",
        "Burrowing",
        "Calculating",
        "Canoodling",
        "Caramelizing",
        "Cascading",
        "Catapulting",
        "Cerebrating",
        "Channeling",
        "Channelling",
        "Choreographing",
        "Churning",
        "Clauding",
        "Coalescing",
        "Cogitating",
        "Combobulating",
        "Composing",
        "Computing",
        "Concocting",
        "Considering",
        "Contemplating",
        "Cooking",
        "Crafting",
        "Creating",
        "Crunching",
        "Crystallizing",
        "Cultivating",
        "Deciphering",
        "Deliberating",
        "Determining",
        "Dilly-dallying",
        "Discombobulating",
        "Doing",
        "Doodling",
        "Drizzling",
        "Ebbing",
        "Effecting",
        "Elucidating",
        "Embellishing",
        "Enchanting",
        "Envisioning",
        "Evaporating",
        "Fermenting",
        "Fiddle-faddling",
        "Finagling",
        "Flambéing",
        "Flibbertigibbeting",
        "Flowing",
        "Flummoxing",
        "Fluttering",
        "Forging",
        "Forming",
        "Frolicking",
        "Frosting",
        "Gallivanting",
        "Galloping",
        "Garnishing",
        "Generating",
        "Gesticulating",
        "Germinating",
        "Gitifying",
        "Grooving",
        "Gusting",
        "Harmonizing",
        "Hashing",
        "Hatching",
        "Herding",
        "Honking",
        "Hullaballooing",
        "Hyperspacing",
        "Ideating",
        "Imagining",
        "Improvising",
        "Incubating",
        "Inferring",
        "Infusing",
        "Ionizing",
        "Jitterbugging",
        "Julienning",
        "Kneading",
        "Leavening",
        "Levitating",
        "Lollygagging",
        "Manifesting",
        "Marinating",
        "Meandering",
        "Metamorphosing",
        "Misting",
        "Moonwalking",
        "Moseying",
        "Mulling",
        "Mustering",
        "Musing",
        "Nebulizing",
        "Nesting",
        "Newspapering",
        "Noodling",
        "Nucleating",
        "Orbiting",
        "Orchestrating",
        "Osmosing",
        "Perambulating",
        "Percolating",
        "Perusing",
        "Philosophising",
        "Photosynthesizing",
        "Pollinating",
        "Pondering",
        "Pontificating",
        "Pouncing",
        "Precipitating",
        "Prestidigitating",
        "Processing",
        "Proofing",
        "Propagating",
        "Puttering",
        "Puzzling",
        "Quantumizing",
        "Razzle-dazzling",
        "Razzmatazzing",
        "Recombobulating",
        "Reticulating",
        "Roosting",
        "Ruminating",
        "Sautéing",
        "Scampering",
        "Schlepping",
        "Scurrying",
        "Seasoning",
        "Shenaniganing",
        "Shimmying",
        "Simmering",
        "Skedaddling",
        "Sketching",
        "Slithering",
        "Smooshing",
        "Sock-hopping",
        "Spelunking",
        "Spinning",
        "Sprouting",
        "Stewing",
        "Sublimating",
        "Swirling",
        "Swooping",
        "Symbioting",
        "Synthesizing",
        "Tempering",
        "Thinking",
        "Thundering",
        "Tinkering",
        "Tomfoolering",
        "Topsy-turvying",
        "Transfiguring",
        "Transmuting",
        "Twisting",
        "Undulating",
        "Unfurling",
        "Unravelling",
        "Vibing",
        "Waddling",
        "Wandering",
        "Warping",
        "Whatchamacalliting",
        "Whirlpooling",
        "Whirring",
        "Whisking",
        "Wibbling",
        "Working",
        "Wrangling",
        "Zesting",
        "Zigzagging",
    };
    return kVerbs;
}

/// The characters the spinner cycles through.
const std::vector<std::string>& spinner_characters() {
    static const std::vector<std::string> kCharacters =
#if defined(__APPLE__)
        std::vector<std::string>{"·", "✢", "✳", "✶", "✻", "✽"};
#else
        std::vector<std::string>{"·", "✢", "*", "✶", "✻", "✽"};
#endif
    return kCharacters;
}

/// The number of milliseconds a frame is shown for.
constexpr int64_t kFrameIntervalMs = 120;

/// The current time in milliseconds.
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Picks a random element of `values`.
template <typename T>
const T& sample(const std::vector<T>& values) {
    static std::mt19937 engine{std::random_device{}()};
    std::uniform_int_distribution<size_t> distribution(0, values.size() - 1);
    return values[distribution(engine)];
}

/// The number of milliseconds the answer may stand still before the
/// spinner turns red.
constexpr int64_t kStallAfterMs = 3000;

/// The number of milliseconds the band of the shimmer takes to move one
/// column.
constexpr int64_t kGlimmerSpeedMs = 200;

/// The number of milliseconds the band of the shimmer takes to move one
/// column while the request is on its way to the model.
///
/// The reference sweeps the band faster while the request is in flight.
constexpr int64_t kRequestGlimmerSpeedMs = 50;

/// The grey the thinking of the model is drawn in before it shimmers.
///
/// The reference keeps the two greys of the thinking to itself: they are the
/// same in every theme.
constexpr Rgb kThinkingInactive{153, 153, 153};

/// The grey the thinking of the model shimmers towards.
constexpr Rgb kThinkingInactiveShimmer{185, 185, 185};

/// The number of milliseconds the thinking waits before it begins to
/// shimmer.
constexpr int64_t kThinkingDelayMs = 3000;

/// The period of the shimmer of the thinking, in milliseconds.
constexpr int64_t kThinkingGlowPeriodMs = 2000;

/// Mixes two colours, the way the reference mixes them.
///
/// `share` is the part of the second colour, from zero to one.
Rgb interpolate_colour(const Rgb& from, const Rgb& to, double share) {
    const auto mix = [share](uint8_t a, uint8_t b) {
        const double value = static_cast<double>(a) + (static_cast<double>(b) - a) * share;
        return static_cast<uint8_t>(std::lround(value));
    };
    return Rgb{mix(from.r, to.r), mix(from.g, to.g), mix(from.b, to.b)};
}

/// The colour the thinking of the model is drawn in.
///
/// The reference pulses the word between the two greys with a sine of two
/// seconds, and the pulse begins after a delay of three seconds.
Rgb thinking_shimmer_colour(int64_t elapsed_ms) {
    if (elapsed_ms < kThinkingDelayMs) return kThinkingInactive;

    const double seconds = static_cast<double>(elapsed_ms - kThinkingDelayMs) / 1000.0;
    const double period = static_cast<double>(kThinkingGlowPeriodMs) / 1000.0;
    const double share = (std::sin(seconds * 2.0 * std::numbers::pi / period) + 1.0) / 2.0;
    return interpolate_colour(kThinkingInactive, kThinkingInactiveShimmer, share);
}

/// Splits the text at the given column.
///
/// The text before the column ends up in `before`, the rest in `after`.
void split_at_column(std::string_view text, size_t column, std::string& before, std::string& after) {
    before.clear();
    after.clear();

    size_t used = 0;
    size_t offset = 0;
    for (const auto& cluster : cc::graphemes(text)) {
        const size_t columns = cc::display_width(cluster);
        if (used + columns > column) break;
        used += columns;
        offset += cluster.size();
    }
    before.assign(text.substr(0, offset));
    after.assign(text.substr(offset));
}

} // namespace

/// Formats a duration the way Claude Code formats it.
///
/// A turn that lasts less than a minute is shown in seconds; a longer one
/// names the hours and the minutes as well.
std::string format_duration(int64_t ms) {
    if (ms < 60000) {
        if (ms <= 0) return "0s";
        return std::to_string(ms / 1000) + "s";
    }

    int64_t days = ms / 86400000;
    int64_t hours = (ms % 86400000) / 3600000;
    int64_t minutes = (ms % 3600000) / 60000;
    int64_t seconds = ((ms % 60000) + 500) / 1000;

    // The rounding may push the seconds over the minute.
    if (seconds == 60) {
        seconds = 0;
        ++minutes;
    }
    if (minutes == 60) {
        minutes = 0;
        ++hours;
    }
    if (hours == 24) {
        hours = 0;
        ++days;
    }

    std::string out;
    const auto append = [&out](int64_t value, const char* name) {
        if (!out.empty()) out.push_back(' ');
        out += std::to_string(value);
        out += name;
    };

    if (days > 0) {
        append(days, "d");
        append(hours, "h");
        append(minutes, "m");
        return out;
    }
    if (hours > 0) {
        append(hours, "h");
        append(minutes, "m");
        append(seconds, "s");
        return out;
    }
    append(minutes, "m");
    append(seconds, "s");
    return out;
}

Spinner::Spinner(Theme theme) : theme_(std::move(theme)), verbs_(spinner_verbs()) {
    verb_ = sample(verbs_);
}

std::string Spinner::glyph() const {
    // With the animations turned off the glyph is a dot that fades in and
    // out, the way the reference shows it.
    if (reduced_motion_) return "●";
    return frames()[frame_index_];
}

void Spinner::set_reduced_motion(bool reduced) { reduced_motion_ = reduced; }

std::string Spinner::line() const {
    Style style;
    style.foreground = stalled() ? theme_.error : theme_.claude;

    if (reduced_motion_) {
        // The dot is dim for a second and bright for a second.
        const bool dim = (now() / 1000) % 2 == 1;
        Style dot = style;
        dot.dim = dim;
        return styled(glyph(), dot) + " " + styled(verb_ + "…", style);
    }

    // The glyph occupies two cells, so a space always follows it.
    return styled(glyph() + " ", style) + glimmered(verb_ + "…", style);
}

/// The message of the spinner, with the band of the shimmer painted on it.
///
/// Claude Code sweeps a band of three columns across the message.
std::string Spinner::glimmered(const std::string& message, const Style& style) const {
    Style shimmer;
    shimmer.foreground = theme_.claude_shimmer;

    const size_t width = cc::display_width(message);
    if (width == 0) return styled(message, style);

    const int64_t moment = now();

    // While a tool runs, the whole word pulses between the colour of the
    // model and the colour of the shimmer, the way the reference pulses it.
    // A spinner that has stalled keeps the colour of the error.
    if (mode_ == SpinnerMode::ToolUse && !stalled()) {
        const double share =
            (std::sin(static_cast<double>(moment) / 1000.0 * std::numbers::pi) + 1.0) / 2.0;
        Style pulse = style;
        pulse.foreground = interpolate_colour(theme_.claude, theme_.claude_shimmer, share);
        return styled(message, pulse);
    }

    // The band moves from the right to the left, and from the left to the
    // right while the request is on its way to the model.
    const int64_t cycle_length = static_cast<int64_t>(width) + 20;
    const int64_t speed = mode_ == SpinnerMode::Requesting ? kRequestGlimmerSpeedMs : kGlimmerSpeedMs;
    const int64_t position = (moment / speed) % cycle_length;
    const int64_t index = mode_ == SpinnerMode::Requesting
                              ? position - 10
                              : static_cast<int64_t>(width) + 10 - position;

    const int64_t start = index - 1;
    const int64_t end = index + 1;
    if (start >= static_cast<int64_t>(width) || end < 0) return styled(message, style);

    const size_t clamped_start = static_cast<size_t>(std::max<int64_t>(0, start));
    // The band is three columns wide: the column of the index and the ones
    // on either side of it.
    const size_t band_end =
        static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(width), end + 1));

    std::string before;
    std::string rest;
    split_at_column(message, clamped_start, before, rest);

    std::string band;
    std::string after;
    split_at_column(rest, band_end - clamped_start, band, after);

    return styled(before, style) + styled(band, shimmer) + styled(after, style);
}

std::string Spinner::line(int width, int64_t total_tokens, bool verbose) const {
    Style style;
    style.foreground = stalled() ? theme_.error : theme_.claude;

    const std::string message = verb_ + "…";

    std::string head;
    if (reduced_motion_) {
        // The dot is dim for a second and bright for a second; the shimmer
        // of the message does not move.
        const bool dim = (now() / 1000) % 2 == 1;
        Style dot = style;
        dot.dim = dim;
        head = styled(glyph(), dot) + " " + styled(message, style);
    } else {
        head = styled(glyph() + " ", style) + glimmered(message, style);
    }

    // The reference waits half a minute before it shows the elapsed time
    // and the token count.
    constexpr int64_t kShowAfterMs = 30000;
    const int64_t elapsed = started_ms_ == 0 ? 0 : now() - started_ms_;
    const bool show_details = verbose || elapsed >= kShowAfterMs;

    Style dim;
    dim.foreground = theme_.inactive;

    // The parts of the status, in the order the reference shows them. Every
    // one of them is dim but the thinking of the model.
    std::vector<std::string> parts;
    // The width of the text of the parts, for the width of the line.
    size_t text_width = 0;

    const auto add = [&parts, &text_width](const std::string& text, const Style& part_style) {
        parts.push_back(styled(text, part_style));
        text_width += text.size();
    };

    if (show_details) add(format_duration(elapsed), dim);
    if (show_details && total_tokens > 0) {
        // The arrow points up while the request is on its way to the model.
        const std::string arrow = mode_ == SpinnerMode::Requesting ? "↑ " : "↓ ";
        add(arrow + cc::format_number(total_tokens) + " tokens", dim);
    }

    // The word of the thinking is in parentheses when it is the only part of
    // the status, and it shimmers while the model thinks.
    const std::string thinking = thinking_text();
    const bool thinking_alone = parts.empty() && !thinking.empty();
    const bool thinking_live = thinking_started_ms_ != 0 && thinking_ended_ms_ == 0;
    if (!thinking.empty()) {
        if (thinking_live && !reduced_motion_) {
            Style shimmer;
            shimmer.foreground = thinking_shimmer_colour(elapsed);
            add(thinking_alone ? "(" + thinking + ")" : thinking, shimmer);
        } else {
            add(thinking, dim);
        }
    }

    if (parts.empty()) return head;

    // The parts are joined with a middot.
    std::string status;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            status += styled(" · ", dim);
            text_width += 3;
        }
        status += parts[i];
    }

    const size_t needed =
        cc::display_width(glyph()) + 1 + cc::display_width(message) + text_width + 3;
    if (width > 0 && needed > static_cast<size_t>(width)) {
        // There is no room for the details, so they are left out.
        return head;
    }

    // The reference leaves the brackets of the status away when the
    // thinking of the model is the only part of it.
    if (thinking_alone && thinking_live) return head + " " + status;

    return head + styled(" (", dim) + status + styled(")", dim);
}

int64_t Spinner::elapsed_ms() const { return started_ms_ == 0 ? 0 : now() - started_ms_; }

void Spinner::set_verb(std::string verb) { verb_ = std::move(verb); }

void Spinner::reshuffle() { verb_ = sample(verbs_); }

void Spinner::set_verbs(std::vector<std::string> verbs) {
    if (verbs.empty()) return;
    verbs_ = std::move(verbs);
    verb_ = sample(verbs_);
}

void Spinner::note_response(size_t length) {
    if (length <= response_length_) return;
    response_length_ = length;
    last_token_ms_ = now();
}

void Spinner::note_tool_active() {
    tool_active_ = true;
    last_token_ms_ = now();
}

void Spinner::note_thinking_start() {
    if (thinking_started_ms_ != 0 && thinking_ended_ms_ == 0) return;
    thinking_started_ms_ = now();
    thinking_ended_ms_ = 0;
}

void Spinner::note_thinking_end() {
    if (thinking_started_ms_ == 0 || thinking_ended_ms_ != 0) return;
    thinking_ended_ms_ = now();
}

/// The time the animations are drawn at.
///
/// The clock of the spinner is used when it is set; the time of the machine
/// otherwise.
int64_t Spinner::now() const {
    if (clock_) return clock_();
    return now_ms();
}

/// What the spinner says about the thinking of the model.
///
/// The reference says `thinking` while the model thinks and `thought for 8s`
/// afterwards; both states are shown for at least two seconds.
std::string Spinner::thinking_text() const {
    if (thinking_started_ms_ == 0) return {};

    if (thinking_ended_ms_ == 0) return "thinking";

    const int64_t moment = now();
    const int64_t until = std::max(thinking_ended_ms_, thinking_started_ms_ + 2000) + 2000;
    if (moment > until) return {};

    const int64_t seconds = std::max<int64_t>(1, (thinking_ended_ms_ - thinking_started_ms_ + 500) / 1000);
    return "thought for " + std::to_string(seconds) + "s";
}

bool Spinner::stalled() const {
    if (tool_active_) return false;
    if (last_token_ms_ == 0) return false;
    return now() - last_token_ms_ > kStallAfterMs;
}

void Spinner::reset() {
    started_ms_ = now();
    last_tick_ms_ = 0;
    response_length_ = 0;
    last_token_ms_ = now();
    tool_active_ = false;
    // The reference picks a new verb for every turn.
    reshuffle();
}

void Spinner::tick() {
    const int64_t moment = now();
    if (last_tick_ms_ == 0) {
        last_tick_ms_ = moment;
        if (started_ms_ == 0) started_ms_ = moment;
        return;
    }
    if (moment <= last_tick_ms_) return;

    const int64_t elapsed = moment - last_tick_ms_;
    const int64_t steps = elapsed / kFrameIntervalMs;
    if (steps <= 0) return;

    frame_index_ = (frame_index_ + static_cast<size_t>(steps)) % frames().size();
    last_tick_ms_ += steps * kFrameIntervalMs;
}

const std::vector<std::string>& Spinner::verbs() { return spinner_verbs(); }

const std::vector<std::string>& Spinner::frames() {
    static const std::vector<std::string> kFrames = [] {
        const std::vector<std::string>& characters = spinner_characters();
        std::vector<std::string> frames = characters;
        for (auto it = characters.rbegin(); it != characters.rend(); ++it) frames.push_back(*it);
        return frames;
    }();
    return kFrames;
}

} // namespace cc::ui
