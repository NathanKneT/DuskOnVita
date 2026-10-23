// G1: GXM state mappings (gfx_gxm/gxm_state.hpp), memory bookkeeping (gxm_memory.hpp)
// and the GXP cache format (gxm_shader_cache.hpp).
#include "gfx_gxm/gxm_memory.hpp"
#include "gfx_gxm/gxm_shader_cache.hpp"
#include "gfx_gxm/gxm_state.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace twv::gxm;

static void state() {
    // Blending off: function NONE with vitaGL's default factors, colour mask from the writes.
    auto k=blend_key(0,0,4,5,1,0);
    assert(k.colorFunc==BlendFuncNone && k.alphaFunc==BlendFuncNone && k.colorMask==(MaskR|MaskG|MaskB));
    assert(k.colorSrc==FactorSrcAlpha && k.colorDst==FactorOneMinusSrcAlpha && k.alphaSrc==FactorOne && k.alphaDst==FactorZero);
    // GX blend SRC_ALPHA / INV_SRC_ALPHA, both channels.
    k=blend_key(1,0,4,5,1,1);
    assert(k.colorFunc==BlendFuncAdd && k.alphaFunc==BlendFuncAdd && k.colorMask==MaskAll);
    assert(k.colorSrc==FactorSrcAlpha && k.alphaSrc==FactorSrcAlpha && k.colorDst==FactorOneMinusSrcAlpha);
    // Source 2/3 are destination colour, destination 2/3 source colour (GX_BL_DSTCLR / SRCCLR).
    k=blend_key(1,0,2,3,1,1);
    assert(k.colorSrc==FactorDstColor && k.colorDst==FactorOneMinusSrcColor);
    assert(blend_factor(9,false)==FactorOne && blend_factor(-1,true)==FactorOne);
    // Subtract: dst - src with ONE/ONE.
    k=blend_key(1,1,4,5,0,1);
    assert(k.colorFunc==BlendFuncReverseSubtract && k.colorSrc==FactorOne && k.colorDst==FactorOne && k.colorMask==MaskA);
    assert(blend_key(1,0,4,5,1,1).packed()!=blend_key(1,0,4,5,1,0).packed());
    assert(depth_func(0)==0 && depth_func(3)==0x00C00000u && depth_func(7)==0x01C00000u && depth_func(9)==depth_func(7));
    assert(cull_mode(0)==CullNone && cull_mode(1)==CullCw && cull_mode(2)==CullCcw && cull_mode(3)==CullNone);
    // Sampler codes: 0..5 = NEAREST, LINEAR, then the four mipmap filters.
    auto s=sampler_words(0,0,0,0);
    assert(s.minFilter==FilterPoint && s.mipFilter==MipFilterDisabled && !s.mips && s.magFilter==FilterPoint && s.u==AddrClamp);
    s=sampler_words(3,1,1,2);
    assert(s.minFilter==FilterLinear && s.mipFilter==MipFilterDisabled && s.mips && s.u==AddrRepeat && s.v==AddrMirror);
    s=sampler_words(4,1,0,0);
    assert(s.minFilter==FilterPoint && s.mipFilter==MipFilterEnabled && s.mips);
    s=sampler_words(7,1,0,0); // invalid -> GL_LINEAR, as the vitaGL backend
    assert(s.minFilter==FilterLinear && !s.mips);
    // Viewport: the game view inside the 960x544 display (GL origin bottom-left).
    const auto v=viewport(118,0,724,544);
    assert(v.xOffset==480.f && v.xScale==362.f && v.yOffset==272.f && v.yScale==-272.f && v.zOffset==.5f && v.zScale==.5f);
    const auto r=scissor_region(118,0,724,544);
    assert(!r.empty && r.xMin==118 && r.xMax==841 && r.yMin==0 && r.yMax==543);
    const auto top=scissor_region(200,500,10,44); // GL y=500..543 is the top band
    assert(top.yMin==0 && top.yMax==43 && top.xMin==200 && top.xMax==209);
    assert(scissor_region(950,0,40,10).xMax==959 && scissor_region(0,0,0,10).empty);
    const auto q=clear_quad(0,0,960,544,1.f);
    assert(q.x0==-1.f && q.x1==1.f && q.yTop==1.f && q.yBottom==-1.f && q.z==1.f);
    const auto g=clear_quad(118,0,724,544,0.5f);
    assert(g.z==0.f && g.x0>-0.76f && g.x0<-0.75f);
    // Linear mips: level 0 ALIGN(w,8) rows; later levels max(w,8) rows of the power-of-two size.
    auto m=mip_level(64,32,0);assert(m.offset==0 && m.stride==64 && m.width==64);
    m=mip_level(64,32,1);assert(m.offset==64*32*4 && m.stride==32 && m.width==32 && m.height==16);
    m=mip_level(64,32,3);assert(m.offset==(64*32+32*16+16*8)*4 && m.stride==8 && m.width==8);
    m=mip_level(64,32,4);assert(m.offset==(64*32+32*16+16*8+8*4)*4 && m.stride==8 && m.width==4 && m.height==2);
    assert(texture_bytes(64,32,1)==64*32*4 && texture_bytes(6,4,1)==8*4*4);
    assert(texture_bytes(64,32,5)==(64*32+32*16+16*8+8*4+8*2)*4);
    m=mip_level(48,48,1);assert(m.offset==64*64*4 && m.stride==32); // rounded up, as vitaGL
}

static void memory() {
    RangeAllocator a;a.reset(4096);
    uint32_t x=0,y=0,z=0;
    assert(a.allocate(1000,256,x) && x==0);
    assert(a.allocate(1000,256,y) && y==1024);
    assert(a.allocate(1000,256,z) && z==2048);
    assert(a.used()==3000 && a.fragments()==3); // 24-byte alignment gaps before y and z, then the tail
    uint32_t big=0;assert(!a.allocate(2000,256,big));
    a.release(y,1000);a.release(x,1000);
    assert(a.allocate(2000,256,big) && big==0); // coalesced head
    a.release(big,2000);a.release(z,1000);
    assert(a.used()==0 && a.fragments()==1 && a.peak()==3000);
    assert(!a.allocate(0,256,x) && !a.allocate(8,3,x));

    FrameRing ring;ring.reset(1024);
    ring.begin(1);
    assert(ring.allocate(100,16)==1024+0 && ring.allocate(10,16)==1024+112);
    assert(ring.allocate(1000,16)==UINT32_MAX && ring.overflows()==1);
    ring.begin(3);assert(ring.allocate(8,16)==0 && ring.slot()==0);
    ring.begin(5);assert(ring.allocate(1024,16)==2048 && ring.peak()==1024);

    RetireList<int> retired;std::vector<int> freed;
    retired.retire(1,5);retired.retire(2,7);retired.retire(3,6);
    retired.collect(6,[&](int v){freed.push_back(v);});
    assert((freed==std::vector<int>{1,3}) && retired.pending()==1);
}

static void cache() {
    const char* source="float4 main() : COLOR { return float4(1.0,0.0,0.0,1.0); }\n";
    const uint64_t k=cache_key(StageFragment,0x1234,source);
    assert(k==cache_key(StageFragment,0x1234,source));
    assert(k!=cache_key(StageVertex,0x1234,source) && k!=cache_key(StageFragment,0x1235,source));
    assert(k!=cache_key(StageFragment,0x1234,"float4 main() : COLOR { return float4(0.0); }\n"));
    char path[128];cache_path(path,sizeof path,"root",StageFragment,0xab00000000000001ULL);
    assert(std::string(path)=="root/f/ab/ab00000000000001.gxp");
    const std::vector<unsigned char> gxp={'G','X','P',0,1,2,3,4};
    const auto h=make_header(StageFragment,k,source_hash(source),uint32_t(gxp.size()));
    std::vector<unsigned char> file(sizeof h);std::memcpy(file.data(),&h,sizeof h);
    file.insert(file.end(),gxp.begin(),gxp.end());
    uint32_t size=0;
    const auto* found=parse_cache_file(file.data(),file.size(),StageFragment,k,source_hash(source),size);
    assert(found && size==gxp.size() && std::memcmp(found,gxp.data(),size)==0);
    // Any disagreement rejects the file: stage, key, source, length, magic.
    assert(!parse_cache_file(file.data(),file.size(),StageVertex,k,source_hash(source),size));
    assert(!parse_cache_file(file.data(),file.size(),StageFragment,k+1,source_hash(source),size));
    assert(!parse_cache_file(file.data(),file.size(),StageFragment,k,source_hash("x"),size));
    assert(!parse_cache_file(file.data(),file.size()-1,StageFragment,k,source_hash(source),size));
    file[0]='X';assert(!parse_cache_file(file.data(),file.size(),StageFragment,k,source_hash(source),size));
}

int main() {
    state();memory();cache();
    std::printf("gxm_state_test: blend/depth/cull/sampler/viewport/scissor/clear/mip mappings, allocator, ring, retire list, GXP cache format passed\n");
}
