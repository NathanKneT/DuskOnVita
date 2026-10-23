#include "internal.hpp"
#include "gfx/texture_convert.hpp"
#include "gfx/texture_replacement.hpp"
#include "gfx/tex_palette_conv.hpp"
#include "gx/texture.hpp"
#include <algorithm>
#include <cstring>
#include "bundle_options.h"
#include "texture_upload.hpp"
#include "texture_upload_api.h"
#include "texture_conversion_cache.hpp"

namespace aurora::webgpu {
bool g_textureComponentSwizzleSupported=false;
bool g_bcTexturesSupported=false;
}
namespace twv::vgl {
static TextureConversionCache conversionCache;
Texture::Texture(uint32_t w,uint32_t h,uint32_t mips,uint32_t format,bool deferred)
 :TextureRef({}, {}, {}, wgpu::Extent3D{w,h,1},wgpu::TextureFormat::RGBA8Unorm,mips,format) {
    deferredUpload=deferred;
    if(!deferred) {
        gpu=twv_vgl_texture(w,h,mips);
        if(!gpu) count("texture_allocation_failure");
    }
}
Texture::~Texture() {
    // Serialized resource teardown also ends the tiny conversion-cache lifetime.
    // This covers room resource unloading without guessing a stage transition.
    conversionCache.clear();twv_vgl_texture_delete(gpu);
}
void upload(Texture& texture,aurora::ArrayRef<uint8_t> source) {
    const bool optimized=twv_vita_bundle_feature(TWV_BUNDLE_TEXTURE_UPLOAD)!=0;
    size_t required=0;
    if(!rgba_chain_size(texture.size.width,texture.size.height,texture.mipCount,required)||required>UINT32_MAX) {
        count("truncated_texture_upload");return;
    }
    const size_t sourceBytes=aurora::gx::texture::texture_source_size(texture.gxFormat,
        texture.size.width,texture.size.height,texture.mipCount);
    if(!sourceBytes||!source.data()||source.size()<sourceBytes) {
        count("truncated_texture_upload");return;
    }
    if(optimized) {
        const size_t budget=twv_vgl_texture_cache_budget(uint32_t(conversionCache.used()));
        if(conversionCache.used()>budget)conversionCache.clear();
    }
    size_t size=0;
    bool arbitrary=false;
    const uint8_t* data=optimized?conversionCache.find(texture.gxFormat,texture.size.width,
        texture.size.height,texture.mipCount,source.data(),sourceBytes,arbitrary,size):nullptr;
    aurora::gfx::ConvertedTexture converted{};
    if(data) count("texture_conversion_cache_hit");
    else {
    converted=aurora::gfx::convert_texture(texture.gxFormat,texture.size.width,
                         texture.size.height,texture.mipCount,source);
    data=converted.data.empty()?source.data():converted.data.data();
    size=converted.data.empty()?source.size():converted.data.size();
    if(converted.format!=wgpu::TextureFormat::RGBA8Unorm) {
        count("unsupported_texture_format");return;
    }
    arbitrary=converted.hasArbitraryMips;
    count("texture_conversion_reference");
    // Only cache actual conversions; direct RGBA8 has no decode cost to save.
    if(optimized&&!converted.data.empty()&&size>=required) {
        const auto budget=twv_vgl_texture_cache_budget(uint32_t(conversionCache.used()));
        conversionCache.put(texture.gxFormat,texture.size.width,texture.size.height,
            texture.mipCount,source.data(),sourceBytes,data,required,arbitrary,budget);
    }
    }
    if(!complete_rgba_chain(texture.size.width,texture.size.height,texture.mipCount,data,size)) {
        count("truncated_texture_upload");return;
    }
    texture.hasArbitraryMips=arbitrary;
    if(!texture.gpu&&optimized&&texture.deferredUpload) {
        texture.gpu=twv_vgl_texture_static_rgba(texture.size.width,texture.size.height,
            texture.mipCount,data,size);
        if(texture.gpu) {
            texture.deferredUpload=false;
            count("texture_static_direct_upload");count("textures_uploaded");
            count("texture_upload_bytes",uint32_t(required));return;
        }
    }
    if(!texture.gpu)texture.gpu=twv_vgl_texture(texture.size.width,texture.size.height,texture.mipCount);
    texture.deferredUpload=false;
    if(!texture.gpu){count("texture_allocation_failure");return;}
    size_t offset=0;
    for(uint32_t level=0;level<texture.mipCount;++level) {
        const uint32_t w=std::max(texture.size.width>>level,1u), h=std::max(texture.size.height>>level,1u);
        const size_t bytes=size_t(w)*h*4;
        if(offset>size||bytes>size-offset) {count("truncated_texture_upload");return;}
        twv_vgl_texture_write(texture.gpu,level,w,h,data+offset); offset+=bytes;
    }
    count("textures_uploaded"); count("texture_upload_bytes",uint32_t(offset));
}
}
extern "C" void twv_vgl_texture_conversion_cache_clear(void) {
    twv::vgl::conversionCache.clear();
}
namespace aurora::gfx {
uint64_t calc_texture_size(wgpu::TextureFormat,uint32_t width,uint32_t height,uint32_t mips) noexcept {
    uint64_t total=0;
    size_t size=0;
    if(twv::vgl::rgba_chain_size(width,height,mips,size))total=size;
    return total;
}
wgpu::SamplerDescriptor TextureBind::get_descriptor() const noexcept {return {};}
TextureHandle new_static_texture_2d(uint32_t w,uint32_t h,uint32_t mips,u32 fmt,ArrayRef<uint8_t> data,bool tlut,const char*) noexcept {
    const bool deferred=!tlut&&twv_vita_bundle_feature(TWV_BUNDLE_TEXTURE_UPLOAD);
    auto texture=std::make_shared<twv::vgl::Texture>(w,h,mips,fmt,deferred);
    if(!tlut) twv::vgl::upload(*texture,data);
    else {
        // Palettes are linear, not GX tiled images. Use Aurora's actual TLUT
        // decoder; dynamic EFB index-to-palette conversion remains pending.
        if((fmt!=GX_TF_IA8&&fmt!=GX_TF_RGB565&&fmt!=GX_TF_RGB5A3)||
           !data.data()||data.size()<size_t(w)*2) {
            twv::vgl::count("tlut_upload_failure");return texture;
        }
        const auto converted=convert_tlut(fmt,w,data);
        if(texture->gpu && converted.data.size()==size_t(w)*4) {
            twv_vgl_texture_write(texture->gpu,0,w,1,converted.data.data());
            twv::vgl::count("tluts_uploaded");
        } else twv::vgl::count("tlut_upload_failure");
    }
    // Deferred ownership never escapes without initialized storage, including
    // unsupported formats, truncated sources, and bridge allocation failures.
    if(deferred&&!texture->gpu)texture->gpu=twv_vgl_texture(w,h,mips);
    texture->deferredUpload=false;
    return texture;
}
TextureHandle new_dynamic_texture_2d(uint32_t w,uint32_t h,uint32_t mips,u32 fmt,const char*) noexcept {
    return std::make_shared<twv::vgl::Texture>(w,h,mips,fmt);
}
TextureHandle new_render_texture(uint32_t w,uint32_t h,u32 fmt,const char*) noexcept {
    return std::make_shared<twv::vgl::Texture>(w,h,1,fmt);
}
TextureHandle new_conv_texture(uint32_t w,uint32_t h,u32 fmt,const char*) noexcept {
    return std::make_shared<twv::vgl::Texture>(w,h,1,fmt);
}
void write_texture(TextureRef& ref,ArrayRef<uint8_t> data) noexcept {twv::vgl::upload(static_cast<twv::vgl::Texture&>(ref),data);}
void queue_texture_upload(TextureUpload) {twv::vgl::count("unsupported_webgpu_texture_upload");}
void queue_texture_upload_data(const uint8_t*,uint32_t,uint32_t,wgpu::TexelCopyTextureInfo,wgpu::Extent3D) {twv::vgl::count("unsupported_webgpu_texture_upload");}
void queue_palette_conv(tex_palette_conv::ConvRequest) {twv::vgl::count("unsupported_efb_palette_conversion");}
}
namespace aurora::gfx::texture_replacement {
StreamingStats process_streaming() noexcept {return {};}
std::optional<ReplacementResult> find_pointer_replacement(const GXTexObj_&) noexcept {return std::nullopt;}
std::optional<ReplacementResult> find_source_replacement(const GXTexObj_&,const texture::TextureSourceKey&) noexcept {return std::nullopt;}
bool should_build_source_key() noexcept {return false;}
std::string build_texture_replacement_name(const GXTexObj_&) noexcept {return {};}
std::string build_texture_replacement_name(const texture::TextureSourceKey&) noexcept {return {};}
}
