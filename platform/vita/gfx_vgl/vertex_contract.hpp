#pragma once
#include "gpu_api.h"
#include <cstring>

namespace twv::vgl {
constexpr unsigned VertexUniformVectors = 4+30+30+8*5+4+4+8;
static_assert(VertexUniformVectors==120 && VertexUniformVectors<=128);
constexpr unsigned vertex_uniform_vectors(bool lighting,unsigned texgenRows,unsigned lights=8) {
    return (lighting ? (lights ? 4+30+30+5*lights+4+4+8 : 4+30+4+4) : 4+30+4)+texgenRows;
}
constexpr bool texgen_uniforms_fit(bool lighting,unsigned rows) {
    return vertex_uniform_vectors(lighting,rows)<=128;
}

// GL attribute locations are shared by the Cg generator and SDK bridge.
constexpr uint32_t AttributeComponents[TwvVertexAttributeCount]={3,4,4,3,3,3,3,3,3,3,3,3,1};
inline TwvVitaVertexLayout compact_vertex_layout(uint32_t texMask,uint32_t colorMask,bool normal,bool rawST=false) {
    TwvVitaVertexLayout result{};
    for(auto& offset:result.offsets) offset=TwvVertexAttributeAbsent;
    for(unsigned a=0;a<TwvVertexAttributeCount;++a)
        result.components[a]=(rawST && a>=3 && a<=10)?2:AttributeComponents[a];
    const auto add=[&](unsigned attr) {
        result.offsets[attr]=result.stride;
        result.stride+=result.components[attr]*sizeof(float);
    };
    add(0); add(12);
    if(normal) add(11);
    for(unsigned ch=0;ch<2;++ch) if(colorMask&(1u<<ch)) add(1+ch);
    for(unsigned tc=0;tc<8;++tc) if(texMask&(1u<<tc)) add(3+tc);
    return result;
}
inline TwvVitaVertexLayout legacy_vertex_layout() {
    auto result=compact_vertex_layout(0,0,false);
    result.stride=sizeof(TwvVitaVertex);
    result.offsets[12]=TwvVertexAttributeAbsent;
    for(unsigned ch=0;ch<2;++ch)result.offsets[1+ch]=offsetof(TwvVitaVertex,color)+ch*4*sizeof(float);
    for(unsigned tc=0;tc<8;++tc)result.offsets[3+tc]=offsetof(TwvVitaVertex,texcoord)+tc*3*sizeof(float);
    return result;
}
inline void store_vertex_attribute(void* vertex,const TwvVitaVertexLayout& layout,unsigned attr,const void* values) {
    if(layout.offsets[attr]!=TwvVertexAttributeAbsent)
        std::memcpy(static_cast<unsigned char*>(vertex)+layout.offsets[attr],values,
                    layout.components[attr]*sizeof(float));
}

template<class Matrix>
void pack_matrix_rows(float* dst,const Matrix& matrix) {
    for(unsigned k=0;k<4;++k) {
        dst[k]=matrix.m0[k]; dst[4+k]=matrix.m1[k]; dst[8+k]=matrix.m2[k];
    }
}

// Only raw packet indices select this extent. Matrix values remain live and no
// slot is remapped. Shader declarations stay at ten slots: no extra programs or
// first-use compilation. The caller has rejected every index outside [0,9].
template<class Palette>
void pack_matrix_palette(float* pos,float* nrm,const Palette& matrices,unsigned slots,bool lighting) {
    for(unsigned i=0;i<slots;++i) {
        pack_matrix_rows(pos+i*12,matrices[i].pos);
        if(lighting)pack_matrix_rows(nrm+i*12,matrices[i].nrm);
    }
}

inline void pack_light_mask(float* dst,uint32_t mask) {
    for(unsigned i=0;i<8;++i) dst[i]=(mask&(1u<<i))?1.f:0.f;
}
}
