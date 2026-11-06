#pragma once

#include <string>

namespace cc {

/// A random slug of three words, the way the reference names the plan of a
/// session: an adjective, a verb and a noun, joined by dashes.
///
/// The example is `gleaming-brewing-phoenix`.
std::string generate_word_slug();

/// A random slug of two words: an adjective and a noun, joined by a dash.
///
/// The example is `graceful-unicorn`.
std::string generate_short_word_slug();

} // namespace cc
