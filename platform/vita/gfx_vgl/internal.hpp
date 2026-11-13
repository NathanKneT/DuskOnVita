#pragma once
// Aurora/game domain: int enums. SDK access exclusively through gpu_api.h.
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gpu_api.h"
#include "vertex_contract.hpp"
#include "light_plan.hpp"
#include "counter_batch.hpp"
#include <string>
#include <vector>

namespace twv::vgl {
struct Pipeline {
    aurora::gx::PipelineConfig config{};
    aurora::gx::ShaderInfo info{};
    uint32_t programs[9]{};
    bool attempted[9]{};
    bool drawn = false;
    bool vertexTraced = false;
    bool gpuVertex = false;
    bool gpuTexgen = false;
    int texgenRows[8]{}, postRows[8]{};
    unsigned texgenVectors = 0;
    uint32_t sourceArrayMask = 0;
    bool usesCurrentPn = true;
    TwvVitaVertexLayout vertexLayout{};
    // G2 packed formats (GXM backend): set once, before the pipeline's first decode and shader.
    PackedFormats packed{};
    bool packedConfigured = false;
};
struct Texture : aurora::gfx::TextureRef {
    uint32_t gpu = 0;
    bool deferredUpload = false;
    // Destination of an EFB copy (or of its palette conversion). Nothing writes it while
    // resolve_pass_into is unsupported: its storage stays as created (zero on GXM).
    bool efbCopy = false;
    // CPU copy, for the map screens (map_pass.cpp): a palette's colours (RGBA, one per entry), or
    // a map picture's palette indices (one byte per pixel) inside its copy's texture. Empty otherwise.
    std::vector<uint8_t> cpu;
    Texture(uint32_t w,uint32_t h,uint32_t mips,uint32_t format,bool deferred=false);
    ~Texture();
};
void count(const char* name, uint32_t amount=1) noexcept;
void count(CounterId id,uint32_t amount=1) noexcept;
void count_game(CounterId id,uint32_t amount=1) noexcept; // game thread only, see backend.cpp
TwvVitaVertex* build_vertices(const Pipeline&,const aurora::gx::DrawData&) noexcept;
// vertex.cpp: where each vertex of a draw lands in a target whose viewport is (left, top, width,
// height): x and y in pixels, y down; and, when `st` is given, its first texture coordinate as
// sent. For draws that are drawn on the CPU (map_pass.cpp).
bool decode_screen_positions(const Pipeline&,const aurora::gx::DrawData&,float left,float top,float width,float height,float* xy,
                             float* st=nullptr) noexcept;
// map_pass.cpp: the map screens' off-screen pass, drawn on the CPU.
void map_pass_begin(uint32_t w,uint32_t h) noexcept;
bool map_pass_draw(const Pipeline&,const aurora::gx::DrawData&,float left,float top,float width,float height) noexcept;
bool map_pass_resolve(Texture& into,uint32_t format) noexcept;
bool map_palette_convert(Texture* source,Texture* target,Texture* palette,bool eightBit) noexcept;
bool gpu_vertex_supported(const Pipeline&) noexcept;
void* build_gpu_vertices(const Pipeline&,const aurora::gx::DrawData&,unsigned& pnSlots,bool& culled) noexcept;
TwvVitaVertexLayout gpu_vertex_layout(const Pipeline&) noexcept;
void geometry_cache_end_frame() noexcept;
void geometry_cache_clear() noexcept;
void geometry_cache_report() noexcept;
void configure_gpu_texgen(Pipeline&) noexcept;
void configure_gpu_cache_dependencies(Pipeline&) noexcept;
void configure_packed_formats(Pipeline&) noexcept;
const void* static_resident_indices(const uint8_t* data,size_t size) noexcept;
LightPlan gpu_light_plan(const Pipeline&) noexcept;
void gpu_vertex_uniforms(TwvVitaDrawState&,const Pipeline&,unsigned pnSlots,const LightPlan&) noexcept;
std::string gpu_vertex_shader(const Pipeline&,unsigned lightCount);
std::string vertex_shader(const Pipeline&,unsigned lightCount);
std::string fragment_shader(const Pipeline&);
}
