#pragma once
// Painter profile (painter_profile.cpp): sections, keyed detail tables and receipt text. Plain types
// only, no game headers, so the host tests can check the table, top-N choice and text.
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace twv::vita::paintprof {

// One section per painter call the profile wraps (m_Do_graphic.cpp mDoGph_Painter order). Only the
// outermost wrapped call is charged; "other" is the painter time outside every section (static
// functions of m_Do_graphic.cpp, GX calls, and JFWDisplay::beginRender/endRender, virtual calls the
// link wrappers cannot see: the frame limiter's sleep is in there, reported apart).
enum Section : unsigned {
    SecCopy2D, SecImageShadow, SecSetLight, SecSkyOpa, SecSkyXlu, SecBgOpa, SecBgOpaDark,
    SecMiddleOpa, SecPaFogPri0B, SecPaNormalPri0B, SecShadow, SecOpa, SecOpaDark, SecOpaPacket, SecBgXlu,
    SecBgXluDark, SecPaFogPri0A, SecPaNormalPri0A, SecXlu, SecXluDark, SecJ3dInit, SecOpaInvisible,
    SecXluInvisible, SecPaFogPri4, SecPaProjection, SecZxlu, SecFilter, SecPaFogPri1, SecPaNormal,
    SecPaFogPri2, SecPaFog, SecPaFogPri3, SecPaP1, SecPaDarkworld, SecPaScreen, SecIndScreen,
    Sec2DScreen, Sec3Dlast, SecPa2Dgame, SecCalcWipe, SecPa2Dback, SecPa2DmenuBack, Sec2DOpa,
    Sec2DOpaTop, Sec2DXlu, SecPa2Dfore, SecPa2DmenuFore, SecOther, SectionCount
};
constexpr const char* SectionNames[SectionCount] = {
    "copy2d", "image_shadow", "set_light", "sky_opa", "sky_xlu", "bg_opa", "bg_opa_dark",
    "middle_opa", "pa_fog_pri0_b", "pa_normal_pri0_b", "shadow", "opa", "opa_dark", "opa_packet", "bg_xlu",
    "bg_xlu_dark", "pa_fog_pri0_a", "pa_normal_pri0_a", "xlu", "xlu_dark", "j3d_init", "opa_invisible",
    "xlu_invisible", "pa_fog_pri4", "pa_projection", "zxlu", "filter", "pa_fog_pri1", "pa_normal",
    "pa_fog_pri2", "pa_fog", "pa_fog_pri3", "pa_p1", "pa_darkworld", "pa_screen", "ind_screen",
    "xlu_2dscreen", "opa_3dlast", "pa_2dgame", "calc_wipe", "pa_2dback", "pa_2dmenu_back", "2d_opa",
    "2d_opa_top", "2d_xlu", "pa_2dfore", "pa_2dmenu_fore", "other"};

// Sampling: one painted frame in Period (frame numbers from 1, remainder Phase: never an actor or a
// J3D frame of actor_profile_stats.hpp, which use remainders 1 and 5 of 8).
constexpr unsigned Period = 4, Phase = 3;
inline bool sampled(uint64_t frame) {return frame % Period == Phase;}

// Keyed detail (JPA resource user index, 2D draw class): open addressing over a fixed table; a key
// that finds no free slot is charged to the overflow entry.
struct Entry {
    uintptr_t key = 0;
    const char* name = nullptr;
    uint64_t us = 0;
    uint32_t calls = 0, items = 0;
    bool used = false;
};
template<unsigned N> struct KeyTable {
    static_assert((N & (N - 1)) == 0, "power of two");
    Entry slots[N];
    Entry overflow;
    Entry& at(uintptr_t key) {
        unsigned h = unsigned((key >> 3) ^ (key >> 13) ^ key) & (N - 1);
        for (unsigned probe = 0; probe < 8; ++probe, h = (h + 1) & (N - 1)) {
            Entry& e = slots[h];
            if (!e.used) {e.used = true; e.key = key; return e;}
            if (e.key == key) return e;
        }
        return overflow;
    }
    void clear() {for (Entry& e : slots) e = Entry{}; overflow = Entry{};}
    // Indices of the n used slots with the largest time, largest first (ties: lower slot first).
    std::size_t top(unsigned* out, std::size_t n) const {
        std::size_t used = 0;
        for (unsigned i = 0; i < N; ++i) {
            if (!slots[i].used || !slots[i].calls) continue;
            std::size_t pos = used < n ? used : n;
            while (pos > 0 && slots[out[pos - 1]].us < slots[i].us) --pos;
            if (pos >= n) continue;
            const std::size_t last = used < n ? used : n - 1;
            for (std::size_t k = last; k > pos; --k) out[k] = out[k - 1];
            out[pos] = i;
            if (used < n) ++used;
        }
        return used;
    }
    // Totals over every entry (overflow included), for the coverage of the top lines.
    void total(uint64_t& us, uint32_t& calls, uint32_t& items) const {
        us = overflow.us; calls = overflow.calls; items = overflow.items;
        for (const Entry& e : slots) {us += e.us; calls += e.calls; items += e.items;}
    }
};

inline int part_text(char* out, std::size_t size, const char* name, uint64_t us, uint32_t calls) {
    return std::snprintf(out, size, "n=%s;us=%llu;c=%lu", name, (unsigned long long)us, (unsigned long)calls);
}
// JPA: resource user index, draw time, emitter draws, particles alive in those emitters.
inline int jpa_text(char* out, std::size_t size, unsigned id, const Entry& e) {
    return std::snprintf(out, size, "id=%u;us=%llu;c=%lu;p=%lu", id, (unsigned long long)e.us,
                         (unsigned long)e.calls, (unsigned long)e.items);
}
// 2D: draw class name (';' and '=' replaced), draw time, draws.
inline int class_text(char* out, std::size_t size, const char* name, const Entry& e) {
    int n = std::snprintf(out, size, "n=");
    for (const char* c = name ? name : "?"; *c && n + 1 < int(size) && n < 64; ++c)
        out[n++] = (*c == ';' || *c == '=') ? '_' : *c;
    if (n < int(size)) out[n] = 0;
    return n + std::snprintf(out + n, size > std::size_t(n) ? size - n : 0, ";us=%llu;c=%lu",
                             (unsigned long long)e.us, (unsigned long)e.calls);
}

}  // namespace twv::vita::paintprof
