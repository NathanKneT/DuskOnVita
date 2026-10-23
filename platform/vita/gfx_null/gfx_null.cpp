// Null gfx backend for the Vita's headless milestone (M2).
//
// Aurora's GX frontend (lib/dolphin/gx), FIFO, command processor, register state and
// display-list reader run unchanged; they hand finished draws to a renderer through
// the contract Aurora's own GX unit tests stub (extern/aurora/tests/gx_test_stubs.cpp,
// pinned 3227d76c). This file implements that contract without a GPU:
//   * vertex/index/uniform/storage pushes, pipelines, draws, texture creation and
//     uploads, EFB resolves and offscreen passes are accepted and dropped, and
//     counted in AuroraStats (aurora_get_stats) so receipts show the GX work done;
//   * GX view state (viewport policy, logical/render viewport and scissor) is
//     kept exactly as lib/gx/gx.cpp does, with the render target equal to the
//     VI framebuffer, because game code reads it back (GXGetViewportv etc.);
//   * GXPeekZ finds no depth snapshot (depth_peek::read_latest returns false),
//     so the game sees the same result as a frame where the readback is pending;
//   * texture replacements (aurora/texture.hpp, Dusklight's texture packs and HQ
//     minimap) only change uploaded pixels, and there is nothing to upload to: the
//     registry registers nothing and says so in the log once.
// gfx_gxm (M3) replaces this file. Symbols outside the contract (the WebGPU C API,
// ImGui/RmlUi rendering, the mods gfx service) are left unresolved on purpose and
// become named trap stubs.
#include "gx/gx.hpp"
#include "gfx/clear.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/frame_packet.hpp"
#include "gfx/recording.hpp"
#include "gfx/resources.hpp"
#include "gfx/tex_copy_conv.hpp"
#include "gfx/tex_palette_conv.hpp"
#include "gfx/texture.hpp"
#include "gfx/texture_replacement.hpp"
#include "gx/pipeline.hpp"
#include "gx/shader_info.hpp"
#include "dolphin/vi/vi_internal.hpp"
#include "internal.hpp"
#include "io.hpp"
#include "window.hpp"
#include "gfx_null.hpp"

#include <aurora/texture.hpp>
#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace {
// Counters of the frame being recorded; published to resources().stats at end of
// frame, so aurora_get_stats() reports the last completed frame as upstream does.
AuroraStats sFrameStats{};
}

namespace aurora::gfx::detail {
Resources& resources() noexcept {
  static Resources resources;
  return resources;
}

void increment_merged_draw_count() noexcept { ++sFrameStats.mergedDrawCallCount; }
} // namespace aurora::gfx::detail

void twv::gfx_null::end_frame() noexcept {
  aurora::gfx::detail::resources().stats = sFrameStats;
  sFrameStats = {};
}

// --- GX view state (lib/gx/gx.cpp, unchanged logic) ---
namespace aurora::gx {
GXState g_gxState{};

namespace {
std::atomic<int> sPendingViewportPolicy{-1};
}

void set_viewport_policy(AuroraViewportPolicy policy) noexcept {
  sPendingViewportPolicy.store(policy, std::memory_order_release);
}

void update() noexcept {
  if (const int pending = sPendingViewportPolicy.exchange(-1, std::memory_order_acq_rel); pending != -1) {
    const auto policy = static_cast<AuroraViewportPolicy>(pending);
    g_gxState.viewportPolicy = policy;
    window::set_frame_buffer_aspect_fit(policy == AURORA_VIEWPORT_FIT);
  }
}

Vec2<uint32_t> logical_fb_size() noexcept {
  return gfx::is_offscreen() ? gfx::get_render_target_size() : vi::configured_fb_size();
}

gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalViewport;
  }
  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalViewport;
  }
  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  return {
      .left = logicalViewport.left * scaleX,
      .top = logicalViewport.top * scaleY,
      .width = logicalViewport.width * scaleX,
      .height = logicalViewport.height * scaleY,
      .znear = logicalViewport.znear,
      .zfar = logicalViewport.zfar,
  };
}

gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept {
  if (g_gxState.viewportPolicy == AURORA_VIEWPORT_NATIVE) {
    return logicalScissor;
  }
  const auto [logicalFbWidth, logicalFbHeight] = logical_fb_size();
  const auto [targetWidth, targetHeight] = gfx::get_render_target_size();
  if (logicalFbWidth == 0 || logicalFbHeight == 0 || targetWidth == 0 || targetHeight == 0) {
    return logicalScissor;
  }
  const float scaleX = static_cast<float>(targetWidth) / static_cast<float>(logicalFbWidth);
  const float scaleY = static_cast<float>(targetHeight) / static_cast<float>(logicalFbHeight);
  const float left = static_cast<float>(logicalScissor.x) * scaleX;
  const float top = static_cast<float>(logicalScissor.y) * scaleY;
  const float right = static_cast<float>(logicalScissor.x + logicalScissor.width) * scaleX;
  const float bottom = static_cast<float>(logicalScissor.y + logicalScissor.height) * scaleY;
  const auto mappedLeft = std::clamp(static_cast<int32_t>(std::floor(left)), 0, static_cast<int32_t>(targetWidth));
  const auto mappedTop = std::clamp(static_cast<int32_t>(std::floor(top)), 0, static_cast<int32_t>(targetHeight));
  const auto mappedRight =
      std::clamp(static_cast<int32_t>(std::ceil(right)), mappedLeft, static_cast<int32_t>(targetWidth));
  const auto mappedBottom =
      std::clamp(static_cast<int32_t>(std::ceil(bottom)), mappedTop, static_cast<int32_t>(targetHeight));
  return {
      .x = mappedLeft,
      .y = mappedTop,
      .width = mappedRight - mappedLeft,
      .height = mappedBottom - mappedTop,
  };
}

void set_logical_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport.left != g_gxState.logicalViewport.left || viewport.width != g_gxState.logicalViewport.width ||
      viewport.height != g_gxState.logicalViewport.height) {
    g_gxState.dirty |= DirtyUniform;
  }
  g_gxState.logicalViewport = viewport;
  set_render_viewport(map_logical_viewport(viewport));
}

void set_render_viewport(const gfx::Viewport& viewport) noexcept {
  if (viewport.left != g_gxState.renderViewport.left || viewport.width != g_gxState.renderViewport.width ||
      viewport.height != g_gxState.renderViewport.height) {
    g_gxState.dirty |= DirtyUniform;
  }
  g_gxState.renderViewport = viewport;
  gfx::set_viewport(viewport);
}

void set_logical_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.logicalScissor = scissor;
  set_render_scissor(map_logical_scissor(g_gxState.logicalScissor));
}

void set_render_scissor(const gfx::ClipRect& scissor) noexcept {
  g_gxState.renderScissor = scissor;
  gfx::set_scissor(scissor);
}

const gfx::TextureBind& get_texture(GXTexMapID id) noexcept { return g_gxState.textures[static_cast<size_t>(id)]; }

// Texture objects carry no GPU resources here; eviction only marks them, as the test seam does.
namespace texture {
void invalidate_bindings() noexcept {}
uint64_t current_bind_generation() noexcept { return 1; }
} // namespace texture
void evict_texture_object(u32 texObjId) noexcept {
  for (auto& obj : g_gxState.loadedTextures) {
    if (obj.texObjId == texObjId) {
      obj.set_no_cache(true);
    }
  }
}
void evict_tlut_object(u32 tlutObjId) noexcept {
  for (auto& obj : g_gxState.loadedTluts) {
    if (obj.tlutObjId == tlutObjId) {
      obj.set_no_cache(true);
    }
  }
}
void evict_copy_texture(const void* dest) noexcept {
  g_gxState.copyTextures.erase(dest);
  for (auto it = g_gxState.copyTextureCache.begin(); it != g_gxState.copyTextureCache.end();) {
    if (it->first.dest == dest) {
      g_gxState.copyTextureCache.erase(it++);
    } else {
      ++it;
    }
  }
}
void shutdown() noexcept {}

// No shader generation without a GPU: pipelines are counted, never built.
void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept {
  (void)config;
  (void)primitive;
  (void)fmt;
}
GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept {
  (void)info;
  return {};
}
ShaderInfo build_shader_info(const ShaderConfig& config) noexcept {
  (void)config;
  return {};
}
gfx::Range build_uniform(const ShaderInfo& info) noexcept {
  (void)info;
  return {.size = 1};
}
void resolve_sampled_textures(const ShaderInfo& info) noexcept { (void)info; }
} // namespace aurora::gx

// --- Buffers, render target, draws ---
namespace aurora::gfx {
namespace {
std::atomic<bool> sOffscreen{false};
std::atomic<uint32_t> sOffscreenWidth{0};
std::atomic<uint32_t> sOffscreenHeight{0};
}

Range push_verts(const uint8_t* data, size_t length, size_t alignment) {
  (void)data;
  (void)alignment;
  sFrameStats.lastVertSize += length;
  return {};
}
Range push_indices(const uint8_t* data, size_t length, size_t alignment) {
  (void)data;
  (void)alignment;
  sFrameStats.lastIndexSize += length;
  return {};
}
Range push_uniform(const uint8_t* data, size_t length) {
  (void)data;
  sFrameStats.lastUniformSize += length;
  return {};
}
Range push_storage(const uint8_t* data, size_t length) {
  (void)data;
  sFrameStats.lastStorageSize += length;
  return {};
}

Vec2<uint32_t> get_render_target_size() noexcept {
  if (sOffscreen.load(std::memory_order_acquire)) {
    return {sOffscreenWidth.load(std::memory_order_relaxed), sOffscreenHeight.load(std::memory_order_relaxed)};
  }
  return vi::configured_fb_size();
}
// lib/gfx/recording.hpp. (Aurora's test stub defines a stale 4-integer set_scissor;
// the contract at this pin takes a ClipRect, which set_render_scissor calls.)
void set_viewport(const Viewport& viewport) noexcept { (void)viewport; }
void set_scissor(const ClipRect& scissor) noexcept { (void)scissor; }
bool has_normal_attachment() noexcept { return false; }
RenderTargetLayout get_render_target_layout() noexcept {
  RenderTargetLayout layout{
      .colorAttachmentCount = 1,
      .colorAttachments = {{{ColorAttachmentSemantic::SceneColor, wgpu::TextureFormat::RGBA8Unorm}}},
      .depthStencilFormat = wgpu::TextureFormat::Depth24Plus,
      .sampleCount = 1,
  };
  detail::finalize_render_target_layout(layout);
  return layout;
}

template <>
PipelineRef pipeline_ref<clear::PipelineConfig>(const clear::PipelineConfig& config) {
  (void)config;
  return 0;
}
template <>
void push_draw_command<clear::DrawData>(clear::DrawData data) {
  (void)data;
}
template <>
PipelineRef pipeline_ref<gx::PipelineConfig>(const gx::PipelineConfig& config) {
  (void)config;
  return 0;
}
template <>
void push_draw_command<gx::DrawData>(gx::DrawData data) {
  (void)data;
  ++sFrameStats.drawCallCount;
}
template <>
gx::DrawData* get_last_draw_command() {
  return nullptr; // never merge: every draw is dropped on its own
}

wgpu::SamplerDescriptor TextureBind::get_descriptor() const noexcept { return wgpu::SamplerDescriptor{}; }

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                    ArrayRef<uint8_t> data, bool tlut, const char* label) noexcept {
  (void)width, (void)height, (void)mips, (void)gxFormat, (void)tlut, (void)label;
  sFrameStats.lastTextureUploadSize += data.size();
  return {};
}
TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char* label) noexcept {
  (void)width, (void)height, (void)mips, (void)gxFormat, (void)label;
  return {};
}
TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  (void)width, (void)height, (void)gxFormat, (void)label;
  return {};
}
TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char* label) noexcept {
  (void)width, (void)height, (void)gxFormat, (void)label;
  return {};
}
void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {
  (void)ref;
  sFrameStats.lastTextureUploadSize += data.size();
}
void queue_texture_upload(TextureUpload upload) { (void)upload; }
void queue_texture_upload_data(const uint8_t* data, size_t length, uint32_t bytesPerRow, uint32_t rowsPerImage,
                               wgpu::TexelCopyTextureInfo tex, wgpu::Extent3D size) {
  (void)data, (void)bytesPerRow, (void)rowsPerImage, (void)tex, (void)size;
  sFrameStats.lastTextureUploadSize += length;
}
void queue_palette_conv(tex_palette_conv::ConvRequest req) { (void)req; }

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                       GXPixelFmt sourceFormat) {
  (void)texture, (void)rect, (void)clearColor, (void)clearAlpha, (void)clearDepth, (void)clearColorValue,
      (void)clearDepthValue, (void)resolveFormat, (void)sourceFormat;
}
void begin_offscreen(uint32_t width, uint32_t height) {
  sOffscreenWidth.store(width, std::memory_order_relaxed);
  sOffscreenHeight.store(height, std::memory_order_relaxed);
  sOffscreen.store(true, std::memory_order_release);
}
void end_offscreen() { sOffscreen.store(false, std::memory_order_release); }
bool is_offscreen() noexcept { return sOffscreen.load(std::memory_order_acquire); }

void push_debug_group(std::string) {}
void insert_debug_marker(std::string) {}
} // namespace aurora::gfx

void push_debug_group(const char*) {}
void pop_debug_group() {}

// --- Depth peek, copy/palette conversion, texture replacements ---
namespace aurora::gfx::depth_peek {
void initialize() {}
void shutdown() {}
void request_snapshot() noexcept {}
void poll() noexcept {}
void encode_frame_snapshot(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& depthView,
                           wgpu::Extent3D sourceSize, uint32_t msaaSamples) noexcept {
  (void)cmd, (void)depthView, (void)sourceSize, (void)msaaSamples;
}
void after_submit() noexcept {}
bool read_latest(uint16_t x, uint16_t y, uint32_t& z) noexcept {
  (void)x, (void)y, (void)z;
  return false;
}
} // namespace aurora::gfx::depth_peek

namespace aurora::gfx::tex_copy_conv {
bool needs_conversion(GXTexFmt fmt) {
  (void)fmt;
  return false;
}
} // namespace aurora::gfx::tex_copy_conv

namespace aurora::gfx::tex_palette_conv {
void queue(ConvRequest req) { (void)req; }
} // namespace aurora::gfx::tex_palette_conv

namespace aurora::gfx::texture_replacement {
u32 compute_texture_upload_size(const GXTexObj_& obj) noexcept {
  (void)obj;
  return 0;
}
bool has_replacement(const GXTexObj_&) noexcept { return false; }
bool has_replacement(const GXTexObj_&, const GXTlutObj_&) noexcept { return false; }
} // namespace aurora::gfx::texture_replacement

// --- Texture replacement registry (include/aurora/texture.hpp) ---
namespace aurora::texture {
namespace {
constexpr Module Log{"aurora::gfx_null"};
void log_unavailable_once(std::string_view what) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    Log.info("Null gfx: texture replacements need a GPU backend; {} and later requests register nothing", what);
  }
}
} // namespace

ReplacementRegistration register_virtual_replacement(std::string_view path, VirtualFileSource source,
                                                     ReplacementOptions options) {
  (void)source, (void)options;
  log_unavailable_once(fmt::format("virtual replacement '{}'", path));
  return {};
}
ReplacementRegistration register_replacement(ReplacementKey key, RawTextureReplacement replacement,
                                             ReplacementOptions options) {
  (void)options;
  log_unavailable_once(fmt::format("replacement '{}'", replacement.label));
  return {.id = 0, .key = std::move(key)}; // id 0: nothing registered
}
void unregister_replacement(const ReplacementRegistration& registration) { (void)registration; }
void unregister_replacements(const ReplacementGroup& group) { (void)group; }
ReplacementGroup load_replacement_directory(const std::filesystem::path& root, ReplacementOptions options) {
  (void)options;
  log_unavailable_once(fmt::format("directory '{}'", io::fs_path_to_string(root)));
  return {};
}
bool has_replacement(const GXTexObj* obj, const GXTlutObj* tlut) {
  (void)obj, (void)tlut;
  return false;
}
} // namespace aurora::texture

// Frame statistics (lib/gfx/frame.cpp's C API): the last completed frame.
const AuroraStats* aurora_get_stats() { return &aurora::gfx::detail::resources().stats; }
