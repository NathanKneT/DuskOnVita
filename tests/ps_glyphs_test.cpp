// Host test for platform/vita/ui/ps_glyphs.cpp: lookups, ResTIMG headers, tiling, and what the
// glyphs look like where it matters (transparent corners, coloured symbol, white layout symbols).
// TWV_PS_GLYPH_DUMP=<dir> writes each glyph as <label>.rgba (8-byte header: u32 w, u32 h) for review.
#include "ui/ps_glyphs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using twv::ui::GlyphTexture;

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static unsigned be16(const std::uint8_t* p) { return unsigned(p[0]) << 8 | p[1]; }
static unsigned be32(const std::uint8_t* p) { return unsigned(p[0]) << 24 | unsigned(p[1]) << 16 | unsigned(p[2]) << 8 | p[3]; }

static void texel(const GlyphTexture& g, int x, int y, std::uint8_t out[4]) { twv::ui::ps_glyph_texel(g, x, y, out); }

static int opaque_count(const GlyphTexture& g) {
    int n = 0;
    std::uint8_t p[4];
    for (int y = 0; y < g.height; ++y)
        for (int x = 0; x < g.width; ++x) {
            texel(g, x, y, p);
            n += p[3] > 128;
        }
    return n;
}

int main() {
    // Every glyph: a valid big-endian ResTIMG, 32-byte aligned, image right after the header.
    const std::size_t count = twv::ui::ps_glyph_count();
    CHECK(count == 25);
    for (std::size_t i = 0; i < count; ++i) {
        const GlyphTexture* g = twv::ui::ps_glyph_at(i);
        CHECK(g && g->timg);
        if (!g) continue;
        CHECK(reinterpret_cast<std::uintptr_t>(g->timg) % 32 == 0);
        CHECK(g->timg[0] == g->format);
        CHECK(g->format == 3 || g->format == 6);
        CHECK(be16(g->timg + 2) == g->width && be16(g->timg + 4) == g->height);
        CHECK(be32(g->timg + 0x1C) == 0x20);
        CHECK(g->timg[0x18] == 1 && g->timg[0x10] == 0 && g->timg[0x08] == 0);
        const unsigned blocks = unsigned((g->width + 3) / 4) * unsigned((g->height + 3) / 4);
        CHECK(g->bytes == 0x20 + blocks * (g->format == 6 ? 64u : 32u));
        CHECK(opaque_count(*g) > g->width * g->height / 50); // something is drawn
        std::uint8_t corner[4];
        texel(*g, 0, 0, corner);
        CHECK(corner[3] == 0); // every glyph leaves its corners clear
    }
    CHECK(twv::ui::ps_glyph_at(count) == nullptr);

    // Layout lookups: listed name + owning archive + original format and size, else no swap.
    // Fake originals: just the ResTIMG fields the check reads (format, width, height).
    auto original = [](std::uint8_t format, unsigned w, unsigned h) {
        static std::uint8_t t[64][0x20]; // never reused within the test
        static int next = 0;
        std::uint8_t* p = t[next++ % 64];
        std::memset(p, 0, 0x20);
        p[0] = format; p[2] = std::uint8_t(w >> 8); p[3] = std::uint8_t(w); p[4] = std::uint8_t(h >> 8); p[5] = std::uint8_t(h);
        return static_cast<const void*>(p);
    };
    const void* aText = original(0, 32, 46);
    const GlyphTexture* a = twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", "main2d", aText);
    CHECK(a && a->width == 64 && a->height == 92 && a->format == 3);
    CHECK(twv::ui::ps_layout_glyph("TT_ZELDA_BUTTON_A_TEXT.BTI", "Main2D", aText) == a); // case-insensitive
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", "saveres", aText) == a);
    // File select, save and name entry: /res/Object/fileSel.arc (root "filesel").
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", "filesel", aText) == a);
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_b_text.bti", "filesel", original(0, 24, 40)) != nullptr);
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_8ia.bti", "filesel", original(1, 32, 46)) != nullptr);
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", "e3_2006", aText) == nullptr); // E3 demo
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", "stage", aText) == nullptr);   // other archive
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", "main2", aText) == nullptr);   // no prefix match
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", nullptr, aText) == nullptr);   // owner unknown
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", "main2d", nullptr) == nullptr);
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", "main2d", original(0, 32, 45)) == nullptr); // other size
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_a_text.bti", "main2d", original(1, 32, 46)) == nullptr); // other format
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_x.bti", "ringres", original(2, 32, 48)) != nullptr);
    CHECK(twv::ui::ps_layout_glyph("tt_zelda_button_x.bti", "main2d", original(2, 32, 48)) == nullptr);
    CHECK(twv::ui::ps_layout_glyph("tt_title3dstickanim_00.bti", "optres", original(8, 24, 24)) != nullptr);
    CHECK(twv::ui::ps_layout_glyph("c_stick_anim_otah.10.bti", "button", original(3, 56, 56)) != nullptr);
    CHECK(twv::ui::ps_layout_listed("tt_zelda_button_a_text.bti"));
    CHECK(!twv::ui::ps_layout_listed("tt_zelda_button_a_text"));      // exact names only
    CHECK(!twv::ui::ps_layout_listed("tt_zelda_button_ab_maru.bti")); // kept
    CHECK(!twv::ui::ps_layout_listed("im__zelda_button_r_text.bti")); // R stays R
    CHECK(!twv::ui::ps_layout_listed("tt_start_00.bti"));
    CHECK(!twv::ui::ps_layout_listed(nullptr));
    CHECK(twv_ps_glyph_layout_listed("tt_ia4_3d_btn.bti") == 1);
    CHECK(twv_ps_glyph_layout("tt_zelda_button_a_text.bti", "button", aText) == a->timg);
    CHECK(twv_ps_glyph_layout("tt_zelda_button_a_text.bti", "stage", aText) == nullptr);
    if (a) {
        // The cross: white where the diagonals cross, clear between the arms.
        std::uint8_t p[4];
        texel(*a, 32, 46, p);
        CHECK(p[3] == 255 && p[0] == 255);
        texel(*a, 32, 20, p);
        CHECK(p[3] == 0);
    }
    const GlyphTexture* xb = twv::ui::ps_layout_glyph("tt_zelda_button_x_base.bti", "button", original(2, 32, 48));
    CHECK(xb != nullptr);
    if (xb) {
        std::uint8_t p[4];
        texel(*xb, 32, 48, p);
        CHECK(p[3] == 255 && p[0] == 255); // white face, the layout tints it
        texel(*xb, 32, 20, p);              // the disc (radius 30) is centred in the 64x96 frame
        CHECK(p[3] == 255 && p[0] < 64);    // dark rim
    }

    // HUD colours: dark bases, coloured face symbols, everything else untouched.
    {
        unsigned rgb = 0;
        const void* maru = original(2, 40, 40);
        CHECK(!twv::ui::ps_hud_tint(maru, &rgb));                                   // not seen yet
        CHECK(twv_ps_glyph_layout_listed("tt_zelda_button_ab_maru.bti") == 1);
        CHECK(twv_ps_glyph_layout("tt_zelda_button_ab_maru.bti", "saveres", maru) == nullptr);
        CHECK(!twv::ui::ps_hud_tint(maru, &rgb));                                   // menu archive: no
        CHECK(twv_ps_glyph_layout("tt_zelda_button_ab_maru.bti", "main2d", original(2, 40, 41)) == nullptr);
        CHECK(twv_ps_glyph_layout("tt_zelda_button_ab_maru.bti", "main2d", maru) == nullptr); // art kept
        CHECK(twv::ui::ps_hud_tint(maru, &rgb) && rgb == twv::ui::kHudBase);
        CHECK(twv_ps_glyph_hud_tint(maru, &rgb) == 1);
        const GlyphTexture* yb = twv::ui::ps_layout_glyph("tt_zelda_button_y_base.bti", "main2d", original(2, 48, 32));
        CHECK(yb && twv::ui::ps_hud_tint(yb->timg, &rgb) && rgb == twv::ui::kHudBase);
        CHECK(a && twv::ui::ps_hud_tint(a->timg, &rgb) && rgb == twv::ui::kHudCross);
        const GlyphTexture* bt = twv::ui::ps_layout_glyph("tt_zelda_button_b_text.bti", "main2d", original(0, 24, 40));
        CHECK(bt && twv::ui::ps_hud_tint(bt->timg, &rgb) && rgb == twv::ui::kHudCircle);
        const GlyphTexture* sel = twv::ui::ps_layout_glyph("im_zelda_button_z_text.bti", "main2d", original(2, 24, 40));
        CHECK(sel && !twv::ui::ps_hud_tint(sel->timg, &rgb));                       // SELECT stays white
        CHECK(!twv::ui::ps_hud_tint(aText, &rgb) && !twv::ui::ps_hud_tint(nullptr, &rgb));
        CHECK(twv_ps_glyph_hud_tint(aText, nullptr) == 0);
    }

    // In-text icons: icon numbers from d_msg_out_font.cpp, RGBA8, 48x48.
    const GlyphTexture* cross = twv::ui::ps_text_glyph(0, 0);
    CHECK(cross && cross->format == 6 && cross->width == 48 && cross->height == 48);
    if (cross) {
        std::uint8_t p[4];
        texel(*cross, 24, 24, p);
        CHECK(p[3] == 255 && p[2] > 200 && p[0] < 150); // blue cross in the middle
        texel(*cross, 24, 4, p);
        CHECK(p[3] == 255 && p[0] < 40);                // dark disc around it
    }
    CHECK(twv::ui::ps_text_glyph(1, 0) && twv::ui::ps_text_glyph(1, 0) != cross);
    CHECK(twv::ui::ps_text_glyph(8, 0) == nullptr);   // D-pad keeps its art
    CHECK(twv::ui::ps_text_glyph(10, 0) == nullptr);
    CHECK(twv::ui::ps_text_glyph(0, 1) == nullptr);   // only the stick has a second texture
    CHECK(twv::ui::ps_text_glyph(9, 0) == twv::ui::ps_text_glyph(14, 0));
    CHECK(twv::ui::ps_text_glyph(9, 1) && twv::ui::ps_text_glyph(9, 1) != twv::ui::ps_text_glyph(9, 0));
    CHECK(twv::ui::ps_text_glyph(-1, 0) == nullptr && twv::ui::ps_text_glyph(70, 0) == nullptr);
    CHECK(twv_ps_glyph_text(5, 0) == twv::ui::ps_text_glyph(5, 0)->timg);

    if (const char* dir = std::getenv("TWV_PS_GLYPH_DUMP")) {
        for (std::size_t i = 0; i < count; ++i) {
            const GlyphTexture* g = twv::ui::ps_glyph_at(i);
            const std::string path = std::string(dir) + "/" + std::to_string(i) + "_" + g->label + ".rgba";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                const std::uint32_t wh[2] = {g->width, g->height};
                std::fwrite(wh, sizeof wh, 1, f);
                for (int y = 0; y < g->height; ++y)
                    for (int x = 0; x < g->width; ++x) {
                        std::uint8_t p[4];
                        texel(*g, x, y, p);
                        std::fwrite(p, 4, 1, f);
                    }
                std::fclose(f);
            }
        }
    }

    if (failures) return 1;
    std::printf("ps_glyphs_test: %zu glyphs, ResTIMG headers, IA8/RGBA8 tiling, layout names and in-text icons passed\n", count);
    return 0;
}
