#include "gfx_vgl/vertex_contract.hpp"
#include <array>
#include <cassert>
#include <cstdio>

int main() {
    using namespace twv::vgl;
    for(unsigned normal=0;normal<2;++normal)for(unsigned colors=0;colors<4;++colors)
      for(unsigned mask=0;mask<256;++mask) {
        const auto old=compact_vertex_layout(mask,colors,normal);
        const auto st=compact_vertex_layout(mask,colors,normal,true);
        unsigned texCount=0;
        std::array<float,47> compact,reference;compact.fill(-12345.f);reference.fill(-12345.f);
        auto* actual=compact.data()+4;auto* expected=reference.data()+4;
        for(unsigned a=0;a<TwvVertexAttributeCount;++a) {
            const float values[]={float(a)+.25f,float(a)+.5f,7.25f,1.f};
            store_vertex_attribute(actual,st,a,values);store_vertex_attribute(expected,old,a,values);
            assert(st.offsets[a]==TwvVertexAttributeAbsent || st.offsets[a]+st.components[a]*4<=st.stride);
            if(st.offsets[a]==TwvVertexAttributeAbsent)continue;
            if(a>=3 && a<=10)++texCount;
            assert(st.components[a]==((a>=3 && a<=10)?2:AttributeComponents[a]));
            for(unsigned k=0;k<st.components[a];++k)
                assert(actual[st.offsets[a]/4+k]==expected[old.offsets[a]/4+k]);
            if(a>=3 && a<=10) {
                // GPU TEX source is explicitly (S,T,1); incoming Q is unused.
                const auto project=[](const float* p) {return p[0]*2+p[1]*3+5;};
                assert(project(actual+st.offsets[a]/4)==project(expected+old.offsets[a]/4));
                assert(expected[old.offsets[a]/4+2]==7.25f); // CPU fallback retains Q.
            }
        }
        assert(old.stride-st.stride==4*texCount);
        assert(st.components[12]==1 && st.components[1]==4 && st.components[2]==4);
        assert(actual[st.offsets[12]/4]==12.25f);
        for(unsigned i=0;i<4;++i)assert(compact[i]==-12345.f);
        for(unsigned i=4+st.stride/4;i<compact.size();++i)assert(compact[i]==-12345.f);
        // SDK layout equality uses all bytes, so the component contract is keyed.
        auto changed=st;changed.components[3]=3;
        assert(std::memcmp(&changed,&st,sizeof st)!=0);
      }
    const auto legacy=legacy_vertex_layout();
    assert(legacy.stride==156 && legacy.components[3]==3 && legacy.components[10]==3);
    std::puts("compact_st_test: 2048 sparse layouts, bounded ST stores, unchanged PN/colors, texgen consumption and STQ fallback passed");
}
