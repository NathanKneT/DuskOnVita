// The map screens' CPU pictures (platform/vita/gfx_vgl/map_raster.hpp).
#include "gfx_vgl/map_raster.hpp"
#include <cassert>
#include <cstdio>
#include <limits>

using twv::vgl::MapPicture;
using twv::vgl::MapShape;

static unsigned count_of(const MapPicture& p, uint8_t index) {
    unsigned n = 0;
    for (uint8_t v : p.px) n += v == index;
    return n;
}
static uint8_t at(const MapPicture& p, int x, int y) { return p.px[size_t(y) * p.w + x]; }

// The back quad spans twice the target, as the game's does: every pixel takes its index.
static void back_quad_covers() {
    MapPicture p;
    p.begin(16, 12, 0);
    const float xy[] = {-16, -12, 32, -12, 32, 24, -16, 24};
    assert(twv::vgl::map_draw(p, MapShape::Quads, xy, 4, nullptr, 0, 4, 1.f) == 2);
    assert(count_of(p, 4) == 16 * 12);
    std::puts("map_raster_test: the back quad fills the picture");
}

// Pixel centres decide; two triangles that share an edge leave no gap and the shared pixels are
// drawn once per triangle at most (no blending, so a second write changes nothing).
static void triangles_and_orders() {
    MapPicture p;
    p.begin(8, 8, 0);
    const float left[] = {0, 0, 8, 0, 0, 8}, right[] = {8, 0, 8, 8, 0, 8};
    p.triangle(left, left + 2, left + 4, 1);
    const unsigned first = count_of(p, 1);
    assert(first > 20 && first < 44);
    p.triangle(right, right + 2, right + 4, 2);
    assert(count_of(p, 0) == 0);
    // The other winding draws the same pixels.
    MapPicture q;
    q.begin(8, 8, 0);
    q.triangle(left + 4, left + 2, left, 1);
    assert(count_of(q, 1) == first);
    // A strip of four points is the same square; so is a fan, and an index list.
    const float square[] = {1, 1, 5, 1, 1, 5, 5, 5};
    MapPicture s;
    s.begin(8, 8, 0);
    assert(twv::vgl::map_draw(s, MapShape::Strip, square, 4, nullptr, 0, 7, 1.f) == 2);
    assert(count_of(s, 7) == 16 && at(s, 1, 1) == 7 && at(s, 4, 4) == 7 && at(s, 5, 5) == 0 && at(s, 0, 0) == 0);
    const float ring[] = {1, 1, 5, 1, 5, 5, 1, 5};
    MapPicture f;
    f.begin(8, 8, 0);
    twv::vgl::map_draw(f, MapShape::Fan, ring, 4, nullptr, 0, 7, 1.f);
    assert(f.px == s.px);
    const uint16_t list[] = {0, 1, 2, 2, 3, 0};
    MapPicture l;
    l.begin(8, 8, 0);
    assert(twv::vgl::map_draw(l, MapShape::Triangles, ring, 4, list, 6, 7, 1.f) == 2);
    assert(l.px == s.px);
    // An index past the vertices is left out, not read.
    const uint16_t bad[] = {0, 1, 9};
    assert(twv::vgl::map_draw(l, MapShape::Triangles, ring, 4, bad, 3, 9, 1.f) == 0 && count_of(l, 9) == 0);
    std::puts("map_raster_test: triangles, strips, fans, quads and index lists");
}

// A line is at least one pixel wide and has no gap, whatever its slope.
static void lines_are_connected() {
    for (int end = 0; end <= 30; ++end) {
        MapPicture p;
        p.begin(32, 32, 0);
        const float xy[] = {0.5f, 0.5f, 31.5f, float(end) + 0.5f};
        assert(twv::vgl::map_draw(p, MapShape::Lines, xy, 2, nullptr, 0, 3, 0.5f) == 1);
        for (int x = 0; x < 32; ++x) {
            bool column = false;
            for (int y = 0; y < 32; ++y) column |= at(p, x, y) == 3;
            assert(column);
        }
        assert(count_of(p, 3) < 32 * 4);
    }
    // A horizontal one-pixel line is one row; a three-pixel one is three.
    MapPicture p;
    p.begin(16, 8, 0);
    const float flat[] = {2, 3.5f, 12, 3.5f};
    twv::vgl::map_draw(p, MapShape::Lines, flat, 2, nullptr, 0, 5, 1.f);
    for (int y = 0; y < 8; ++y) assert((at(p, 6, y) == 5) == (y == 3));
    twv::vgl::map_draw(p, MapShape::Lines, flat, 2, nullptr, 0, 6, 3.f);
    for (int y = 0; y < 8; ++y) assert((at(p, 6, y) == 6) == (y >= 2 && y <= 4));
    // A strip of three points is two segments that meet at the corner.
    MapPicture s;
    s.begin(16, 16, 0);
    const float corner[] = {2.5f, 2.5f, 12.5f, 2.5f, 12.5f, 12.5f};
    assert(twv::vgl::map_draw(s, MapShape::LineStrip, corner, 3, nullptr, 0, 8, 1.f) == 2);
    assert(at(s, 12, 2) == 8 && at(s, 7, 2) == 8 && at(s, 12, 7) == 8 && at(s, 7, 7) == 0);
    std::puts("map_raster_test: lines of every slope, widths, strips");
}

// Shapes far outside, and coordinates that are not numbers, write nothing outside the picture.
static void clipped_and_refused() {
    MapPicture p;
    p.begin(8, 8, 0);
    const float huge[] = {-1e7f, -1e7f, 1e7f, -1e7f, 0, 1e7f};
    p.triangle(huge, huge + 2, huge + 4, 1);
    assert(count_of(p, 1) == 64);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const float bad[] = {nan, 0, 4, 0, 4, 4, inf, 2};
    MapPicture q;
    q.begin(8, 8, 0);
    q.triangle(bad, bad + 2, bad + 4, 1);
    q.segment(bad + 2, bad + 6, 1.f, 1);
    assert(count_of(q, 1) == 0);
    const float away[] = {20, 20, 30, 20, 20, 30};
    q.triangle(away, away + 2, away + 4, 1);
    q.segment(away, away + 2, 2.f, 1);
    assert(count_of(q, 1) == 0);
    MapPicture empty;
    empty.triangle(away, away + 2, away + 4, 1);
    std::puts("map_raster_test: clipping and refused coordinates");
}

// A textured square: an I4 icon whose left half is intensity 0 (not drawn) and right half 4.
static void icons() {
    uint8_t texture[32] = {};
    for (int row = 0; row < 8; ++row) {
        texture[row * 4 + 2] = 0x44;
        texture[row * 4 + 3] = 0x44;
    }
    assert(twv::vgl::map_i4_texel(texture, 8, 8, 0.1f, 0.5f) == 0 && twv::vgl::map_i4_texel(texture, 8, 8, 0.9f, 0.5f) == 4);
    assert(twv::vgl::map_i4_texel(texture, 8, 8, -3.f, 9.f) == 0 && twv::vgl::map_i4_texel(texture, 8, 8, 5.f, -1.f) == 4);
    uint8_t index = 0;
    assert(!twv::vgl::map_icon_index(0, 100, 100, index));
    assert(twv::vgl::map_icon_index(1, 100, 100, index) && index == 104);
    assert(twv::vgl::map_icon_index(4, 100, 100, index) && index == 117);
    assert(twv::vgl::map_icon_index(1, 100, 110, index) && index == 0);
    MapPicture p;
    p.begin(16, 16, 4);
    const float xy[] = {4, 4, 12, 4, 12, 12, 4, 12}, st[] = {0, 0, 1, 0, 1, 1, 0, 1};
    const auto texel = [&](float u, float v, uint8_t& out) {
        return twv::vgl::map_icon_index(twv::vgl::map_i4_texel(texture, 8, 8, u, v), 100, 100, out);
    };
    p.textured(xy, xy + 2, xy + 4, st, st + 2, st + 4, texel);
    p.textured(xy, xy + 4, xy + 6, st, st + 4, st + 6, texel);
    assert(count_of(p, 117) == 4 * 8 && at(p, 8, 4) == 117 && at(p, 11, 11) == 117 && at(p, 7, 8) == 4 && at(p, 12, 8) == 4);
    std::puts("map_raster_test: textured icon squares");
}

static void palette() {
    const uint8_t indexed[] = {0, 1, 2, 9};
    const uint8_t colours[] = {10, 11, 12, 13, 20, 21, 22, 23, 30, 31, 32, 33};
    uint8_t rgba[16];
    twv::vgl::map_colours(indexed, 4, colours, 3, rgba);
    assert(rgba[0] == 10 && rgba[3] == 13 && rgba[4] == 20 && rgba[8] == 30 && rgba[11] == 33);
    assert(rgba[12] == 0 && rgba[15] == 0);
    std::puts("map_raster_test: palette index to colour");
}

int main() {
    back_quad_covers();
    triangles_and_orders();
    lines_are_connected();
    clipped_and_refused();
    icons();
    palette();
    return 0;
}
