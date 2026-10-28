// PlayStation-style button glyphs, drawn in code (see ps_glyphs.hpp). Shapes are signed distance
// functions in texel units with one texel of anti-aliasing; letters come from a 5x7 pixel font.
#include "ui/ps_glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__vita__)
#include "runtime/telemetry.hpp"
#endif

namespace twv::ui {
namespace {

struct Color {
    float r, g, b, a;
};
constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color kOutline{0.16f, 0.16f, 0.16f, 1.0f}; // layout bases: the dark rim the originals have
constexpr Color kFill{0.82f, 0.82f, 0.82f, 1.0f};    // layout full icons: light face
constexpr Color kInk{0.20f, 0.20f, 0.20f, 1.0f};     // layout full icons: dark symbol
constexpr Color kBlack{0.06f, 0.06f, 0.07f, 1.0f};
// In-text icons: dark key or disc, light rim, coloured symbols.
constexpr Color kKey{0.10f, 0.10f, 0.12f, 1.0f};
constexpr Color kRim{0.46f, 0.46f, 0.50f, 1.0f};
constexpr Color kStick{0.74f, 0.74f, 0.78f, 1.0f};
constexpr Color kBlue{0.45f, 0.68f, 0.96f, 1.0f};
constexpr Color kRed{0.97f, 0.38f, 0.40f, 1.0f};
constexpr Color kPink{0.94f, 0.56f, 0.81f, 1.0f};
constexpr Color kGreen{0.22f, 0.84f, 0.64f, 1.0f};

struct Canvas {
    int w, h;
    std::vector<float> px; // premultiplied RGBA
    // Horizontal pre-stretch around x = pivot, for layout panes narrower than their texture: the
    // shape is drawn `stretch` times wider so it comes out with its own proportions on screen.
    float stretch = 1.0f, pivot = 0.0f;
    Canvas(int width, int height) : w(width), h(height), px(std::size_t(width) * height * 4, 0.0f) {}
    float sx(int x) const { return pivot + (x + 0.5f - pivot) / stretch; }
    void blend(int x, int y, Color c, float cov) {
        const float a = c.a * cov;
        float* p = &px[(std::size_t(y) * w + x) * 4];
        p[0] = c.r * a + p[0] * (1.0f - a);
        p[1] = c.g * a + p[1] * (1.0f - a);
        p[2] = c.b * a + p[2] * (1.0f - a);
        p[3] = a + p[3] * (1.0f - a);
    }
    template <class Sdf> void fill(const Sdf& sdf, Color c) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float cov = std::clamp(0.5f - sdf(sx(x), y + 0.5f), 0.0f, 1.0f);
                if (cov > 0.0f) blend(x, y, c, cov);
            }
        }
    }
    // 4x4 samples of an inside test: exact for unions of touching boxes (letters), where the
    // distance estimate of a union is too small along the shared edges and shows seams.
    template <class Sdf> void fill_sampled(const Sdf& sdf, Color c) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int n = 0;
                for (int s = 0; s < 16; ++s) {
                    const float px_ = pivot + (x + (s % 4 + 0.5f) / 4.0f - pivot) / stretch;
                    n += sdf(px_, y + (s / 4 + 0.5f) / 4.0f) < 0.0f;
                }
                if (n) blend(x, y, c, n / 16.0f);
            }
        }
    }
};

float len(float x, float y) { return std::sqrt(x * x + y * y); }
float seg(float px, float py, float ax, float ay, float bx, float by) {
    const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    const float t = std::clamp((pax * bax + pay * bay) / (bax * bax + bay * bay), 0.0f, 1.0f);
    return len(pax - bax * t, pay - bay * t);
}
float box(float px, float py, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::fabs(px - cx) - hw + r, qy = std::fabs(py - cy) - hh + r;
    return len(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

void disc(Canvas& c, float cx, float cy, float r, Color col) {
    c.fill([=](float x, float y) { return len(x - cx, y - cy) - r; }, col);
}
void ring(Canvas& c, float cx, float cy, float r, float w, Color col) {
    c.fill([=](float x, float y) { return std::fabs(len(x - cx, y - cy) - r) - w * 0.5f; }, col);
}
void rounded(Canvas& c, float cx, float cy, float hw, float hh, float r, Color col) {
    c.fill([=](float x, float y) { return box(x, y, cx, cy, hw, hh, r); }, col);
}

// Face-button symbols inside a square of side s centred on (cx, cy).
void cross(Canvas& c, float cx, float cy, float s, Color col) {
    const float w = s * 0.21f, e = s * 0.5f - w * 0.5f;
    c.fill([=](float x, float y) {
        return std::min(seg(x, y, cx - e, cy - e, cx + e, cy + e), seg(x, y, cx - e, cy + e, cx + e, cy - e)) - w * 0.5f;
    }, col);
}
void circle(Canvas& c, float cx, float cy, float s, Color col) {
    const float w = s * 0.19f;
    ring(c, cx, cy, s * 0.5f - w * 0.5f, w, col);
}
void square(Canvas& c, float cx, float cy, float s, Color col) {
    const float w = s * 0.18f, e = s * 0.5f - w * 0.5f;
    c.fill([=](float x, float y) { return std::fabs(box(x, y, cx, cy, e, e, w * 0.35f)) - w * 0.5f; }, col);
}
void triangle(Canvas& c, float cx, float cy, float s, Color col) {
    const float w = s * 0.15f, half = s * 0.5f - w * 0.5f, height = half * 1.7320508f;
    const float top = cy - height * 0.58f, bottom = cy + height * 0.42f;
    c.fill([=](float x, float y) {
        return std::min({seg(x, y, cx, top, cx - half, bottom), seg(x, y, cx - half, bottom, cx + half, bottom),
                         seg(x, y, cx + half, bottom, cx, top)}) - w * 0.5f;
    }, col);
}

// 5x7 pixel font, only the letters the glyphs use.
struct Letter {
    char ch;
    const char* rows[7];
};
constexpr Letter kFont[] = {
    {'C', {".####", "#....", "#....", "#....", "#....", "#....", ".####"}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
};
const Letter* letter(char ch) {
    for (const Letter& l : kFont) {
        if (l.ch == ch) return &l;
    }
    return nullptr;
}
// One fill per string so neighbouring cells join without seams. (x0, y0) is the top-left corner.
void text(Canvas& c, const char* str, float x0, float y0, float cw, float ch, Color col) {
    struct Cell {
        float cx, cy;
    };
    std::vector<Cell> cells;
    for (int i = 0; str[i]; ++i) {
        const Letter* l = letter(str[i]);
        if (!l) continue;
        for (int r = 0; r < 7; ++r) {
            for (int k = 0; k < 5; ++k) {
                if (l->rows[r][k] == '#') cells.push_back({x0 + (i * 6 + k + 0.5f) * cw, y0 + (r + 0.5f) * ch});
            }
        }
    }
    const float hw = cw * 0.56f, hh = ch * 0.56f; // a little overlap: bolder strokes
    c.fill_sampled([&](float x, float y) {
        float d = 1e9f;
        for (const Cell& cell : cells) d = std::min(d, box(x, y, cell.cx, cell.cy, hw, hh, 0.0f));
        return d;
    }, col);
}
float text_width(const char* str, float cw) { return (std::strlen(str) * 6.0f - 1.0f) * cw; }
void text_centred(Canvas& c, const char* str, float cx, float cy, float cw, float ch, Color col) {
    text(c, str, cx - text_width(str, cw) * 0.5f, cy - ch * 3.5f, cw, ch, col);
}

enum Kind {
    // Layout: white symbols and white bases (the layout tints them), IA8.
    SymCross, SymCircle, SymSquare, SymTriangle, SymSelect,
    BaseDisc, BaseKey, FullSquare, FullTriangle, DarkCircle, LeftStick, RightStick,
    // In-text: full colour, RGBA8.
    TextCross, TextCircle, TextSquare, TextTriangle, TextL, TextR, TextSelect,
    TextLeftStick, TextLeftStickTilt, TextRightStick,
};

void draw(Canvas& c, Kind kind, float stretch) {
    const float W = float(c.w), H = float(c.h), cx = W * 0.5f, cy = H * 0.5f, m = std::min(W, H);
    c.stretch = stretch;
    c.pivot = cx;
    const float fit = std::min(m, W * 0.94f / stretch); // the stretched shape stays inside
    switch (kind) {
    // Sized to the visual weight of the GameCube letters they replace (checked in Vita3K captures).
    case SymCross: cross(c, cx, cy, m * 0.86f, kWhite); break;
    case SymCircle: circle(c, cx, cy, std::min(m * 0.96f, fit), kWhite); break;
    case SymSquare: square(c, cx, cy, std::min(m * 0.84f, fit), kWhite); break;
    case SymTriangle: triangle(c, cx, cy, m * 0.96f, kWhite); break;
    case SymSelect: { // two lines, SEL over ECT: a one-line SELECT would be unreadable at this size
        const float cw = W * 0.92f / 17.0f, ch = cw * 1.55f, gap = ch * 1.6f;
        const float top = cy - (14.0f * ch + gap) * 0.5f;
        text(c, "SEL", cx - text_width("SEL", cw) * 0.5f, top, cw, ch, kWhite);
        text(c, "ECT", cx - text_width("ECT", cw) * 0.5f, top + 7.0f * ch + gap, cw, ch, kWhite);
        break;
    }
    case BaseDisc: {
        const float r = m * 0.47f;
        disc(c, cx, cy, r, kOutline);
        disc(c, cx, cy, r - 3.5f, kWhite);
        break;
    }
    case BaseKey: {
        const float hw = W * 0.40f, hh = H * 0.40f, r = W * 0.14f;
        rounded(c, cx, cy, hw, hh, r, kOutline);
        rounded(c, cx, cy, hw - 3.5f, hh - 3.5f, r - 2.0f, kWhite);
        break;
    }
    case FullSquare:
    case FullTriangle: {
        const float r = m * 0.47f;
        disc(c, cx, cy, r, kOutline);
        disc(c, cx, cy, r - 3.5f, kFill);
        if (kind == FullSquare) square(c, cx, cy, r * 1.0f, kInk);
        else triangle(c, cx, cy, r * 1.2f, kInk);
        break;
    }
    case DarkCircle: {
        const float r = m * 0.48f;
        disc(c, cx, cy, r, kBlack);
        circle(c, cx, cy, r * 1.15f, kWhite);
        break;
    }
    case LeftStick:
    case RightStick: {
        const float r = m * 0.47f, k = r * 0.58f;
        disc(c, cx, cy, r, kOutline);
        disc(c, cx, cy, r - 3.0f, kFill);
        disc(c, cx, cy, k, kOutline);
        disc(c, cx, cy, k - 3.0f, kWhite);
        if (kind == RightStick) {
            const float cw = k * 0.95f / 5.0f;
            text_centred(c, "R", cx, cy, cw, cw * 1.12f, kInk);
        }
        break;
    }
    case TextCross:
    case TextCircle:
    case TextSquare:
    case TextTriangle:
    case TextLeftStick:
    case TextLeftStickTilt:
    case TextRightStick: {
        const float r = m * 0.48f;
        disc(c, cx, cy, r, kRim);
        disc(c, cx, cy, r - m * 0.045f, kKey);
        if (kind == TextCross) cross(c, cx, cy, m * 0.50f, kBlue);
        if (kind == TextCircle) circle(c, cx, cy, m * 0.54f, kRed);
        if (kind == TextSquare) square(c, cx, cy, m * 0.48f, kPink);
        if (kind == TextTriangle) triangle(c, cx, cy, m * 0.58f, kGreen);
        if (kind == TextLeftStick || kind == TextLeftStickTilt || kind == TextRightStick) {
            // The tilted frame moves the knob down-left; the message code mirrors it for the
            // other directions, which is why the left stick carries no letter.
            const float off = kind == TextLeftStickTilt ? m * 0.10f : 0.0f;
            ring(c, cx, cy, m * 0.31f, m * 0.05f, kStick);
            disc(c, cx - off, cy + off, m * 0.21f, kStick);
            if (kind == TextRightStick) {
                const float cw = m * 0.046f;
                text_centred(c, "R", cx, cy, cw, cw * 1.1f, kKey);
            }
        }
        break;
    }
    case TextL:
    case TextR:
    case TextSelect: {
        const bool select = kind == TextSelect;
        const float hw = m * 0.47f, hh = select ? m * 0.44f : m * 0.32f, r = m * 0.14f;
        rounded(c, cx, cy, hw, hh, r, kRim);
        rounded(c, cx, cy, hw - m * 0.045f, hh - m * 0.045f, r - m * 0.03f, kKey);
        if (select) {
            const float cw = m * 0.048f, ch = cw, gap = ch * 1.3f, top = cy - (14.0f * ch + gap) * 0.5f;
            text(c, "SEL", cx - text_width("SEL", cw) * 0.5f, top, cw, ch, kWhite);
            text(c, "ECT", cx - text_width("ECT", cw) * 0.5f, top + 7.0f * ch + gap, cw, ch, kWhite);
        } else {
            const float cw = m * 0.065f;
            text_centred(c, kind == TextL ? "L" : "R", cx, cy, cw, cw * 1.05f, kWhite);
        }
        break;
    }
    }
}

// Transparent texels take the colour of their visible neighbours, so bilinear filtering at the
// edges blends towards the shape's own colour instead of black.
void bleed(Canvas& c) {
    std::vector<float> straight(c.px.size(), 0.0f);
    std::vector<char> known(std::size_t(c.w) * c.h, 0);
    for (std::size_t i = 0; i < known.size(); ++i) {
        const float a = c.px[i * 4 + 3];
        if (a > 1.0f / 512.0f) {
            for (int k = 0; k < 3; ++k) straight[i * 4 + k] = std::min(1.0f, c.px[i * 4 + k] / a);
            known[i] = 1;
        }
    }
    for (int pass = 0; pass < 6; ++pass) {
        std::vector<char> next = known;
        for (int y = 0; y < c.h; ++y) {
            for (int x = 0; x < c.w; ++x) {
                const std::size_t i = std::size_t(y) * c.w + x;
                if (known[i]) continue;
                float sum[3] = {0, 0, 0};
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= c.w || ny >= c.h) continue;
                        const std::size_t j = std::size_t(ny) * c.w + nx;
                        if (!known[j]) continue;
                        for (int k = 0; k < 3; ++k) sum[k] += straight[j * 4 + k];
                        ++n;
                    }
                }
                if (n) {
                    for (int k = 0; k < 3; ++k) straight[i * 4 + k] = sum[k] / n;
                    next[i] = 1;
                }
            }
        }
        known.swap(next);
    }
    // Store straight colour + alpha back into the canvas (encoders read straight values).
    for (std::size_t i = 0; i < known.size(); ++i) {
        for (int k = 0; k < 3; ++k) c.px[i * 4 + k] = straight[i * 4 + k];
    }
}

std::uint8_t byte(float v) { return std::uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
void put16(std::uint8_t* p, unsigned v) { p[0] = std::uint8_t(v >> 8); p[1] = std::uint8_t(v); }
void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = std::uint8_t(v >> 24); p[1] = std::uint8_t(v >> 16); p[2] = std::uint8_t(v >> 8); p[3] = std::uint8_t(v);
}

constexpr std::uint8_t kIA8 = 3, kRGBA8 = 6;
constexpr std::uint32_t kHeader = 0x20;

// GX tiled images: 4x4 blocks, 2 bytes per texel (IA8: alpha, intensity) or 64-byte RGBA8
// blocks (16 AR pairs, then 16 GB pairs). Sizes are padded to whole blocks.
std::uint32_t image_bytes(int w, int h, std::uint8_t format) {
    const std::uint32_t blocks = std::uint32_t((w + 3) / 4) * std::uint32_t((h + 3) / 4);
    return blocks * (format == kRGBA8 ? 64u : 32u);
}

struct Owned {
    std::vector<std::uint8_t> storage; // over-allocated for 32-byte alignment
    GlyphTexture tex;
};

void encode(const Canvas& c, std::uint8_t format, std::uint8_t* out) {
    const int bw = (c.w + 3) / 4, bh = (c.h + 3) / 4;
    std::uint8_t* p = out;
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            std::uint8_t ar[32] = {}, gb[32] = {};
            for (int t = 0; t < 16; ++t) {
                const int x = bx * 4 + t % 4, y = by * 4 + t / 4;
                float r = 0, g = 0, b = 0, a = 0;
                if (x < c.w && y < c.h) {
                    const float* s = &c.px[(std::size_t(y) * c.w + x) * 4];
                    r = s[0]; g = s[1]; b = s[2]; a = s[3];
                }
                if (format == kIA8) {
                    p[t * 2] = byte(a);
                    p[t * 2 + 1] = byte(0.299f * r + 0.587f * g + 0.114f * b);
                } else {
                    ar[t * 2] = byte(a); ar[t * 2 + 1] = byte(r);
                    gb[t * 2] = byte(g); gb[t * 2 + 1] = byte(b);
                }
            }
            if (format == kRGBA8) {
                std::memcpy(p, ar, 32);
                std::memcpy(p + 32, gb, 32);
                p += 64;
            } else {
                p += 32;
            }
        }
    }
}

void make(Owned& o, const char* label, int width, int height, Kind kind, float stretch) {
    const std::uint8_t format = kind >= TextCross ? kRGBA8 : kIA8;
    Canvas c(width, height);
    draw(c, kind, stretch);
    bleed(c);
    const std::uint32_t bytes = kHeader + image_bytes(width, height, format);
    o.storage.assign(bytes + 32, 0);
    std::uint8_t* t = o.storage.data();
    t += (32 - reinterpret_cast<std::uintptr_t>(t) % 32) % 32;
    // ResTIMG (JUTTexture.h), big-endian like a .bti from the disc.
    t[0x00] = format;
    t[0x01] = 1;                       // alphaEnabled
    put16(t + 0x02, unsigned(width));
    put16(t + 0x04, unsigned(height));
    t[0x06] = 0; t[0x07] = 0;          // clamp
    t[0x14] = 1; t[0x15] = 1;          // linear min/mag
    t[0x18] = 1;                       // one image, no mipmaps
    put32(t + 0x1C, kHeader);          // imageOffset
    encode(c, format, t + kHeader);
    o.tex = GlyphTexture{t, bytes, std::uint16_t(width), std::uint16_t(height), format, label};
}

// Layout textures: name, the archives that hold it (root names, GZ2P01 /res/Layout, plus
// /res/Object/fileSel.arc for the file select, save and name-entry screens), and the
// original's GX format and size. A swap needs all three to match. The glyph is drawn at twice the
// original size (the layout's pane fixes the drawn size, so only the aspect has to match).
struct LayoutSpec {
    const char* name;
    const char* archives; // space-separated
    std::uint8_t format;
    int w, h;
    Kind kind;
    // Horizontal pre-stretch where the HUD pane is narrower than the texture (measured in Vita3K
    // HUD captures: the X and B letter panes show a square shape at about 0.70 and 0.78 of its
    // height), so the square and the circle stay square and round.
    float stretch = 1.0f;
};
constexpr const char* kMenus = "button main2d clctres clctrese3 clctresr dmapres fishres fmapres insectres letres "
                               "msgres05 optres saveres skillres title2d ringres filesel";
constexpr LayoutSpec kLayout[] = {
    {"tt_zelda_button_a_text.bti", kMenus, 0, 32, 46, SymCross},
    {"tt_zelda_button_a_8ia.bti", "clctrese3 clctresr optres title2d filesel", 1, 32, 46, SymCross},
    // The circle can be no wider on screen than the narrow B letter pane (about 8-10 px in the file
    // select), so its size there cannot grow by scale alone; it keeps the measured stretch.
    {"tt_zelda_button_b_text.bti", kMenus, 0, 24, 40, SymCircle, 1.28f},
    {"tt_zelda_button_x_text.bti", "button main2d ringres", 0, 32, 45, SymSquare, 1.43f},
    {"tt_zelda_button_y_text.bti", "button main2d ringres", 0, 32, 40, SymTriangle},
    {"im_zelda_button_z_text.bti", "button fmapres main2d optres", 2, 24, 40, SymSelect},
    {"tt_zelda_button_x_base.bti", "button main2d ringres", 2, 32, 48, BaseDisc},
    {"tt_zelda_button_y_base.bti", "button main2d ringres", 2, 48, 32, BaseDisc},
    {"im_zelda_button_z_base.bti", "button fmapres main2d optres", 2, 48, 48, BaseKey},
    {"tt_zelda_button_x.bti", "ringres", 2, 32, 48, FullSquare},
    {"tt_zelda_button_y.bti", "ringres", 2, 48, 32, FullTriangle},
    {"tt_ia4_bbtn.bti", "clctrese3 saveres", 2, 32, 32, DarkCircle},
    {"tt_ia4_3d_btn.bti", "button fmapres ringres", 2, 32, 32, LeftStick},
    {"tt_title3DstickAnim_00.bti", "clctrese3 dmapres msgres05 optres", 8, 24, 24, LeftStick},
    {"c_stick_anim_otah.10.bti", "button", 3, 56, 56, RightStick},
};
constexpr int kLayoutCount = int(sizeof(kLayout) / sizeof(kLayout[0]));

// Textures kept from the disc whose HUD panes only change colour: the round A/B base.
constexpr LayoutSpec kTintOnly[] = {
    {"tt_zelda_button_ab_maru.bti", "button main2d", 2, 40, 40, BaseDisc},
};
constexpr int kTintOnlyCount = int(sizeof(kTintOnly) / sizeof(kTintOnly[0]));
// Their resolved originals (one per archive that holds them; a handful at most).
const void* gTintOnly[8] = {};
int gTintOnlyCount = 0;

// In-text icons (d_msg_out_font.cpp getBtiName): 24x24 originals, drawn at 48x48.
enum TextSlot { TCross, TCircle, TSquare, TTriangle, TL, TR, TSelect, TLeft, TLeftTilt, TRight, TextCount };
constexpr Kind kTextKind[TextCount] = {TextCross, TextCircle, TextSquare, TextTriangle, TextL, TextR,
                                       TextSelect, TextLeftStick, TextLeftStickTilt, TextRightStick};
constexpr const char* kTextLabel[TextCount] = {"text_cross", "text_circle", "text_square", "text_triangle",
                                               "text_l", "text_r", "text_select", "text_left_stick",
                                               "text_left_stick_tilt", "text_right_stick"};

struct Set {
    Owned layout[kLayoutCount];
    Owned text[TextCount];
    Set() {
        for (int i = 0; i < kLayoutCount; ++i)
            make(layout[i], kLayout[i].name, kLayout[i].w * 2, kLayout[i].h * 2, kLayout[i].kind, kLayout[i].stretch);
        for (int i = 0; i < TextCount; ++i) make(text[i], kTextLabel[i], 48, 48, kTextKind[i], 1.0f);
    }
};
const Set& glyphs() {
    static const Set set;
    return set;
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
bool same_name(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if (lower(*a) != lower(*b)) return false;
    }
    return *a == *b;
}
// Whether `volume` is one of the space-separated names in `list` (case-insensitive).
bool in_list(const char* list, const char* volume) {
    const std::size_t n = std::strlen(volume);
    for (const char* p = list; *p;) {
        const char* end = std::strchr(p, ' ');
        const std::size_t len = end ? std::size_t(end - p) : std::strlen(p);
        if (len == n) {
            std::size_t i = 0;
            while (i < n && lower(p[i]) == lower(volume[i])) ++i;
            if (i == n) return true;
        }
        if (!end) break;
        p = end + 1;
    }
    return false;
}
int listed(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < kLayoutCount; ++i) {
        if (same_name(name, kLayout[i].name)) return i;
    }
    return -1;
}
bool matches(const LayoutSpec& s, const char* volume, const void* original) {
    if (!volume || !original || !in_list(s.archives, volume)) return false;
    const auto* t = static_cast<const std::uint8_t*>(original); // ResTIMG, big-endian
    return t[0] == s.format && (t[2] << 8 | t[3]) == s.w && (t[4] << 8 | t[5]) == s.h;
}
int tint_only(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < kTintOnlyCount; ++i) {
        if (same_name(name, kTintOnly[i].name)) return i;
    }
    return -1;
}

} // namespace

bool ps_layout_listed(const char* name) { return listed(name) >= 0; }

bool ps_tint_only_note(const char* name, const char* volume, const void* original) {
    const int i = tint_only(name);
    if (i < 0 || !matches(kTintOnly[i], volume, original)) return false;
    for (int k = 0; k < gTintOnlyCount; ++k) {
        if (gTintOnly[k] == original) return true;
    }
    if (gTintOnlyCount < int(sizeof(gTintOnly) / sizeof(gTintOnly[0]))) gTintOnly[gTintOnlyCount++] = original;
    return true;
}

bool ps_hud_tint(const void* timg, unsigned* rgb) {
    if (!timg) return false;
    for (int k = 0; k < gTintOnlyCount; ++k) {
        if (gTintOnly[k] == timg) { *rgb = kHudBase; return true; }
    }
    for (int i = 0; i < kLayoutCount; ++i) {
        if (glyphs().layout[i].tex.timg != timg) continue;
        switch (kLayout[i].kind) {
        case BaseDisc: case BaseKey: *rgb = kHudBase; return true;
        case SymCross: *rgb = kHudCross; return true;
        case SymCircle: *rgb = kHudCircle; return true;
        case SymSquare: *rgb = kHudSquare; return true;
        case SymTriangle: *rgb = kHudTriangle; return true;
        default: return false; // SELECT lettering, sticks and whole icons keep their colours
        }
    }
    return false;
}

const GlyphTexture* ps_layout_glyph(const char* name, const char* volume, const void* original) {
    const int i = listed(name);
    if (i < 0 || !matches(kLayout[i], volume, original)) return nullptr;
    return &glyphs().layout[i].tex;
}

const GlyphTexture* ps_text_glyph(int index, int layer) {
    int slot = -1;
    if (layer == 0) {
        switch (index) {
        case 0: slot = TCross; break;     // A
        case 1: slot = TCircle; break;    // B
        case 2: slot = TRight; break;     // C stick
        case 3: slot = TL; break;         // L
        case 4: slot = TR; break;         // R
        case 5: slot = TSquare; break;    // X
        case 6: slot = TTriangle; break;  // Y
        case 7: slot = TSelect; break;    // Z
        case 9:
        case 14: case 15: case 16: case 17: case 18: case 19: slot = TLeft; break; // control stick
        default: break;                   // 8 D-pad and the rest keep their art
        }
    } else if (layer == 1 && (index == 9 || (index >= 14 && index <= 19))) {
        slot = TLeftTilt;
    }
    return slot < 0 ? nullptr : &glyphs().text[slot].tex;
}

std::size_t ps_glyph_count() { return std::size_t(kLayoutCount + TextCount); }

const GlyphTexture* ps_glyph_at(std::size_t i) {
    if (i < std::size_t(kLayoutCount)) return &glyphs().layout[i].tex;
    i -= std::size_t(kLayoutCount);
    return i < std::size_t(TextCount) ? &glyphs().text[i].tex : nullptr;
}

void ps_glyph_texel(const GlyphTexture& g, int x, int y, std::uint8_t rgba[4]) {
    const std::uint8_t* img = g.timg + kHeader;
    const int bw = (g.width + 3) / 4;
    const int block = (y / 4) * bw + x / 4, t = (y % 4) * 4 + x % 4;
    if (g.format == kIA8) {
        const std::uint8_t* p = img + block * 32 + t * 2;
        rgba[0] = rgba[1] = rgba[2] = p[1];
        rgba[3] = p[0];
    } else {
        const std::uint8_t* p = img + block * 64;
        rgba[3] = p[t * 2]; rgba[0] = p[t * 2 + 1];
        rgba[1] = p[32 + t * 2]; rgba[2] = p[32 + t * 2 + 1];
    }
}

} // namespace twv::ui

// Receipts: the glyph set once, then every swap (layouts and the message font load a handful of
// textures per screen, so this stays small).
namespace {
#if defined(__vita__)
void report_ready() {
    static bool done = false;
    if (done) return;
    done = true;
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < twv::ui::ps_glyph_count(); ++i) bytes += twv::ui::ps_glyph_at(i)->bytes;
    twv::vita::telemetryEvent("ps_glyphs_ready", (long long)twv::ui::ps_glyph_count());
    twv::vita::telemetryEvent("ps_glyphs_bytes", (long long)bytes);
}
void report_layout(const char* event, const char* name, const char* volume) {
    char text[96];
    std::snprintf(text, sizeof text, "%s@%s", name, volume ? volume : "?");
    twv::vita::telemetryText(event, text);
}
void report_text(int index, int layer) { report_ready(); twv::vita::telemetryEvent("ps_glyph_text_swap", index + 100 * layer); }
#else
void report_ready() {}
void report_layout(const char*, const char*, const char*) {}
void report_text(int, int) {}
#endif
} // namespace

extern "C" int twv_ps_glyph_layout_listed(const char* name) {
    return twv::ui::ps_layout_listed(name) || twv::ui::tint_only(name) >= 0;
}

// Listed names only: a swap is logged as ps_glyph_layout_swap, a listed name from another archive
// or with other contents as ps_glyph_layout_kept (it keeps its art). The A/B base keeps its art
// and is only remembered for the HUD colours.
extern "C" const void* twv_ps_glyph_layout(const char* name, const char* volume, const void* original) {
    if (!twv::ui::ps_layout_listed(name)) {
        twv::ui::ps_tint_only_note(name, volume, original);
        return nullptr;
    }
    const twv::ui::GlyphTexture* g = twv::ui::ps_layout_glyph(name, volume, original);
    if (!g) {
        if (twv::ui::ps_layout_listed(name)) report_layout("ps_glyph_layout_kept", name, volume);
        return nullptr;
    }
    report_ready();
    report_layout("ps_glyph_layout_swap", name, volume);
    return g->timg;
}

extern "C" int twv_ps_glyph_hud_tint(const void* timg, unsigned* rgb) {
    return rgb && twv::ui::ps_hud_tint(timg, rgb) ? 1 : 0;
}

extern "C" void twv_ps_glyph_hud_tinted(const char* screen, int panes) {
#if defined(__vita__)
    char text[64];
    std::snprintf(text, sizeof text, "%s:%d", screen ? screen : "?", panes);
    twv::vita::telemetryText("ps_glyph_hud_tinted", text);
#else
    (void)screen;
    (void)panes;
#endif
}

extern "C" const void* twv_ps_glyph_text(int index, int layer) {
    const twv::ui::GlyphTexture* g = twv::ui::ps_text_glyph(index, layer);
    if (!g) return nullptr;
    report_text(index, layer);
    return g->timg;
}
