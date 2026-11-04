#include "ui/progress_bar.hpp"

#include "ui/ansi.hpp"

#include <algorithm>
#include <cmath>

namespace cc::ui {

std::string progress_bar(const Theme& theme, double ratio, int width) {
    static const char* const kBlocks[] = {" ", "▏", "▎", "▍", "▌", "▋", "▊", "▉", "█"};

    ratio = std::min(1.0, std::max(0.0, ratio));
    if (width < 1) return {};

    const int whole = static_cast<int>(std::floor(ratio * width));

    std::string segments;
    for (int i = 0; i < whole; ++i) segments += "█";
    if (whole < width) {
        const double remainder = ratio * width - whole;
        const int middle = static_cast<int>(std::floor(remainder * 9));
        segments += kBlocks[std::min(middle, 8)];
        segments += std::string(static_cast<size_t>(width - whole - 1), ' ');
    }

    Style style;
    style.foreground = theme.rate_limit_fill;
    style.background = theme.rate_limit_empty;
    return styled(segments, style);
}

} // namespace cc::ui
