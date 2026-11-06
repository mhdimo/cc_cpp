#pragma once

#include <string>

namespace cc::query {

/// The header that asks for the window of a million tokens.
inline constexpr std::string_view kContext1mBeta = "context-1m-2025-08-07";

/// The name of the model, the way the API knows it.
///
/// The reference takes the tag of the context window off the name of the
/// model before it sends it — `claude-sonnet-4-5[1m]` travels as
/// `claude-sonnet-4-5`, and the window of a million tokens is asked for with
/// a header of its own. A gateway that knows no such tag would refuse the
/// name, so the tag is taken off here too.
std::string model_for_api(const std::string& model);

/// True when the name of the model carries the tag of the context window of
/// a million tokens.
///
/// The reference looks at the tag to decide whether the window of a million
/// tokens is asked for.
bool has_1m_context(const std::string& model);

/// The number of output tokens the model is given, and the limit it may be
/// raised to.
struct OutputTokens {
    /// The number the model is asked for by default.
    int default_tokens = 32000;
    /// The number the model may be asked for at most.
    int upper_limit = 64000;
};

/// The number of output tokens the model takes.
///
/// The reference asks every model for its own number: the newest ones answer
/// with more, the oldest ones with less.
OutputTokens output_tokens_for(const std::string& model);

/// True when the model is one of Anthropic's.
///
/// The parameters of the thinking are only sent to Anthropic's own models: a
/// gateway that speaks to another model does not know them.
bool is_anthropic_model(const std::string& model);

/// True when the model takes the thinking of the adaptive kind.
///
/// The reference asks the newest models for the adaptive thinking, and the
/// older ones for a budget of tokens.
bool supports_adaptive_thinking(const std::string& model);

} // namespace cc::query
