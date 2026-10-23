#include "gfx_vgl/pipeline_fast.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// Same member order as aurora::gx::PipelineConfig / ShaderConfig, with non-zero defaults like the real
// TEV stage and texgen types.
struct Tev { uint32_t a = 7, b = 9; };
struct Tcg { uint32_t type = 1, src = 21; };
struct Ind { uint32_t coord = 0, map = 0; };
struct Shader {
    uint32_t header = 3;
    std::array<uint32_t, 5> attrs{};
    std::array<uint32_t, 4> tevSwapTable{};
    std::array<Tev, 16> tevStages{};
    uint32_t tevStageCount = 0;
    std::array<uint32_t, 4> colorChannels{};
    std::array<Tcg, 8> tcgs{};
    uint32_t alphaCompare = 0;
    std::array<Ind, 4> indStages{};
    uint32_t numIndStages = 0;
};
struct Config {
    uint32_t version = 13, msaa = 1;
    Shader shaderConfig{};
    uint32_t blendMode = 0, depthUpdate = 0;
};

// What populate_pipeline_config writes: a default config with the first count entries filled.
Config populate(std::mt19937& rng) {
    Config c{};
    auto& s = c.shaderConfig;
    const auto v = [&](uint32_t n) { return uint32_t(rng() % n); };
    s.attrs[1] = v(2);
    s.tevStageCount = v(3);
    for (uint32_t i = 0; i < s.tevStageCount; ++i) s.tevStages[i] = {7 + v(2), 9};
    const uint32_t tcgs = v(3);
    for (uint32_t i = 0; i < tcgs; ++i) s.tcgs[i] = {1, 20 + v(2)}; // src 21 is the default entry
    s.numIndStages = v(2);
    for (uint32_t i = 0; i < s.numIndStages; ++i) s.indStages[i] = {v(2), 0};
    c.blendMode = v(2);
    return c;
}

int main() {
    using namespace twv::vgl;
    assert(pipeline_compact_layout_ok<Config>());
    std::mt19937 rng(0x3227d76c);
    std::vector<Config> configs;
    for (int i = 0; i < 4000; ++i) configs.push_back(populate(rng));
    std::vector<std::vector<uint8_t>> keys;
    std::vector<uint32_t> hashes;
    for (const auto& c : configs) {
        const CompactKey k = pipeline_compact_key(c);
        std::vector<uint8_t> key(k.size);
        key_copy(key.data(), k);
        assert(key_equal(key.data(), k));
        assert(key.size() < sizeof(Config) && key.size() >= sizeof(Config) - sizeof(Shader::tevStages)
               - sizeof(Shader::tcgs) - sizeof(Shader::indStages));
        keys.push_back(std::move(key));
        hashes.push_back(key_hash(k));
    }
    // Equal keys exactly when the whole configs are equal; equal keys hash alike.
    unsigned equalPairs = 0;
    for (size_t i = 0; i < configs.size(); i += 7)
        for (size_t j = 0; j < configs.size(); ++j) {
            const bool sameConfig = std::memcmp(&configs[i], &configs[j], sizeof(Config)) == 0;
            assert(sameConfig == (keys[i] == keys[j]));
            assert(sameConfig == (keys[i].size() == keys[j].size() &&
                                  key_equal(keys[i].data(), pipeline_compact_key(configs[j]))));
            if (sameConfig) assert(hashes[i] == hashes[j]);
            equalPairs += sameConfig && i != j;
        }
    assert(equalPairs > 0);
    // The index returns the id of an equal key only, through hash collisions.
    CompactPipelineIndex index;
    for (size_t i = 0; i < configs.size(); ++i) {
        const CompactKey k = pipeline_compact_key(configs[i]);
        const uint32_t hash = k.size % 3; // force collisions
        const uint32_t found = index.find(hash, k);
        if (found == CompactPipelineIndex::Missing) index.insert(hash, k, uint32_t(i));
        else assert(std::memcmp(&configs[found], &configs[i], sizeof(Config)) == 0);
    }
    // reset_entries puts entries back to their default.
    std::array<Tev, 16> stages{};
    for (auto& s : stages) s = {1, 2};
    reset_entries(stages, 3, 40);
    for (size_t i = 0; i < stages.size(); ++i) assert((stages[i].a == 7 && stages[i].b == 9) == (i >= 3));
    std::printf("pipeline_fast_test: %zu configs, compact keys equal exactly for equal configs (%u pairs), "
                "index through collisions, %zu distinct pipelines, reset to defaults passed\n",
                configs.size(), equalPairs, index.size());
}
