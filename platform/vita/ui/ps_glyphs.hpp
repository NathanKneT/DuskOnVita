#pragma once
// PlayStation-style button glyphs for game.buttonPrompts=playstation (patches/dusklight/0016).
//
// Every glyph is drawn here in code at first use: plain geometric shapes (cross, circle, square,
// triangle, keys, sticks) and a 5x7 pixel font. No Sony, Nintendo or firmware artwork is used.
// The result is a GameCube texture (ResTIMG header + tiled image) that goes through the game's
// normal texture path, so vitaGL and GXM draw it like any texture from the disc.
//
// Layout textures (the HUD, button guides and menus) keep the original's role: white symbols and
// white bases in IA8 that the layout tints exactly as it tints the GameCube letters and bases.
// In-text icons are full-colour RGBA8 and are drawn with a neutral tint (d_msg_out_font.cpp).
// Mapping (platform/vita/InputVita.cpp): A=cross, B=circle, X=square, Y=triangle, Z=SELECT,
// control stick=left stick, C stick=right stick; L, R, START and the D-pad keep their art.
#include <cstddef>
#include <cstdint>

namespace twv::ui {

struct GlyphTexture {
    const std::uint8_t* timg = nullptr; // ResTIMG (big-endian header) followed by the image
    std::uint32_t bytes = 0;
    std::uint16_t width = 0, height = 0;
    std::uint8_t format = 0;            // GX_TF_IA8 (3) or GX_TF_RGBA8 (6)
    const char* label = "";
};

// Whether a layout texture name is on the button list (case-insensitive, as the layouts store it).
bool ps_layout_listed(const char* name);
// Replacement for a listed layout texture, only when it resolved from one of the archives that
// hold that button texture (volume = the archive's root name, e.g. "main2d") and the original
// (its ResTIMG) has the expected format and size. Anything else keeps its art: nullptr.
const GlyphTexture* ps_layout_glyph(const char* name, const char* volume, const void* original);
// Replacement for an in-text icon: index is COutFont_c's icon number (0..69), layer 0 the icon's
// texture, layer 1 the second texture some icons blend with (the tilted stick). nullptr = keep.
const GlyphTexture* ps_text_glyph(int index, int layer);
// The A/B base (tt_zelda_button_ab_maru) keeps its art but is remembered, when it resolves from
// the HUD archives with its expected format and size, so the HUD can recolour its panes.
bool ps_tint_only_note(const char* name, const char* volume, const void* original);
// The HUD colour for a picture's texture (see twv_ps_glyph_hud_tint below).
bool ps_hud_tint(const void* timg, unsigned* rgb);
// Every generated glyph, for tests and receipts.
std::size_t ps_glyph_count();
const GlyphTexture* ps_glyph_at(std::size_t i);
// Decodes one texel of a generated glyph back to straight RGBA8 (tests).
void ps_glyph_texel(const GlyphTexture& g, int x, int y, std::uint8_t rgba[4]);

} // namespace twv::ui

extern "C" int twv_ps_glyph_layout_listed(const char* name);
extern "C" const void* twv_ps_glyph_layout(const char* name, const char* volume, const void* original);
extern "C" const void* twv_ps_glyph_text(int index, int layer);
// HUD colours (playstation only, the main HUD and its button guide): for a picture's texture,
// the RGB its "white" colour takes, or 0 to leave it. Button bases (the A/B disc, kept from the
// disc and remembered when a layout resolves it, and the X/Y/Z base glyphs) turn dark; the
// face-button symbols take their own colours.
extern "C" int twv_ps_glyph_hud_tint(const void* timg, unsigned* rgb);
extern "C" void twv_ps_glyph_hud_tinted(const char* screen, int panes);
namespace twv::ui {
constexpr unsigned kHudBase = 0x2C2C30, kHudCross = 0x7CB4F0, kHudCircle = 0xFF6A6E, kHudSquare = 0xF09CD0,
                   kHudTriangle = 0x40E0B0;
}
