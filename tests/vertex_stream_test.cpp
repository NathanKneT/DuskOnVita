#include "gfx_vgl/vertex_contract.hpp"
#include "gfx_vgl/exact_key_index.hpp"
#include <array>
#include <cassert>
#include <cstdio>

int main() {
    using namespace twv::vgl;
    // Every texcoord/color mask, including holes and the last TC7, must produce
    // disjoint word-aligned spans within the advertised stride. Test the actual
    // store helper against sentinels, including both adjacent vertex boundaries.
    for(unsigned normal=0;normal<2;++normal) for(unsigned colors=0;colors<4;++colors)
      for(unsigned tex=0;tex<256;++tex) {
        const auto layout=compact_vertex_layout(tex,colors,normal);
        assert(layout.stride>=16 && layout.stride<=sizeof(TwvVitaVertex) && layout.stride%4==0);
        std::array<float,47> memory; memory.fill(-9999.25f);
        float* vertex=memory.data()+4;
        std::array<bool,39> occupied{};
        unsigned written=0;
        for(unsigned attr=0;attr<TwvVertexAttributeCount;++attr) {
            const float values[]={float(attr)+.25f,float(attr)+.5f,float(attr)+.75f,float(attr)+1.f};
            store_vertex_attribute(vertex,layout,attr,values);
            const bool needed=attr==0 || attr==12 || (attr==11 && normal) ||
                (attr>=1 && attr<=2 && (colors&(1u<<(attr-1)))) ||
                (attr>=3 && attr<=10 && (tex&(1u<<(attr-3))));
            assert((layout.offsets[attr]!=TwvVertexAttributeAbsent)==needed);
            if(!needed) continue;
            assert(layout.offsets[attr]%4==0);
            for(unsigned k=0;k<AttributeComponents[attr];++k) {
                const unsigned slot=layout.offsets[attr]/4+k;
                assert(slot<layout.stride/4 && !occupied[slot]);
                occupied[slot]=true; ++written;
                assert(vertex[slot]==values[k]);
            }
        }
        assert(written==layout.stride/4);
        for(unsigned i=0;i<4;++i)assert(memory[i]==-9999.25f);
        for(unsigned i=4+layout.stride/4;i<memory.size();++i)assert(memory[i]==-9999.25f);
      }
    const auto legacy=legacy_vertex_layout();
    assert(legacy.stride==156 && legacy.offsets[1]==12 && legacy.offsets[2]==28 && legacy.offsets[10]==128);
    assert(legacy.offsets[11]==TwvVertexAttributeAbsent && legacy.offsets[12]==TwvVertexAttributeAbsent);
    struct Key { uint32_t a,b; };
    ExactKeyIndex<Key> cache;
    cache.insert(42,{1,2},3); cache.insert(42,{1,4},8);
    assert(cache.find(42,{1,2})==3 && cache.find(42,{1,4})==8);
    assert(cache.find(42,{1,3})==UINT32_MAX && cache.find(41,{1,2})==UINT32_MAX);
    cache.clear(); assert(cache.find(42,{1,2})==UINT32_MAX);
    cache.insert(42,{1,2},9); assert(cache.find(42,{1,2})==9);
    std::puts("vertex_stream_test: 2048 compact layouts, guard stores, legacy ABI, exact hash collision/clear passed");
}
