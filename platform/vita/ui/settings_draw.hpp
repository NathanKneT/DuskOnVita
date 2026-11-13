#pragma once
// Draws the settings page (ui/settings_page.hpp) on the start-up screens' canvas, with the page's
// own font and shapes (ui/ui_paint.hpp): the CPU writes a 960x544 A8B8G8R8 buffer that sceDisplay
// shows. Behind everything, the game's last frame dimmed to a quarter (or a plain colour when there
// is none), under two translucent panels: the section list on the left, the chosen section's entries
// on the right. SDK-free: the host tests draw it into memory (tests/settings_page_test.cpp).
//
// What a row shows: a dot when its value is not the default, the label, "next start" or "next area"
// when the change waits for one, and the value as a switch (Off / On), a stepped slider (sizes and
// sensitivities), a choice with arrows on the chosen row, or a button (actions).
#include "ui/settings_page.hpp"
#include "ui/ui_paint.hpp"

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
constexpr uint32_t Dim = rgb(8, 10, 14), Sidebar = rgb(13, 11, 22), Panel = rgb(21, 19, 34),
                   Accent = rgb(255, 184, 92), AccentInk = rgb(44, 30, 8), Text = rgb(240, 238, 248),
                   Label = rgb(208, 205, 224), Value = Accent, Help = rgb(176, 172, 198), Footer = rgb(142, 138, 166),
                   Faint = rgb(106, 102, 130), Selected = rgb(54, 50, 90), SectionSelected = rgb(41, 38, 68),
                   Track = rgb(74, 70, 100), Knob = rgb(216, 214, 228), Status = rgb(130, 220, 150),
                   Rule = rgb(54, 51, 78), Button = rgb(60, 56, 96);
constexpr unsigned SidebarAlpha = 238, PanelAlpha = 218;
} // namespace colors

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

// How an entry's value is drawn.
enum class Control : uint8_t { Switch, Slider, Choice, Button };
constexpr Control control_of(const Entry& e) {
    if (e.kind == Kind::Action) return Control::Button;
    if (e.choices == text::OffOn) return Control::Switch;
    if (e.id == Id::RenderScale || e.id == Id::GyroSensitivity || e.id == Id::HudSize) return Control::Slider;
    return Control::Choice;
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

// A button's name in a small rounded box; returns the box's width.
inline int key_cap(const Canvas& c, int x, int y, const char* name) {
    const int text = text_width(font::Small, name) + 14, w = text < 24 ? 24 : text;
    round_rect(c, x, y, w, 22, 6, colors::Track);
    draw_text_centred(c, font::Small, x + w / 2, y + 2, colors::Text, name);
    return w;
}
// The same box with the outline of a triangle in it (the Triangle button).
inline int key_cap_triangle(const Canvas& c, int x, int y) {
    round_rect(c, x, y, 24, 22, 6, colors::Track);
    // An upright triangle, 13 wide and 11.5 high, drawn by each pixel's distance to its three edges.
    const float cx = float(x) + 12.0f, top = float(y) + 5.0f, bottom = float(y) + 16.5f, half = 6.5f;
    const float slope = (bottom - top) / half, norm = std::sqrt(slope * slope + 1.0f);
    for (int py = y + 3; py < y + 20; ++py)
        for (int px = x + 3; px < x + 21; ++px) {
            const float fx = float(px) + 0.5f - cx, fy = float(py) + 0.5f;
            // Positive inside: the base, then the nearer side.
            const float base = bottom - fy;
            const float side = ((fy - top) - slope * (fx < 0 ? -fx : fx)) / norm;
            const float inside = base < side ? base : side;
            // A line 1.5 pixels wide along the edge.
            const float cover = inside < 0.75f ? inside + 0.5f : 2.25f - inside;
            if (cover <= 0.0f || px < 0 || py < 0 || px >= c.width || py >= c.height) continue;
            uint32_t& p = c.pixels[std::size_t(py) * std::size_t(c.stride) + std::size_t(px)];
            p = mix(p, colors::Text, cover >= 1.0f ? 255u : unsigned(cover * 255.0f));
        }
    return 24;
}

// The value of entry i in its row at `y` (the row's top).
inline void draw_value(const Canvas& c, const Page& page, int i, int y, bool sel, const PageText& t) {
    const Entry& e = Entries[i];
    const int textY = y + (RowHeight - font::Body.lineHeight) / 2, mid = y + RowHeight / 2;
    char value[40];
    value_text(page, i, value, sizeof value, t);
    switch (control_of(e)) {
    case Control::Button: {
        const int w = text_width(font::Body, value) + 32;
        round_rect(c, ValueRight - w, y + 4, w, RowHeight - 8, (RowHeight - 8) / 2, sel ? colors::Accent : colors::Button);
        draw_text_centred(c, font::Body, ValueRight - w / 2, textY, sel ? colors::AccentInk : colors::Label, value);
        break;
    }
    case Control::Switch: {
        const bool on = page.index[i] == 1;
        const int w = 46, h = 24, x = ValueRight - w;
        round_rect(c, x, mid - h / 2, w, h, h / 2, on ? colors::Accent : colors::Track);
        round_rect(c, on ? x + w - 21 : x + 3, mid - 9, 18, 18, 9, on ? rgb(255, 255, 255) : colors::Knob);
        draw_text_right(c, font::Body, x - 14, textY, on ? colors::Value : colors::Footer, value);
        break;
    }
    case Control::Slider: {
        // The value, then a track with one mark per step and the knob on the chosen one.
        const int x0 = ValueX + 84, x1 = ValueRight - 9, steps = e.count > 1 ? e.count - 1 : 1;
        const int knob = x0 + (x1 - x0) * page.index[i] / steps;
        draw_text_right(c, font::Body, ValueX + 64, textY, colors::Value, value);
        round_rect(c, x0 - 3, mid - 3, x1 - x0 + 6, 6, 3, colors::Track);
        round_rect(c, x0 - 3, mid - 3, knob - x0 + 6, 6, 3, colors::Accent);
        for (int k = 0; k <= steps; ++k) {
            const int at = x0 + (x1 - x0) * k / steps;
            fill_rect(c, at - 1, mid - 1, 2, 2, k <= page.index[i] ? colors::AccentInk : colors::Footer);
        }
        round_rect(c, knob - 9, mid - 9, 18, 18, 9, rgb(255, 255, 255));
        break;
    }
    case Control::Choice: {
        // The value ends where an arrow would start; the arrows show on the chosen row only, and
        // only when there is something to change to.
        const bool several = page.choice_count(e.id) > 1;
        const int arrow = text_width(font::Body, ">"), right = ValueRight - arrow - 10;
        const int w = text_width(font::Body, value);
        draw_text(c, font::Body, right - w, textY, several ? colors::Value : colors::Footer, value);
        if (sel && several) {
            draw_text(c, font::Body, ValueRight - arrow, textY, colors::Text, ">");
            draw_text(c, font::Body, right - w - 10 - arrow, textY, colors::Text, "<");
        }
        break;
    }
    }
}

// Draws the whole page.
inline void draw_page(const Canvas& c, const Page& page, const Backdrop& backdrop, const PageText& t) {
    fill_backdrop(c, backdrop, 0, 0, c.width, c.height);
    blend_rect(c, 0, 0, SidebarW, Height, colors::Sidebar, colors::SidebarAlpha);
    blend_rect(c, SidebarW, 0, Width - SidebarW, Height, colors::Panel, colors::PanelAlpha);

    // --- left: the title, the sections, what the shoulder buttons do, the build and the disc ---
    draw_text(c, font::Title, TitleX, TitleY, palette::Title, "Settings");
    const int current = page.section();
    for (int k = 0; k < SectionCount; ++k) {
        const int y = SectionTop + k * SectionHeight;
        const bool sel = k == current;
        if (sel) {
            round_rect(c, SectionX, y + 2, SectionW, SectionHeight - 4, 9, colors::SectionSelected);
            round_rect(c, SectionX + 6, y + 10, 4, SectionHeight - 20, 2, colors::Accent);
        }
        draw_text(c, sel ? font::Strong : font::Body, SectionX + 20, y + (SectionHeight - font::Body.lineHeight) / 2,
                  sel ? colors::Text : page.section_shown(k) ? colors::Label : colors::Faint, SectionNames[k]);
    }
    {
        int x = TitleX;
        x += key_cap(c, x, SidebarInfoY, "L") + 4;
        x += key_cap(c, x, SidebarInfoY, "R") + 9;
        draw_text(c, font::Small, x, SidebarInfoY + 2, colors::Footer, "Section");
        // The build on up to two lines, then the disc.
        TextLine lines[2];
        const int count = wrap_text(font::Small, t.build, SidebarW - 2 * TitleX, lines, 2);
        int y = SidebarInfoY + 36;
        for (int k = 0; k < count; ++k, y += font::Small.lineHeight)
            draw_text(c, font::Small, TitleX, y, colors::Faint, t.build + lines[k].start, lines[k].length);
        draw_text(c, font::Small, TitleX, y, colors::Faint, t.disc);
    }

    // --- right: the section, its entries ---
    {
        const int w = draw_text(c, font::Strong, ContentX + 4, HeadingY, colors::Accent, SectionNames[current]);
        draw_text(c, font::Small, ContentX + 4 + w + 14, HeadingY + 4, colors::Footer, SectionNotes[current]);
        draw_text_right(c, font::Small, ContentRight, HeadingY + 4, colors::Faint, "Game paused");
    }
    fill_rect(c, ContentX, ListTop - 10, ContentRight - ContentX, 1, colors::Rule);

    int rows[VisibleRows];
    const int n = page.visible(rows);
    for (int r = 0; r < n; ++r) {
        const int i = rows[r];
        const Entry& e = Entries[i];
        const int y = ListTop + r * RowHeight, textY = y + (RowHeight - font::Body.lineHeight) / 2;
        const bool sel = i == page.selected;
        if (sel) round_rect(c, ContentX, y + 1, ContentRight - ContentX, RowHeight - 2, 9, colors::Selected);
        if (e.kind == Kind::Choice && page.index[i] != page.defaults[i])
            round_rect(c, ContentX + 7, y + RowHeight / 2 - 3, 6, 6, 3, colors::Accent);
        const int w = draw_text(c, sel ? font::Strong : font::Body, LabelX, textY, sel ? colors::Text : colors::Label,
                                e.label);
        if (e.applies != Applies::Now)
            draw_text(c, font::Small, LabelX + w + 10, textY + 4, colors::Faint,
                      e.applies == Applies::Restart ? "next start" : "next area");
        draw_value(c, page, i, y, sel, t);
    }
    {
        // A bar on the right when the section has more entries than rows.
        int above = 0;
        const int total = page.section_rows(&above), height = VisibleRows * RowHeight;
        if (total > VisibleRows) {
            const int thumb = height * VisibleRows / total, offset = height * above / total;
            round_rect(c, ContentRight + 8, ListTop, 4, height, 2, colors::Rule);
            round_rect(c, ContentRight + 8, ListTop + offset, 4, thumb, 2, colors::Footer);
        }
    }

    // --- the help of the chosen entry, or the result of the last action ---
    fill_rect(c, ContentX, HelpY - 8, ContentRight - ContentX, 1, colors::Rule);
    const Entry& e = page.current();
    char help[320];
    if (e.id == Id::Language && page.choice_count(Id::Language) == 1) {
        std::snprintf(help, sizeof help, "Your disc has %s only (Europe: English, German, French, Spanish, Italian).",
                      e.choices[page.index[int(Id::Language)]]);
    } else {
        std::snprintf(help, sizeof help, "%s", page.status ? page.status : e.help);
    }
    TextLine lines[HelpLines];
    const int count = wrap_text(font::Small, help, ContentRight - ContentX - 8, lines, HelpLines);
    for (int k = 0; k < count; ++k)
        draw_text(c, font::Small, ContentX + 4, HelpY + k * HelpLineHeight, page.status ? colors::Status : colors::Help,
                  help + lines[k].start, lines[k].length);

    // --- the controls, the Close button, how the page opens ---
    if (t.releaseWait) {
        draw_text(c, font::Small, ContentX + 4, FooterY + 2, colors::Status, "Release every button to resume the game...");
    } else {
        int x = ContentX + 4;
        const auto hint = [&](const char* what) {
            x += 7;
            x += draw_text(c, font::Small, x, FooterY + 2, colors::Footer, what) + 18;
        };
        x += key_cap(c, x, FooterY, t.confirm);
        hint("Change");
        x += key_cap(c, x, FooterY, t.back);
        hint("Close");
        x += key_cap_triangle(c, x, FooterY);
        hint("Default");
        x += key_cap(c, x, FooterY, "L") + 4;
        x += key_cap(c, x, FooterY, "R");
        hint("Section");
    }
    round_rect(c, CloseButtonX, CloseButtonY, CloseButtonW, CloseButtonH, CloseButtonH / 2, colors::Button);
    draw_text_centred(c, font::Body, CloseButtonX + CloseButtonW / 2,
                      CloseButtonY + (CloseButtonH - font::Body.lineHeight) / 2, colors::Text, "Close");
    {
        // How the page opens, cut to the line's width.
        std::size_t fits = std::strlen(t.opens);
        while (fits && text_width(font::Small, t.opens, fits) > ContentRight - ContentX - 8) --fits;
        draw_text(c, font::Small, ContentX + 4, FooterLine2Y, colors::Faint, t.opens, fits);
    }
}

} // namespace twv::ui::settings
