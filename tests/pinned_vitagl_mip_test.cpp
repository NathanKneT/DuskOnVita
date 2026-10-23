// Compile with pinned_vitagl_mipmap_fixture.inc extracted verbatim from the
// pinned SDK gpu_utils.c gpu_alloc_mipmaps function (handoff audit has recipe).
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
using SceGxmTextureFormat=int;using SceGxmTransferFormat=int;
#define MAX(a,b) ((a)>(b)?(a):(b))
#define VGL_ALIGN(a,b) (((a)+(b)-1)&~((b)-1))
#define TEX_VALID 1
#define OBJ_NOT_USED 0
struct Descriptor {unsigned w,h;};
struct texture {int mip_count=1;Descriptor gxm_tex{};bool use_mips=true;void* data=nullptr;void* palette_data=nullptr;int status=TEX_VALID,last_frame=OBJ_NOT_USED;};
struct Transfer {const uint8_t* src;uint8_t* dst;unsigned w,h,srcStride,dstStride;};
static std::vector<Transfer> pending;
static unsigned nearest_po2(unsigned n){unsigned out=1;while(out<n)out*=2;return out;}
static int vglGetTexFormat(Descriptor*){return 0;}
static unsigned tex_format_to_bytespp(int){return 4;}
static void vglGetTexSizes(Descriptor* d,uint32_t* w,uint32_t* h){*w=d->w;*h=d->h;}
static int tex_format_to_transfer(int){return 0;}
static void* vgl_realloc(void* p,size_t n){auto* q=std::realloc(p,n);assert(q);return q;}
static void* gpu_alloc_mapped_for_gpu(size_t n){auto* q=std::malloc(n);assert(q);return q;}
static void vgl_memcpy(void* d,const void* s,size_t n){std::memcpy(d,s,n);}
static void sceClibMemcpy(void* d,const void* s,size_t n){std::memcpy(d,s,n);}
static void gpu_free_texture_data(texture* t){std::free(t->data);}
static void vglInitLinearTexture(Descriptor* d,void*,int,unsigned w,unsigned h,int){*d={w,h};}
static void sceGxmTransferDownscale(int,const void* src,int,int,unsigned w,unsigned h,unsigned stride,int,void* dst,int,int,unsigned dstStride,void*,int,void*){
 pending.push_back({static_cast<const uint8_t*>(src),static_cast<uint8_t*>(dst),w,h,stride,dstStride});
}
static void finish_transfers(){
 for(const auto& t:pending)for(unsigned y=0;y<t.h/2;++y)for(unsigned x=0;x<t.w/2;++x)
  std::memcpy(t.dst+y*t.dstStride+x*4,t.src+y*2*t.srcStride+x*8,4);
 pending.clear();
}
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "pinned_vitagl_mipmap_fixture.inc"
#pragma GCC diagnostic pop
static uint8_t* mip(texture& t,unsigned level){auto* p=static_cast<uint8_t*>(t.data);unsigned w=t.gxm_tex.w,h=t.gxm_tex.h;for(unsigned i=0;i<level;++i){p+=std::max(w,8u)*h*4;w/=2;h/=2;}return p;}
static void fill_mip(texture& t,unsigned level,uint8_t value){unsigned w=t.gxm_tex.w>>level,h=t.gxm_tex.h>>level;auto* p=mip(t,level);for(unsigned y=0;y<h;++y)std::memset(p+y*std::max(w,8u)*4,value,w*4);}
static void expect_mip(texture& t,unsigned level,uint8_t value){unsigned w=t.gxm_tex.w>>level,h=t.gxm_tex.h>>level;auto* p=mip(t,level);for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w*4;++x)assert(p[y*std::max(w,8u)*4+x]==value);}
int main(){
 texture t;t.gxm_tex={8,8};t.data=std::malloc(256);std::memset(t.data,0x11,256);
 // Exactly pinned textures.c923-925's lower-level dispatch: supplied data
 // never reaches this function. No eager GL-copy assumption is allowed.
 gpu_alloc_mipmaps(1,&t);assert(!pending.empty());finish_transfers();expect_mip(t,1,0x11);
 fill_mip(t,1,0x42);expect_mip(t,1,0x42);
 // Defining a later level regenerates level1 as well, overwriting earlier data.
 gpu_alloc_mipmaps(2,&t);finish_transfers();expect_mip(t,1,0x11);
 // Unsynchronized CPU write loses against queued transfer work.
 gpu_alloc_mipmaps(3,&t);fill_mip(t,2,0x63);finish_transfers();expect_mip(t,2,0x11);
 // Allocate/generate the ENTIRE chain, finish transfers, then supply originals.
 fill_mip(t,1,0x42);fill_mip(t,2,0x63);fill_mip(t,3,0x84);
 finish_transfers();expect_mip(t,0,0x11);expect_mip(t,1,0x42);expect_mip(t,2,0x63);expect_mip(t,3,0x84);
 std::free(t.data);
}
