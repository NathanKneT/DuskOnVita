#pragma once
// Draws the settings page (ui/settings_page.hpp) with the start-up screens' font and canvas
// (ui/startup_canvas.hpp): the CPU writes a 960x544 A8B8G8R8 buffer that sceDisplay shows. Behind
// the text, the game's last frame dimmed to a quarter (or a plain colour when there is none).
// SDK-free: the host tests draw it into memory (tests/settings_page_test.cpp).
#include "ui/settings_page.hpp"
#include "ui/startup_canvas.hpp"

#include <cstdio>

namespace twv::ui::settings {

// The game's last frame as the display shows it (any size up to 960x544; scaled to the screen).
struct Backdrop {
    const uint32_t* pixels = nullptr;
    int width = 0, height = 0, stride = 0;
};

struct PageText {
    const char* build = "";      // "DuskOnVita 00.01 build 1a2b3c4d (2026-10-28)"
    const char* disc = "";       // "Disc GZ2P01 (Europe)"
    const char* opens = "";      // how the page opens, for the footer
    const char* confirm = "X";   // the confirm button's name ("X" or "O")
    const char* back = "O";
    bool releaseWait = false;    // closing: waiting for every button to be released
};

namespace colors {
constexpr uint32_t Dim = rgb(8, 10, 14), Selected = rgb(36, 58, 92), SelectedEdge = rgb(110, 160, 230),
                   Label = rgb(225, 230, 238), Value = rgb(255, 206, 110), Arrow = rgb(140, 170, 210),
                   Help = rgb(185, 192, 204), Footer = rgb(150, 158, 172), Status = rgb(130, 220, 150),
                   Button = rgb(54, 70, 96), Applies = rgb(130, 210, 255), Rule = rgb(70, 80, 96);
}

// Fills a rectangle with the dimmed backdrop (nearest sample, a quarter of the brightness).
inline void fill_backdrop(const Canvas& c, const Backdrop& b, int x, int y, int w, int h) {
    if (!b.pixels || b.width <= 0 || b.height <= 0) {
        fill_rect(c, x, y, w, h, colors::Dim);
        return;
    }
    const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const int x1 = x + w > c.width ? c.width : x + w, y1 = y + h > c.height ? c.height : y + h;
    for (int yy = y0; yy < y1; ++yy) {
        const int sy = yy * b.height / c.height;
        const uint32_t* src = b.pixels + std::size_t(sy) * std::size_t(b.stride);
        uint32_t* dst = c.pixels + std::size_t(yy) * std::size_t(c.stride);
        for (int xx = x0; xx < x1; ++xx) {
            const uint32_t p = src[xx * b.width / c.width];
            // Each channel to a quarter, opaque.
            dst[xx] = 0xff000000u | ((p >> 2) & 0x003f3f3fu);
        }
    }
}

inline int value_text(const Page& page, int i, char* out, std::size_t size, const PageText& t) {
    const Entry& e = Entries[i];
    if (e.kind == Kind::Action) {
        if ((e.id == Id::ResetDefaults && page.resetArmed) || (e.id == Id::RestartGame && page.restartArmed))
            return std::snprintf(out, size, "Press %s again", t.confirm);
        return std::snprintf(out, size, "Press %s", t.confirm);
    }
    return std::snprintf(out, size, "%s", e.choices[page.index[i]]);
}

// Draws the whole page.
inline void draw_page(const Canvas& c, const Page& page, const Backdrop& backdrop, const PageText& t) {
    fill_backdrop(c, backdrop, 0, 0, c.width, c.height);
    draw_text(c, LabelX, TitleY, RowScale, palette::Title, "Settings");
    {
        // The section of the chosen entry; L and R (or a tap on it) change it.
        char section[48];
        std::snprintf(section, sizeof section, "L <  %s  > R", SectionNames[page.section()]);
        draw_text(c, SectionX, SectionY, SmallScale, colors::Value, section);
    }
    {
        const char* paused = "Game paused";
        draw_text(c, ValueRight - text_width(std::strlen(paused), SmallScale), TitleY + 6, SmallScale, colors::Footer,
                  paused);
    }
    fill_rect(c, LabelX, ListTop - 10, ValueRight - LabelX, 2, colors::Rule);

    int rows[VisibleRows];
    const int n = page.visible(rows);
    for (int r = 0; r < n; ++r) {
        const int i = rows[r];
        const Entry& e = Entries[i];
        const int y = ListTop + r * RowHeight;
        const bool sel = i == page.selected;
        if (sel) {
            fill_rect(c, LabelX - 12, y - 4, ValueRight - LabelX + 24, RowHeight - 2, colors::Selected);
            fill_rect(c, LabelX - 12, y - 4, 4, RowHeight - 2, colors::SelectedEdge);
        }
        draw_text(c, LabelX, y, RowScale, sel ? palette::Text : colors::Label, e.label);
        char value[40];
        value_text(page, i, value, sizeof value, t);
        const std::size_t len = std::strlen(value);
        const int w = text_width(len, RowScale);
        const int arrowW = text_width(1, RowScale);
        if (e.kind == Kind::Choice) {
            const bool several = page.choice_count(e.id) > 1;
            if (several) {
                draw_text(c, ValueX, y, RowScale, colors::Arrow, "<");
                draw_text(c, ValueRight - arrowW, y, RowScale, colors::Arrow, ">");
            }
            const int mid = (ValueX + ValueRight) / 2;
            draw_text(c, mid - w / 2, y, RowScale, several ? colors::Value : colors::Footer, value);
        } else {
            draw_text(c, ValueRight - w, y, RowScale, sel ? colors::Value : colors::Footer, value);
        }
    }
    if (page.more_above()) draw_text(c, ValueRight + 14, ListTop, SmallScale, colors::Arrow, "^");
    if (page.more_below())
        draw_text(c, ValueRight + 14, ListTop + (VisibleRows - 1) * RowHeight + 8, SmallScale, colors::Arrow, "v");

    // Help for the chosen entry: when it applies, then its text (or the last action's result).
    fill_rect(c, LabelX, HelpY - 6, ValueRight - LabelX, 2, colors::Rule);
    const Entry& e = page.current();
    char help[320];
    const char* applies = e.applies == Applies::Restart ? "Applies at the next start. " :
                          e.applies == Applies::NextArea ? "Applies at the next area. " : "";
    if (e.id == Id::Language && page.choice_count(Id::Language) == 1) {
        std::snprintf(help, sizeof help, "%sYour disc has %s only (Europe: English, German, French, Spanish, Italian).",
                      applies, e.choices[page.index[int(Id::Language)]]);
    } else {
        std::snprintf(help, sizeof help, "%s%s", applies, page.status ? page.status : e.help);
    }
    TextLine lines[HelpLines];
    const std::size_t columns = std::size_t((ValueRight - LabelX) / (FontAdvance * SmallScale));
    const int count = wrap_text(help, columns, lines, HelpLines);
    const std::size_t appliesLen = std::strlen(applies);
    for (int k = 0; k < count; ++k) {
        const int y = HelpY + k * line_height(SmallScale);
        const char* line = help + lines[k].start;
        // "Applies at ..." in its own colour on the first line.
        if (k == 0 && appliesLen && lines[k].start == 0 && lines[k].length >= appliesLen - 1) {
            const int w = draw_text(c, LabelX, y, SmallScale, colors::Applies, line, appliesLen - 1);
            const std::size_t rest = lines[k].length > appliesLen ? lines[k].length - appliesLen : 0;
            draw_text(c, LabelX + w + FontAdvance * SmallScale, y, SmallScale, page.status ? colors::Status : colors::Help,
                      line + appliesLen, rest);
        } else {
            draw_text(c, LabelX, y, SmallScale, page.status && k == count - 1 ? colors::Status : colors::Help, line,
                      lines[k].length);
        }
    }

    // Footer: the controls, the Close button; how the page opens, the build and the disc.
    char controls[160];
    if (t.releaseWait)
        std::snprintf(controls, sizeof controls, "Release every button to resume the game...");
    else
        std::snprintf(controls, sizeof controls, "Up/Down: entry  Left/Right: change  %s: select  %s: close",
                      t.confirm, t.back);
    draw_text(c, LabelX, FooterY, SmallScale, t.releaseWait ? colors::Status : colors::Footer, controls);
    fill_rect(c, CloseButtonX, CloseButtonY, CloseButtonW, CloseButtonH, colors::Button);
    {
        const char* close = "Close";
        draw_text(c, CloseButtonX + (CloseButtonW - text_width(5, SmallScale)) / 2, CloseButtonY + 7, SmallScale,
                  palette::Text, close);
    }
    const std::size_t columns2 = std::size_t((Width - LabelX - 16) / (FontAdvance * SmallScale));
    draw_text(c, LabelX, FooterLine2Y, SmallScale, colors::Footer, t.opens, columns2);
    char info[200];
    std::snprintf(info, sizeof info, "%s  |  %s", t.build, t.disc);
    draw_text(c, LabelX, FooterLine3Y, SmallScale, colors::Footer, info, columns2);
}

} // namespace twv::ui::settings
