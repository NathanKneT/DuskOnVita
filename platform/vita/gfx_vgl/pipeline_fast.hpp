#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace twv::vgl {
// --vita-pipeline-fast (pipeline_fast.cpp). The config Aurora asks a pipeline for is ~2.7 KB, mostly
// sixteen TEV stages, eight texgens and four indirect stages of which a draw uses a few. Each pipeline
// change filled all of it twice over and then hashed and compared all of it.

// Exact key of a pipeline config without the unused tail of those three arrays, read in place as seven
// parts plus the texgen count. Texgen entries are trimmed when they equal the default, so that part
// needs no assumption. TEV and indirect stages past their stored counts hold their defaults in every
// config populate_pipeline_config writes (it starts from a default ShaderConfig and fills the first
// count entries), and the fast populate keeps that. Under that form, equal keys mean equal configs.
struct CompactKey {
    static constexpr size_t Parts = 7;
    std::array<const uint8_t*, Parts> part{};
    std::array<uint32_t, Parts> length{};
    uint8_t tcgs = 0;
    uint32_t size = 0; // bytes of the parts plus the texgen count
};

template<class Config> CompactKey pipeline_compact_key(const Config& c) noexcept {
    static_assert(std::has_unique_object_representations_v<Config>);
    const auto& s = c.shaderConfig;
    using Tcg = typename std::remove_cvref_t<decltype(s.tcgs)>::value_type;
    static const Tcg defaultTcg{};
    size_t tcgs = s.tcgs.size();
    while (tcgs && std::memcmp(&s.tcgs[tcgs - 1], &defaultTcg, sizeof(Tcg)) == 0) --tcgs;
    const size_t tev = std::min<size_t>(s.tevStageCount, s.tevStages.size());
    const size_t ind = std::min<size_t>(s.numIndStages, s.indStages.size());
    const auto* base = reinterpret_cast<const uint8_t*>(&c);
    const auto b = [](const void* p) { return static_cast<const uint8_t*>(p); };
    const std::array<std::pair<const uint8_t*, const uint8_t*>, CompactKey::Parts> parts{{
        {base, b(s.tevStages.data())},                          // config header, attributes, swap table
        {b(s.tevStages.data()), b(s.tevStages.data() + tev)},
        {b(&s.tevStageCount), b(s.tcgs.data())},                // TEV stage count, colour channels
        {b(s.tcgs.data()), b(s.tcgs.data() + tcgs)},
        {b(&s.alphaCompare), b(s.indStages.data())},
        {b(s.indStages.data()), b(s.indStages.data() + ind)},
        {b(&s.numIndStages), base + sizeof(Config)},            // indirect count, blend/depth state
    }};
    CompactKey key;
    for (size_t i = 0; i < CompactKey::Parts; ++i) {
        key.part[i] = parts[i].first;
        key.length[i] = static_cast<uint32_t>(parts[i].second - parts[i].first);
        key.size += key.length[i];
    }
    key.tcgs = static_cast<uint8_t>(tcgs);
    key.size += 1;
    return key;
}

// Hash over the parts' 32-bit words (each part is whole, aligned words: pipeline_compact_layout_ok).
// It only narrows candidates; key_equal decides.
inline uint32_t key_hash(const CompactKey& k) noexcept {
    uint32_t h = 0x9E3779B9u ^ k.size;
    for (size_t i = 0; i < CompactKey::Parts; ++i)
        for (uint32_t o = 0; o < k.length[i]; o += 4) {
            uint32_t w;
            std::memcpy(&w, k.part[i] + o, 4);
            h ^= w * 0xCC9E2D51u;
            h = ((h << 15) | (h >> 17)) * 0x1B873593u + 0xE6546B64u;
        }
    h ^= k.tcgs;
    h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return h;
}
inline bool key_equal(const uint8_t* stored, const CompactKey& k) noexcept {
    for (size_t i = 0; i < CompactKey::Parts; ++i) {
        if (std::memcmp(stored, k.part[i], k.length[i]) != 0) return false;
        stored += k.length[i];
    }
    return *stored == k.tcgs;
}
inline void key_copy(uint8_t* out, const CompactKey& k) noexcept {
    for (size_t i = 0; i < CompactKey::Parts; ++i) {
        std::memcpy(out, k.part[i], k.length[i]);
        out += k.length[i];
    }
    *out = k.tcgs;
}

// Member order and word alignment the key relies on (checked once per config type, at start-up).
template<class Config> bool pipeline_compact_layout_ok() noexcept {
    const Config c{};
    const auto& s = c.shaderConfig;
    const auto p = [](const void* v) { return static_cast<const uint8_t*>(v); };
    const auto* base = p(&c);
    const bool ordered =
        base < p(s.tevStages.data()) && p(s.tevStages.data() + s.tevStages.size()) <= p(&s.tevStageCount) &&
        p(&s.tevStageCount) < p(s.tcgs.data()) && p(s.tcgs.data() + s.tcgs.size()) <= p(&s.alphaCompare) &&
        p(&s.alphaCompare) < p(s.indStages.data()) &&
        p(s.indStages.data() + s.indStages.size()) <= p(&s.numIndStages) &&
        p(&s.numIndStages) < base + sizeof(Config);
    const CompactKey k = pipeline_compact_key(c);
    bool words = sizeof s.tevStages[0] % 4 == 0 && sizeof s.tcgs[0] % 4 == 0 && sizeof s.indStages[0] % 4 == 0;
    for (size_t i = 0; i < CompactKey::Parts; ++i)
        words = words && (k.part[i] - base) % 4 == 0 && k.length[i] % 4 == 0;
    return ordered && words;
}

// Compact key -> pipeline index. Hashing narrows candidates; the full key comparison decides.
class CompactPipelineIndex {
    struct Entry { uint32_t offset, size, id; };
    std::unordered_multimap<uint32_t, Entry> entries;
    std::vector<uint8_t> keys;
public:
    static constexpr uint32_t Missing = UINT32_MAX;
    uint32_t find(uint32_t hash, const CompactKey& key) const {
        const auto range = entries.equal_range(hash);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second.size == key.size && key_equal(keys.data() + it->second.offset, key))
                return it->second.id;
        return Missing;
    }
    void insert(uint32_t hash, const CompactKey& key, uint32_t id) {
        const size_t offset = keys.size();
        keys.resize(offset + key.size);
        key_copy(keys.data() + offset, key);
        entries.emplace(hash, Entry{static_cast<uint32_t>(offset), key.size, id});
    }
    size_t size() const noexcept { return entries.size(); }
};

// Entries [from, to) of an array back to their default value (the fast populate's reset of entries a
// previous config used and this one does not).
template<class Array> void reset_entries(Array& a, size_t from, size_t to) noexcept {
    to = std::min(to, a.size());
    for (size_t i = from; i < to; ++i) a[i] = {};
}
} // namespace twv::vgl
