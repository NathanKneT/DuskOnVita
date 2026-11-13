#pragma once
// The map screens' pictures, drawn on the CPU.
//
// The game draws the dungeon map and the field map into a frame buffer of their own, in flat
// colours whose red byte is a palette index, copies that to an 8-bit texture and shows it through a
// palette (d_map_path.cpp). The renderer has no off-screen frame buffer, so these few flat polygons
// and lines are drawn here instead, straight into an 8-bit picture: one byte per pixel, row 0 at
// the top. No blending and no anti-aliasing: the original has neither in this pass (GX_BM_NONE,
// and four identical palette entries per colour).
//
// SDK-free: the host test (tests/map_raster_test.cpp) builds this file alone.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace twv::vgl {
struct MapPicture {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;

    // A new picture, every pixel at the back colour.
    void begin(uint32_t width, uint32_t height, uint8_t back) {
        w = width;
        h = height;
        px.assign(size_t(w) * h, back);
    }

    // A pixel is covered when its centre is inside. Either winding (the pass culls nothing).
    void triangle(const float* a, const float* b, const float* c, uint8_t index) noexcept {
        const float* v[3] = {a, b, c};
        for (const float* p : v)
            if (!std::isfinite(p[0]) || !std::isfinite(p[1])) return;
        if (!w || !h) return;
        const float top = std::clamp(std::min({a[1], b[1], c[1]}), -1.f, float(h) + 1.f);
        const float bottom = std::clamp(std::max({a[1], b[1], c[1]}), -1.f, float(h) + 1.f);
        const int y0 = std::max(0, int(std::ceil(top - 0.5f)));
        const int y1 = std::min(int(h) - 1, int(std::ceil(bottom - 0.5f)) - 1);
        for (int y = y0; y <= y1; ++y) {
            const float yc = float(y) + 0.5f;
            float left = 0.f, right = 0.f;
            bool any = false;
            for (int e = 0; e < 3; ++e) {
                const float* p = v[e];
                const float* q = v[(e + 1) % 3];
                if ((p[1] <= yc) == (q[1] <= yc)) continue;
                // In double: a map zoomed in puts corners far outside the picture.
                const float x = float(double(p[0]) + (double(yc) - p[1]) * (double(q[0]) - p[0]) / (double(q[1]) - p[1]));
                left = any ? std::min(left, x) : x;
                right = any ? std::max(right, x) : x;
                any = true;
            }
            if (!any) continue;
            left = std::clamp(left, -1.f, float(w) + 1.f);
            right = std::clamp(right, -1.f, float(w) + 1.f);
            const int x0 = std::max(0, int(std::ceil(left - 0.5f)));
            const int x1 = std::min(int(w) - 1, int(std::ceil(right - 0.5f)) - 1);
            if (x1 >= x0) std::memset(px.data() + size_t(y) * w + x0, index, size_t(x1 - x0 + 1));
        }
    }

    // A textured triangle (the door and icon squares): `texel(u, v, index)` gives a pixel's palette
    // index from its texture coordinates, or false to leave the pixel as it is.
    template <class Texel>
    void textured(const float* a, const float* b, const float* c, const float* ta, const float* tb, const float* tc,
                  Texel&& texel) noexcept {
        const float* v[3] = {a, b, c};
        for (const float* p : v)
            if (!std::isfinite(p[0]) || !std::isfinite(p[1])) return;
        if (!w || !h) return;
        const double area = (double(b[0]) - a[0]) * (double(c[1]) - a[1]) - (double(c[0]) - a[0]) * (double(b[1]) - a[1]);
        if (std::fabs(area) < 1e-9) return;
        const auto lo = [](float value, float limit) { return int(std::floor(std::clamp(value, -1.f, limit + 1.f))); };
        const int x0 = std::max(0, lo(std::min({a[0], b[0], c[0]}), float(w)));
        const int x1 = std::min(int(w) - 1, lo(std::max({a[0], b[0], c[0]}), float(w)));
        const int y0 = std::max(0, lo(std::min({a[1], b[1], c[1]}), float(h)));
        const int y1 = std::min(int(h) - 1, lo(std::max({a[1], b[1], c[1]}), float(h)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double wa = ((b[0] - px) * (c[1] - py) - (c[0] - px) * (b[1] - py)) / area;
                const double wb = ((c[0] - px) * (a[1] - py) - (a[0] - px) * (c[1] - py)) / area;
                const double wc = 1.0 - wa - wb;
                if (wa < 0 || wb < 0 || wc < 0) continue;
                uint8_t index = 0;
                if (texel(float(wa * ta[0] + wb * tb[0] + wc * tc[0]), float(wa * ta[1] + wb * tb[1] + wc * tc[1]), index))
                    px_at(x, y) = index;
            }
        }
    }
    uint8_t& px_at(int x, int y) noexcept { return px[size_t(y) * w + x]; }

    // A line of `width` pixels (never thinner than one: a thinner one would have gaps), with square
    // ends so that the segments of a strip meet.
    void segment(const float* a, const float* b, float width, uint8_t index) noexcept {
        if (!std::isfinite(a[0]) || !std::isfinite(a[1]) || !std::isfinite(b[0]) || !std::isfinite(b[1])) return;
        const float half = std::max(width, 1.f) * 0.5f;
        float dx = b[0] - a[0], dy = b[1] - a[1];
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 1e-4f) {
            dx = 1.f;
            dy = 0.f;
        } else {
            dx /= length;
            dy /= length;
        }
        const float ex = dx * half, ey = dy * half; // along the line
        const float nx = -dy * half, ny = dx * half; // across it
        const float q[4][2] = {{a[0] - ex + nx, a[1] - ey + ny}, {a[0] - ex - nx, a[1] - ey - ny},
                               {b[0] + ex - nx, b[1] + ey - ny}, {b[0] + ex + nx, b[1] + ey + ny}};
        triangle(q[0], q[1], q[2], index);
        triangle(q[0], q[2], q[3], index);
    }
};

enum class MapShape : uint8_t { Triangles, Strip, Fan, Quads, Lines, LineStrip };

// One draw: `count` screen positions (x, y pairs), as the game sent them. `indices` (a triangle
// list) replaces the shape's own order when the front end already expanded it.
inline unsigned map_draw(MapPicture& picture, MapShape shape, const float* xy, uint32_t count, const uint16_t* indices,
                         uint32_t indexCount, uint8_t index, float lineWidth) noexcept {
    unsigned shapes = 0;
    const auto at = [&](uint32_t i) { return xy + size_t(i) * 2; };
    const auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a < count && b < count && c < count) {
            picture.triangle(at(a), at(b), at(c), index);
            ++shapes;
        }
    };
    if (shape == MapShape::Lines) {
        for (uint32_t i = 0; i + 1 < count; i += 2, ++shapes) picture.segment(at(i), at(i + 1), lineWidth, index);
    } else if (shape == MapShape::LineStrip) {
        for (uint32_t i = 0; i + 1 < count; ++i, ++shapes) picture.segment(at(i), at(i + 1), lineWidth, index);
    } else if (indices && indexCount) {
        for (uint32_t i = 0; i + 2 < indexCount; i += 3) tri(indices[i], indices[i + 1], indices[i + 2]);
    } else if (shape == MapShape::Triangles) {
        for (uint32_t i = 0; i + 2 < count; i += 3) tri(i, i + 1, i + 2);
    } else if (shape == MapShape::Strip) {
        for (uint32_t i = 0; i + 2 < count; ++i) tri(i, i + 1, i + 2);
    } else if (shape == MapShape::Fan) {
        for (uint32_t i = 1; i + 1 < count; ++i) tri(0, i, i + 1);
    } else {
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            tri(i, i + 1, i + 2);
            tri(i, i + 2, i + 3);
        }
    }
    return shapes;
}

// One texel of a GX I4 texture (8x8 blocks, four bits per texel, high nibble first): 0..15.
// Coordinates are clamped to the texture.
inline unsigned map_i4_texel(const uint8_t* data, uint32_t width, uint32_t height, float u, float v) noexcept {
    const int x = std::clamp(int(std::floor(u * float(width))), 0, int(width) - 1);
    const int y = std::clamp(int(std::floor(v * float(height))), 0, int(height) - 1);
    const uint32_t blocksPerRow = (width + 7) / 8;
    const uint8_t byte = data[(size_t(y / 8) * blocksPerRow + size_t(x / 8)) * 32 + size_t(y % 8) * 4 + size_t(x % 8) / 2];
    return (x & 1) ? byte & 0xFu : byte >> 4;
}

// The game's "intensity texture to palette index" stage for doors and icons (d_map_path.cpp,
// setTevSettingIntensityTextureToCI): index = base + a quarter of the texel's intensity, kept only
// above `floor`, else index 0; a texel of intensity 0 is not drawn (its alpha is 0). `base` and
// `floor` are the red bytes of GX_TEVREG1 and GX_TEVREG2.
inline bool map_icon_index(unsigned level, unsigned base, unsigned floor, uint8_t& index) noexcept {
    if (!level) return false;
    const unsigned value = unsigned(float(base) + 63.75f * float(level) / 15.f);
    index = uint8_t(value > floor ? std::min(value, 255u) : 0u);
    return true;
}

// The copy's second half: palette index to colour. `palette` is RGBA, `entries` colours; an index
// past the palette's end gives transparent black.
inline void map_colours(const uint8_t* indexed, size_t pixels, const uint8_t* palette, size_t entries, uint8_t* rgba) noexcept {
    for (size_t i = 0; i < pixels; ++i) {
        const size_t index = indexed[i];
        if (index < entries) std::memcpy(rgba + i * 4, palette + index * 4, 4);
        else std::memset(rgba + i * 4, 0, 4);
    }
}
}
