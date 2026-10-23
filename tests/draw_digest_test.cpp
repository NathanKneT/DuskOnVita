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
    std::printf("draw_digest_test: handle-free, read-bytes-only, upload/sampler/state/index sensitive, per-frame reset passed\n");
}
