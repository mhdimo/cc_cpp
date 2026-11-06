#include "ui/welcome.hpp"

#include "ui/ansi.hpp"
#include "ui/theme.hpp"

#include <string>
#include <utility>
#include <vector>

namespace cc::ui {
namespace {

/// The mark of the version of the session in the art.
///
/// The reference writes the version of the session into the picture; the
/// port does the same, so the mark is replaced by the version.
constexpr char kVersionMark = '\x01';

/// The rows of the Dark theme.
const std::vector<std::vector<std::pair<int, std::string>>> kDark = {
    {{1, "Welcome to Claude Code "}, {2, "v\x01"}},
    {{0, "…………………………………………………………………………………………………………………………………………………………"}},
    {{0, "                                                          "}},
    {{0, "   *                                       █████▓▓░       "}},
    {{0, "                              *         ███▓░     ░░      "}},
    {{0, "        ░░░░░░                        ███▓░              "}},
    {{0, "░░░   ░░░░░░░░░░                      ███▓░             "}},
    {{0, "░░░░░░░░░░░░░░░░░░░    "}, {4, "*"}, {0, "                ██▓░░      ▓      "}},
    {{0, "                                         ░▓▓███▓▓░    "}, {2, " *  "}},
    {{2, "                              ░░░░                        "}},
    {{2, "                           ░░░░░░░░                       "}},
    {{2, "                        ░░░░░░░░░░░░░░░░           "}},
    {{3, "█████████ "}, {0, "                                             "}, {2, "*"}, {0, "  "}},
    {{0, "    "}, {3, "██▄█████▄██"}, {0, "                        "}, {4, "*"}, {0, "                  "}},
    {{0, "   "}, {3, " █████████ "}, {0, "     *                                  "}},
    {{0, "…………………"}, {3, "█ █   █ █"}, {0, "………………………………………………………………………………………………………………"}},
};

/// The rows of the Light theme.
const std::vector<std::vector<std::pair<int, std::string>>> kLight = {
    {{1, "Welcome to Claude Code "}, {2, "v\x01"}},
    {{0, "…………………………………………………………………………………………………………………………………………………………"}},
    {{0, "                                                          "}},
    {{0, "                                                          "}},
    {{0, "                                                          "}},
    {{0, "        ░░░░░░                                           "}},
    {{0, "░░░   ░░░░░░░░░░                                        "}},
    {{0, "░░░░░░░░░░░░░░░░░░░                                       "}},
    {{0, "                                                      "}, {2, "    "}},
    {{2, "                      ░░░░"}, {0, "                     ██    "}, {2, "     "}},
    {{2, "                   ░░░░░░░░░░"}, {0, "               ██▒▒██        "}},
    {{0, "                                     ▒▒      ██   ▒      "}},
    {{3, "█████████ "}, {0, "                         ▒▒░░▒▒      ▒ ▒▒     "}},
    {{3, "██▄█████▄██"}, {0, "                           ▒▒         ▒▒       "}},
    {{3, "█████████ "}, {0, "                          ░          ▒  "}},
    {{0, "…………………"}, {3, "█ █   █ █"}, {0, "……………………………………………………………………░…………………………▒…………"}},
};


/// The style of a piece of the art.
///
/// The pieces carry the name of the colour they are drawn in, the way the
/// reference names them.
Style style_of(const Theme& theme, int id) {
    switch (id) {
    case 1:
        return Style::fg(theme.claude);
    case 2: {
        Style dim;
        dim.dim = true;
        return dim;
    }
    case 3:
        return Style::fg(theme.clawd_body);
    case 4: {
        Style bold;
        bold.bold = true;
        return bold;
    }
    default:
        return Style{};
    }
}

/// The rows of the art, with the styles of their pieces.
std::vector<std::string> draw(const std::vector<std::vector<std::pair<int, std::string>>>& rows,
                               const Theme& theme, const std::string& version) {
    std::vector<std::string> lines;
    lines.reserve(rows.size());
    for (const auto& row : rows) {
        std::string line;
        for (const auto& [id, text] : row) {
            std::string piece = text;
            const size_t mark = piece.find(kVersionMark);
            if (mark != std::string::npos) piece.replace(mark, 1, version);
            line += styled(piece, style_of(theme, id));
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

} // namespace

std::vector<std::string> welcome_lines(const Theme& theme, const std::string& version, bool light) {
    return light ? draw(kLight, theme, version) : draw(kDark, theme, version);
}

} // namespace cc::ui
