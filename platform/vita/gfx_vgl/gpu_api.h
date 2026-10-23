#pragma once
// Enum-ABI firewall: plain fixed-width values only. No SDK/game enum crosses.
#include <stdint.h>
#include <stddef.h>

struct TwvVitaVertex {
    float position[3];
    float color[2][4];
    float texcoord[8][3];
    float normal[3];
    float pn_index;
};
constexpr uint32_t TwvVertexAttributeCount = 13;
constexpr uint32_t TwvVertexAttributeAbsent = UINT32_MAX;
struct TwvVitaVertexLayout {
    uint32_t stride;
    uint32_t offsets[TwvVertexAttributeCount];
    uint32_t components[TwvVertexAttributeCount];
};
static_assert(sizeof(TwvVitaVertexLayout)==108);
struct TwvVitaDrawState {
    uint32_t program, textures[8];
    int depth_test, depth_write, depth_func, cull, blend, src_factor, dst_factor;
    int color_write, alpha_write, subtract;
    int viewport[4], scissor[4];
    float projection[16], color_regs[16], kcolors[16], fog[8];
    uint32_t gpu_vertex;
    // Prefix of the unchanged ten-slot shader palette used by this packet.
    uint32_t pn_vectors;
    uint32_t light_count;
    TwvVitaVertexLayout vertex_layout;
    // Vertex stage uses at most120vec4 uniforms (below the pinned128 limit).
    float pn_pos[30*4], pn_nrm[30*4];
    float light_pos[8*4], light_dir[8*4], light_color[8*4];
    float light_cos[8*4], light_dist[8*4];
    float channel_mat[4*4], channel_amb[4*4], light_masks[8*4];
    uint32_t texgen_vectors;
    float texgen_rows[48*4];
    // G2 (GXM backend features; zero keeps the G1 meaning: triangles from the index list,
    // or listed when there is none, and F32 attributes). The draw digest hashes the
    // canonical G1 form, so these fields never change a digest by themselves.
    uint32_t primitive;          // TwvPrimitive*
    uint8_t vertex_formats[13];  // TwvFormat* per attribute (gpu_api attribute order)
    uint8_t vertex_counts[13];   // stored components of a packed attribute
    int8_t vertex_fracs[13];     // integer formats: value = raw * 2^-frac
    uint8_t g2_reserved;
};
constexpr uint32_t TwvPrimitiveTriangles=0, TwvPrimitiveStrip=1, TwvPrimitiveFan=2, TwvPrimitiveQuads=3;
constexpr uint8_t TwvFormatF32=0, TwvFormatS16=1, TwvFormatU16=2, TwvFormatS8=3, TwvFormatU8=4,
    TwvFormatU8N=5; // U8N: colour bytes normalised by the GPU (c/255)
static_assert(offsetof(TwvVitaDrawState,primitive)%4==0 && sizeof(TwvVitaDrawState)%4==0);
static_assert(sizeof(TwvVitaVertex)==156 && offsetof(TwvVitaVertex,texcoord)==44 &&
              offsetof(TwvVitaVertex,normal)==140 && offsetof(TwvVitaVertex,pn_index)==152,
              "Game/SDK vertex layout must agree across enum ABI domains");
extern "C" {
int twv_vgl_init(void);
void twv_vgl_shutdown(void);
void twv_vgl_begin(void);
void twv_vgl_end(void);
void* twv_vgl_alloc(size_t size);
uint32_t twv_vgl_geometry_cache_budget(uint32_t current_payload);
uint32_t twv_vgl_vertex_is_vram(const void* address);
// SDK classification only;0 unknown,1 legacyVRAM,2 exact verified uncached RAM,
// 3 other. Only use on existing GPU scratch outputs; never proof of GPU mapping.
uint32_t twv_vgl_vertex_write_class(const void* address,size_t bytes);
// Optional low-rate cumulative telemetry,4 events/call; does not force a flush.
void twv_vgl_vertex_write_class_report(void);
// Main-thread wall-time categories, integer ABI only. Values are microseconds;
// GL submission time includes CPU driver work, not isolated GPU execution time.
constexpr uint32_t TWV_PROFILE_VERTEX = 0, TWV_PROFILE_SUBMIT = 1,
    TWV_PROFILE_SHADER = 2, TWV_PROFILE_STATE = 3, TWV_PROFILE_PIPELINE = 4,
    TWV_PROFILE_PRESENT = 5, TWV_PROFILE_FIFO = 6,
    TWV_PROFILE_GAME_EXECUTE = 7, TWV_PROFILE_GAME_AUDIO = 8,
    TWV_PROFILE_VI_CALLBACKS = 9, TWV_PROFILE_PAINTER = 10,
    TWV_PROFILE_EVENTS = 11, TWV_PROFILE_COUNT = 12;
uint64_t twv_vgl_time_us(void);
void twv_vgl_profile_add(uint32_t category, uint64_t microseconds);
uint32_t twv_vgl_program(const char* vertex, const char* fragment);
void twv_vgl_program_delete(uint32_t program);
void twv_vgl_draw(const TwvVitaDrawState* state, const void* vertices,
                  uint32_t vertex_count, const uint16_t* indices, uint32_t index_count);
uint32_t twv_vgl_texture(uint32_t width, uint32_t height, uint32_t levels);
void twv_vgl_texture_write(uint32_t texture, uint32_t level, uint32_t width, uint32_t height, const void* rgba);
void twv_vgl_texture_delete(uint32_t texture);
// Sets object parameters; does not promise to leave that object bound. Draws
// explicitly bind every sampled texture unit after all parameter updates.
void twv_vgl_texture_sampler(uint32_t texture, int min_filter, int mag_filter, int wrap_s, int wrap_t);
// Current-draw request for logical GX unit0..7, independently of texture IDs.
// Requires initialized persistent sampler objects. Returns0 on invalid unit,
// initialization or GL error; caller must stop before required draw submission.
// Draw consumes requests even when rejected; absent units use legacy sampler0.
int twv_vgl_texture_sampler_unit(uint32_t unit,int min_filter,int mag_filter,int wrap_s,int wrap_t);
void twv_vgl_clear(float r, float g, float b, float a, float depth, unsigned mask,
                   int x, int y, int width, int height);
// G2: backend features chosen at start-up (gpu_dispatch.cpp). Only the GXM backend offers
// them; with vitaGL every bit reads 0 and the front end keeps its G1 behaviour.
constexpr uint32_t TwvGpuFeaturePrimitives=1u;     // TwvVitaDrawState.primitive may be non-zero
constexpr uint32_t TwvGpuFeatureCompact=2u;        // packed integer attributes (positions, normals, UVs)
constexpr uint32_t TwvGpuFeatureCompactColors=4u;  // RGBA8 colours as U8N
constexpr uint32_t TwvGpuFeatureResident=8u;      // certified geometry and static-list indices in GPU memory
constexpr uint32_t TwvGpuFeatureTiming=16u;       // diagnostic: GPU and present timing receipts
constexpr uint32_t TwvGpuFeatureSplit=32u;        // diagnostic: frames split into twv_gpu_split_scenes() scenes
constexpr uint32_t TwvGpuFeatureScale75=64u,TwvGpuFeatureScale67=128u,TwvGpuFeatureScale50=256u; // diagnostic render size
uint32_t twv_gpu_features(void);
uint32_t twv_gpu_split_scenes(void);              // 0 without the split
// GPU memory for cached geometry that the backend retires only after the GPU is done with it
// (vitaGL: its resident arena; GXM: a CDRAM arena retired by frame fence).
int twv_gpu_resident_available(void);
void* twv_gpu_resident_alloc(size_t bytes);
void twv_gpu_resident_release(void* ptr);
}
