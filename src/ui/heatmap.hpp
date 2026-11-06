#pragma once

#include "ui/ansi.hpp"
#include "ui/theme.hpp"

#include <map>
#include <string>
#include <vector>

namespace cc::ui {

/// The heatmap of the activity, the way the reference draws it.
///
/// The grid holds a week per column and a day per row; the days are the
/// ones with no activity that are dim, and the ones with activity grow from
/// a light block to a full one as the number of the messages grows.
std::vector<std::string> generate_heatmap(const std::map<std::string, size_t>& daily_activity,
                                          const Theme& theme, int width);

} // namespace cc::ui
