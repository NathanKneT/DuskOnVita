// The map screens' pictures: the off-screen pass of d_map_path.cpp, drawn on the CPU.
//
// The game draws a map into a frame buffer of its own (GXCreateFrameBuffer), in flat colours whose
// red byte is a palette index, copies it to an 8-bit texture (GXCopyTex, GX_CTF_R8) and shows that
// texture through its palette. The renderer has no off-screen frame buffer and no copy, so this
// one pass is followed here from end to end:
//   - the pass begins (map_pass_begin);
//   - its first draw tells whether it is a map: the game's "back quad", four vertices in one flat
//     colour with no texture, no blending and no depth test (map_pass_draw). Any other pass
//     (shadows, bloom, depth of field) is left to the caller, which drops it as before;
//   - each following flat polygon and line is drawn into an 8-bit picture (map_raster.hpp);
//   - the copy keeps that picture inside the copy's own texture (map_pass_resolve): Aurora may ask
//     for the palette conversion again later without a new copy;
//   - the conversion turns indices into colours and uploads them to the texture the game samples
//     (map_palette_convert).
// A pass that sent only its back quad keeps nothing: that is the minimap while
// --vita-fast-unseen-map leaves its path out (patches/dusklight/0052 and 0073), and its picture
// stays undrawn as before.
//
// The textured squares (dungeon doors, minimap icons) are drawn too, from the icon's own I4 texels
// and the game's two-stage "intensity to palette index" rule. Not drawn: the round line ends
// (GX_POINTS) of the field map's decoration lines.
//
// Everything here runs where Aurora processes the GX stream (the GX worker thread in the play
// profile), in stream order: no lock.
#include "internal.hpp"
#include "map_raster.hpp"
#include <cmath>

namespace twv::vgl {
namespace {
enum class Pass : uint8_t { Unknown, Map, Other };
Pass pass = Pass::Other;
uint32_t passW = 0, passH = 0;
uint8_t backIndex = 0;
unsigned pathDraws = 0;
MapPicture picture;
std::vector<float> positions, coordinates;
std::vector<uint8_t> colours;

bool flat_colour(const Pipeline& p) noexcept { return p.info.sampledTextures.none(); }
// The red byte of a TEV colour register (1 = GX_TEVREG0): the game sets {index, 0, 0, 0}.
uint8_t register_red(unsigned reg) noexcept {
    return uint8_t(std::clamp(std::lround(aurora::gx::g_gxState.colorRegs[reg][0] * 255.f), 0l, 255l));
}
uint8_t palette_index() noexcept { return register_red(1); }

// A door or icon square: quads with a texture coordinate, texture 0 an I4 picture, two TEV stages.
bool icon_squares(const Pipeline& p, const aurora::gx::DrawData& data, float left, float top, float width, float height) noexcept {
    const auto& c = p.config.shaderConfig;
    const auto& object = aurora::gx::g_gxState.textures[0].texObj;
    const auto* texels = static_cast<const uint8_t*>(object.data);
    if (c.lineMode || c.tevStageCount != 2 || !p.info.sampledTextures.test(0) || !texels || object.format() != GX_TF_I4 ||
        !object.width() || !object.height() || data.vtxCount < 4)
        return false;
    positions.resize(size_t(data.vtxCount) * 2);
    coordinates.resize(size_t(data.vtxCount) * 2);
    if (!decode_screen_positions(p, data, left, top, width, height, positions.data(), coordinates.data())) return false;
    const unsigned base = register_red(2), floor = register_red(3); // GX_TEVREG1, GX_TEVREG2
    const uint32_t w = object.width(), h = object.height();
    const auto texel = [&](float u, float v, uint8_t& index) { return map_icon_index(map_i4_texel(texels, w, h, u, v), base, floor, index); };
    const auto tri = [&](uint32_t i, uint32_t j, uint32_t k) {
        if (i < data.vtxCount && j < data.vtxCount && k < data.vtxCount)
            picture.textured(&positions[2 * i], &positions[2 * j], &positions[2 * k], &coordinates[2 * i], &coordinates[2 * j],
                             &coordinates[2 * k], texel);
    };
    if (data.indexCount) {
        const auto* indices = reinterpret_cast<const uint16_t*>(uintptr_t(data.idxRange.offset));
        for (uint32_t i = 0; i + 2 < data.indexCount; i += 3) tri(indices[i], indices[i + 1], indices[i + 2]);
    } else if (data.primitive == GX_QUADS) {
        for (uint32_t i = 0; i + 3 < data.vtxCount; i += 4) {
            tri(i, i + 1, i + 2);
            tri(i, i + 2, i + 3);
        }
    } else {
        return false;
    }
    return true;
}
}

void map_pass_begin(uint32_t w, uint32_t h) noexcept {
    pass = Pass::Unknown;
    passW = w;
    passH = h;
    pathDraws = 0;
}

bool map_pass_draw(const Pipeline& p, const aurora::gx::DrawData& data, float left, float top, float width, float height) noexcept {
    const auto& c = p.config;
    if (pass == Pass::Unknown) {
        const bool back = flat_colour(p) && !c.shaderConfig.lineMode && c.shaderConfig.tevStageCount == 1 && data.vtxCount == 4 &&
                          c.blendMode == GX_BM_NONE && !c.depthCompare && passW && passH && passW <= 1024 && passH <= 1024;
        pass = back ? Pass::Map : Pass::Other;
        if (back) backIndex = palette_index();
        return back; // the back quad covers the whole target: the picture starts filled with it
    }
    if (pass != Pass::Map) return false;
    if (!data.vtxCount) return true;
    if (!flat_colour(p) || c.shaderConfig.lineMode == 3) {
        // A square on a picture that has no path yet would start it: only on one that has.
        if (pathDraws && icon_squares(p, data, left, top, width, height)) count("map_icons_drawn");
        else count("map_draws_not_drawn");
        return true;
    }
    positions.resize(size_t(data.vtxCount) * 2);
    if (!decode_screen_positions(p, data, left, top, width, height, positions.data())) {
        count("map_draws_not_drawn");
        return true;
    }
    if (!pathDraws) picture.begin(passW, passH, backIndex);
    ++pathDraws;
    const int line = c.shaderConfig.lineMode;
    const MapShape shape = line == 1 ? MapShape::Lines : line == 2 ? MapShape::LineStrip :
        data.primitive == GX_TRIANGLESTRIP ? MapShape::Strip : data.primitive == GX_TRIANGLEFAN ? MapShape::Fan :
        data.primitive == GX_QUADS ? MapShape::Quads : MapShape::Triangles;
    // A line's index list is the quad Aurora's own shader would instance: not the line.
    const auto* indices = line ? nullptr : reinterpret_cast<const uint16_t*>(uintptr_t(data.idxRange.offset));
    // GX line widths are sixths of a pixel.
    map_draw(picture, shape, positions.data(), data.vtxCount, indices, line ? 0 : data.indexCount, palette_index(),
             float(aurora::gx::g_gxState.lineWidth) / 6.f);
    count("map_draws_drawn");
    return true;
}

bool map_pass_resolve(Texture& into, uint32_t format) noexcept {
    if (format != GX_CTF_R8) return false;
    if (pass != Pass::Map || !pathDraws || into.size.width != picture.w || into.size.height != picture.h) {
        into.cpu.clear(); // no picture this time: a conversion asked for later must not show an old one
        return false;
    }
    into.cpu = picture.px;
    count("map_pictures_copied");
    return true;
}

bool map_palette_convert(Texture* source, Texture* target, Texture* palette, bool eightBit) noexcept {
    if (!source || !target || !palette) return false;
    const size_t pixels = size_t(target->size.width) * target->size.height;
    if (!eightBit || !target->gpu || source->cpu.size() != pixels || source->size.width != target->size.width ||
        palette->cpu.size() < 4) {
        target->efbCopy = true; // nothing to show: draws that sample it are left out, as before
        return false;
    }
    colours.resize(pixels * 4);
    map_colours(source->cpu.data(), pixels, palette->cpu.data(), palette->cpu.size() / 4, colours.data());
    twv_vgl_texture_write(target->gpu, 0, target->size.width, target->size.height, colours.data());
    target->efbCopy = false;
    count("map_pictures_shown");
    return true;
}
}
