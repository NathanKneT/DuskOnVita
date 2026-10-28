#pragma once
// Start-up screens drawn by the CPU into a 32-bit display buffer (A8B8G8R8, the format the GXM backend
// and sceDisplay use): a progress screen while the shader warm-up bundle is prepared, and plain error
// screens. SDK-free; startup_screen.cpp puts the buffer on the display.
//
// The font is our own 5x9 pixel font, drawn for this project: printable ASCII, 7 rows above the
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
constexpr uint32_t Background = rgb(12, 16, 22), Name = rgb(140, 150, 165), Title = rgb(255, 206, 110),
                   Text = rgb(235, 238, 242), Path = rgb(130, 210, 255), Note = rgb(160, 168, 180),
                   BarFrame = rgb(120, 130, 145), BarFill = rgb(120, 200, 140), BarEmpty = rgb(28, 34, 44);
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

// --- screen layouts (960x544) ---
constexpr const char* AppName = "DuskOnVita";
constexpr int ScreenWidth = 960, ScreenHeight = 544, Margin = 48;

enum class Style : uint8_t { Text, Path, Note };
struct MessageItem {
    Style style = Style::Text;
    const char* text = nullptr; // null: unused
};
struct Message {
    const char* title = "";
    std::array<MessageItem, 6> items{};
    const char* footer = "Press any button to exit.";
};
struct Layout {
    int lines = 0, bottom = 0;
    bool fits = false;
};

// Draws the whole error screen; with a null canvas only measures. `fits`: every line is on the screen
// above the footer.
inline Layout draw_message(const Canvas* c, const Message& m) {
    constexpr int TitleScale = 4, TextScale = 3, NoteScale = 2, NameScale = 2;
    constexpr int FooterY = ScreenHeight - 36 - FontRows * TextScale;
    Layout out;
    if (c) fill_rect(*c, 0, 0, c->width, c->height, palette::Background);
    int y = 32;
    if (c) draw_text(*c, Margin, y, NameScale, palette::Name, AppName);
    y += line_height(NameScale) + 18;
    TextLine lines[12];
    bool cut = false, anyCut = false;
    const auto columns_at = [](int scale) { return std::size_t((ScreenWidth - 2 * Margin) / (FontAdvance * scale)); };
    int n = wrap_text(m.title, columns_at(TitleScale), lines, 2, &cut);
    anyCut |= cut;
    for (int i = 0; i < n; ++i, y += line_height(TitleScale)) {
        if (c) draw_text(*c, Margin, y, TitleScale, palette::Title, m.title + lines[i].start, lines[i].length);
        ++out.lines;
    }
    y += 14;
    for (const MessageItem& item : m.items) {
        if (!item.text) continue;
        const uint32_t color = item.style == Style::Path ? palette::Path : item.style == Style::Note ? palette::Note : palette::Text;
        const int scale = item.style == Style::Note ? NoteScale : TextScale;
        const int indent = item.style == Style::Path ? 2 * FontAdvance * scale : 0;
        const std::size_t columns = columns_at(scale) - (indent ? 2 : 0);
        n = wrap_text(item.text, columns, lines, 12, &cut);
        anyCut |= cut;
        for (int i = 0; i < n; ++i, y += line_height(scale)) {
            if (c && y + FontRows * scale <= FooterY - 8)
                draw_text(*c, Margin + indent, y, scale, color, item.text + lines[i].start, lines[i].length);
            ++out.lines;
        }
        y += 10;
    }
    out.bottom = y - 10;
    out.fits = !anyCut && out.bottom <= FooterY - 8;
    if (c && m.footer) draw_text(*c, Margin, FooterY, TextScale, palette::Note, m.footer);
    return out;
}

// Progress screen: the app name, a label, and (once compiles are seen) a bar with "done / total".
struct Progress {
    const char* label = "Loading...";
    uint32_t done = 0, total = 0;
    bool bar = false;
    const char* note = nullptr;
};
constexpr int BarX = 180, BarY = 300, BarW = 600, BarH = 26, CountY = 346, LabelY = 244, NoteY = 420;
// full: the whole screen; otherwise the bar interior, the count and the label lines only.
inline void draw_progress(const Canvas& c, const Progress& p, bool full) {
    constexpr int CountScale = 3, LabelScale = 3;
    if (full) {
        fill_rect(c, 0, 0, c.width, c.height, palette::Background);
        draw_centered(c, 150, 6, palette::Text, AppName);
    } else {
        fill_rect(c, 0, LabelY, c.width, FontRows * LabelScale, palette::Background);
    }
    draw_centered(c, LabelY, LabelScale, palette::Text, p.label);
    if (!p.bar) return;
    if (full) {
        fill_rect(c, BarX - 3, BarY - 3, BarW + 6, BarH + 6, palette::BarFrame);
        if (p.note) draw_centered(c, NoteY, 2, palette::Note, p.note);
    }
    const uint32_t done = p.done > p.total ? p.total : p.done;
    const int filled = p.total ? int(uint64_t(BarW) * done / p.total) : 0;
    fill_rect(c, BarX, BarY, filled, BarH, palette::BarFill);
    fill_rect(c, BarX + filled, BarY, BarW - filled, BarH, palette::BarEmpty);
    char count[32];
    int length = 0;
    {   // "done / total" without snprintf (host and Vita alike).
        char digits[2][12];
        int lengths[2] = {0, 0};
        const uint32_t values[2] = {done, p.total};
        for (int k = 0; k < 2; ++k) {
            uint32_t v = values[k];
            char tmp[12];
            int t = 0;
            do { tmp[t++] = char('0' + v % 10); v /= 10; } while (v);
            while (t) digits[k][lengths[k]++] = tmp[--t];
        }
        for (int i = 0; i < lengths[0]; ++i) count[length++] = digits[0][i];
        for (const char* s = " / "; *s; ++s) count[length++] = *s;
        for (int i = 0; i < lengths[1]; ++i) count[length++] = digits[1][i];
        count[length] = 0;
    }
    fill_rect(c, 0, CountY, c.width, FontRows * CountScale, palette::Background);
    draw_centered(c, CountY, CountScale, palette::Text, count);
}

} // namespace twv::ui
