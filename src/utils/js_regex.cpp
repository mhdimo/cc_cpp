#include "utils/js_regex.hpp"

#include <cctype>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cc {
namespace {

/// A range of code points.
struct Range {
    uint32_t first;
    uint32_t last;
};

/// True when the code point is in one of the ranges.
bool in_ranges(uint32_t code, std::initializer_list<Range> ranges) {
    for (const Range& range : ranges) {
        if (code >= range.first && code <= range.last) return true;
    }
    return false;
}

/// The white space of the Unicode standard.
bool is_white_space(uint32_t code) {
    return in_ranges(code, {{0x0009, 0x000D},
                            {0x0020, 0x0020},
                            {0x0085, 0x0085},
                            {0x00A0, 0x00A0},
                            {0x1680, 0x1680},
                            {0x2000, 0x200A},
                            {0x2028, 0x2029},
                            {0x202F, 0x202F},
                            {0x205F, 0x205F},
                            {0x3000, 0x3000}});
}

/// The control characters.
bool is_control(uint32_t code) { return in_ranges(code, {{0x0000, 0x001F}, {0x007F, 0x009F}}); }

/// The decimal digits.
bool is_digit(uint32_t code) {
    return in_ranges(code, {{0x0030, 0x0039},
                            {0x0660, 0x0669},
                            {0x06F0, 0x06F9},
                            {0x07C0, 0x07C9},
                            {0x0966, 0x096F},
                            {0x09E6, 0x09EF},
                            {0x0A66, 0x0A6F},
                            {0x0AE6, 0x0AEF},
                            {0x0B66, 0x0B6F},
                            {0x0BE6, 0x0BEF},
                            {0x0C66, 0x0C6F},
                            {0x0CE6, 0x0CEF},
                            {0x0D66, 0x0D6F},
                            {0x0E50, 0x0E59},
                            {0x0ED0, 0x0ED9},
                            {0x0F20, 0x0F29},
                            {0x1040, 0x1049},
                            {0x17E0, 0x17E9},
                            {0x1810, 0x1819},
                            {0xFF10, 0xFF19}});
}

/// The uppercase letters.
bool is_uppercase(uint32_t code) {
    return in_ranges(code, {{0x0041, 0x005A},
                            {0x00C0, 0x00D6},
                            {0x00D8, 0x00DE},
                            {0x0178, 0x0179},
                            {0x0181, 0x0182},
                            {0x0186, 0x0187},
                            {0x0388, 0x038A},
                            {0x038C, 0x038C},
                            {0x038E, 0x038F},
                            {0x0391, 0x03A1},
                            {0x03A3, 0x03AB},
                            {0x0400, 0x042F},
                            {0x0460, 0x0460},
                            {0x0531, 0x0556},
                            {0x1E00, 0x1E95},
                            {0x1F08, 0x1F0F},
                            {0x1F18, 0x1F1D},
                            {0x1F28, 0x1F2F},
                            {0x1F68, 0x1F6F},
                            {0x1FB8, 0x1FB9},
                            {0x1FC8, 0x1FCB},
                            {0x1FD8, 0x1FD9},
                            {0x1FE8, 0x1FEF},
                            {0x2102, 0x2102},
                            {0x2107, 0x2107},
                            {0x210B, 0x210D},
                            {0x2110, 0x2112},
                            {0x2139, 0x213B},
                            {0x2145, 0x2149},
                            {0x2C00, 0x2C2F},
                            {0x2C60, 0x2C7F},
                            {0xFF21, 0xFF3A}});
}

/// The lowercase letters.
bool is_lowercase(uint32_t code) {
    return in_ranges(code, {{0x0061, 0x007A},
                            {0x00B5, 0x00B5},
                            {0x00DF, 0x00F6},
                            {0x00F8, 0x00FF},
                            {0x0131, 0x0131},
                            {0x017E, 0x017E},
                            {0x0250, 0x02AF},
                            {0x03AC, 0x03CE},
                            {0x03D0, 0x03D1},
                            {0x0430, 0x045F},
                            {0x0461, 0x0461},
                            {0x0561, 0x0587},
                            {0x1E00, 0x1E95},
                            {0x1F00, 0x1F07},
                            {0x1F10, 0x1F15},
                            {0x1F20, 0x1F27},
                            {0x1F30, 0x1F37},
                            {0x1F40, 0x1F45},
                            {0x1F50, 0x1F57},
                            {0x1F60, 0x1F67},
                            {0x1F70, 0x1F7F},
                            {0x1F80, 0x1FB7},
                            {0x1FC2, 0x1FC7},
                            {0x1FD0, 0x1FD7},
                            {0x1FE0, 0x1FE7},
                            {0x2100, 0x2101},
                            {0x210A, 0x210A},
                            {0x210E, 0x210F},
                            {0x2113, 0x2113},
                            {0x2118, 0x2118},
                            {0x2128, 0x2128},
                            {0x212A, 0x212A},
                            {0x2132, 0x2132},
                            {0x2184, 0x2184},
                            {0x2C30, 0x2C5F},
                            {0x2D00, 0x2D2F},
                            {0xFF41, 0xFF5A}});
}

/// The titlecase letters.
bool is_titlecase(uint32_t code) {
    return in_ranges(code, {{0x01C4, 0x01C4},
                            {0x01C7, 0x01C7},
                            {0x01CA, 0x01CA},
                            {0x01F1, 0x01F1},
                            {0x1F88, 0x1F8F},
                            {0x1F98, 0x1F9F},
                            {0x1FA8, 0x1FAF},
                            {0x1FBC, 0x1FBC},
                            {0x1FCC, 0x1FCC},
                            {0x1FFC, 0x1FFC}});
}

/// The letters that are neither upper nor lower nor titlecase.
bool is_other_letter(uint32_t code) {
    return in_ranges(code, {{0x00AA, 0x00AA},
                            {0x00BA, 0x00BA},
                            {0x02C7, 0x02C7},
                            {0x02BB, 0x02BC},
                            {0x0370, 0x0373},
                            {0x0376, 0x0377},
                            {0x037B, 0x037D},
                            {0x037F, 0x037F},
                            {0x03D2, 0x03D2},
                            {0x03F2, 0x03D2},
                            {0x03F4, 0x03F5},
                            {0x0483, 0x0487},
                            {0x04F6, 0x04F7},
                            {0x055A, 0x055F},
                            {0x05D0, 0x05EA},
                            {0x0620, 0x064A},
                            {0x0670, 0x0670},
                            {0x06D5, 0x06D5},
                            {0x0710, 0x072F},
                            {0x0904, 0x0939},
                            {0x093D, 0x093D},
                            {0x0950, 0x0950},
                            {0x0958, 0x0961},
                            {0x0985, 0x09B9},
                            {0x0A05, 0x0A39},
                            {0x0A85, 0x0AB9},
                            {0x0B05, 0x0B39},
                            {0x0B85, 0x0BB9},
                            {0x0C05, 0x0C39},
                            {0x0C85, 0x0CB9},
                            {0x0D05, 0x0D3A},
                            {0x0E01, 0x0E30},
                            {0x0E40, 0x0E46},
                            {0x0E81, 0x0EB0},
                            {0x0EDC, 0x0EDF},
                            {0x0F40, 0x0F6C},
                            {0x1000, 0x102A},
                            {0x10A0, 0x10FA},
                            {0x1100, 0x1248},
                            {0x13A0, 0x13F5},
                            {0x1401, 0x166C},
                            {0x1681, 0x169A},
                            {0x16A0, 0x16EA},
                            {0x1700, 0x170C},
                            {0x1780, 0x17B3},
                            {0x1820, 0x1877},
                            {0x1880, 0x18A8},
                            {0x2C00, 0x2C5F},
                            {0x2D00, 0x2D2F},
                            {0x3041, 0x3096},
                            {0x309D, 0x309F},
                            {0x30A1, 0x30FA},
                            {0x3400, 0x4DBF},
                            {0x4E00, 0x9FFF},
                            {0xA000, 0xA48C},
                            {0xA4D0, 0xA4FD},
                            {0xAC00, 0xD7A3},
                            {0xF900, 0xFA6D},
                            {0xFB00, 0xFB06},
                            {0xFB13, 0xFB17},
                            {0xFF21, 0xFF3A},
                            {0xFF41, 0xFF5A},
                            {0xFF66, 0xFFDC},
                            {0x10000, 0x1000B},
                            {0x1D400, 0x1D7CB},
                            {0x20000, 0x2A6DF},
                            {0x2A700, 0x2EBEF},
                            {0x30000, 0x3134F}});
}

/// The letters of the Unicode standard.
bool is_letter(uint32_t code) {
    return is_uppercase(code) || is_lowercase(code) || is_titlecase(code) || is_other_letter(code);
}

/// The letters that are not upper, lower or titlecase.
bool is_modifier_letter(uint32_t code) {
    return in_ranges(code, {{0x02B0, 0x02C1},
                            {0x02C6, 0x02D1},
                            {0x02E0, 0x02E4},
                            {0x037A, 0x037A},
                            {0x1D2C, 0x1D6A},
                            {0x1D78, 0x1D78},
                            {0x1D9B, 0x1DBF},
                            {0x2071, 0x2071},
                            {0x207F, 0x207F},
                            {0x2090, 0x209C},
                            {0x3005, 0x3005},
                            {0x30FC, 0x30FE}});
}

/// The marks of the Unicode standard.
bool is_mark(uint32_t code) {
    return in_ranges(code, {{0x0300, 0x036F},
                            {0x0483, 0x0489},
                            {0x0591, 0x05BD},
                            {0x05BF, 0x05BF},
                            {0x05C1, 0x05C2},
                            {0x05C4, 0x05C5},
                            {0x05C7, 0x05C7},
                            {0x0610, 0x061A},
                            {0x064B, 0x065F},
                            {0x0670, 0x0670},
                            {0x06D6, 0x06DC},
                            {0x06DF, 0x06E4},
                            {0x06E7, 0x06E8},
                            {0x06EA, 0x06ED},
                            {0x0711, 0x0711},
                            {0x0730, 0x074A},
                            {0x07A6, 0x07B0},
                            {0x0816, 0x0819},
                            {0x081B, 0x0823},
                            {0x0900, 0x0903},
                            {0x093A, 0x094F},
                            {0x0951, 0x0957},
                            {0x0962, 0x0963},
                            {0x0981, 0x0983},
                            {0x09BC, 0x09BC},
                            {0x09BE, 0x09C4},
                            {0x0A01, 0x0A03},
                            {0x0E31, 0x0E31},
                            {0x0E34, 0x0E3A},
                            {0x0E47, 0x0E4E},
                            {0x0EB1, 0x0EB1},
                            {0x0EB4, 0x0EBC},
                            {0x0EC8, 0x0ECD},
                            {0x1AB0, 0x1AFF},
                            {0x1DC0, 0x1DFF},
                            {0x20D0, 0x20F0},
                            {0xFE00, 0xFE0F},
                            {0xFE20, 0xFE2F}});
}

/// The punctuation of the Unicode standard.
bool is_punctuation(uint32_t code) {
    return in_ranges(code, {{0x0021, 0x0023},
                            {0x0025, 0x002A},
                            {0x002C, 0x002F},
                            {0x003A, 0x003B},
                            {0x003F, 0x0040},
                            {0x005B, 0x005D},
                            {0x005F, 0x005F},
                            {0x007B, 0x007B},
                            {0x007D, 0x007D},
                            {0x00A1, 0x00A1},
                            {0x00A7, 0x00A7},
                            {0x00AB, 0x00AB},
                            {0x00B6, 0x00B7},
                            {0x00BB, 0x00BB},
                            {0x00BF, 0x00BF},
                            {0x037E, 0x037E},
                            {0x0387, 0x0387},
                            {0x055A, 0x055F},
                            {0x0589, 0x058A},
                            {0x05BE, 0x05BE},
                            {0x05C0, 0x05C0},
                            {0x05C3, 0x05C3},
                            {0x05C6, 0x05C6},
                            {0x05F3, 0x05F4},
                            {0x0609, 0x060A},
                            {0x060C, 0x060D},
                            {0x061B, 0x061B},
                            {0x061E, 0x061F},
                            {0x066A, 0x066D},
                            {0x06D4, 0x06D4},
                            {0x2010, 0x2027},
                            {0x2030, 0x205E},
                            {0x3001, 0x3003},
                            {0x3008, 0x3011},
                            {0x3014, 0x301F},
                            {0xFF01, 0xFF03},
                            {0xFF05, 0xFF0A},
                            {0xFF0C, 0xFF0F},
                            {0xFF1A, 0xFF1B},
                            {0xFF1F, 0xFF20},
                            {0xFF3B, 0xFF3D},
                            {0xFF5B, 0xFF5B},
                            {0xFF5D, 0xFF5D},
                            {0xFF5F, 0xFF65}});
}

/// The symbols of the Unicode standard.
bool is_symbol(uint32_t code) {
    return in_ranges(code, {{0x0024, 0x0024},
                            {0x002B, 0x002B},
                            {0x003C, 0x003E},
                            {0x005E, 0x005E},
                            {0x0060, 0x0060},
                            {0x007C, 0x007C},
                            {0x007E, 0x007E},
                            {0x00A2, 0x00A6},
                            {0x00A8, 0x00A9},
                            {0x00AC, 0x00AC},
                            {0x00AE, 0x00B1},
                            {0x00B4, 0x00B4},
                            {0x00B8, 0x00B8},
                            {0x00D7, 0x00D7},
                            {0x00F7, 0x00F7},
                            {0x02C2, 0x02C5},
                            {0x02D2, 0x02DF},
                            {0x02E5, 0x02EB},
                            {0x02ED, 0x02ED},
                            {0x02EF, 0x02FF},
                            {0x0375, 0x0375},
                            {0x0384, 0x0385},
                            {0x1FBD, 0x1FBD},
                            {0x1FBF, 0x1FC1},
                            {0x1FCD, 0x1FCF},
                            {0x1FDD, 0x1FDF},
                            {0x1FED, 0x1FEF},
                            {0x1FFD, 0x1FFE},
                            {0x2044, 0x2044},
                            {0x2052, 0x2052},
                            {0x20A0, 0x20BF},
                            {0x2100, 0x2101},
                            {0x2103, 0x2106},
                            {0x2108, 0x2109},
                            {0x2114, 0x2114},
                            {0x2116, 0x2117},
                            {0x211E, 0x2123},
                            {0x2125, 0x2125},
                            {0x2127, 0x2127},
                            {0x2129, 0x2129},
                            {0x212E, 0x212E},
                            {0x213A, 0x213B},
                            {0x2140, 0x2144},
                            {0x214A, 0x214D},
                            {0x214F, 0x214F},
                            {0x218A, 0x218B},
                            {0x2190, 0x2426},
                            {0x2440, 0x244A},
                            {0x249C, 0x24E9},
                            {0x2500, 0x2B73},
                            {0x2B76, 0x2B95},
                            {0x2B97, 0x2BFF},
                            {0x2CE5, 0x2CEA},
                            {0x2E00, 0x2E5D},
                            {0x2E80, 0x2E99},
                            {0x2E9B, 0x2EF3},
                            {0x2F00, 0x2FD5},
                            {0x2FF0, 0x2FFB},
                            {0x3004, 0x3004},
                            {0x3012, 0x3013},
                            {0x3020, 0x3020},
                            {0x3036, 0x3037},
                            {0x303E, 0x303F},
                            {0x309B, 0x309C},
                            {0x3190, 0x3191},
                            {0x3196, 0x319F},
                            {0x31C0, 0x31E3},
                            {0x3200, 0x321E},
                            {0x322A, 0x3247},
                            {0x3250, 0x3250},
                            {0x3260, 0x327F},
                            {0x328A, 0x32B0},
                            {0x32C0, 0x33FF},
                            {0x4DC0, 0x4DFF},
                            {0xA700, 0xA716},
                            {0xA720, 0xA721},
                            {0xFB29, 0xFB29},
                            {0xFDFC, 0xFDFD},
                            {0xFE62, 0xFE62},
                            {0xFE64, 0xFE66},
                            {0xFE69, 0xFE69},
                            {0xFF04, 0xFF04},
                            {0xFF0B, 0xFF0B},
                            {0xFF1C, 0xFF1E},
                            {0xFF3E, 0xFF3E},
                            {0xFF40, 0xFF40},
                            {0xFF5C, 0xFF5C},
                            {0xFF5E, 0xFF5E},
                            {0xFFE0, 0xFFE6},
                            {0xFFF9, 0xFFFB}});
}

/// The numbers that are not digits.
bool is_other_number(uint32_t code) {
    return in_ranges(code, {{0x00B2, 0x00B3},
                            {0x00B9, 0x00B9},
                            {0x00BC, 0x00BE},
                            {0x09F4, 0x09F9},
                            {0x2070, 0x2070},
                            {0x2074, 0x2079},
                            {0x2080, 0x2089},
                            {0x2150, 0x215F},
                            {0x2189, 0x2189},
                            {0x2460, 0x249B},
                            {0x24EA, 0x24FF},
                            {0x2776, 0x2793},
                            {0x2CFD, 0x2CFD},
                            {0x3192, 0x3195},
                            {0x3220, 0x3229},
                            {0x3248, 0x324F},
                            {0x3251, 0x325F},
                            {0x3280, 0x3289},
                            {0x32B1, 0x32BF}});
}

/// The numbers that are written as letters.
bool is_letter_number(uint32_t code) {
    return in_ranges(code, {{0x2160, 0x2182},
                            {0x2185, 0x2188},
                            {0x3007, 0x3007},
                            {0x3021, 0x3029}});
}

/// The numbers of the Unicode standard.
bool is_number(uint32_t code) {
    return is_digit(code) || is_letter_number(code) || is_other_number(code);
}

/// The name of the property, without its underscores and in small letters.
std::string property_key(std::string_view name) {
    // The general category may be written with its long name.
    const size_t equals = name.find('=');
    if (equals != std::string_view::npos) name = name.substr(equals + 1);

    std::string key;
    key.reserve(name.size());
    for (const char c : name) {
        if (c == '_' || c == ' ') continue;
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return key;
}

/// The names of the properties the port knows.
const char* const kKnownProperties[] = {"l",
                                         "letter",
                                         "alphabetic",
                                         "alpha",
                                         "lu",
                                         "uppercaseletter",
                                         "uppercase",
                                         "ll",
                                         "lowercaseletter",
                                         "lowercase",
                                         "lt",
                                         "titlecaseletter",
                                         "titlecase",
                                         "lm",
                                         "modifierletter",
                                         "lo",
                                         "otherletter",
                                         "n",
                                         "number",
                                         "nd",
                                         "decimalnumber",
                                         "digit",
                                         "nl",
                                         "letternumber",
                                         "no",
                                         "othernumber",
                                         "p",
                                         "punctuation",
                                         "punct",
                                         "s",
                                         "symbol",
                                         "m",
                                         "mark",
                                         "combiningmark",
                                         "z",
                                         "zs",
                                         "spaceseparator",
                                         "whitespace",
                                         "c",
                                         "cc",
                                         "control",
                                         "ascii",
                                         "any",
                                         "word",
                                         "latin",
                                         "greek",
                                         "cyrillic",
                                         "armenian",
                                         "hebrew",
                                         "arabic",
                                         "syriac",
                                         "devanagari",
                                         "thai",
                                         "georgian",
                                         "hangul",
                                         "hiragana",
                                         "katakana",
                                         "han"};

/// True when the name is one the port knows.
bool property_is_known(std::string_view name) {
    const std::string key = property_key(name);
    for (const char* candidate : kKnownProperties) {
        if (key == candidate) return true;
    }
    return false;
}

/// True when the code point is written in the script.
bool in_script(const std::string& name, uint32_t code) {
    if (name == "latin") {
        return in_ranges(code, {{0x0041, 0x005A},
                                {0x0061, 0x007A},
                                {0x00AA, 0x00AA},
                                {0x00BA, 0x00BA},
                                {0x00C0, 0x00D6},
                                {0x00D8, 0x00F6},
                                {0x00F8, 0x02B8},
                                {0x02E0, 0x02E4},
                                {0x1D00, 0x1D25},
                                {0x1D2C, 0x1D5C},
                                {0x1D62, 0x1D65},
                                {0x1D6B, 0x1D77},
                                {0x1D79, 0x1DBE},
                                {0x1E00, 0x1EFF},
                                {0x2071, 0x2071},
                                {0x207F, 0x207F},
                                {0x2090, 0x209C},
                                {0x212A, 0x212B},
                                {0x2132, 0x2132},
                                {0x214E, 0x214E},
                                {0x2160, 0x2188},
                                {0x2C60, 0x2C7F},
                                {0xA722, 0xA787},
                                {0xA78B, 0xA7AD},
                                {0xA7B0, 0xA7B7},
                                {0xA7F7, 0xA7FF},
                                {0xAB30, 0xAB5A},
                                {0xAB5C, 0xAB64},
                                {0xFB00, 0xFB06},
                                {0xFF21, 0xFF3A},
                                {0xFF41, 0xFF5A}});
    }
    if (name == "greek") {
        return in_ranges(code, {{0x0370, 0x0373},
                                {0x0375, 0x0377},
                                {0x037A, 0x037D},
                                {0x037F, 0x037F},
                                {0x0384, 0x0384},
                                {0x0386, 0x0386},
                                {0x0388, 0x038A},
                                {0x038C, 0x038C},
                                {0x038E, 0x03A1},
                                {0x03A3, 0x03E1},
                                {0x03F0, 0x03FF},
                                {0x1D26, 0x1D2A},
                                {0x1F00, 0x1FFE},
                                {0x2126, 0x2126}});
    }
    if (name == "cyrillic") {
        return in_ranges(code, {{0x0400, 0x0484},
                                {0x0487, 0x052F},
                                {0x1C80, 0x1C88},
                                {0x1D2B, 0x1D2B},
                                {0x1D78, 0x1D78},
                                {0x2DE0, 0x2DFF},
                                {0xA640, 0xA69F},
                                {0xFE2E, 0xFE2F}});
    }
    if (name == "armenian") {
        return in_ranges(code, {{0x0531, 0x0556}, {0x0559, 0x055F}, {0x0561, 0x0587}, {0x058A, 0x058A}});
    }
    if (name == "hebrew") {
        return in_ranges(code, {{0x0591, 0x05C7}, {0x05D0, 0x05EA}, {0x05EF, 0x05F4}, {0xFB1D, 0xFB4F}});
    }
    if (name == "arabic") {
        return in_ranges(code, {{0x0600, 0x0604},
                                {0x0606, 0x060B},
                                {0x060D, 0x061A},
                                {0x061E, 0x061E},
                                {0x0620, 0x063F},
                                {0x0641, 0x064A},
                                {0x0656, 0x066F},
                                {0x0671, 0x06DC},
                                {0x06DE, 0x06FF},
                                {0x0750, 0x077F},
                                {0x08A0, 0x08FF},
                                {0xFB50, 0xFBC1},
                                {0xFBD3, 0xFD3D},
                                {0xFD50, 0xFD8F},
                                {0xFDF0, 0xFDFD},
                                {0xFE70, 0xFEFC}});
    }
    if (name == "syriac") {
        return in_ranges(code, {{0x0700, 0x074A}, {0x074D, 0x074F}});
    }
    if (name == "devanagari") {
        return in_ranges(code, {{0x0900, 0x0950}, {0x0953, 0x0963}, {0x0966, 0x097F},
                                {0xA8E0, 0xA8FF}});
    }
    if (name == "thai") {
        return in_ranges(code, {{0x0E01, 0x0E3A}, {0x0E40, 0x0E5B}});
    }
    if (name == "georgian") {
        return in_ranges(code, {{0x10A0, 0x10C5}, {0x10C7, 0x10C7}, {0x10CD, 0x10CD},
                                {0x10D0, 0x10FA}, {0x10FC, 0x10FF}});
    }
    if (name == "hangul") {
        return in_ranges(code, {{0x1100, 0x11FF}, {0x302E, 0x302F}, {0x3131, 0x318E},
                                {0x3200, 0x321E}, {0x3260, 0x327E}, {0xA960, 0xA97C},
                                {0xAC00, 0xD7A3}, {0xD7B0, 0xD7C6}, {0xD7CB, 0xD7FB},
                                {0xFFA0, 0xFFBE}, {0xFFC2, 0xFFC7}, {0xFFCA, 0xFFCF},
                                {0xFFD2, 0xFFD7}, {0xFFDA, 0xFFDC}});
    }
    if (name == "hiragana") {
        return in_ranges(code, {{0x3041, 0x3096}, {0x309D, 0x309F}, {0x1B001, 0x1B11E},
                                {0x1B150, 0x1B152}});
    }
    if (name == "katakana") {
        return in_ranges(code, {{0x30A1, 0x30FA}, {0x30FD, 0x30FF}, {0x31F0, 0x31FF},
                                {0x32D0, 0x32FE}, {0x3300, 0x3357}, {0xFF66, 0xFF6F},
                                {0xFF71, 0xFF9D}});
    }
    if (name == "han") {
        return in_ranges(code, {{0x2E80, 0x2E99}, {0x2E9B, 0x2EF3}, {0x2F00, 0x2FD5},
                                {0x3005, 0x3005}, {0x3007, 0x3007}, {0x3021, 0x3029},
                                {0x3038, 0x303B}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF},
                                {0xF900, 0xFA6D}, {0xFA70, 0xFAD9}, {0x20000, 0x2A6DF},
                                {0x2A700, 0x2EBEF}, {0x2F800, 0x2FA1D}, {0x30000, 0x3134F}});
    }
    return false;
}

/// True when the code point is in the property of the Unicode standard.
bool property_matches(std::string_view name, uint32_t code) {
    const std::string key = property_key(name);

    if (key == "l" || key == "letter" || key == "alphabetic" || key == "alpha") return is_letter(code);
    if (key == "lu" || key == "uppercaseletter" || key == "uppercase") return is_uppercase(code);
    if (key == "ll" || key == "lowercaseletter" || key == "lowercase") return is_lowercase(code);
    if (key == "lt" || key == "titlecaseletter" || key == "titlecase") return is_titlecase(code);
    if (key == "lm" || key == "modifierletter") return is_modifier_letter(code);
    if (key == "lo" || key == "otherletter") return is_other_letter(code);
    if (key == "n" || key == "number") return is_number(code);
    if (key == "nd" || key == "decimalnumber" || key == "digit") return is_digit(code);
    if (key == "nl" || key == "letternumber") return is_letter_number(code);
    if (key == "no" || key == "othernumber") return is_other_number(code);
    if (key == "p" || key == "punctuation" || key == "punct") return is_punctuation(code);
    if (key == "s" || key == "symbol") return is_symbol(code);
    if (key == "m" || key == "mark" || key == "combiningmark") return is_mark(code);
    if (key == "z" || key == "zs" || key == "spaceseparator" || key == "whitespace") {
        return is_white_space(code);
    }
    if (key == "c" || key == "cc" || key == "control") return is_control(code);
    if (key == "ascii") return code < 0x80;
    if (key == "any") return true;
    if (key == "word") {
        return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
               (code >= '0' && code <= '9') || code == '_';
    }
    // The scripts of the Unicode standard.
    return in_script(key, code);
}

/// The number of bytes of the character at the position.
size_t character_width(std::string_view text, size_t position) {
    if (position >= text.size()) return 0;

    const unsigned char first = static_cast<unsigned char>(text[position]);
    size_t width = 1;
    if ((first & 0xE0) == 0xC0) {
        width = 2;
    } else if ((first & 0xF0) == 0xE0) {
        width = 3;
    } else if ((first & 0xF8) == 0xF0) {
        width = 4;
    }

    if (position + width > text.size()) return 1;

    for (size_t i = 1; i < width; ++i) {
        if ((static_cast<unsigned char>(text[position + i]) & 0xC0) != 0x80) return 1;
    }
    return width;
}

/// The code point of the character at the position.
uint32_t code_point(std::string_view text, size_t position) {
    const size_t width = character_width(text, position);
    const unsigned char first = static_cast<unsigned char>(text[position]);

    switch (width) {
        case 1:
            return first;
        case 2:
            return static_cast<uint32_t>((first & 0x1Fu) << 6) |
                   static_cast<uint32_t>(static_cast<unsigned char>(text[position + 1]) & 0x3Fu);
        case 3:
            return static_cast<uint32_t>((first & 0x0Fu) << 12) |
                   static_cast<uint32_t>((static_cast<unsigned char>(text[position + 1]) & 0x3Fu) << 6) |
                   static_cast<uint32_t>(static_cast<unsigned char>(text[position + 2]) & 0x3Fu);
        case 4:
            return static_cast<uint32_t>((first & 0x07u) << 18) |
                   static_cast<uint32_t>((static_cast<unsigned char>(text[position + 1]) & 0x3Fu) << 12) |
                   static_cast<uint32_t>((static_cast<unsigned char>(text[position + 2]) & 0x3Fu) << 6) |
                   static_cast<uint32_t>(static_cast<unsigned char>(text[position + 3]) & 0x3Fu);
        default:
            return first;
    }
}

/// The start of the character that ends at the position.
size_t previous_character_start(std::string_view text, size_t position) {
    size_t start = position;
    while (start > 0 && (static_cast<unsigned char>(text[start - 1]) & 0xC0) == 0x80) --start;
    return start == position ? position - 1 : start;
}

/// True when the code point is a word character, the way JavaScript knows it.
bool is_word_character(uint32_t code) {
    return (code >= 'a' && code <= 'z') || (code >= 'A' && code <= 'Z') ||
           (code >= '0' && code <= '9') || code == '_';
}

/// True when the position is between a word character and another one.
bool is_word_boundary(std::string_view text, size_t position) {
    bool before = false;
    if (position > 0) {
        before = is_word_character(code_point(text, previous_character_start(text, position)));
    }
    const bool after = position < text.size() && is_word_character(code_point(text, position));
    return before != after;
}

/// True when the code point ends a line.
bool is_line_terminator(uint32_t code) {
    return code == '\n' || code == '\r' || code == 0x2028 || code == 0x2029;
}

/// The lower case of the code point, the way the flag of the case folds it.
uint32_t fold(uint32_t code) {
    if (code >= 'A' && code <= 'Z') return code + 0x20;
    if (code >= 0x00C0 && code <= 0x00DE && code != 0x00D7) return code + 0x20;
    if (code >= 0x0100 && code <= 0x0137) return (code % 2 == 0) ? code + 1 : code;
    if (code >= 0x0139 && code <= 0x0148) return (code % 2 == 1) ? code + 1 : code;
    if (code >= 0x014A && code <= 0x0177) return (code % 2 == 0) ? code + 1 : code;
    if (code >= 0x0179 && code <= 0x017E) return (code % 2 == 1) ? code + 1 : code;
    if (code == 0x0386) return 0x03AC;
    if (code >= 0x0388 && code <= 0x038A) return code + 0x0025;
    if (code == 0x038C) return 0x03CC;
    if (code == 0x038E || code == 0x038F) return code + 0x003F;
    if (code >= 0x0391 && code <= 0x03A1) return code + 0x20;
    if (code >= 0x03A3 && code <= 0x03AB) return code + 0x20;
    if (code >= 0x0400 && code <= 0x040F) return code + 0x0050;
    if (code >= 0x0410 && code <= 0x042F) return code + 0x0020;
    return code;
}

/// The upper case of the code point, the way the flag of the case folds it.
uint32_t upper_case(uint32_t code) {
    if (code >= 'a' && code <= 'z') return code - 0x20;
    if (code >= 0x00E0 && code <= 0x00FE && code != 0x00F7) return code - 0x20;
    if (code >= 0x0100 && code <= 0x0137) return (code % 2 == 1) ? code - 1 : code;
    if (code >= 0x0139 && code <= 0x0148) return (code % 2 == 0) ? code - 1 : code;
    if (code >= 0x014A && code <= 0x0177) return (code % 2 == 1) ? code - 1 : code;
    if (code >= 0x0179 && code <= 0x017E) return (code % 2 == 0) ? code - 1 : code;
    if (code == 0x03AC) return 0x0386;
    if (code >= 0x03AD && code <= 0x03AF) return code - 0x0025;
    if (code == 0x03CC) return 0x038C;
    if (code == 0x03CD || code == 0x03CE) return code - 0x003F;
    if (code >= 0x03B1 && code <= 0x03C1) return code - 0x0020;
    if (code >= 0x03C3 && code <= 0x03CB) return code - 0x0020;
    if (code >= 0x0450 && code <= 0x045F) return code - 0x0050;
    if (code >= 0x0430 && code <= 0x044F) return code - 0x0020;
    return code;
}

} // namespace

/// One piece of a pattern.
struct JsRegex::Piece {
    /// The kind of the piece.
    enum class Kind {
        Literal,        ///< A single character.
        Any,            ///< `.`
        Class,          ///< `[…]`
        Sequence,       ///< A list of pieces, in order.
        Alternative,    ///< The branches of a `|`.
        Repeat,         ///< A piece with a count.
        AnchorStart,    ///< `^`
        AnchorEnd,      ///< `$`
        WordBoundary,   ///< `\b`
        NotWordBoundary,///< `\B`
        Lookahead,      ///< `(?=…)`, `(?!…)`
        Lookbehind,     ///< `(?<=…)`, `(?<!…)`
        Backreference,  ///< `\1`, `\k<name>`
    };

    /// The kind of the piece.
    Kind kind = Kind::Literal;

    /// The character of a literal piece.
    uint32_t literal = 0;

    /// The number of the group a piece captures or refers to.
    int group_number = 0;

    /// True when the class holds everything but its ranges.
    bool negated = false;

    /// The ranges of a class.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;

    /// The properties of a class, and whether they are negated.
    std::vector<std::pair<std::string, bool>> properties;

    /// The children of a sequence, or the branches of an alternative.
    std::vector<std::shared_ptr<Piece>> children;

    /// The piece that is repeated, or the one that is looked at.
    std::shared_ptr<Piece> child;

    /// The least number of times the piece is repeated.
    int min_count = 0;

    /// The most number of times the piece is repeated; -1 for no limit.
    int max_count = -1;

    /// True when the repetition takes as much as it can.
    bool greedy = true;

    /// True when the lookaround has to hold.
    bool positive = true;
};

/// The parser of a pattern.
struct JsRegex::Parser {
    Parser(std::string_view pattern, std::string* error, bool unicode)
        : pattern_(pattern), error_(error), unicode_(unicode) {
        scan_groups();
    }

    /// The branch of an alternation a group stands in.
    using Branch = std::pair<size_t, size_t>;

    /// The branches a group stands in, from the outside in.
    using Branches = std::vector<Branch>;

    /// The pieces of the whole pattern.
    std::vector<PiecePtr> parse() {
        PiecePtr root = parse_alternation();
        if (!root || failed_) return {};

        if (position_ < pattern_.size()) {
            fail("The pattern has something left over");
            return {};
        }
        return {root};
    }

    /// True when the pattern has already failed.
    bool failed() const { return failed_; }

    /// The number of the groups that capture.
    int group_count() const { return group_count_; }

    /// The number of the groups the parser has given a number.
    int numbered_groups() const { return next_group_number_; }

private:
    /// Finds the groups of the pattern and the names they have.
    ///
    /// The reference to a group that has a name may name a group that comes
    /// later, so the names are found before the pattern is read.
    void scan_groups() {
        for (size_t i = 0; i < pattern_.size();) {
            const char c = pattern_[i];

            if (c == '\\') {
                i += 2;
                continue;
            }

            if (c == '[') {
                // The class of characters, which holds no group.
                ++i;
                if (i < pattern_.size() && pattern_[i] == '^') ++i;
                if (i < pattern_.size() && pattern_[i] == ']') {
                    // The class holds no character at all.
                    ++i;
                    continue;
                }
                while (i < pattern_.size() && pattern_[i] != ']') {
                    if (pattern_[i] == '\\') ++i;
                    ++i;
                }
                ++i;
                continue;
            }

            if (c != '(') {
                ++i;
                continue;
            }

            // The group.
            if (i + 1 < pattern_.size() && pattern_[i + 1] == '?') {
                if (i + 2 < pattern_.size() && pattern_[i + 2] == '<' && i + 3 < pattern_.size() &&
                    pattern_[i + 3] != '=' && pattern_[i + 3] != '!') {
                    // The group has a name.
                    const size_t end = pattern_.find('>', i + 3);
                    if (end == std::string_view::npos) {
                        i = pattern_.size();
                        continue;
                    }
                    group_names_[std::string(pattern_.substr(i + 3, end - i - 3))] = ++group_count_;
                    i = end + 1;
                    continue;
                }
                // The group does not capture.
                i += 2;
                continue;
            }

            group_count_ += 1;
            ++i;
        }
    }

    /// The character at the position, or 0 when the pattern has ended.
    char peek() const { return position_ < pattern_.size() ? pattern_[position_] : '\0'; }

    /// Moves past the character at the position.
    void advance() { ++position_; }

    /// Reports the failure of the pattern.
    bool fail(const std::string& message) {
        if (failed_) return false;
        failed_ = true;
        if (error_ != nullptr) {
            *error_ = message + " in /" + std::string(pattern_) + "/";
        }
        return false;
    }

    /// The piece of a literal character.
    PiecePtr literal(uint32_t code) const {
        auto piece = std::make_shared<Piece>();
        piece->kind = Piece::Kind::Literal;
        piece->literal = code;
        return piece;
    }

    /// True when a count may follow a piece of that kind.
    static bool can_be_repeated(Piece::Kind kind) {
        switch (kind) {
            case Piece::Kind::AnchorStart:
            case Piece::Kind::AnchorEnd:
            case Piece::Kind::WordBoundary:
            case Piece::Kind::NotWordBoundary:
            case Piece::Kind::Lookbehind:
                return false;
            default:
                return true;
        }
    }

    /// True when the name may be the name of a group.
    static bool is_group_name(const std::string& name) {
        if (name.empty()) return false;

        for (size_t i = 0; i < name.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
            const bool digit = c >= '0' && c <= '9';
            if (i == 0) {
                if (!letter && c != '_' && c != '$') return false;
                continue;
            }
            if (!letter && !digit && c != '_' && c != '$') return false;
        }
        return true;
    }

    /// True when two groups of the same name stand on the same path.
    ///
    /// Two groups of the same name are allowed when they stand in different
    /// branches of the same alternation, the way JavaScript allows them.
    static bool branches_conflict(const Branches& one, const Branches& other) {
        const size_t common = one.size() < other.size() ? one.size() : other.size();
        for (size_t i = 0; i < common; ++i) {
            if (one[i].first != other[i].first) return true;
            if (one[i].second != other[i].second) return false;
        }
        // One of the paths is the beginning of the other.
        return true;
    }

    /// Records the name of a group, and fails when it is used twice.
    bool declare_name(const std::string& name) {
        for (const auto& entry : named_groups_) {
            if (entry.first != name) continue;
            if (!branches_conflict(entry.second, branch_stack_)) continue;
            fail("The name of the group '" + name + "' is used twice");
            return false;
        }
        named_groups_.emplace_back(name, branch_stack_);
        return true;
    }

    /// The piece of a list of characters.
    PiecePtr sequence(std::vector<PiecePtr> pieces) const {
        auto piece = std::make_shared<Piece>();
        piece->kind = Piece::Kind::Sequence;
        piece->children = std::move(pieces);
        return piece;
    }

    /// The piece of a set of branches.
    PiecePtr alternative(std::vector<PiecePtr> branches) const {
        if (branches.size() == 1) return branches.front();
        auto piece = std::make_shared<Piece>();
        piece->kind = Piece::Kind::Alternative;
        piece->children = std::move(branches);
        return piece;
    }

    /// The branches of the pattern, separated by the `|`.
    PiecePtr parse_alternation() {
        std::vector<PiecePtr> branches;
        const size_t alternation = next_alternation_++;
        size_t index = 0;
        while (true) {
            branch_stack_.emplace_back(alternation, index);
            branches.push_back(sequence(parse_sequence()));
            branch_stack_.pop_back();

            if (peek() != '|') break;
            advance();
            ++index;
        }
        return alternative(std::move(branches));
    }

    /// The pieces of a branch.
    std::vector<PiecePtr> parse_sequence() {
        std::vector<PiecePtr> pieces;
        while (position_ < pattern_.size() && pattern_[position_] != '|' && pattern_[position_] != ')') {
            PiecePtr term = parse_term();
            if (!term) return {};
            pieces.push_back(term);
        }
        return pieces;
    }

    /// A piece with a count.
    PiecePtr parse_term() {
        PiecePtr atom = parse_atom();
        if (!atom) return nullptr;

        int min_count = -1;
        int max_count = -1;
        const char c = peek();
        if (c == '*') {
            min_count = 0;
            max_count = -1;
            advance();
        } else if (c == '+') {
            min_count = 1;
            max_count = -1;
            advance();
        } else if (c == '?') {
            min_count = 0;
            max_count = 1;
            advance();
        } else if (c == '{') {
            if (!parse_count(&min_count, &max_count)) return nullptr;
        }

        if (min_count < 0) return atom;

        // The anchors and the lookbehind cannot be repeated, the way
        // JavaScript reads it.
        if (!can_be_repeated(atom->kind)) {
            fail("There is nothing to repeat");
            return nullptr;
        }

        bool greedy = true;
        if (peek() == '?') {
            greedy = false;
            advance();
        }

        auto piece = std::make_shared<Piece>();
        piece->kind = Piece::Kind::Repeat;
        // The piece that is repeated is held in a list of one, so that the
        // matcher always has a list to walk through.
        piece->child = sequence({atom});
        piece->min_count = min_count;
        piece->max_count = max_count;
        piece->greedy = greedy;
        return piece;
    }

    /// The count of a `{n}`, `{n,}` or `{n,m}`.
    bool parse_count(int* min_count, int* max_count) {
        const size_t start = position_;
        advance();  // the '{'

        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            // A `{` that is not a count is a character of its own.
            position_ = start;
            return true;
        }

        int low = 0;
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            low = low * 10 + (peek() - '0');
            advance();
        }

        int high = low;
        if (peek() == ',') {
            advance();
            high = -1;
            if (std::isdigit(static_cast<unsigned char>(peek()))) {
                high = 0;
                while (std::isdigit(static_cast<unsigned char>(peek()))) {
                    high = high * 10 + (peek() - '0');
                    advance();
                }
            }
        }

        if (peek() != '}') {
            position_ = start;
            return true;
        }
        advance();  // the '}'

        if (high >= 0 && high < low) {
            fail("The count of the repetition is out of order");
            return false;
        }

        *min_count = low;
        *max_count = high;
        return true;
    }

    /// A piece of the pattern.
    PiecePtr parse_atom() {
        if (position_ >= pattern_.size()) return nullptr;

        const char c = pattern_[position_];
        switch (c) {
            case '^': {
                advance();
                auto piece = std::make_shared<Piece>();
                piece->kind = Piece::Kind::AnchorStart;
                return piece;
            }
            case '$': {
                advance();
                auto piece = std::make_shared<Piece>();
                piece->kind = Piece::Kind::AnchorEnd;
                return piece;
            }
            case '.': {
                advance();
                auto piece = std::make_shared<Piece>();
                piece->kind = Piece::Kind::Any;
                return piece;
            }
            case '(':
                return parse_group();
            case '[':
                return parse_class();
            case '\\':
                return parse_escape(false);
            case '*':
            case '+':
            case '?':
                fail("There is nothing to repeat");
                return nullptr;
            default: {
                const size_t width = character_width(pattern_, position_);
                const uint32_t code = code_point(pattern_, position_);
                position_ += width;
                return literal(code);
            }
        }
    }

    /// The piece of a group.
    PiecePtr parse_group() {
        advance();  // the '('

        bool capturing = true;
        std::string name;

        if (peek() == '?') {
            advance();
            const char c = peek();
            if (c == ':') {
                advance();
                capturing = false;
            } else if (c == '=') {
                advance();
                return parse_lookaround(Piece::Kind::Lookahead, true);
            } else if (c == '!') {
                advance();
                return parse_lookaround(Piece::Kind::Lookahead, false);
            } else if (c == '<') {
                advance();
                if (peek() == '=') {
                    advance();
                    return parse_lookaround(Piece::Kind::Lookbehind, true);
                }
                if (peek() == '!') {
                    advance();
                    return parse_lookaround(Piece::Kind::Lookbehind, false);
                }
                // The name of the group.
                while (position_ < pattern_.size() && pattern_[position_] != '>') {
                    name.push_back(pattern_[position_]);
                    advance();
                }
                if (position_ >= pattern_.size()) {
                    fail("The name of the group has no end");
                    return nullptr;
                }
                advance();  // the '>'

                if (!is_group_name(name)) {
                    fail("The name of the group is not one the port knows");
                    return nullptr;
                }
                if (!declare_name(name)) return nullptr;
            } else {
                fail("The group is not one the port knows");
                return nullptr;
            }
        }

        PiecePtr body = parse_alternation();
        if (!body) return nullptr;

        if (peek() != ')') {
            fail("The group has no end");
            return nullptr;
        }
        advance();  // the ')'

        auto group = sequence({body});
        if (capturing) group->group_number = ++next_group_number_;
        return group;
    }

    /// The piece of a lookaround.
    PiecePtr parse_lookaround(Piece::Kind kind, bool positive) {
        PiecePtr body = parse_alternation();
        if (!body) return nullptr;

        if (peek() != ')') {
            fail("The lookaround has no end");
            return nullptr;
        }
        advance();  // the ')'

        auto piece = std::make_shared<Piece>();
        piece->kind = kind;
        piece->child = sequence({body});
        piece->positive = positive;
        return piece;
    }

    /// The piece of a class of characters.
    PiecePtr parse_class() {
        advance();  // the '['

        auto piece = std::make_shared<Piece>();
        piece->kind = Piece::Kind::Class;

        if (peek() == '^') {
            piece->negated = true;
            advance();
        }

        // The class `[]` holds no character at all, and the class `[^]` holds
        // every one of them.
        if (peek() == ']') {
            advance();
            return piece;
        }

        bool first = true;
        while (true) {
            if (position_ >= pattern_.size()) {
                fail("The class of characters has no end");
                return nullptr;
            }

            if (pattern_[position_] == ']' && !first) {
                advance();
                break;
            }
            first = false;

            uint32_t low = 0;

            if (pattern_[position_] == '\\') {
                PiecePtr escaped = parse_escape(true);
                if (!escaped) return nullptr;

                if (escaped->kind == Piece::Kind::Class) {
                    // A shorthand or a property of the Unicode standard.
                    for (const auto& range : escaped->ranges) piece->ranges.push_back(range);
                    for (const auto& property : escaped->properties) {
                        piece->properties.push_back(property);
                    }
                    continue;
                }
                if (escaped->kind != Piece::Kind::Literal) {
                    fail("Only the characters and the properties may be used in a class");
                    return nullptr;
                }
                low = escaped->literal;
            } else {
                const size_t width = character_width(pattern_, position_);
                low = code_point(pattern_, position_);
                position_ += width;
            }

            // The range of the class.
            if (peek() == '-' && position_ + 1 < pattern_.size() && pattern_[position_ + 1] != ']') {
                advance();  // the '-'

                uint32_t high = 0;
                if (pattern_[position_] == '\\') {
                    PiecePtr escaped = parse_escape(true);
                    if (!escaped) return nullptr;
                    if (escaped->kind != Piece::Kind::Literal) {
                        fail("A range of a class cannot hold a property");
                        return nullptr;
                    }
                    high = escaped->literal;
                } else {
                    const size_t width = character_width(pattern_, position_);
                    high = code_point(pattern_, position_);
                    position_ += width;
                }

                if (high < low) {
                    fail("The range of the class is out of order");
                    return nullptr;
                }
                piece->ranges.emplace_back(low, high);
                continue;
            }

            piece->ranges.emplace_back(low, low);
        }

        return piece;
    }

    /// The piece of an escape.
    PiecePtr parse_escape(bool in_class) {
        advance();  // the '\\'

        if (position_ >= pattern_.size()) {
            fail("The escape has no end");
            return nullptr;
        }

        const char c = pattern_[position_];
        switch (c) {
            case 'b': {
                advance();
                auto piece = std::make_shared<Piece>();
                if (in_class) {
                    // Inside a class the escape is the backspace.
                    piece->kind = Piece::Kind::Literal;
                    piece->literal = '\b';
                } else {
                    piece->kind = Piece::Kind::WordBoundary;
                }
                return piece;
            }
            case 'B': {
                advance();
                auto piece = std::make_shared<Piece>();
                piece->kind = Piece::Kind::NotWordBoundary;
                return piece;
            }
            case 'd':
            case 'D':
            case 's':
            case 'S':
            case 'w':
            case 'W': {
                advance();
                return shorthand(c);
            }
            case 'p':
            case 'P': {
                // The escape of a property is only read as such when the
                // pattern is read in the Unicode mode; otherwise it is the
                // letter itself, the way JavaScript reads it.
                if (!unicode_) break;
                advance();
                if (peek() != '{') {
                    fail("The property has no name");
                    return nullptr;
                }
                advance();  // the '{'
                std::string name;
                while (position_ < pattern_.size() && pattern_[position_] != '}') {
                    name.push_back(pattern_[position_]);
                    advance();
                }
                if (position_ >= pattern_.size()) {
                    fail("The property has no name");
                    return nullptr;
                }
                advance();  // the '}'
                if (!property_is_known(name)) {
                    fail("The property '" + name + "' is not one the port knows");
                    return nullptr;
                }
                auto piece = std::make_shared<Piece>();
                piece->kind = Piece::Kind::Class;
                piece->properties.emplace_back(name, c == 'P');
                return piece;
            }
            case 'u': {
                advance();
                return parse_unicode_escape();
            }
            case 'x': {
                advance();
                return parse_hex_escape(2);
            }
            case 'k': {
                // The backreference of a group that has a name. The name may
                // be one of the groups that come later, the way JavaScript
                // reads it.
                if (!group_names_.empty() && position_ + 1 < pattern_.size() &&
                    pattern_[position_ + 1] == '<') {
                    const size_t end = pattern_.find('>', position_ + 2);
                    if (end == std::string_view::npos) {
                        fail("The backreference has no name");
                        return nullptr;
                    }
                    const std::string name(pattern_.substr(position_ + 2, end - position_ - 2));
                    const auto found = group_names_.find(name);
                    if (found == group_names_.end()) {
                        fail("The group '" + name + "' is not one the port knows");
                        return nullptr;
                    }
                    position_ = end + 1;
                    auto piece = std::make_shared<Piece>();
                    piece->kind = Piece::Kind::Backreference;
                    piece->group_number = found->second;
                    return piece;
                }
                // The pattern has no group with a name, so the escape is the
                // letter itself, the way JavaScript reads it.
                break;
            }
            case 'n':
            case 'r':
            case 't':
            case 'f':
            case 'v': {
                advance();
                auto piece = std::make_shared<Piece>();
                piece->kind = Piece::Kind::Literal;
                switch (c) {
                    case 'n':
                        piece->literal = '\n';
                        break;
                    case 'r':
                        piece->literal = '\r';
                        break;
                    case 't':
                        piece->literal = '\t';
                        break;
                    case 'f':
                        piece->literal = '\f';
                        break;
                    default:
                        piece->literal = '\v';
                        break;
                }
                return piece;
            }
            case 'c': {
                advance();
                if (position_ >= pattern_.size()) {
                    fail("The escape has no end");
                    return nullptr;
                }
                const char letter = pattern_[position_];
                advance();
                auto piece = std::make_shared<Piece>();
                piece->kind = Piece::Kind::Literal;
                piece->literal = static_cast<uint32_t>(letter % 32);
                return piece;
            }
            default: {
                // A digit may be the number of a group, which is the text
                // that group captured.
                if (c >= '0' && c <= '9') {
                    const size_t start = position_;
                    int number = 0;
                    while (std::isdigit(static_cast<unsigned char>(peek()))) {
                        number = number * 10 + (peek() - '0');
                        advance();
                    }

                    if (number >= 1 && number <= group_count_) {
                        auto piece = std::make_shared<Piece>();
                        piece->kind = Piece::Kind::Backreference;
                        piece->group_number = number;
                        return piece;
                    }

                    // The number is not one of the groups, so the escape is
                    // the octal one, the way JavaScript reads it.
                    position_ = start;
                    uint32_t code = 0;
                    size_t digits = 0;
                    while (digits < 3 && position_ < pattern_.size() && pattern_[position_] >= '0' &&
                           pattern_[position_] <= '7') {
                        code = code * 8 + static_cast<uint32_t>(pattern_[position_] - '0');
                        advance();
                        ++digits;
                    }
                    if (digits == 0) {
                        // A digit that is not an octal one is the digit
                        // itself.
                        advance();
                        return literal(static_cast<uint32_t>(pattern_[start] - '0'));
                    }
                    return literal(code);
                }

                // The escape of a character that has no escape of its own is
                // the character itself, the way JavaScript reads it.
                const size_t width = character_width(pattern_, position_);
                const uint32_t code = code_point(pattern_, position_);
                position_ += width;
                return literal(code);
            }
        }

        // The `p` and the `P` of a pattern that is not read in the Unicode
        // mode are letters of their own.
        const size_t width = character_width(pattern_, position_);
        const uint32_t code = code_point(pattern_, position_);
        position_ += width;
        return literal(code);
    }

    /// The piece of a shorthand such as `\d`.
    PiecePtr shorthand(char c) const {
        auto piece = std::make_shared<Piece>();
        piece->kind = Piece::Kind::Class;
        switch (c) {
            case 'd':
                piece->ranges.emplace_back('0', '9');
                break;
            case 'D':
                piece->properties.emplace_back("Decimal_Number", true);
                break;
            case 'w':
                piece->ranges.emplace_back('a', 'z');
                piece->ranges.emplace_back('A', 'Z');
                piece->ranges.emplace_back('0', '9');
                piece->ranges.emplace_back('_', '_');
                break;
            case 'W':
                piece->properties.emplace_back("word", true);
                break;
            default:
                piece->properties.emplace_back("White_Space", c == 'S');
                break;
        }
        return piece;
    }

    /// The piece of a hexadecimal escape.
    PiecePtr parse_hex_escape(size_t digits) {
        uint32_t code = 0;
        for (size_t i = 0; i < digits; ++i) {
            const char c = peek();
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                fail("The escape has no hexadecimal digits");
                return nullptr;
            }
            code = code * 16 + static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(c))
                                                         ? c - '0'
                                                         : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
            advance();
        }
        return literal(code);
    }

    /// The piece of a Unicode escape.
    PiecePtr parse_unicode_escape() {
        if (peek() == '{') {
            advance();
            uint32_t code = 0;
            bool any = false;
            while (position_ < pattern_.size() && pattern_[position_] != '}') {
                const char c = pattern_[position_];
                if (!std::isxdigit(static_cast<unsigned char>(c))) {
                    fail("The escape has no hexadecimal digits");
                    return nullptr;
                }
                code = code * 16 + static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(c))
                                                             ? c - '0'
                                                             : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
                any = true;
                advance();
            }
            if (!any || position_ >= pattern_.size()) {
                fail("The escape has no hexadecimal digits");
                return nullptr;
            }
            advance();  // the '}'
            return literal(code);
        }
        return parse_hex_escape(4);
    }

    /// The pattern that is parsed.
    std::string_view pattern_;

    /// Where the parser is in the pattern.
    size_t position_ = 0;

    /// Where the failure is reported.
    std::string* error_ = nullptr;

    /// True when the escapes of the properties are read as such.
    bool unicode_ = false;

    /// True when the pattern has already failed.
    bool failed_ = false;

    /// The names of the groups, and the branches they stand in.
    std::vector<std::pair<std::string, Branches>> named_groups_;

    /// The number of the groups that capture.
    int group_count_ = 0;

    /// The number the next group that captures is given.
    int next_group_number_ = 0;

    /// The number of the groups that have a name.
    std::map<std::string, int> group_names_;

    /// The branches the parser is in.
    Branches branch_stack_;

    /// The number of the alternations the parser has read.
    size_t next_alternation_ = 0;
};

std::optional<JsRegex> JsRegex::compile(std::string_view pattern, std::string* error) {
    return compile(pattern, "", error);
}

std::optional<JsRegex> JsRegex::compile(std::string_view pattern, std::string_view flags,
                                         std::string* error) {
    JsRegex regex;
    for (const char flag : flags) {
        switch (flag) {
            case 'i':
                regex.case_insensitive_ = true;
                break;
            case 'm':
                regex.multiline_ = true;
                break;
            case 's':
                regex.dot_all_ = true;
                break;
            case 'u':
                regex.unicode_ = true;
                break;
            case 'g':
                // The flag of the whole matches does not change the result
                // of a search.
                break;
            default:
                if (error != nullptr) {
                    *error = "The flag '" + std::string(1, flag) + "' is not one the port knows";
                }
                return std::nullopt;
        }
    }

    Parser parser(pattern, error, regex.unicode_);
    const std::vector<PiecePtr> pieces = parser.parse();
    if (parser.failed()) return std::nullopt;

    // The groups the parser found and the groups it gave a number must be
    // the same ones.
    if (parser.group_count() != parser.numbered_groups()) {
        if (error != nullptr) {
            *error = "The pattern is not one the port reads the same way twice";
        }
        return std::nullopt;
    }

    regex.root_ = pieces;
    regex.group_count_ = parser.group_count();
    return regex;
}

bool JsRegex::search(std::string_view text) const {
    auto captures = std::make_shared<std::vector<Capture>>(static_cast<size_t>(group_count_));
    for (size_t start = 0; start <= text.size();) {
        const auto next = [](size_t, const Captures&) { return true; };
        if (match(root_, 0, text, start, captures, next)) return true;
        if (start == text.size()) break;
        // The match may only begin at the start of a character.
        start += character_width(text, start);
    }
    return false;
}

bool JsRegex::same_character(uint32_t one, uint32_t other) const {
    if (one == other) return true;
    if (!case_insensitive_) return false;
    return fold(one) == fold(other);
}

bool JsRegex::in_range(uint32_t code, uint32_t first, uint32_t last) const {
    if (code >= first && code <= last) return true;
    if (!case_insensitive_) return false;

    // The range may hold the other case of the character, and the character
    // may be the other case of one the range holds.
    const uint32_t lower = fold(code);
    if (lower >= first && lower <= last) return true;
    const uint32_t upper = upper_case(code);
    return upper >= first && upper <= last;
}

bool JsRegex::at_line_start(std::string_view text, size_t position) const {
    if (position == 0) return true;
    if (!multiline_) return false;
    return is_line_terminator(code_point(text, previous_character_start(text, position)));
}

bool JsRegex::at_line_end(std::string_view text, size_t position) const {
    if (position == text.size()) return true;
    if (!multiline_) return false;
    return is_line_terminator(code_point(text, position));
}

bool JsRegex::match(const std::vector<PiecePtr>& pieces, size_t index, std::string_view text, size_t position,
                    const Captures& captures, const Continuation& next) const {
    if (index >= pieces.size()) return next(position, captures);

    const Piece& piece = *pieces[index];
    switch (piece.kind) {
        case Piece::Kind::Literal: {
            if (position >= text.size()) return false;
            if (!same_character(code_point(text, position), piece.literal)) return false;
            const size_t width = character_width(text, position);
            return match(pieces, index + 1, text, position + width, captures, next);
        }
        case Piece::Kind::Any: {
            if (position >= text.size()) return false;
            const uint32_t code = code_point(text, position);
            if (!dot_all_ && is_line_terminator(code)) return false;
            const size_t width = character_width(text, position);
            return match(pieces, index + 1, text, position + width, captures, next);
        }
        case Piece::Kind::Class: {
            if (position >= text.size()) return false;
            if (!class_matches(piece, text, position)) return false;
            const size_t width = character_width(text, position);
            return match(pieces, index + 1, text, position + width, captures, next);
        }
        case Piece::Kind::Sequence: {
            if (piece.group_number > 0) {
                // The group takes the text it matched.
                if (static_cast<size_t>(piece.group_number) > captures->size()) return false;
                const auto inner = [&](size_t after, const Captures& inner_captures) {
                    auto taken = std::make_shared<std::vector<Capture>>(*inner_captures);
                    (*taken)[static_cast<size_t>(piece.group_number - 1)] = Capture{position, after};
                    return match(pieces, index + 1, text, after, taken, next);
                };
                return match(piece.children, 0, text, position, captures, inner);
            }
            const auto inner = [&](size_t after, const Captures& inner_captures) {
                return match(pieces, index + 1, text, after, inner_captures, next);
            };
            return match(piece.children, 0, text, position, captures, inner);
        }
        case Piece::Kind::Alternative: {
            for (const auto& branch : piece.children) {
                const auto inner = [&](size_t after, const Captures& inner_captures) {
                    return match(pieces, index + 1, text, after, inner_captures, next);
                };
                if (match(branch->children, 0, text, position, captures, inner)) return true;
            }
            return false;
        }
        case Piece::Kind::Repeat:
            return match_repeat(piece, pieces, index, text, position, 0, captures, next);
        case Piece::Kind::AnchorStart: {
            if (!at_line_start(text, position)) return false;
            return match(pieces, index + 1, text, position, captures, next);
        }
        case Piece::Kind::AnchorEnd: {
            if (!at_line_end(text, position)) return false;
            return match(pieces, index + 1, text, position, captures, next);
        }
        case Piece::Kind::WordBoundary:
        case Piece::Kind::NotWordBoundary: {
            const bool boundary = is_word_boundary(text, position);
            const bool holds = (piece.kind == Piece::Kind::WordBoundary) ? boundary : !boundary;
            if (!holds) return false;
            return match(pieces, index + 1, text, position, captures, next);
        }
        case Piece::Kind::Lookahead:
        case Piece::Kind::Lookbehind: {
            const bool holds = lookaround_matches(piece, text, position, captures);
            if (holds != piece.positive) return false;
            return match(pieces, index + 1, text, position, captures, next);
        }
        case Piece::Kind::Backreference: {
            if (static_cast<size_t>(piece.group_number) > captures->size()) return false;
            const Capture& taken = (*captures)[static_cast<size_t>(piece.group_number - 1)];
            if (taken.first == taken.second) {
                // The group took nothing, so the reference takes nothing.
                return match(pieces, index + 1, text, position, captures, next);
            }
            const size_t length = taken.second - taken.first;
            if (position + length > text.size()) return false;
            if (!same_text(text, position, text, taken.first, length)) return false;
            return match(pieces, index + 1, text, position + length, captures, next);
        }
    }
    return false;
}

bool JsRegex::same_text(std::string_view one, size_t one_position, std::string_view other,
                        size_t other_position, size_t length) const {
    for (size_t i = 0; i < length; ++i) {
        const size_t one_width = character_width(one, one_position);
        const size_t other_width = character_width(other, other_position);
        if (!same_character(code_point(one, one_position), code_point(other, other_position))) return false;
        one_position += one_width;
        other_position += other_width;
    }
    return true;
}

bool JsRegex::match_repeat(const Piece& piece, const std::vector<PiecePtr>& pieces, size_t index,
                           std::string_view text, size_t position, size_t count, const Captures& captures,
                           const Continuation& next) const {
    const bool can_stop = static_cast<int>(count) >= piece.min_count;
    const bool can_go = piece.max_count < 0 || static_cast<int>(count) < piece.max_count;

    // The piece is taken once more; the repetition ends when the piece takes
    // nothing and the least number of times has been reached.
    const auto inner = [&](size_t after, const Captures& inner_captures) {
        if (after == position && can_stop) return false;
        return match_repeat(piece, pieces, index, text, after, count + 1, inner_captures, next);
    };

    if (piece.greedy) {
        if (can_go && match(piece.child->children, 0, text, position, captures, inner)) return true;
        if (can_stop) return match(pieces, index + 1, text, position, captures, next);
        return false;
    }

    // The lazy repetition takes the least number of times it can.
    if (can_stop && match(pieces, index + 1, text, position, captures, next)) return true;
    if (can_go && match(piece.child->children, 0, text, position, captures, inner)) return true;
    return false;
}

bool JsRegex::lookaround_matches(const Piece& piece, std::string_view text, size_t position,
                                 const Captures& captures) const {
    if (piece.kind == Piece::Kind::Lookahead) {
        // The piece has to match at the position, without taking anything.
        const auto inner = [](size_t, const Captures&) { return true; };
        return match(piece.child->children, 0, text, position, captures, inner);
    }

    // The piece has to match exactly up to the position.
    for (size_t start = 0; start <= position;) {
        const auto inner = [&](size_t after, const Captures&) { return after == position; };
        if (match(piece.child->children, 0, text, start, captures, inner)) return true;
        if (start == position) break;
        start += character_width(text, start);
    }
    return false;
}

bool JsRegex::class_matches(const Piece& piece, std::string_view text, size_t position) const {
    const uint32_t code = code_point(text, position);

    bool inside = false;
    for (const auto& range : piece.ranges) {
        if (in_range(code, range.first, range.second)) {
            inside = true;
            break;
        }
    }

    if (!inside) {
        for (const auto& property : piece.properties) {
            bool holds = property_matches(property.first, code);
            if (!holds && case_insensitive_) {
                // The other case of the character may be in the property.
                holds = property_matches(property.first, upper_case(code)) ||
                        property_matches(property.first, fold(code));
            }
            if (holds != property.second) {
                inside = true;
                break;
            }
        }
    }

    return inside != piece.negated;
}

} // namespace cc
