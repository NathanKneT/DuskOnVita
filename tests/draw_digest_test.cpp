// G0 draw digest: content, not handles; only the bytes the GPU reads; texture
// uploads and per-unit sampler requests count; frames reset.
#include "gfx_vgl/draw_digest.hpp"
#include "gfx_vgl/vertex_contract.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

namespace {
using twv::gpu::DrawDigest;

struct Scene {
    uint32_t program, texture;
    std::vector<unsigned char> vertices;
    std::vector<uint16_t> indices{0,1,2};
    TwvVitaDrawState state{};
};

Scene scene(uint32_t program,uint32_t texture,const TwvVitaVertexLayout& layout) {
    Scene s{program,texture,std::vector<unsigned char>(size_t(layout.stride)*3)};
    for(size_t i=0;i<s.vertices.size();++i)s.vertices[i]=static_cast<unsigned char>(i*7+3);
    s.state.program=program;s.state.textures[0]=texture;
    s.state.depth_test=1;s.state.viewport[2]=724;s.state.viewport[3]=544;
    s.state.vertex_layout=layout;s.state.projection[0]=1.5f;
    return s;
}
// One frame: program + texture creation, an upload, a sampler request, a clear and a draw.
uint64_t frame(DrawDigest& d,Scene& s,bool request=true) {
    d.program_created(s.program,"vertex main","fragment main");
    d.texture_created(s.texture,4,4,1);
    const std::vector<unsigned char> rgba(64,0x40);
    d.texture_written(s.texture,0,4,4,rgba.data());
    d.texture_sampler(s.texture,1,1,0,0);
    if(request)d.sampler_unit(0,5,1,1,2,1);
    d.clear(0,0,0,1,1,7,118,0,724,544);
    d.draw(s.state,s.vertices.data(),3,s.indices.data(),3);
    return d.end_frame().digest;
}
}

int main() {
    const auto legacy=twv::vgl::legacy_vertex_layout();
    const auto spans=twv::gpu::layout_spans(legacy);
    // Position, both colours and eight texture coordinates are contiguous; normal and
    // pn_index (140..155) are never written on this path and never hashed.
    assert(legacy.stride==156 && spans.count==1 && spans.begin[0]==0 && spans.end[0]==140 && !spans.dense());
    const auto compact=twv::vgl::compact_vertex_layout(0x3,0x1,true);
    assert(twv::gpu::layout_spans(compact).dense());

    // Same content behind different handles gives the same digest.
    DrawDigest a,b;
    Scene sa=scene(5,3,legacy),sb=scene(9,11,legacy);
    const uint64_t da=frame(a,sa),db=frame(b,sb);
    assert(da==db && da!=0);
    // A byte the GPU never reads does not change it; a read byte does.
    {
        DrawDigest c;Scene sc=scene(5,3,legacy);sc.vertices[156+150]^=0xff;
        assert(frame(c,sc)==da);
        DrawDigest e;Scene se=scene(5,3,legacy);se.vertices[156+20]^=0x01;
        assert(frame(e,se)!=da);
    }
    // Indices, draw state, program source, the upload and the sampler request all count.
    {
        DrawDigest c;Scene sc=scene(5,3,legacy);sc.indices[2]=1;assert(frame(c,sc)!=da);
        DrawDigest e;Scene se=scene(5,3,legacy);se.state.projection[0]=1.25f;assert(frame(e,se)!=da);
        DrawDigest f;Scene sf=scene(5,3,legacy);assert(frame(f,sf,false)!=da);
        DrawDigest g;Scene sg=scene(5,3,legacy);
        g.program_created(5,"vertex main","fragment other");
        g.texture_created(3,4,4,1);
        const std::vector<unsigned char> rgba(64,0x40);g.texture_written(3,0,4,4,rgba.data());
        g.texture_sampler(3,1,1,0,0);g.sampler_unit(0,5,1,1,2,1);g.clear(0,0,0,1,1,7,118,0,724,544);
        g.draw(sg.state,sg.vertices.data(),3,sg.indices.data(),3);
        assert(g.end_frame().digest!=da);
    }
    // A request is consumed by the next draw only; a frame starts from nothing.
    {
        DrawDigest c;Scene s=scene(5,3,legacy);
        c.program_created(5,"v","f");
        c.sampler_unit(1,0,0,0,0,1);
        const uint64_t first=c.draw(s.state,s.vertices.data(),3,s.indices.data(),3);
        const uint64_t second=c.draw(s.state,s.vertices.data(),3,s.indices.data(),3);
        assert(first!=second);
        const auto f1=c.end_frame();
        assert(f1.draws==2 && f1.vertices==6 && f1.indices==6);
        const auto empty=c.end_frame();
        assert(empty.draws==0 && empty.digest!=f1.digest);
        DrawDigest fresh;assert(fresh.end_frame().digest==empty.digest);
    }
    // Deleting a texture forgets it; an unknown handle hashes differently from none.
    {
        DrawDigest c;Scene s=scene(5,3,legacy);
        c.program_created(5,"v","f");c.texture_created(3,4,4,1);
        const uint64_t known=c.draw(s.state,s.vertices.data(),3,s.indices.data(),3);
        c.texture_deleted(3);
        const uint64_t unknown=c.draw(s.state,s.vertices.data(),3,s.indices.data(),3);
        s.state.textures[0]=0;
        const uint64_t none=c.draw(s.state,s.vertices.data(),3,s.indices.data(),3);
        assert(known!=unknown && unknown!=none && known!=none && c.live_textures()==0);
    }
    // G2: a packed draw hashes as its F32 form, a native strip/fan/quad as Aurora's triangle list,
    // and a program source with a /*G2{*/.../*}G2*/ section as the source without it.
    {
        auto floats=twv::vgl::compact_vertex_layout(1,1,true,true); // pos, pn, normal, colour0, tc0 (ST)
        twv::vgl::PackedFormats f{};
        f.format[0]=TwvFormatS16;f.count[0]=3;f.frac[0]=8;
        f.format[11]=TwvFormatS8;f.count[11]=3;f.frac[11]=6;
        f.format[12]=TwvFormatU8;f.count[12]=1;
        f.format[1]=TwvFormatU8N;f.count[1]=4;
        f.format[3]=TwvFormatU16;f.count[3]=2;f.frac[3]=10;
        const auto packedLayout=twv::vgl::packed_layout_of(floats,f);
        assert(floats.stride==(3+1+3+4+2)*4 && packedLayout.stride==8+4+4+4+4);
        const unsigned n=4;
        std::vector<unsigned char> fv(size_t(n)*floats.stride),pv(size_t(n)*packedLayout.stride,0);
        for(unsigned v=0;v<n;++v) {
            const int16_t pos[3]={int16_t(100*v-7),int16_t(-3),int16_t(1000)};
            const int8_t nrm[3]={int8_t(64),int8_t(-32),0};
            const uint8_t col[4]={uint8_t(v),200,13,255};
            const uint16_t tc[2]={uint16_t(1024*v+5),uint16_t(7)};
            float values[4];
            auto* fvert=fv.data()+size_t(v)*floats.stride;auto* pvert=pv.data()+size_t(v)*packedLayout.stride;
            for(unsigned k=0;k<3;++k)values[k]=float(pos[k])/256.f;
            std::memcpy(fvert+floats.offsets[0],values,12);std::memcpy(pvert+packedLayout.offsets[0],pos,6);
            values[0]=float(v%2);std::memcpy(fvert+floats.offsets[12],values,4);pvert[packedLayout.offsets[12]]=uint8_t(v%2);
            for(unsigned k=0;k<3;++k)values[k]=float(nrm[k])/64.f;
            std::memcpy(fvert+floats.offsets[11],values,12);std::memcpy(pvert+packedLayout.offsets[11],nrm,3);
            for(unsigned k=0;k<4;++k)values[k]=twv::vgl::ColorUnorm<8>[col[k]];
            std::memcpy(fvert+floats.offsets[1],values,16);std::memcpy(pvert+packedLayout.offsets[1],col,4);
            for(unsigned k=0;k<2;++k)values[k]=float(tc[k])/1024.f;
            std::memcpy(fvert+floats.offsets[3],values,8);std::memcpy(pvert+packedLayout.offsets[3],tc,4);
        }
        TwvVitaDrawState fs{};fs.program=5;fs.vertex_layout=floats;
        TwvVitaDrawState ps=fs;ps.vertex_layout=packedLayout;
        for(unsigned a=0;a<TwvVertexAttributeCount;++a){ps.vertex_formats[a]=f.format[a];ps.vertex_counts[a]=f.count[a];ps.vertex_fracs[a]=f.frac[a];}
        const std::vector<uint16_t> idx={0,1,2,2,1,3};
        DrawDigest a,b;a.program_created(5,"v","f");b.program_created(5,"v","f");
        assert(a.draw(fs,fv.data(),n,idx.data(),6)==b.draw(ps,pv.data(),n,idx.data(),6));
        // The exact inverse round-trips every packed value; an unrepresentable one is refused.
        uint8_t raw[8]{};const float half=0.5f,third=1.f/3.f;
        assert(twv::vgl::pack_attribute(raw,TwvFormatS16,1,1,&half) && raw[0]==1 && raw[1]==0);
        assert(!twv::vgl::pack_attribute(raw,TwvFormatS16,1,1,&third));
        const float colour=twv::vgl::ColorUnorm<8>[77];
        assert(twv::vgl::pack_attribute(raw,TwvFormatU8N,1,0,&colour) && raw[0]==77);
        // Strip, fan and quads against the indices Aurora generates.
        const std::vector<uint16_t> strip={0,1,2, 2,1,3, 2,3,4},fan={0,1,2, 0,2,3, 0,3,4},quads={0,1,2,2,3,0,4,5,6,6,7,4};
        TwvVitaDrawState tri{};tri.program=5;tri.vertex_layout=floats;
        std::vector<unsigned char> eight(size_t(8)*floats.stride,0x11);
        const auto native=[&](uint32_t primitive,uint32_t count) {
            DrawDigest d;d.program_created(5,"v","f");TwvVitaDrawState s=tri;s.primitive=primitive;
            return d.draw(s,eight.data(),count,nullptr,0);
        };
        const auto listed=[&](const std::vector<uint16_t>& i,uint32_t count) {
            DrawDigest d;d.program_created(5,"v","f");return d.draw(tri,eight.data(),count,i.data(),uint32_t(i.size()));
        };
        assert(native(TwvPrimitiveStrip,5)==listed(strip,5) && native(TwvPrimitiveFan,5)==listed(fan,5));
        assert(native(TwvPrimitiveQuads,8)==listed(quads,8) && native(TwvPrimitiveStrip,5)!=native(TwvPrimitiveFan,5));
        DrawDigest p1,p2;
        p1.program_created(7,"float4 main(){Out o;int m=0;}","f");
        p2.program_created(7,"float4 main(){Out o;/*G2{*/position=position*0.00390625;/*}G2*/int m=0;}","f");
        assert(p1.draw(tri,eight.data(),3,nullptr,0)==p2.draw(tri,eight.data(),3,nullptr,0));
    }
    std::printf("draw_digest_test: handle-free, read-bytes-only, upload/sampler/state/index sensitive, per-frame reset, "
                "G2 packed/primitive/source canonical forms passed\n");
}
