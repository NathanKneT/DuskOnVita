#pragma once
// The start-up screens' layouts (960x544), in the settings page's style: the page's font and shapes
// (ui/ui_paint.hpp) on the same dark background. A progress screen while the shader warm-up bundle
// is prepared, and plain message screens (what is missing and where it goes, the first-run screen).
// The CPU draws them into a display buffer, so they work before GXM starts and when it cannot
// (ui/startup_screen.cpp). SDK-free.
#include "ui/ui_paint.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace twv::ui {

constexpr const char* AppName = "DuskOnVita";
constexpr int ScreenWidth = 960, ScreenHeight = 544, Margin = 56;

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

// Draws the whole message screen; with a null canvas only measures. `fits`: every line is on the
// screen above the footer. A path sits in a box of its own, a note is smaller and dimmer.
inline Layout draw_message(const Canvas* c, const Message& m) {
    constexpr int FooterY = ScreenHeight - 54, TextWidth = ScreenWidth - 2 * Margin, PathPad = 14;
    Layout out;
    if (c) {
        fill_rect(*c, 0, 0, c->width, c->height, palette::Background);
        draw_text(*c, font::Small, Margin, 30, palette::Name, AppName);
    }
    int y = 60;
    TextLine lines[12];
    bool cut = false, anyCut = false;
    int n = wrap_text(font::Title, m.title, TextWidth, lines, 2, &cut);
    anyCut |= cut;
    for (int i = 0; i < n; ++i, y += font::Title.lineHeight) {
        if (c) draw_text(*c, font::Title, Margin, y, palette::Title, m.title + lines[i].start, lines[i].length);
        ++out.lines;
    }
    y += 4;
    if (c) fill_rect(*c, Margin, y, 56, 3, palette::Title);
    y += 3 + 18;
    for (const MessageItem& item : m.items) {
        if (!item.text) continue;
        const bool path = item.style == Style::Path, note = item.style == Style::Note;
        const font::Font& f = note ? font::Small : font::Body;
        const uint32_t color = path ? palette::Path : note ? palette::Note : palette::Text;
        const int step = f.lineHeight + (note ? 2 : 3), indent = path ? PathPad : 0;
        n = wrap_text(f, item.text, TextWidth - 2 * indent, lines, 12, &cut);
        anyCut |= cut;
        if (path) y += 2;
        if (c && path && y + n * step + 6 <= FooterY - 8)
            round_rect(*c, Margin, y - 6, TextWidth, n * step + 10, 9, palette::BarEmpty);
        for (int i = 0; i < n; ++i, y += step) {
            if (c && y + f.lineHeight <= FooterY - 8)
                draw_text(*c, f, Margin + indent, y, color, item.text + lines[i].start, lines[i].length);
            ++out.lines;
        }
        y += path ? 16 : 10;
    }
    out.bottom = y - 10;
    out.fits = !anyCut && out.bottom <= FooterY - 8;
    if (c && m.footer) {
        fill_rect(*c, Margin, FooterY - 12, TextWidth, 1, palette::Rule);
        draw_text(*c, font::Body, Margin, FooterY, palette::Note, m.footer);
    }
    return out;
}

// Progress screen: the app name, a label, and (once compiles are seen) a bar with "done / total".
struct Progress {
    const char* label = "Loading...";
    uint32_t done = 0, total = 0;
    bool bar = false;
    const char* note = nullptr;
    const char* hint = nullptr; // bottom line: how to open the settings page (ui/settings_screen.hpp)
};
constexpr int NameY = 132, LabelY = 256, BarX = 240, BarY = 306, BarW = 480, BarH = 12, CountY = 334, NoteY = 400,
              HintY = 496;
constexpr int LabelH = font::Body.lineHeight, CountH = font::Small.lineHeight; // the bands a partial draw clears
// full: the whole screen; otherwise the bar's inside, the count and the label line only.
inline void draw_progress(const Canvas& c, const Progress& p, bool full) {
    const int centre = c.width / 2;
    if (full) {
        fill_rect(c, 0, 0, c.width, c.height, palette::Background);
        draw_text_centred(c, font::Display, centre, NameY, palette::Text, AppName);
        fill_rect(c, centre - 28, NameY + font::Display.lineHeight + 6, 56, 3, palette::Title);
        if (p.hint) draw_text_centred(c, font::Small, centre, HintY, palette::Note, p.hint);
    } else {
        fill_rect(c, 0, LabelY, c.width, LabelH, palette::Background);
    }
    draw_text_centred(c, font::Body, centre, LabelY, palette::Text, p.label);
    if (!p.bar) return;
    if (full) {
        round_rect(c, BarX - 3, BarY - 3, BarW + 6, BarH + 6, (BarH + 6) / 2, palette::BarFrame);
        if (p.note) draw_text_centred(c, font::Small, centre, NoteY, palette::Note, p.note);
    }
    const uint32_t done = p.done > p.total ? p.total : p.done;
    const int filled = p.total ? int(uint64_t(BarW) * done / p.total) : 0;
    // The bar's inside: the frame's colour, the empty track, then the part done, each with round ends.
    fill_rect(c, BarX, BarY, BarW, BarH, palette::BarFrame);
    round_rect(c, BarX, BarY, BarW, BarH, BarH / 2, palette::BarEmpty);
    if (filled > 0) round_rect(c, BarX, BarY, filled < BarH ? BarH : filled, BarH, BarH / 2, palette::BarFill);
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
    fill_rect(c, 0, CountY, c.width, CountH, palette::Background);
    draw_text_centred(c, font::Small, centre, CountY, palette::Note, count);
}

} // namespace twv::ui
