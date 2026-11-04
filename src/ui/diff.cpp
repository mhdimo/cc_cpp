#include "ui/diff.hpp"

#include "utils/string.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace cc::ui {
namespace {

/// One step of the edit script produced by the diff.
struct Edit {
    enum class Kind { Common, Removed, Added };

    Kind kind = Kind::Common;
    size_t index = 0;  ///< The index of the line in the old or new text.
};

/// The largest edit distance the diff algorithm is willing to search.
constexpr int kMaxEditDistance = 4000;

/// The number of lines of context a hunk of a diff keeps.
constexpr size_t kDiffContext = 3;

/// Runs Myers' diff algorithm over two sequences of lines.
///
/// The result is the edit script that turns `a` into `b`.
std::vector<Edit> myers_diff(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    if (n == 0 && m == 0) return {};
    if (n == 0) {
        std::vector<Edit> edits;
        for (int i = 0; i < m; ++i) edits.push_back(Edit{Edit::Kind::Added, static_cast<size_t>(i)});
        return edits;
    }
    if (m == 0) {
        std::vector<Edit> edits;
        for (int i = 0; i < n; ++i) edits.push_back(Edit{Edit::Kind::Removed, static_cast<size_t>(i)});
        return edits;
    }

    const int max_distance = std::min(n + m, kMaxEditDistance);
    const int offset = n + m + 1;
    std::vector<int> v(static_cast<size_t>(2 * (n + m) + 3), 0);
    std::vector<std::vector<int>> trace;

    int distance = -1;
    for (int d = 0; d <= max_distance; ++d) {
        // The backtrack needs the diagonals of the previous depth, which have
        // the opposite parity and are therefore not updated in this round.
        std::vector<int> snapshot;
        if (d > 0) {
            const int low = -(d - 1);
            const int high = d - 1;
            snapshot.reserve(static_cast<size_t>(high - low + 1));
            for (int k = low; k <= high; ++k) snapshot.push_back(v[static_cast<size_t>(k + offset)]);
        }
        trace.push_back(std::move(snapshot));

        for (int k = -d; k <= d; k += 2) {
            const int left = v[static_cast<size_t>(k - 1 + offset)];
            const int right = v[static_cast<size_t>(k + 1 + offset)];
            int x;
            if (k == -d || (k != d && left < right)) {
                x = right;
            } else {
                x = left + 1;
            }
            int y = x - k;
            while (x < n && y < m && a[static_cast<size_t>(x)] == b[static_cast<size_t>(y)]) {
                ++x;
                ++y;
            }
            v[static_cast<size_t>(k + offset)] = x;
            if (x >= n && y >= m) {
                distance = d;
                break;
            }
        }
        if (distance >= 0) break;
    }

    if (distance < 0) {
        // The two texts are too different to be compared in detail; show the
        // whole block as replaced.
        std::vector<Edit> edits;
        for (int i = 0; i < n; ++i) edits.push_back(Edit{Edit::Kind::Removed, static_cast<size_t>(i)});
        for (int i = 0; i < m; ++i) edits.push_back(Edit{Edit::Kind::Added, static_cast<size_t>(i)});
        return edits;
    }

    // Walk the trace backwards to recover the edit script.
    std::vector<Edit> edits;
    int x = n;
    int y = m;
    for (int d = distance; d > 0; --d) {
        const std::vector<int>& snapshot = trace[static_cast<size_t>(d)];
        const int k = x - y;
        const int low = -(d - 1);
        const auto value = [&](int diagonal) {
            const int index = diagonal - low;
            if (index < 0 || index >= static_cast<int>(snapshot.size())) return 0;
            return snapshot[static_cast<size_t>(index)];
        };
        const bool down = (k == -d) || (k != d && value(k - 1) < value(k + 1));
        const int prev_k = down ? k + 1 : k - 1;
        const int prev_x = value(prev_k);
        const int prev_y = prev_x - prev_k;
        const int snake_x = down ? prev_x : prev_x + 1;
        const int snake_y = down ? prev_y + 1 : prev_y;
        while (x > snake_x && y > snake_y) {
            --x;
            --y;
            edits.push_back(Edit{Edit::Kind::Common, static_cast<size_t>(x)});
        }
        if (down) {
            edits.push_back(Edit{Edit::Kind::Added, static_cast<size_t>(prev_y)});
        } else {
            edits.push_back(Edit{Edit::Kind::Removed, static_cast<size_t>(prev_x)});
        }
        x = prev_x;
        y = prev_y;
    }
    while (x > 0 && y > 0) {
        --x;
        --y;
        edits.push_back(Edit{Edit::Kind::Common, static_cast<size_t>(x)});
    }
    std::reverse(edits.begin(), edits.end());
    return edits;
}

/// The number of digits of `value`.
int digit_count(int value) {
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

} // namespace

std::vector<DiffLine> diff_lines(std::string_view before, std::string_view after) {
    const std::vector<std::string> old_lines = cc::split_lines(before);
    const std::vector<std::string> new_lines = cc::split_lines(after);

    // Trim the common prefix and suffix so the diff only has to look at the
    // part that actually changed.
    size_t prefix = 0;
    while (prefix < old_lines.size() && prefix < new_lines.size() &&
           old_lines[prefix] == new_lines[prefix]) {
        ++prefix;
    }
    size_t suffix = 0;
    while (suffix < old_lines.size() - prefix && suffix < new_lines.size() - prefix &&
           old_lines[old_lines.size() - 1 - suffix] == new_lines[new_lines.size() - 1 - suffix]) {
        ++suffix;
    }

    const std::vector<std::string> old_middle(old_lines.begin() + static_cast<long>(prefix),
                                              old_lines.end() - static_cast<long>(suffix));
    const std::vector<std::string> new_middle(new_lines.begin() + static_cast<long>(prefix),
                                              new_lines.end() - static_cast<long>(suffix));

    const std::vector<Edit> edits = myers_diff(old_middle, new_middle);

    std::vector<DiffLine> out;
    out.reserve(old_lines.size() + new_lines.size());

    int old_number = 1;
    int new_number = 1;

    for (size_t i = 0; i < prefix; ++i) {
        out.push_back(DiffLine{DiffLine::Kind::Context, old_lines[i], old_number, new_number});
        ++old_number;
        ++new_number;
    }

    for (const auto& edit : edits) {
        switch (edit.kind) {
            case Edit::Kind::Common:
                out.push_back(DiffLine{DiffLine::Kind::Context, old_middle[edit.index], old_number, new_number});
                ++old_number;
                ++new_number;
                break;
            case Edit::Kind::Removed:
                out.push_back(DiffLine{DiffLine::Kind::Removed, old_middle[edit.index], old_number, 0});
                ++old_number;
                break;
            case Edit::Kind::Added:
                out.push_back(DiffLine{DiffLine::Kind::Added, new_middle[edit.index], 0, new_number});
                ++new_number;
                break;
        }
    }

    for (size_t i = 0; i < suffix; ++i) {
        const size_t old_index = old_lines.size() - suffix + i;
        out.push_back(DiffLine{DiffLine::Kind::Context, old_lines[old_index], old_number, new_number});
        ++old_number;
        ++new_number;
    }
    return out;
}

std::string unified_diff(std::string_view before, std::string_view after) {
    const auto lines = diff_lines(before, after);
    if (lines.empty()) return {};

    // The lines that changed, and the runs they form.
    std::vector<std::pair<size_t, size_t>> runs;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].kind == DiffLine::Kind::Context) continue;
        if (!runs.empty() && runs.back().second == i) {
            runs.back().second = i + 1;
        } else {
            runs.emplace_back(i, i + 1);
        }
    }
    if (runs.empty()) return {};

    // The runs that are close to one another belong to the same hunk.
    std::vector<std::pair<size_t, size_t>> hunks;
    for (const auto& run : runs) {
        if (!hunks.empty() && run.first - hunks.back().second <= 2 * kDiffContext) {
            hunks.back().second = run.second;
        } else {
            hunks.push_back(run);
        }
    }

    std::string out;
    for (const auto& hunk : hunks) {
        const size_t begin = hunk.first > kDiffContext ? hunk.first - kDiffContext : 0;
        const size_t end = std::min(lines.size(), hunk.second + kDiffContext);

        // The header of the hunk: where it starts in the old file and in the
        // new one, and how many lines it holds on either side.
        int old_start = 0;
        int new_start = 0;
        int old_count = 0;
        int new_count = 0;
        for (size_t i = begin; i < end; ++i) {
            const auto& line = lines[i];
            if (line.old_line > 0 && old_start == 0) old_start = line.old_line;
            if (line.new_line > 0 && new_start == 0) new_start = line.new_line;
            if (line.kind != DiffLine::Kind::Added) ++old_count;
            if (line.kind != DiffLine::Kind::Removed) ++new_count;
        }

        out += "@@ -" + std::to_string(old_start) + "," + std::to_string(old_count) + " +" +
               std::to_string(new_start) + "," + std::to_string(new_count) + " @@\n";
        for (size_t i = begin; i < end; ++i) {
            const auto& line = lines[i];
            switch (line.kind) {
            case DiffLine::Kind::Context:
                out += " " + line.text + "\n";
                break;
            case DiffLine::Kind::Removed:
                out += "-" + line.text + "\n";
                break;
            case DiffLine::Kind::Added:
                out += "+" + line.text + "\n";
                break;
            }
        }
    }
    return out;
}

std::string diff_summary(const std::vector<DiffLine>& lines) {
    int additions = 0;
    int removals = 0;
    for (const auto& line : lines) {
        if (line.kind == DiffLine::Kind::Added) ++additions;
        if (line.kind == DiffLine::Kind::Removed) ++removals;
    }

    Style bold;
    bold.bold = true;

    std::string out;
    if (additions > 0) {
        out += "Added ";
        out += styled(std::to_string(additions), bold);
        out += additions > 1 ? " lines" : " line";
    }
    if (additions > 0 && removals > 0) out += ", ";
    if (removals > 0) {
        out += additions == 0 ? "Removed " : "removed ";
        out += styled(std::to_string(removals), bold);
        out += removals > 1 ? " lines" : " line";
    }
    return out;
}

std::vector<std::string> render_diff(const std::vector<DiffLine>& lines, const Theme& theme, int width) {
    const int safe_width = std::max(1, width);

    int max_number = 1;
    for (const auto& line : lines) {
        max_number = std::max(max_number, line.old_line);
        max_number = std::max(max_number, line.new_line);
    }
    const int field = digit_count(max_number) + 1;

    std::vector<std::string> out;
    out.reserve(lines.size());
    for (const auto& line : lines) {
        const int number = line.kind == DiffLine::Kind::Added ? line.new_line : line.old_line;
        std::string gutter;
        if (number > 0) {
            std::string digits = std::to_string(number);
            if (static_cast<int>(digits.size()) < field) {
                gutter.append(static_cast<size_t>(field) - digits.size(), ' ');
            }
            gutter += digits;
        } else {
            gutter.append(static_cast<size_t>(field), ' ');
        }
        gutter += ' ';
        switch (line.kind) {
            case DiffLine::Kind::Added:
                gutter += '+';
                break;
            case DiffLine::Kind::Removed:
                gutter += '-';
                break;
            case DiffLine::Kind::Context:
                gutter += ' ';
                break;
        }

        const std::string content = line.text;
        const size_t used = cc::display_width(gutter) + cc::display_width(content);
        const size_t padding = used < static_cast<size_t>(safe_width)
                                   ? static_cast<size_t>(safe_width) - used
                                   : 0;
        const std::string body = content + std::string(padding, ' ');

        // The reference draws the row in two pieces: the gutter, which
        // carries the number and the sign, and the text of the line. The
        // gutter of an unchanged line is dimmed, its text is not; a changed
        // line carries the background of the theme on both pieces, which
        // the terminal then draws as one run.
        switch (line.kind) {
            case DiffLine::Kind::Added:
                out.push_back(styled(gutter + body, Style::bg(theme.diff_added)));
                break;
            case DiffLine::Kind::Removed:
                out.push_back(styled(gutter + body, Style::bg(theme.diff_removed)));
                break;
            case DiffLine::Kind::Context: {
                Style gutter_style;
                gutter_style.dim = true;
                out.push_back(styled(gutter, gutter_style) + body);
                break;
            }
        }
    }
    return out;
}

} // namespace cc::ui
