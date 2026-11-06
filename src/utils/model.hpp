#pragma once

#include <string>
#include <string_view>

namespace cc {

/// The display name for a model, e.g. `Sonnet 4.5`.
///
/// Unknown models are shown by their raw identifier.
std::string model_display_name(std::string_view model);

/// The name of the model the way the reference writes it in full.
///
/// The wording is "Claude Opus 4.6"; a model the reference does not know
/// is written as "Claude (<model>)".
std::string model_public_name(std::string_view model);

/// The billing label Claude Code shows next to the model.
std::string billing_label();

} // namespace cc
