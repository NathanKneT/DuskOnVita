#pragma once
// Aurora/game domain: int enums. SDK access exclusively through gpu_api.h.
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "gpu_api.h"
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
};
struct Texture : aurora::gfx::TextureRef {
    uint32_t gpu = 0;
    bool deferredUpload = false;
    Texture(uint32_t w,uint32_t h,uint32_t mips,uint32_t format,bool deferred=false);
    ~Texture();
};
void count(const char* name, uint32_t amount=1) noexcept;
void count(CounterId id,uint32_t amount=1) noexcept;
void count_game(CounterId id,uint32_t amount=1) noexcept; // game thread only, see backend.cpp
TwvVitaVertex* build_vertices(const Pipeline&,const aurora::gx::DrawData&) noexcept;
bool gpu_vertex_supported(const Pipeline&) noexcept;
void* build_gpu_vertices(const Pipeline&,const aurora::gx::DrawData&,unsigned& pnSlots,bool& culled) noexcept;
TwvVitaVertexLayout gpu_vertex_layout(const Pipeline&) noexcept;
void geometry_cache_end_frame() noexcept;
void geometry_cache_clear() noexcept;
void geometry_cache_report() noexcept;
void configure_gpu_texgen(Pipeline&) noexcept;
void configure_gpu_cache_dependencies(Pipeline&) noexcept;
LightPlan gpu_light_plan(const Pipeline&) noexcept;
void gpu_vertex_uniforms(TwvVitaDrawState&,const Pipeline&,unsigned pnSlots,const LightPlan&) noexcept;
std::string gpu_vertex_shader(const Pipeline&,unsigned lightCount);
std::string vertex_shader(const Pipeline&,unsigned lightCount);
std::string fragment_shader(const Pipeline&);
}
