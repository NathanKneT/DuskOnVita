#include "../platform/vita/gfx_vgl/texture_upload.hpp"
#include <cassert>
#include <cstdint>
#ifdef TWV_TEXTURE_UPLOAD_LIFECYCLE_TEST
static void lifecycle_test();
#endif
int main(){
    using namespace twv::vgl;
    size_t size=0;
    assert(rgba_chain_size(8,4,4,size)&&size==172);
    uint8_t data[172]{};
    assert(complete_rgba_chain(8,4,4,data,sizeof(data)));
    assert(!complete_rgba_chain(8,4,4,data,sizeof(data)-1));
    assert(!complete_rgba_chain(8,4,4,nullptr,sizeof(data)));
    assert(!rgba_chain_size(0,4,1,size));
    assert(!rgba_chain_size(4,0,1,size));
    assert(!rgba_chain_size(4,4,0,size));
    assert(!rgba_chain_size(4,4,33,size));
    assert(!rgba_chain_size(UINT32_MAX,UINT32_MAX,1,size));
    assert(rgba_chain_size(1,1,32,size)&&size==128);
#ifdef TWV_TEXTURE_UPLOAD_LIFECYCLE_TEST
    lifecycle_test();
#endif
}
// Optional integration mode: extract upload() and texture factories from
// textures.cpp into texture_upload_functions.inc (tools/texture_upload_test_fixture.py).
#ifdef TWV_TEXTURE_UPLOAD_LIFECYCLE_TEST
#include "../platform/vita/gfx_vgl/texture_conversion_cache.hpp"
#include <vector>
#include <memory>
#include <algorithm>
namespace wgpu {enum class TextureFormat {RGBA8Unorm,Unsupported};}
constexpr uint32_t GX_TF_IA8=1,GX_TF_RGB565=3,GX_TF_RGB5A3=4;
namespace aurora {
template<class T> struct ArrayRef {const T* p;size_t n;const T* data()const{return p;}size_t size()const{return n;}};
namespace gfx {
struct ConvertedTexture {wgpu::TextureFormat format=wgpu::TextureFormat::RGBA8Unorm;std::vector<uint8_t> data;bool hasArbitraryMips=false;};
using u32=uint32_t;
ConvertedTexture convert_texture(uint32_t f,uint32_t w,uint32_t h,uint32_t m,ArrayRef<uint8_t> data){
    ConvertedTexture out;size_t n=0;twv::vgl::rgba_chain_size(w,h,m,n);
    if(f==2)out.format=wgpu::TextureFormat::Unsupported;
    else if(f==1){out.data.assign(n, data.p[0]);out.hasArbitraryMips=true;}
    return out;
}
ConvertedTexture convert_tlut(uint32_t,uint32_t w,ArrayRef<uint8_t>){ConvertedTexture out;out.data.resize(size_t(w)*4);return out;}
}
namespace gx::texture {
size_t texture_source_size(uint32_t,uint32_t w,uint32_t h,uint32_t m){size_t n=0;twv::vgl::rgba_chain_size(w,h,m,n);return n;}
}
}
static unsigned zeros=0,directs=0,writes=0;
static bool bridgeFail=false,enabled=true;
#define TWV_BUNDLE_TEXTURE_UPLOAD 4u
int twv_vita_bundle_feature(uint32_t){return enabled;}
uint32_t twv_vgl_texture_cache_budget(uint32_t){return 2*1024*1024;}
uint32_t twv_vgl_texture(uint32_t,uint32_t,uint32_t){++zeros;return 11;}
uint32_t twv_vgl_texture_static_rgba(uint32_t w,uint32_t h,uint32_t m,const void* p,size_t n){assert(twv::vgl::complete_rgba_chain(w,h,m,p,n));++directs;return bridgeFail?0:12;}
void twv_vgl_texture_write(uint32_t gpu,uint32_t,uint32_t,uint32_t,const void*){assert(gpu);++writes;}
namespace twv::vgl {
struct Texture {
    uint32_t gpu=0;bool deferredUpload=false,hasArbitraryMips=false,efbCopy=false;
    struct {uint32_t width,height;} size;
    uint32_t mipCount,gxFormat;
    Texture(uint32_t w,uint32_t h,uint32_t m,uint32_t f,bool defer=false):deferredUpload(defer),size{w,h},mipCount(m),gxFormat(f){if(!defer)gpu=twv_vgl_texture(w,h,m);}
};
static TextureConversionCache conversionCache;
void count(const char*,uint32_t=1){}
// textures.cpp's pool relief (patches/aurora/0010) is outside the extracted functions: one attempt.
template<class Allocate> uint32_t allocate_or_relieve(Allocate allocate){return allocate();}
}
namespace aurora::gfx {using TextureHandle=std::shared_ptr<twv::vgl::Texture>;}
#include "texture_upload_functions.inc"
static void lifecycle_test(){
    using namespace aurora::gfx;
    uint8_t src[84]{};aurora::ArrayRef<uint8_t> full{src,sizeof(src)},shortSource{src,1};
    auto a=new_static_texture_2d(4,4,3,1,full,false,"");assert(a->gpu==12&&zeros==0&&directs==1&&writes==0);
    auto b=new_dynamic_texture_2d(4,4,3,1,"");assert(b->gpu==11&&zeros==1&&directs==1);
    auto c=new_render_texture(4,4,1,"");auto d=new_conv_texture(4,4,1,"");assert(zeros==3);
    // Only EFB copy destinations (and their conversions) are marked: draws sampling them are skipped.
    assert(c->efbCopy&&d->efbCopy&&!a->efbCopy&&!b->efbCopy);
    auto e=new_static_texture_2d(4,4,3,1,shortSource,false,"");assert(e->gpu==11&&zeros==4&&directs==1);
    auto f=new_static_texture_2d(4,4,3,2,full,false,"");assert(f->gpu==11&&zeros==5&&directs==1);
    auto g=new_static_texture_2d(4,1,1,1,full,true,"");assert(g->gpu==11&&zeros==6&&writes==1);
    bridgeFail=true;
    auto h=new_static_texture_2d(4,4,3,1,full,false,"");assert(h->gpu==11&&zeros==7&&directs==2&&writes==4);
    enabled=false;bridgeFail=false;
    auto i=new_static_texture_2d(4,4,3,1,full,false,"");assert(i->gpu==11&&zeros==8&&directs==2&&writes==7);
}
#endif
