#pragma once

#include "ui/ansi.hpp"
#include "ui/theme.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cc::ui {

/// What the spinner is doing, which decides how it is drawn.
///
/// The reference has a mode for every stage of a turn: the request that is
/// on its way to the model, the thinking of the model, its answer, the
/// input of a tool and the tool that runs.
enum class SpinnerMode {
    Requesting,  ///< The request is on its way to the model.
    Thinking,    ///< The model is thinking.
    Responding,  ///< The model is writing its answer.
    ToolInput,   ///< The model is writing the input of a tool.
    ToolUse,     ///< A tool is running.
};

/// Formats a duration the way Claude Code does: `5000` becomes `5s`.
///
/// A turn that lasts less than a minute is shown in seconds; a longer one
/// names the hours and the minutes as well.
std::string format_duration(int64_t ms);

/// The per-turn activity indicator.
///
/// Claude Code shows a spinning glyph followed by a randomly chosen verb,
/// e.g. `✻ Herding…`. The glyph advances every 120 ms.
class Spinner {
public:
    explicit Spinner(Theme theme);

    /// The glyph for the current moment.
    std::string glyph() const;

    /// The verb the spinner shows, e.g. `Herding`.
    const std::string& verb() const { return verb_; }

    /// The full spinner line, e.g. `✻ Herding…`.
    std::string line() const;

    /// The spinner line with the elapsed time and the token count, the way
    /// Claude Code shows them once there is room.
    ///
    /// The reference waits half a minute before it shows them, so they are
    /// left out until then — unless the detailed output is on.
    std::string line(int width, int64_t total_tokens, bool verbose = false) const;

    /// Starts the clock over, for a new turn.
    void reset();

    /// Picks another verb, the way the reference does for a new turn.
    void reshuffle();

    /// Sets the verbs the spinner picks from.
    ///
    /// The user can add verbs of their own, or replace the ones Claude
    /// Code ships with.
    void set_verbs(std::vector<std::string> verbs);

    /// Overrides the verb, e.g. with the active todo's `activeForm`.
    void set_verb(std::string verb);

    /// Turns the animations off, for the users who asked for that.
    ///
    /// The glyph stops spinning and becomes a dot that fades in and out,
    /// and the shimmer of the message stops as well.
    void set_reduced_motion(bool reduced);

    /// Sets what the spinner is doing, which decides how it is drawn.
    ///
    /// The request that is on its way to the model sweeps its shimmer the
    /// other way and shows an arrow that points up; a tool that runs pulses
    /// its whole word.
    void set_mode(SpinnerMode mode) { mode_ = mode; }

    /// Records that the answer has grown to `length` characters.
    ///
    /// The spinner turns red when the answer stops growing.
    void note_response(size_t length);

    /// Records that a tool is running, which keeps the spinner from
    /// turning red.
    void note_tool_active();

    /// Records that the model started thinking.
    void note_thinking_start();

    /// Records that the model stopped thinking.
    void note_thinking_end();

    /// Advances the animation to the current time.
    void tick();

    /// Sets the clock of the spinner.
    ///
    /// The clock is the time the animations are drawn at. The tests set
    /// it, so that the shimmer can be looked at at a known moment instead
    /// of at the moment the test happens to run.
    void set_clock(std::function<int64_t()> clock) { clock_ = std::move(clock); }

    /// The number of milliseconds the turn has been running.
    int64_t elapsed_ms() const;

    /// The verbs Claude Code picks from.
    static const std::vector<std::string>& verbs();

    /// The glyph frames, in order.
    static const std::vector<std::string>& frames();

private:
    Theme theme_;
    std::string verb_;
    size_t frame_index_ = 0;
    int64_t last_tick_ms_ = 0;

    /// The moment the turn started.
    int64_t started_ms_ = 0;

    /// The length of the answer the last time it grew.
    size_t response_length_ = 0;

    /// The moment the answer last grew.
    int64_t last_token_ms_ = 0;

    /// True while a tool is running.
    bool tool_active_ = false;

    /// True when the animations are turned off.
    bool reduced_motion_ = false;

    /// The moment the model started thinking.
    int64_t thinking_started_ms_ = 0;

    /// The moment the model stopped thinking; zero while it still thinks.
    int64_t thinking_ended_ms_ = 0;

    /// The verbs the spinner picks from.
    std::vector<std::string> verbs_;

    /// What the spinner is doing at the moment.
    SpinnerMode mode_ = SpinnerMode::Responding;

    /// True when the answer has stood still for too long.
    bool stalled() const;

    /// The message of the spinner, with the band of the shimmer painted on
    /// it.
    std::string glimmered(const std::string& message, const Style& style) const;

    /// What the spinner says about the thinking of the model.
    std::string thinking_text() const;

    /// The time the animations are drawn at.
    int64_t now() const;

    /// The clock of the spinner; the time of the machine when it is not
    /// set.
    std::function<int64_t()> clock_;
};

} // namespace cc::ui
