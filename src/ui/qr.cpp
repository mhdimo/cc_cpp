#include "ui/qr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace cc::ui {
namespace {

/// The number of the codewords of a version, the correction of the errors
/// included.
constexpr int kCodewords[40] = {26,  44,  70,  100, 134, 172, 196, 242, 292, 346,
                                404, 466, 532, 581, 655, 733, 815, 901, 991, 1085,
                                1156, 1258, 1364, 1474, 1588, 1706, 1828, 1921, 2051,
                                2185, 2323, 2465, 2611, 2761, 2876, 3034, 3196, 3362,
                                3532, 3706};

/// The number of the codewords of the correction of the errors of a version,
/// for the level that is low and for the middle one.
constexpr int kEcCodewords[40][2] = {
    {7, 10},     {10, 16},    {15, 26},    {20, 36},    {26, 48},    {36, 64},
    {40, 72},    {48, 88},    {60, 110},   {72, 130},   {80, 150},   {96, 176},
    {104, 198},  {120, 216},  {132, 240},  {144, 280},  {168, 308},  {180, 338},
    {196, 364},  {224, 416},  {224, 442},  {252, 476},  {270, 504},  {300, 560},
    {312, 588},  {336, 644},  {360, 700},  {390, 728},  {420, 784},  {450, 812},
    {480, 868},  {510, 924},  {540, 980},  {570, 1036}, {570, 1064}, {600, 1120},
    {630, 1204}, {660, 1260}, {720, 1316}, {750, 1372}};

/// The number of the blocks of a version, for the level that is low and for
/// the middle one.
constexpr int kBlocks[40][2] = {
    {1, 1},   {1, 1},   {1, 1},   {1, 2},   {1, 2},   {2, 4},   {2, 4},   {2, 4},
    {2, 5},   {4, 5},   {4, 5},   {4, 8},   {4, 9},   {4, 9},   {6, 10},  {6, 10},
    {6, 11},  {6, 13},  {7, 14},  {8, 16},  {8, 17},  {9, 17},  {9, 18},  {10, 20},
    {12, 21}, {12, 23}, {12, 25}, {13, 26}, {14, 28}, {15, 29}, {16, 31}, {17, 33},
    {18, 35}, {19, 37}, {19, 38}, {20, 40}, {21, 43}, {22, 45}, {24, 47}, {25, 49}};

/// The places of the patterns of the alignment of a version.
///
/// The patterns stand symmetrically on the diagonal of the code, and the
/// first and the last of them are always the same.
std::vector<int> alignment_positions(int version) {
    if (version == 1) return {};

    const int count = version / 7 + 2;
    const int size = version * 4 + 17;
    const int span = 2 * count - 2;
    const int intervals = size == 145 ? 26 : ((size - 13 + span - 1) / span) * 2;

    std::vector<int> positions;
    positions.push_back(size - 7);
    for (int i = 1; i < count - 1; ++i) {
        positions.push_back(positions.back() - intervals);
    }
    positions.push_back(6);
    std::reverse(positions.begin(), positions.end());
    return positions;
}

/// The mode a part of the text is written in.
enum class Mode {
    Numeric,      ///< The mode of the digits.
    Alphanumeric, ///< The mode of the letters and the symbols.
    Byte,         ///< The mode of everything else.
};

/// The characters the mode of the letters and the symbols knows.
constexpr std::string_view kAlphanumeric =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

/// True when the character is a digit.
bool is_digit(char c) { return c >= '0' && c <= '9'; }

/// True when the character is one the mode of the letters and the symbols
/// knows.
///
/// The mode of the letters and the symbols does not know the small letters.
bool is_alphanumeric(char c) {
    return (c >= 'A' && c <= 'Z') || c == ' ' || c == '$' || c == '%' || c == '*' || c == '+' ||
           c == '-' || c == '.' || c == '/' || c == ':';
}

/// The mode a character is written in.
Mode mode_of(char c) {
    if (is_digit(c)) return Mode::Numeric;
    if (is_alphanumeric(c)) return Mode::Alphanumeric;
    return Mode::Byte;
}

/// The number of the bits of the number of the characters of a segment.
int count_bits(Mode mode, int version) {
    const int bits[3][3] = {
        {10, 12, 14},  // the digits
        {9, 11, 13},   // the letters and the symbols
        {8, 16, 16},   // everything else
    };
    const int row = mode == Mode::Numeric ? 0 : (mode == Mode::Alphanumeric ? 1 : 2);
    const int group = version < 10 ? 0 : (version < 27 ? 1 : 2);
    return bits[row][group];
}

/// The number of the bits of the data of a segment.
int data_bits(Mode mode, size_t length) {
    switch (mode) {
        case Mode::Numeric:
            return 10 * static_cast<int>(length / 3) +
                   (length % 3 ? static_cast<int>(length % 3) * 3 + 1 : 0);
        case Mode::Alphanumeric:
            return 11 * static_cast<int>(length / 2) + 6 * static_cast<int>(length % 2);
        default:
            return 8 * static_cast<int>(length);
    }
}

/// One part of the text, and the mode it may be written in.
struct Run {
    /// The mode the run is written in.
    Mode mode = Mode::Byte;

    /// The text of the run.
    std::string data;

    /// The number of the characters of the run; the bytes for the mode of
    /// everything else.
    size_t length = 0;
};

/// The parts of the text, the way the reference splits them.
///
/// The text is split into the runs of the digits, the runs of the letters and
/// the symbols, and the runs of everything else.
std::vector<Run> runs_of(std::string_view text) {
    std::vector<Run> runs;

    size_t index = 0;
    while (index < text.size()) {
        const Mode mode = mode_of(text[index]);
        size_t end = index;
        while (end < text.size() && mode_of(text[end]) == mode) ++end;

        Run run;
        run.mode = mode;
        run.data = std::string(text.substr(index, end - index));
        run.length = mode == Mode::Byte ? run.data.size() : run.data.size();
        runs.push_back(std::move(run));
        index = end;
    }

    return runs;
}

/// The number of the bits of the runs of the text.
long total_bits(const std::vector<Run>& runs, int version) {
    long bits = 0;
    for (const Run& run : runs) {
        bits += 4 + count_bits(run.mode, version) + data_bits(run.mode, run.length);
    }
    return bits;
}

/// One node of the graph of the parts of the text.
struct Node {
    /// The mode the part is written in.
    Mode mode = Mode::Byte;

    /// The text of the part.
    std::string data;

    /// The number of the characters of the part.
    size_t length = 0;

    /// The number of the characters of the segment the part belongs to,
    /// when the mode does not change.
    size_t last_count = 0;

    /// The cost of the shortest path to the node.
    long cost = 0;

    /// The node of the layer before that the path comes from.
    int parent = -1;
};

/// The parts of the text, the shortest way of writing them.
///
/// Every run of the text may be written in more than one mode; the parts
/// that hold the shortest stream of bits are the ones the code keeps.
std::vector<Run> shortest_parts(const std::vector<Run>& runs, int version) {
    if (runs.empty()) return {};

    // The layers of the graph: every run of the text is a layer, and every
    // layer holds the ways the run may be written in.
    std::vector<std::vector<Node>> layers;
    for (const Run& run : runs) {
        std::vector<Node> layer;

        const auto add = [&](Mode mode) {
            Node node;
            node.mode = mode;
            node.data = run.data;
            node.length = run.length;
            layer.push_back(std::move(node));
        };

        switch (run.mode) {
            case Mode::Numeric:
                add(Mode::Numeric);
                add(Mode::Alphanumeric);
                add(Mode::Byte);
                break;
            case Mode::Alphanumeric:
                add(Mode::Alphanumeric);
                add(Mode::Byte);
                break;
            default:
                add(Mode::Byte);
                break;
        }
        layers.push_back(std::move(layer));
    }

    // The cost of the path from the node of a layer to the node of the next
    // one.
    std::vector<std::vector<std::vector<long>>> edges;
    edges.resize(layers.size());

    for (size_t layer = 1; layer < layers.size(); ++layer) {
        edges[layer].assign(layers[layer].size(), {});
        for (size_t node = 0; node < layers[layer].size(); ++node) {
            edges[layer][node].resize(layers[layer - 1].size(), 0);
            for (size_t previous = 0; previous < layers[layer - 1].size(); ++previous) {
                Node& from = layers[layer - 1][previous];
                const Node& to = layers[layer][node];

                if (from.mode == to.mode) {
                    // The segment goes on, and the bits of the part are only
                    // the ones it adds.
                    edges[layer][node][previous] =
                        data_bits(to.mode, from.last_count + to.length) -
                        data_bits(to.mode, from.last_count);
                    from.last_count += to.length;
                } else {
                    // The segment starts, and the mode and the number of the
                    // characters are written as well.
                    edges[layer][node][previous] = data_bits(to.mode, to.length) + 4 +
                                                   count_bits(to.mode, version);
                    from.last_count = to.length;
                }
            }
        }
    }

    // The shortest path through the layers.
    for (size_t node = 0; node < layers[0].size(); ++node) {
        layers[0][node].cost = data_bits(layers[0][node].mode, layers[0][node].length) + 4 +
                               count_bits(layers[0][node].mode, version);
        layers[0][node].parent = -1;
    }

    for (size_t layer = 1; layer < layers.size(); ++layer) {
        for (size_t node = 0; node < layers[layer].size(); ++node) {
            long best = -1;
            int parent = -1;
            for (size_t previous = 0; previous < layers[layer - 1].size(); ++previous) {
                const long cost = layers[layer - 1][previous].cost + edges[layer][node][previous];
                if (best < 0 || cost < best) {
                    best = cost;
                    parent = static_cast<int>(previous);
                }
            }
            layers[layer][node].cost = best;
            layers[layer][node].parent = parent;
        }
    }

    // The path, from the end to the beginning.
    size_t layer = layers.size() - 1;
    size_t node = 0;
    for (size_t i = 1; i < layers.back().size(); ++i) {
        if (layers.back()[i].cost < layers.back()[node].cost) node = i;
    }

    std::vector<Run> parts;
    while (true) {
        const Node& current = layers[layer][node];
        Run part;
        part.mode = current.mode;
        part.data = current.data;
        part.length = current.length;
        parts.push_back(std::move(part));

        if (current.parent < 0) break;
        node = static_cast<size_t>(current.parent);
        --layer;
    }
    std::reverse(parts.begin(), parts.end());

    // The parts that follow each other and are written in the same mode are
    // one segment.
    std::vector<Run> segments;
    for (Run& part : parts) {
        if (!segments.empty() && segments.back().mode == part.mode) {
            segments.back().data += part.data;
            segments.back().length = segments.back().mode == Mode::Byte
                                         ? segments.back().data.size()
                                         : segments.back().length + part.length;
            continue;
        }
        segments.push_back(std::move(part));
    }

    return segments;
}

/// The product of two numbers of the field of 256, the way the codes of the
/// correction of the errors need it.
uint8_t multiply(uint8_t one, uint8_t two) {
    // The number is kept wide enough to hold the bit that the reduction of
    // the field looks at.
    uint16_t value = one;
    uint16_t result = 0;
    while (two != 0) {
        if (two & 1) result ^= value;
        two >>= 1;
        value <<= 1;
        if (value & 0x100) value ^= 0x11D;
    }
    return static_cast<uint8_t>(result);
}

/// The polynomial that gives the correction of the errors of `degree`
/// codewords.
std::vector<uint8_t> generator(int degree) {
    std::vector<uint8_t> polynomial{1};
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        std::vector<uint8_t> next(polynomial.size() + 1, 0);
        for (size_t j = 0; j < polynomial.size(); ++j) {
            next[j] ^= polynomial[j];
            next[j + 1] ^= multiply(polynomial[j], root);
        }
        polynomial = std::move(next);
        root = multiply(root, 2);
    }
    return polynomial;
}

/// The correction of the errors of a block of codewords.
std::vector<uint8_t> correction(const std::vector<uint8_t>& data, const std::vector<uint8_t>& polynomial) {
    std::vector<uint8_t> result(polynomial.size() - 1, 0);
    for (const uint8_t byte : data) {
        const uint8_t factor = static_cast<uint8_t>(byte ^ result[0]);
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < result.size(); ++i) {
            result[i] = static_cast<uint8_t>(result[i] ^ multiply(polynomial[i + 1], factor));
        }
    }
    return result;
}

/// The bits of a number, the most important one first.
std::vector<bool> bits_of(uint32_t value, int count) {
    std::vector<bool> bits;
    bits.reserve(static_cast<size_t>(count));
    for (int i = count - 1; i >= 0; --i) {
        bits.push_back(((value >> i) & 1) != 0);
    }
    return bits;
}

/// The bits of the correction of the errors of the format, the way the code
/// holds them.
std::vector<bool> format_bits(uint32_t format) {
    uint32_t remainder = format << 10;
    for (int i = 4; i >= 0; --i) {
        if (remainder & (1u << (i + 10))) remainder ^= 0x537u << i;
    }
    remainder &= 0x3FF;
    return bits_of(((format << 10) | remainder) ^ 0x5412u, 15);
}

/// The bits of the number of the version, the way the code holds them.
///
/// The bits are held from the least important one, the way the code places
/// them.
std::vector<bool> version_bits(uint32_t version) {
    uint32_t remainder = version << 12;
    for (int i = 5; i >= 0; --i) {
        if (remainder & (1u << (i + 12))) remainder ^= 0x1F25u << i;
    }
    remainder &= 0xFFF;

    const uint32_t value = (version << 12) | remainder;
    std::vector<bool> bits;
    bits.reserve(18);
    for (int i = 0; i < 18; ++i) bits.push_back(((value >> i) & 1) != 0);
    return bits;
}

/// The matrix of the code, with the places of the patterns of the code
/// marked.
struct Matrix {
    /// The modules of the code.
    std::vector<std::vector<bool>> modules;

    /// The places that hold a pattern of the code, which the data does not
    /// touch.
    std::vector<std::vector<bool>> reserved;

    /// The size of the code, in modules.
    int size = 0;

    /// True when the module at the place may hold data.
    bool free(int x, int y) const {
        return x >= 0 && y >= 0 && x < size && y < size && !reserved[static_cast<size_t>(y)]
                                                          [static_cast<size_t>(x)];
    }
};

/// Puts the finder patterns, the separators, the timing patterns, the
/// patterns of the alignment and the dark module in place.
void draw_patterns(Matrix& matrix, int version) {
    const int size = matrix.size;

    // The finder patterns, with the separators around them.
    const int corners[3][2] = {{0, 0}, {0, size - 7}, {size - 7, 0}};
    for (const auto& corner : corners) {
        const int left = corner[0];
        const int top = corner[1];
        for (int dy = -1; dy <= 7; ++dy) {
            for (int dx = -1; dx <= 7; ++dx) {
                const int x = left + dx;
                const int y = top + dy;
                if (x < 0 || y < 0 || x >= size || y >= size) continue;
                const bool inside = dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6;
                const bool dark = inside && (dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                                             (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4));
                matrix.modules[static_cast<size_t>(y)][static_cast<size_t>(x)] = dark;
                matrix.reserved[static_cast<size_t>(y)][static_cast<size_t>(x)] = true;
            }
        }
    }

    // The timing patterns.
    for (int i = 8; i < size - 8; ++i) {
        const bool dark = (i % 2) == 0;
        matrix.modules[static_cast<size_t>(6)][static_cast<size_t>(i)] = dark;
        matrix.reserved[static_cast<size_t>(6)][static_cast<size_t>(i)] = true;
        matrix.modules[static_cast<size_t>(i)][static_cast<size_t>(6)] = dark;
        matrix.reserved[static_cast<size_t>(i)][static_cast<size_t>(6)] = true;
    }

    // The patterns of the alignment.
    const std::vector<int> places = alignment_positions(version);
    const int count = static_cast<int>(places.size());
    for (int a = 0; a < count; ++a) {
        for (int b = 0; b < count; ++b) {
            // The patterns that would touch a finder pattern are not drawn.
            if ((a == 0 && b == 0) || (a == 0 && b == count - 1) ||
                (a == count - 1 && b == 0)) {
                continue;
            }
            const int centre_y = places[a];
            const int centre_x = places[b];
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int x = centre_x + dx;
                    const int y = centre_y + dy;
                    const bool dark = dx == -2 || dx == 2 || dy == -2 || dy == 2 ||
                                      (dx == 0 && dy == 0);
                    matrix.modules[static_cast<size_t>(y)][static_cast<size_t>(x)] = dark;
                    matrix.reserved[static_cast<size_t>(y)][static_cast<size_t>(x)] = true;
                }
            }
        }
    }

    // The dark module.
    matrix.modules[static_cast<size_t>(size - 8)][8] = true;
    matrix.reserved[static_cast<size_t>(size - 8)][8] = true;

    // The places of the format of the code.
    for (int i = 0; i < 9; ++i) {
        if (i != 6) {
            matrix.reserved[8][static_cast<size_t>(i)] = true;
            matrix.reserved[static_cast<size_t>(i)][8] = true;
        }
    }
    for (int i = 0; i < 8; ++i) {
        matrix.reserved[static_cast<size_t>(size - 1 - i)][8] = true;
        matrix.reserved[8][static_cast<size_t>(size - 1 - i)] = true;
    }

    // The places of the number of the version.
    if (version >= 7) {
        for (int i = 0; i < 6; ++i) {
            for (int j = 0; j < 3; ++j) {
                matrix.reserved[static_cast<size_t>(i)][static_cast<size_t>(size - 11 + j)] = true;
                matrix.reserved[static_cast<size_t>(size - 11 + j)][static_cast<size_t>(i)] = true;
            }
        }
    }
}

/// True when the place is under the mask of that number.
bool masked(int mask, int x, int y) {
    switch (mask) {
        case 0:
            return (x + y) % 2 == 0;
        case 1:
            return y % 2 == 0;
        case 2:
            return x % 3 == 0;
        case 3:
            return (x + y) % 3 == 0;
        case 4:
            return (y / 2 + x / 3) % 2 == 0;
        case 5:
            return x * y % 2 + x * y % 3 == 0;
        case 6:
            return (x * y % 2 + x * y % 3) % 2 == 0;
        default:
            return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

/// The penalty of the shapes that hide the code, the way the library of the
/// reference counts them.
///
/// The shape is one dark, one light, three dark, one light and one dark, and
/// four light modules before it or after it.
int shape_penalty(const Matrix& matrix) {
    const int size = matrix.size;
    int points = 0;

    for (int row = 0; row < size; ++row) {
        int bits_col = 0;
        int bits_row = 0;
        for (int col = 0; col < size; ++col) {
            bits_col = ((bits_col << 1) & 0x7FF) | (matrix.modules[static_cast<size_t>(row)]
                                                        [static_cast<size_t>(col)]
                                                            ? 1
                                                            : 0);
            if (col >= 10 && (bits_col == 0x5D0 || bits_col == 0x05D)) ++points;

            bits_row = ((bits_row << 1) & 0x7FF) | (matrix.modules[static_cast<size_t>(col)]
                                                        [static_cast<size_t>(row)]
                                                            ? 1
                                                            : 0);
            if (col >= 10 && (bits_row == 0x5D0 || bits_row == 0x05D)) ++points;
        }
    }

    return points * 40;
}

/// The penalty of a code, the way the code chooses the mask that hides the
/// code the best.
int penalty(const Matrix& matrix) {
    const int size = matrix.size;
    int total = 0;

    // The runs of the same colour.
    for (int row = 0; row < size; ++row) {
        int same_col = 0;
        int same_row = 0;
        bool last_col = false;
        bool last_row = false;
        bool first = true;

        for (int col = 0; col < size; ++col) {
            const bool module = matrix.modules[static_cast<size_t>(row)][static_cast<size_t>(col)];
            if (!first && module == last_col) {
                ++same_col;
            } else {
                if (same_col >= 5) total += 3 + (same_col - 5);
                last_col = module;
                same_col = 1;
            }

            const bool other = matrix.modules[static_cast<size_t>(col)][static_cast<size_t>(row)];
            if (!first && other == last_row) {
                ++same_row;
            } else {
                if (same_row >= 5) total += 3 + (same_row - 5);
                last_row = other;
                same_row = 1;
            }
            first = false;
        }

        if (same_col >= 5) total += 3 + (same_col - 5);
        if (same_row >= 5) total += 3 + (same_row - 5);
    }

    // The squares of the same colour.
    for (int row = 0; row + 1 < size; ++row) {
        for (int col = 0; col + 1 < size; ++col) {
            const int last = (matrix.modules[static_cast<size_t>(row)][static_cast<size_t>(col)] ? 1 : 0) +
                             (matrix.modules[static_cast<size_t>(row)][static_cast<size_t>(col + 1)] ? 1 : 0) +
                             (matrix.modules[static_cast<size_t>(row + 1)][static_cast<size_t>(col)] ? 1 : 0) +
                             (matrix.modules[static_cast<size_t>(row + 1)][static_cast<size_t>(col + 1)] ? 1 : 0);
            if (last == 4 || last == 0) total += 3;
        }
    }

    // The shapes that hide the code.
    total += shape_penalty(matrix);

    // The share of the dark modules.
    int dark = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (matrix.modules[static_cast<size_t>(y)][static_cast<size_t>(x)]) ++dark;
        }
    }
    const int modules = size * size;
    const double percent = static_cast<double>(dark) * 100.0 / static_cast<double>(modules);
    const int steps = static_cast<int>(std::ceil(percent / 5.0));
    total += std::abs(steps - 10) * 10;

    return total;
}

/// Puts the format of the code in place, the way the code holds it around
/// the finder patterns.
void place_format(Matrix& matrix, int mask, bool low_level) {
    const int size = matrix.size;
    const uint32_t level_bits = low_level ? 0x01 : 0x00;
    const std::vector<bool> format = format_bits((level_bits << 3) | static_cast<uint32_t>(mask));

    for (int i = 0; i < 15; ++i) {
        const bool dark = format[static_cast<size_t>(i)];

        // The copy around the finder of the top left.
        if (i < 6) {
            matrix.modules[8][static_cast<size_t>(i)] = dark;
        } else if (i == 6) {
            matrix.modules[8][7] = dark;
        } else if (i == 7) {
            matrix.modules[8][8] = dark;
        } else if (i == 8) {
            matrix.modules[7][8] = dark;
        } else {
            matrix.modules[static_cast<size_t>(14 - i)][8] = dark;
        }

        // The copy on the other sides.
        if (i < 7) {
            matrix.modules[static_cast<size_t>(size - 1 - i)][8] = dark;
        } else {
            matrix.modules[8][static_cast<size_t>(size - 15 + i)] = dark;
        }
    }

    // The dark module.
    matrix.modules[static_cast<size_t>(size - 8)][8] = true;
}

} // namespace

std::optional<QrCode> QrCode::encode(std::string_view text, Level level) {
    const int level_index = level == Level::Low ? 0 : 1;

    // The number of the bits a version holds.
    const auto capacity_of = [&](int version) {
        const int data_total = kCodewords[version - 1] - kEcCodewords[version - 1][level_index];
        return data_total * 8;
    };

    // The runs of the text, and the version they need.
    const std::vector<Run> runs = runs_of(text);
    int estimated = 0;
    for (int candidate = 1; candidate <= 40; ++candidate) {
        if (total_bits(runs, candidate) <= capacity_of(candidate)) {
            estimated = candidate;
            break;
        }
    }
    if (estimated == 0) return std::nullopt;

    // The parts of the text, the shortest way of writing them.
    const std::vector<Run> segments = shortest_parts(runs, estimated);

    // The version that holds the parts.
    int version = 0;
    for (int candidate = 1; candidate <= 40; ++candidate) {
        if (total_bits(segments, candidate) <= capacity_of(candidate)) {
            version = candidate;
            break;
        }
    }
    if (version == 0) return std::nullopt;

    // The blocks of the code, and the correction of the errors of each of
    // them.
    const int total_codewords = kCodewords[version - 1];
    const int ec_total = kEcCodewords[version - 1][level_index];
    const int data_total = total_codewords - ec_total;
    const int block_count = kBlocks[version - 1][level_index];
    const int second_blocks = total_codewords % block_count;
    const int first_blocks = block_count - second_blocks;
    const int first_length = total_codewords / block_count;
    const int first_data = data_total / block_count;
    const int second_data = first_data + 1;
    const int ec_count = first_length - first_data;
    const int data_codewords = data_total;

    // The bits of the text: the mode, the number of the characters and the
    // characters themselves, for every part.
    std::vector<bool> bits;
    for (const Run& segment : segments) {
        const uint32_t indicator = segment.mode == Mode::Numeric      ? 0x1
                                   : segment.mode == Mode::Alphanumeric ? 0x2
                                                                         : 0x4;
        const std::vector<bool> mode = bits_of(indicator, 4);
        bits.insert(bits.end(), mode.begin(), mode.end());

        const size_t count = segment.mode == Mode::Byte ? segment.data.size() : segment.length;
        const std::vector<bool> count_bits_of_segment =
            bits_of(static_cast<uint32_t>(count), count_bits(segment.mode, version));
        bits.insert(bits.end(), count_bits_of_segment.begin(), count_bits_of_segment.end());

        if (segment.mode == Mode::Numeric) {
            // The digits, in groups of three, and the last one or two.
            size_t index = 0;
            while (index + 3 <= segment.data.size()) {
                uint32_t value = 0;
                for (size_t i = 0; i < 3; ++i) value = value * 10 + static_cast<uint32_t>(
                                                               segment.data[index + i] - '0');
                const std::vector<bool> group = bits_of(value, 10);
                bits.insert(bits.end(), group.begin(), group.end());
                index += 3;
            }
            const size_t remaining = segment.data.size() - index;
            if (remaining > 0) {
                uint32_t value = 0;
                for (size_t i = 0; i < remaining; ++i) {
                    value = value * 10 + static_cast<uint32_t>(segment.data[index + i] - '0');
                }
                const std::vector<bool> group = bits_of(value, static_cast<int>(remaining) * 3 + 1);
                bits.insert(bits.end(), group.begin(), group.end());
            }
        } else if (segment.mode == Mode::Alphanumeric) {
            // The characters, in pairs, and the last one.
            const auto value_of = [](char c) {
                const size_t place = kAlphanumeric.find(c);
                return static_cast<uint32_t>(place == std::string_view::npos ? 0 : place);
            };

            size_t index = 0;
            while (index + 2 <= segment.data.size()) {
                const uint32_t value = value_of(segment.data[index]) * 45 +
                                       value_of(segment.data[index + 1]);
                const std::vector<bool> pair = bits_of(value, 11);
                bits.insert(bits.end(), pair.begin(), pair.end());
                index += 2;
            }
            if (index < segment.data.size()) {
                const std::vector<bool> last = bits_of(value_of(segment.data[index]), 6);
                bits.insert(bits.end(), last.begin(), last.end());
            }
        } else {
            for (const char c : segment.data) {
                const std::vector<bool> byte = bits_of(static_cast<unsigned char>(c), 8);
                bits.insert(bits.end(), byte.begin(), byte.end());
            }
        }
    }

    // The end of the text, and the filling of the codewords.
    const int capacity_bits = data_codewords * 8;
    for (int i = 0; i < 4 && static_cast<int>(bits.size()) < capacity_bits; ++i) bits.push_back(false);
    while (bits.size() % 8 != 0) bits.push_back(false);

    std::vector<uint8_t> data;
    data.reserve(static_cast<size_t>(data_codewords));
    for (size_t i = 0; i < bits.size(); i += 8) {
        uint8_t byte = 0;
        for (size_t j = 0; j < 8; ++j) {
            byte = static_cast<uint8_t>(byte << 1);
            if (bits[i + j]) byte = static_cast<uint8_t>(byte | 1);
        }
        data.push_back(byte);
    }
    const uint8_t padding[2] = {0xEC, 0x11};
    size_t index = 0;
    while (data.size() < static_cast<size_t>(data_codewords)) {
        data.push_back(padding[index % 2]);
        ++index;
    }

    // The blocks, and the correction of the errors of each of them.
    const std::vector<uint8_t> polynomial = generator(ec_count);
    std::vector<std::vector<uint8_t>> data_blocks;
    std::vector<std::vector<uint8_t>> ec_blocks;
    size_t offset = 0;
    for (int i = 0; i < block_count; ++i) {
        const int length = i < first_blocks ? first_data : second_data;
        const std::vector<uint8_t> block(data.begin() + static_cast<long>(offset),
                                        data.begin() + static_cast<long>(offset + length));
        offset += static_cast<size_t>(length);
        data_blocks.push_back(block);
        ec_blocks.push_back(correction(block, polynomial));
    }

    // The codewords of the code, the data and the correction of the errors
    // woven together.
    std::vector<uint8_t> codewords;
    size_t longest = 0;
    for (const auto& block : data_blocks) longest = std::max(longest, block.size());
    for (size_t i = 0; i < longest; ++i) {
        for (const auto& block : data_blocks) {
            if (i < block.size()) codewords.push_back(block[i]);
        }
    }
    for (size_t i = 0; i < static_cast<size_t>(ec_count); ++i) {
        for (const auto& block : ec_blocks) {
            if (i < block.size()) codewords.push_back(block[i]);
        }
    }

    // The bits of the codewords.
    std::vector<bool> code_bits;
    for (const uint8_t byte : codewords) {
        const std::vector<bool> bits_of_byte = bits_of(byte, 8);
        code_bits.insert(code_bits.end(), bits_of_byte.begin(), bits_of_byte.end());
    }

    // The matrix of the code.
    Matrix matrix;
    matrix.size = 17 + 4 * version;
    matrix.modules.assign(static_cast<size_t>(matrix.size),
                           std::vector<bool>(static_cast<size_t>(matrix.size), false));
    matrix.reserved.assign(static_cast<size_t>(matrix.size),
                            std::vector<bool>(static_cast<size_t>(matrix.size), false));
    draw_patterns(matrix, version);

    // The data, in the zigzag of the code.
    size_t bit = 0;
    bool upward = true;
    for (int column = matrix.size - 1; column > 0; column -= 2) {
        // The column of the timing pattern is skipped.
        if (column == 6) column = 5;
        for (int step = 0; step < matrix.size; ++step) {
            const int y = upward ? matrix.size - 1 - step : step;
            for (const int x : {column, column - 1}) {
                if (!matrix.free(x, y)) continue;
                const bool dark = bit < code_bits.size() && code_bits[bit];
                matrix.modules[static_cast<size_t>(y)][static_cast<size_t>(x)] = dark;
                ++bit;
            }
        }
        upward = !upward;
    }

    // The masks, and the one that hides the code the best.
    int best_mask = 0;
    int best_penalty = -1;
    Matrix best = matrix;
    for (int mask = 0; mask < 8; ++mask) {
        Matrix candidate = matrix;
        for (int y = 0; y < candidate.size; ++y) {
            for (int x = 0; x < candidate.size; ++x) {
                if (!candidate.free(x, y)) continue;
                if (masked(mask, x, y)) {
                    candidate.modules[static_cast<size_t>(y)][static_cast<size_t>(x)] =
                        !candidate.modules[static_cast<size_t>(y)][static_cast<size_t>(x)];
                }
            }
        }
        place_format(candidate, mask, level == Level::Low);

        const int value = penalty(candidate);
        if (best_penalty < 0 || value < best_penalty) {
            best_penalty = value;
            best_mask = mask;
            best = candidate;
        }
    }
    (void)best_mask;

    // The format of the code.
    place_format(best, best_mask, level == Level::Low);

    // The number of the version.
    if (version >= 7) {
        const std::vector<bool> number = version_bits(static_cast<uint32_t>(version));
        for (int i = 0; i < 18; ++i) {
            const bool dark = number[static_cast<size_t>(i)];
            const int row = i / 3;
            const int column = i % 3;
            best.modules[static_cast<size_t>(row)][static_cast<size_t>(matrix.size - 11 + column)] = dark;
            best.modules[static_cast<size_t>(matrix.size - 11 + column)][static_cast<size_t>(row)] = dark;
        }
    }

    QrCode code;
    code.modules_ = std::move(best.modules);
    return code;
}

std::vector<std::string> QrCode::lines() const {
    // The margin of the code, the way the reference draws it.
    constexpr int kMargin = 4;
    const size_t size = modules_.size();
    const std::string margin(size + kMargin * 2, ' ');

    std::vector<std::string> out;
    out.reserve(size / 2 + 3);

    // The margin above the code.
    out.push_back(margin);
    out.push_back(margin);

    // The code, two rows of modules on every line.
    for (size_t row = 0; row < size; row += 2) {
        std::string line(kMargin, ' ');
        for (size_t column = 0; column < size; ++column) {
            const bool top = modules_[row][column];
            const bool bottom = row + 1 < size && modules_[row + 1][column];
            if (top && bottom) {
                line += "█";
            } else if (top) {
                line += "▀";
            } else if (bottom) {
                line += "▄";
            } else {
                line += ' ';
            }
        }
        line += std::string(kMargin, ' ');
        out.push_back(std::move(line));
    }

    // The margin below the code. The reference writes the quiet zone of four
    // modules as two lines above the code and two lines below it.
    out.push_back(margin);
    out.push_back(margin);

    return out;
}

} // namespace cc::ui
