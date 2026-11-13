#pragma once
// A canvas the CPU draws into: a 32-bit display buffer (A8B8G8R8, the format the GXM backend and
// sceDisplay use), with filled rectangles and a pixel font. The start-up screens and the settings
// page draw on it with their own font and shapes (ui/ui_paint.hpp, ui/startup_layouts.hpp,
// ui/settings_draw.hpp); the pixel font here is what the FPS counter uses. SDK-free.
//
// The pixel font is our own 5x9 font, drawn for this project: printable ASCII, 7 rows above the
// baseline and 2 for descenders. Each glyph is up to nine 5-column rows ('#' set, '.' clear) separated
// by spaces; missing rows are clear. Anything outside printable ASCII is drawn as '?'.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace twv::ui {

constexpr int FontColumns = 5, FontRows = 9, FontAdvance = 6, FontFirst = 32, FontLast = 126;
constexpr const char* FontArt[FontLast - FontFirst + 1] = {
    "",                                                          // ' '
    "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#..",                 // !
    ".#.#. .#.#.",                                               // "
    ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#.",                 // #
    "..#.. .#### #.#.. .###. ..#.# ####. ..#..",                 // $
    "##... ##..# ...#. ..#.. .#... #..## ...##",                 // %
    ".##.. #..#. #.#.. .#... #.#.# #..#. .##.#",                 // &
    "..#.. ..#..",                                               // '
    "...#. ..#.. .#... .#... .#... ..#.. ...#.",                 // (
    ".#... ..#.. ...#. ...#. ...#. ..#.. .#...",                 // )
    "..... ..#.. #.#.# .###. #.#.# ..#..",                       // *
    "..... ..#.. ..#.. ##### ..#.. ..#..",                       // +
    "..... ..... ..... ..... ..... .##.. .##.. ..#.. .#...",     // ,
    "..... ..... ..... .###.",                                   // -
    "..... ..... ..... ..... ..... .##.. .##..",                 // .
    "....# ....# ...#. ..#.. .#... #.... #....",                 // /
    ".###. #...# #..## #.#.# ##..# #...# .###.",                 // 0
    "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###.",                 // 1
    ".###. #...# ....# ...#. ..#.. .#... #####",                 // 2
    "##### ...#. ..#.. ...#. ....# #...# .###.",                 // 3
    "...#. ..##. .#.#. #..#. ##### ...#. ...#.",                 // 4
    "##### #.... ####. ....# ....# #...# .###.",                 // 5
    "..##. .#... #.... ####. #...# #...# .###.",                 // 6
    "##### ....# ...#. ..#.. .#... .#... .#...",                 // 7
    ".###. #...# #...# .###. #...# #...# .###.",                 // 8
    ".###. #...# #...# .#### ....# ...#. .##..",                 // 9
    "..... .##.. .##.. ..... .##.. .##..",                       // :
    "..... .##.. .##.. ..... .##.. .##.. ..#.. .#...",           // ;
    "...#. ..#.. .#... #.... .#... ..#.. ...#.",                 // <
    "..... ..... ##### ..... #####",                             // =
    ".#... ..#.. ...#. ....# ...#. ..#.. .#...",                 // >
    ".###. #...# ....# ...#. ..#.. ..... ..#..",                 // ?
    ".###. #...# #.### #.#.# #.### #.... .####",                 // @
    ".###. #...# #...# ##### #...# #...# #...#",                 // A
    "####. #...# #...# ####. #...# #...# ####.",                 // B
    ".###. #...# #.... #.... #.... #...# .###.",                 // C
    "###.. #..#. #...# #...# #...# #..#. ###..",                 // D
    "##### #.... #.... ####. #.... #.... #####",                 // E
    "##### #.... #.... ####. #.... #.... #....",                 // F
    ".###. #...# #.... #.### #...# #...# .####",                 // G
    "#...# #...# #...# ##### #...# #...# #...#",                 // H
    ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###.",                 // I
    "..### ...#. ...#. ...#. ...#. #..#. .##..",                 // J
    "#...# #..#. #.#.. ##... #.#.. #..#. #...#",                 // K
    "#.... #.... #.... #.... #.... #.... #####",                 // L
    "#...# ##.## #.#.# #.#.# #...# #...# #...#",                 // M
    "#...# #...# ##..# #.#.# #..## #...# #...#",                 // N
    ".###. #...# #...# #...# #...# #...# .###.",                 // O
    "####. #...# #...# ####. #.... #.... #....",                 // P
    ".###. #...# #...# #...# #.#.# #..#. .##.#",                 // Q
    "####. #...# #...# ####. #.#.. #..#. #...#",                 // R
    ".#### #.... #.... .###. ....# ....# ####.",                 // S
    "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#..",                 // T
    "#...# #...# #...# #...# #...# #...# .###.",                 // U
    "#...# #...# #...# #...# #...# .#.#. ..#..",                 // V
    "#...# #...# #...# #.#.# #.#.# #.#.# .#.#.",                 // W
    "#...# #...# .#.#. ..#.. .#.#. #...# #...#",                 // X
    "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#..",                 // Y
    "##### ....# ...#. ..#.. .#... #.... #####",                 // Z
    ".###. .#... .#... .#... .#... .#... .###.",                 // [
    "#.... #.... .#... ..#.. ...#. ....# ....#",                 // backslash
    ".###. ...#. ...#. ...#. ...#. ...#. .###.",                 // ]
    "..#.. .#.#. #...#",                                         // ^
    "..... ..... ..... ..... ..... ..... ..... #####",           // _
    ".#... ..#..",                                               // `
    "..... ..... .###. ....# .#### #...# .####",                 // a
    "#.... #.... ####. #...# #...# #...# ####.",                 // b
    "..... ..... .###. #.... #.... #...# .###.",                 // c
    "....# ....# .#### #...# #...# #...# .####",                 // d
    "..... ..... .###. #...# ##### #.... .###.",                 // e
    "..##. .#..# .#... ###.. .#... .#... .#...",                 // f
    "..... ..... .#### #...# #...# #...# .#### ....# .###.",     // g
    "#.... #.... #.##. ##..# #...# #...# #...#",                 // h
    "..#.. ..... .##.. ..#.. ..#.. ..#.. .###.",                 // i
    "...#. ..... ..##. ...#. ...#. ...#. ...#. #..#. .##..",     // j
    "#.... #.... #..#. #.#.. ##... #.#.. #..#.",                 // k
    ".##.. ..#.. ..#.. ..#.. ..#.. ..#.. .###.",                 // l
    "..... ..... ##.#. #.#.# #.#.# #.#.# #.#.#",                 // m
    "..... ..... #.##. ##..# #...# #...# #...#",                 // n
    "..... ..... .###. #...# #...# #...# .###.",                 // o
    "..... ..... ####. #...# #...# #...# ####. #.... #....",     // p
    "..... ..... .#### #...# #...# #...# .#### ....# ....#",     // q
    "..... ..... #.##. ##..# #.... #.... #....",                 // r
    "..... ..... .#### #.... .###. ....# ####.",                 // s
    ".#... .#... ###.. .#... .#... .#..# ..##.",                 // t
    "..... ..... #...# #...# #...# #..## .##.#",                 // u
    "..... ..... #...# #...# #...# .#.#. ..#..",                 // v
    "..... ..... #...# #...# #.#.# #.#.# .#.#.",                 // w
    "..... ..... #...# .#.#. ..#.. .#.#. #...#",                 // x
    "..... ..... #...# #...# #...# #...# .#### ....# .###.",     // y
    "..... ..... ##### ...#. ..#.. .#... #####",                 // z
    "...#. ..#.. ..#.. .#... ..#.. ..#.. ...#.",                 // {
    "..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#.. ..#..",           // |
    ".#... ..#.. ..#.. ...#. ..#.. ..#.. .#...",                 // }
    "..... ..... .#... #.#.# ...#.",                             // ~
};

// Rows as 5-bit masks, bit 4 the leftmost column. Malformed art (a row that is not five '.'/'#',
// more than nine rows) does not compile: the table is built in a constant expression.
using GlyphRows = std::array<uint8_t, FontRows>;
constexpr GlyphRows parse_glyph(const char* art) {
    GlyphRows rows{};
    int row = 0, column = 0;
    for (const char* p = art; *p; ++p) {
        if (*p == ' ') {
            if (column != FontColumns) throw "glyph row must have five columns";
            ++row;
            column = 0;
            continue;
        }
        if (row >= FontRows || column >= FontColumns || (*p != '#' && *p != '.')) throw "bad glyph art";
        if (*p == '#') rows[row] = uint8_t(rows[row] | (1u << (FontColumns - 1 - column)));
        ++column;
    }
    if (column != 0 && column != FontColumns) throw "glyph row must have five columns";
    return rows;
}
constexpr auto Font = [] {
    std::array<GlyphRows, FontLast - FontFirst + 1> font{};
    for (int i = 0; i <= FontLast - FontFirst; ++i) font[i] = parse_glyph(FontArt[i]);
    return font;
}();
constexpr const GlyphRows& glyph(char c) {
    const int code = static_cast<unsigned char>(c);
    return Font[(code < FontFirst || code > FontLast ? '?' : code) - FontFirst];
}

// A8B8G8R8 in memory: R in the lowest byte.
constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xff000000u | (b << 16) | (g << 8) | r; }
namespace palette {
// The colours of the start-up screens, the settings page's own (ui/settings_draw.hpp).
constexpr uint32_t Background = rgb(16, 14, 27), Name = rgb(142, 138, 166), Title = rgb(255, 206, 110),
                   Text = rgb(240, 238, 248), Path = rgb(160, 204, 255), Note = rgb(150, 146, 172),
                   BarFrame = rgb(74, 70, 100), BarFill = rgb(255, 184, 92), BarEmpty = rgb(36, 33, 56),
                   Rule = rgb(54, 51, 78);
}

struct Canvas {
    uint32_t* pixels = nullptr;
    int width = 0, height = 0, stride = 0; // stride in pixels
};

inline void fill_rect(const Canvas& c, int x, int y, int w, int h, uint32_t color) {
    const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const int x1 = x + w > c.width ? c.width : x + w, y1 = y + h > c.height ? c.height : y + h;
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) c.pixels[std::size_t(yy) * std::size_t(c.stride) + std::size_t(xx)] = color;
}

constexpr int text_width(std::size_t chars, int scale) { return chars ? int(chars) * FontAdvance * scale - scale : 0; }
constexpr int line_height(int scale) { return (FontRows + 2) * scale; }

// `count` characters of `text` at (x, y) (top of the cap height); returns the width drawn.
inline int draw_text(const Canvas& c, int x, int y, int scale, uint32_t color, const char* text, std::size_t count) {
    for (std::size_t i = 0; i < count && text[i]; ++i) {
        const GlyphRows& g = glyph(text[i]);
        const int gx = x + int(i) * FontAdvance * scale;
        for (int row = 0; row < FontRows; ++row)
            for (int col = 0; col < FontColumns; ++col)
                if (g[row] & (1u << (FontColumns - 1 - col))) fill_rect(c, gx + col * scale, y + row * scale, scale, scale, color);
    }
    return text_width(count, scale);
}
inline int draw_text(const Canvas& c, int x, int y, int scale, uint32_t color, const char* text) {
    return draw_text(c, x, y, scale, color, text, std::strlen(text));
}
inline void draw_centered(const Canvas& c, int y, int scale, uint32_t color, const char* text) {
    const std::size_t n = std::strlen(text);
    draw_text(c, (c.width - text_width(n, scale)) / 2, y, scale, color, text, n);
}

// Word wrap: lines of at most `columns` characters, broken at spaces; a word longer than a line
// (a path) is cut where the line ends. Returns the line count (at most maxLines; the rest is dropped
// and `truncated` set).
struct TextLine {
    std::size_t start = 0, length = 0;
};
inline int wrap_text(const char* text, std::size_t columns, TextLine* lines, int maxLines, bool* truncated = nullptr) {
    if (truncated) *truncated = false;
    const std::size_t n = std::strlen(text);
    std::size_t at = 0;
    int count = 0;
    while (at < n) {
        while (at < n && text[at] == ' ') ++at;
        if (at >= n) break;
        if (count == maxLines) {
            if (truncated) *truncated = true;
            break;
        }
        std::size_t end = at + columns < n ? at + columns : n;
        if (end < n && text[end] != ' ') {
            std::size_t space = end;
            while (space > at && text[space] != ' ') --space;
            if (space > at) end = space;
        }
        std::size_t length = end - at;
        while (length && text[at + length - 1] == ' ') --length;
        lines[count++] = {at, length};
        at = end;
    }
    return count;
}

} // namespace twv::ui
