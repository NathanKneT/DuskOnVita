#include "gfx_vgl/vertex_contract.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>

struct Matrix { float m0[4],m1[4],m2[4]; };
int main() {
    TwvVitaDrawState state{};
    // Nonuniform scale + translation catches row/column and palette-slot errors.
    const Matrix model{{2,0,0,10},{0,3,0,20},{0,0,4,30}};
    const Matrix normal{{0.5f,0,0,0},{0,1.f/3,0,0},{0,0,0.25f,0}};
    twv::vgl::pack_matrix_rows(state.pn_pos+9*12,model);
    twv::vgl::pack_matrix_rows(state.pn_nrm+9*12,normal);
    const auto transform=[](const float* rows,const std::array<float,4>& v) {
        std::array<float,3> out{};
        for(unsigned r=0;r<3;++r) for(unsigned k=0;k<4;++k)out[r]+=rows[r*4+k]*v[k];
        return out;
    };
    const auto p=transform(state.pn_pos+9*12,{1,2,3,1});
    assert(p[0]==12 && p[1]==26 && p[2]==42);
    const auto n=transform(state.pn_nrm+9*12,{0,0,2,0});
    assert(n[0]==0 && n[1]==0 && n[2]==0.5f);
    for(unsigned i=0;i<9*12;++i) assert(state.pn_pos[i]==0 && state.pn_nrm[i]==0);

    // Each color/alpha channel owns two vec4s; include bit7 and differing masks.
    const uint32_t masks[]={0x81,0x00,0x80,0xFF};
    for(unsigned ch=0;ch<4;++ch)twv::vgl::pack_light_mask(state.light_masks+8*ch,masks[ch]);
    assert(state.light_masks[0]==1 && state.light_masks[7]==1);
    for(unsigned i=1;i<7;++i)assert(state.light_masks[i]==0);
    for(unsigned i=8;i<16;++i)assert(state.light_masks[i]==0);
    assert(state.light_masks[16]==0 && state.light_masks[23]==1);
    for(unsigned i=24;i<32;++i)assert(state.light_masks[i]==1);
    // Last-slot writes must not spill into the next uniform array.
    for(float v:state.light_pos)assert(v==0);
    assert(sizeof(TwvVitaVertex)==156 && offsetof(TwvVitaVertex,pn_index)==152);
    assert(twv::vgl::VertexUniformVectors==120);
    assert(twv::vgl::vertex_uniform_vectors(false,48)==86);
    assert(twv::vgl::vertex_uniform_vectors(true,0,0)==42);
    assert(twv::vgl::vertex_uniform_vectors(true,0,1)==85);
    assert(twv::vgl::texgen_uniforms_fit(true,6));
    assert(!twv::vgl::texgen_uniforms_fit(true,9));
    assert(twv::vgl::texgen_uniforms_fit(false,48));
    struct Pair {Matrix pos,nrm;};
    std::array<Pair,10> palette{};
    for(unsigned i=0;i<10;++i) {
        palette[i].pos=model;palette[i].pos.m0[3]+=i*7;
        palette[i].nrm=normal;palette[i].nrm.m2[2]+=i;
    }
    // Full -> small -> full on the same shader program must read live matrices,
    // leave unused slots alone and preserve slot9 and nonuniform normal math.
    unsigned liveUpdates=0;
    for(unsigned slots: {10u,1u,5u,10u}) {
        std::array<float,122> positions,normals;
        positions.fill(-987.f);normals.fill(-456.f);
        twv::vgl::pack_matrix_palette(positions.data()+1,normals.data()+1,palette,slots,true);
        assert(positions[0]==-987.f && positions[121]==-987.f);
        assert(normals[0]==-456.f && normals[121]==-456.f);
        for(unsigned i=0;i<slots;++i) {
            const auto actual=transform(positions.data()+1+i*12,{1,2,3,1});
            assert(actual[0]==12+i*7+(i==0?liveUpdates:0) && actual[1]==26 && actual[2]==42);
        }
        for(unsigned i=slots*12+1;i<121;++i)assert(positions[i]==-987.f && normals[i]==-456.f);
        palette[0].pos.m0[3]+=1;
        ++liveUpdates;
    }
    std::array<float,120> positions{},normals;
    normals.fill(-456.f);
    twv::vgl::pack_matrix_palette(positions.data(),normals.data(),palette,1,false);
    for(float v:normals)assert(v==-456.f); // Unlit shader declares no normal palette.
    assert(positions[3]==palette[0].pos.m0[3]);
    std::puts("gpu_vertex_contract_test: live palette prefixes, slot9, row order, normal matrix, channel masks and ABI bounds passed");
}
