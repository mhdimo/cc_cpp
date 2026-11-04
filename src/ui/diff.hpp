#pragma once

#include "ui/ansi.hpp"
#include "ui/theme.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace cc::ui {

/// One line of a unified diff.
struct DiffLine {
    enum class Kind { Context, Added, Removed };

    Kind kind = Kind::Context;
    std::string text;
    int old_line = 0;
    int new_line = 0;
};

/// Computes the line-level difference between two texts.
///
/// The result is a unified diff: context lines are kept, removed lines
/// precede the added lines that replaced them.
std::vector<DiffLine> diff_lines(std::string_view before, std::string_view after);

/// Builds the unified diff of two texts, the way git writes it.
///
/// The hunks keep three lines of context around the lines that changed, and
/// their headers name the line the hunk starts at in the old text and in the
/// new one.
std::string unified_diff(std::string_view before, std::string_view after);

/// The summary Claude Code shows above a diff, e.g. `Added 3 lines, removed 1 line`.
std::string diff_summary(const std::vector<DiffLine>& lines);

/// Renders a diff the way Claude Code shows it.
///
/// Each line is prefixed by a gutter holding the line number and a sign
/// (`+`, `-` or a space). Added lines use the `diffAdded` background,
/// removed lines the `diffRemoved` one, and context lines are dimmed.
std::vector<std::string> render_diff(const std::vector<DiffLine>& lines, const Theme& theme,
                                      int width);

} // namespace cc::ui
