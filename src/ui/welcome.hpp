#pragma once

#include "ui/theme.hpp"

#include <string>
#include <vector>

namespace cc::ui {

/// The welcome the session shows when it is set up for the first time.
///
/// The reference opens the session of a user who has not been here before
/// with a picture that is fifty-eight columns wide: the name of the
/// product, the version of the session and a shaded drawing of the
/// mascot.
///
/// The picture of the dark theme and the picture of the light themes are
/// not the same; the themes that only use the colours of the terminal take
/// the picture of the dark theme.
std::vector<std::string> welcome_lines(const Theme& theme, const std::string& version, bool light);


} // namespace cc::ui
