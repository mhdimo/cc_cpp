#pragma once

#include "ui/theme.hpp"

#include <string>

namespace cc::ui {

/// The bar that shows how much of a limit is used.
///
/// The bar is drawn the way the reference draws it: the part that is used is
/// filled with the blocks of the eighths, and the rest is left empty.
std::string progress_bar(const Theme& theme, double ratio, int width);

} // namespace cc::ui
