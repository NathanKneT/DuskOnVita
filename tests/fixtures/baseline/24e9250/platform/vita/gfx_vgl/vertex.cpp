// CPU GX vertex stage adapted from the SDK-free 3DS implementation.
// Source snapshot SHA256 c094ce309d4f14ec082b6ec44aab78ad8844516f3cdce8c1cbbaf906631e432e
// Vita preserves two float raster colors and projective STQ until fragment sampling.
#include "internal.hpp"
#include "vertex_contract.hpp"
#include "bundle_options.h"
#include "resident_geometry_api.h"
#include "geometry_cache.hpp"
#include "vertex_staging.hpp"
#include "fixed_scale.hpp"
#include "color_lut.hpp"
#include "vertex_work_probe.hpp"
#include "attribute_stream.hpp"
#include "vertex_decoder_control.hpp"
#include "small_packet_policy.hpp"
#include "staging_policy.hpp"
#include "gfx/hash.hpp"
#include "runtime/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#ifndef TWV_VITA_PLANNED_FETCH
#define TWV_VITA_PLANNED_FETCH 0
#endif
#ifndef TWV_VITA_STREAM_DECODE
#define TWV_VITA_STREAM_DECODE 0
#endif
#ifndef TWV_VITA_VERTEX_CULL
#define TWV_VITA_VERTEX_CULL 0
#endif
#ifndef TWV_VITA_SPARSE_SPANS
#define TWV_VITA_SPARSE_SPANS 0
#endif
#ifndef TWV_VITA_SMALL_PACKET_CACHE
#define TWV_VITA_SMALL_PACKET_CACHE 0
#endif
#ifndef TWV_VITA_UNCACHED_STAGING
#define TWV_VITA_UNCACHED_STAGING 0
#endif

namespace twv::vgl {
namespace {
using aurora::gx::AttrConfig;
using aurora::gx::g_gxState;
GeometryCache geometryCache;
bool geometryCacheEnabled=true;
bool residentAllocatorConfigured=false;
uint64_t residentReplayPackets=0,residentReplayBytes=0;
uint64_t cacheValidationSerial=0;
VertexStaging vertexStaging;
VertexWorkProbe vertexWorkProbe;
VertexDecoderControl streamDecodeControl{bool(TWV_VITA_STREAM_DECODE)};
VertexDecoderControl neonCompareControl{NeonCompareAvailable};
VertexDecoderControl vertexCullControl{bool(TWV_VITA_VERTEX_CULL)};
VertexDecoderControl sparseSpansControl{bool(TWV_VITA_SPARSE_SPANS)};
VertexDecoderControl smallPacketControl{bool(TWV_VITA_SMALL_PACKET_CACHE)};
VertexDecoderControl uncachedStagingControl{bool(TWV_VITA_UNCACHED_STAGING)};
SmallPacketCache smallPacketCache;
uint64_t smallValidationSerial=0,smallChecks=0,smallFailures=0,smallEligible=0,smallReplayBytes=0;
uint64_t uncachedStagedBytes=0,vramStagedBytes=0;
uint64_t sparseAdmitted=0,sparseSelected=0,sparseBroadBytes=0,sparseSelectedBytes=0;
uint64_t cullEligible=0,cullRejected=0,cullVertices=0,cullBytes=0,cullChecks=0,cullFailures=0;
uint64_t streamPackets=0,streamVertices=0,streamValidationSerial=0;
uint64_t streamValidationPass=0,streamValidationFailure=0;
uint8_t* stage_vertex_output(void* mapped,size_t bytes) {
#if TWV_VITA_CPU_VERTEX_STAGING
  const bool headroom=geometryCache.budget()!=0;
  auto target=VertexWriteClass::Unknown;
  if(headroom && bytes>=VertexStaging::MinBytes && bytes<=VertexStaging::MaxBytes) {
    // Keep the previous VRAM-only path when the new independent switch is OFF.
    target=uncachedStagingControl.enabled()?static_cast<VertexWriteClass>(twv_vgl_vertex_write_class(mapped,bytes)):
        (twv_vgl_vertex_is_vram(mapped)?VertexWriteClass::Vram:VertexWriteClass::Unknown);
  }
  if(should_stage_vertex_output(bytes,headroom,target,uncachedStagingControl.enabled())) {
    if(auto* cpu=vertexStaging.acquire(bytes)) {
      count(CounterId::VertexCpuStagedBytes,uint32_t(bytes));
      (target==VertexWriteClass::Vram?vramStagedBytes:uncachedStagedBytes)+=bytes;return cpu;
    }
    count("vertex_cpu_staging_fallback");
  }
#endif
  count(CounterId::VertexDirectDecodeBytes,uint32_t(bytes));
  return static_cast<uint8_t*>(mapped);
}

struct V3 {
  float x = 0.f, y = 0.f, z = 0.f;
};
struct V4 {
  float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
};

inline float dot3(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 normalize(const V3& v) {
  const float len2 = dot3(v, v);
  if (len2 <= 1e-10f) return v;
  const float inv = 1.f / std::sqrt(len2);
  return {v.x * inv, v.y * inv, v.z * inv};
}
// Row-major 3x4 (XF order): result_i = dot(row_i, v).
inline V3 mul34(const aurora::Mat3x4<float>& m, float x, float y, float z, float w) {
  return {m.m0[0] * x + m.m0[1] * y + m.m0[2] * z + m.m0[3] * w, m.m1[0] * x + m.m1[1] * y + m.m1[2] * z + m.m1[3] * w,
          m.m2[0] * x + m.m2[1] * y + m.m2[2] * z + m.m2[3] * w};
}

inline uint16_t rd16(const uint8_t* p, bool le) {
  return le ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t rd24(const uint8_t* p, bool le) {
  return le ? static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16))
            : static_cast<uint32_t>((p[0] << 16) | (p[1] << 8) | p[2]);
}
inline uint32_t rd32(const uint8_t* p, bool le) {
  return le ? static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24))
            : static_cast<uint32_t>((static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

// Components of a non-colour attribute (fetch_u8/s8/u16/s16/f32), up to 3.
inline void fetch(const AttrConfig& a, const uint8_t* p, bool le, int count, float* out) {
#if TWV_VITA_ATTR_DISPATCH
  if(count<=0)return;
  // One source format per attribute, not per component. F32 is a bit copy:
  // avoid even constructing an unused fractional scale on this path.
  if(a.compType!=GX_U8 && a.compType!=GX_S8 && a.compType!=GX_U16 && a.compType!=GX_S16) {
    if(le)for(int i=0;i<count;++i)std::memcpy(out+i,p+i*4,4);
    else for(int i=0;i<count;++i) {
      const uint32_t bits=rd32(p+i*4,false);std::memcpy(out+i,&bits,4);
    }
    return;
  }
#endif
#if TWV_VITA_FAST_FIXED_SCALE
  const float scale = fixed_point_scale(a.frac);
#else
  const float scale = 1.f / static_cast<float>(1u << a.frac);
#endif
#if TWV_VITA_ATTR_DISPATCH
  switch(a.compType) {
  case GX_U8:
    for(int i=0;i<count;++i)out[i]=static_cast<float>(p[i])*scale;
    break;
  case GX_S8:
    for(int i=0;i<count;++i)out[i]=static_cast<float>(static_cast<int8_t>(p[i]))*scale;
    break;
  case GX_U16:
    for(int i=0;i<count;++i)out[i]=static_cast<float>(rd16(p+i*2,le))*scale;
    break;
  case GX_S16:
    for(int i=0;i<count;++i)out[i]=static_cast<float>(static_cast<int16_t>(rd16(p+i*2,le)))*scale;
    break;
  }
#else
  for (int i = 0; i < count; ++i) {
    switch (a.compType) {
    case GX_U8:
      out[i] = static_cast<float>(p[i]) * scale;
      break;
    case GX_S8:
      out[i] = static_cast<float>(static_cast<int8_t>(p[i])) * scale;
      break;
    case GX_U16:
      out[i] = static_cast<float>(rd16(p + i * 2, le)) * scale;
      break;
    case GX_S16:
      out[i] = static_cast<float>(static_cast<int16_t>(rd16(p + i * 2, le))) * scale;
      break;
    default: {  // GX_F32
      const uint32_t bits = rd32(p + i * 4, le);
      std::memcpy(&out[i], &bits, 4);
      break;
    }
    }
  }
#endif
}

// Select scalar type, component count, byte order and scale once per draw.
// The old fetch() remains independent for rollback and cache-output oracles.
struct PlannedFetch {
  using Function = void (*)(const uint8_t*,float,float*);
  Function function=nullptr;
  float scale=1.f;
  void read(const uint8_t* p,float* out) const {function(p,scale,out);}
};
template<int Type,int N,bool Little> void fetch_specialized(const uint8_t* p,[[maybe_unused]] float scale,float* out) {
  for(int i=0;i<N;++i) {
    if constexpr(Type==GX_U8)out[i]=static_cast<float>(p[i])*scale;
    else if constexpr(Type==GX_S8)out[i]=static_cast<float>(static_cast<int8_t>(p[i]))*scale;
    else if constexpr(Type==GX_U16)out[i]=static_cast<float>(rd16(p+i*2,Little))*scale;
    else if constexpr(Type==GX_S16)out[i]=static_cast<float>(static_cast<int16_t>(rd16(p+i*2,Little)))*scale;
    else if constexpr(Little)std::memcpy(out+i,p+i*4,4);
    else {const uint32_t bits=rd32(p+i*4,false);std::memcpy(out+i,&bits,4);}
  }
}
template<int Type,bool Little> inline PlannedFetch::Function select_fetch_count(int n) {
  if(n==1)return fetch_specialized<Type,1,Little>;
  if(n==2)return fetch_specialized<Type,2,Little>;
  return fetch_specialized<Type,3,Little>;
}
inline PlannedFetch plan_fetch(const AttrConfig& a,bool le,int n) {
  if(n<1 || n>3)return {};
  PlannedFetch result;
  if(a.compType==GX_U8 || a.compType==GX_S8 || a.compType==GX_U16 || a.compType==GX_S16) {
#if TWV_VITA_FAST_FIXED_SCALE
    result.scale=fixed_point_scale(a.frac);
#else
    result.scale=1.f/static_cast<float>(1u<<a.frac);
#endif
  }
  switch(a.compType) {
  case GX_U8:result.function=le?select_fetch_count<GX_U8,true>(n):select_fetch_count<GX_U8,false>(n);break;
  case GX_S8:result.function=le?select_fetch_count<GX_S8,true>(n):select_fetch_count<GX_S8,false>(n);break;
  case GX_U16:result.function=le?select_fetch_count<GX_U16,true>(n):select_fetch_count<GX_U16,false>(n);break;
  case GX_S16:result.function=le?select_fetch_count<GX_S16,true>(n):select_fetch_count<GX_S16,false>(n);break;
  default:result.function=le?select_fetch_count<GX_F32,true>(n):select_fetch_count<GX_F32,false>(n);break;
  }
  return result;
}

inline V4 fetch_color(const AttrConfig& a, const uint8_t* p, bool le) {
  switch (a.compType) {
  case GX_RGB565: {
    const uint16_t v = rd16(p, le);
    return {normalized_color<5>((v >> 11) & 0x1F), normalized_color<6>((v >> 5) & 0x3F), normalized_color<5>(v & 0x1F), 1.f};
  }
  case GX_RGB8:
  case GX_RGBX8:
    return {normalized_color<8>(p[0]), normalized_color<8>(p[1]), normalized_color<8>(p[2]), 1.f};
  case GX_RGBA4: {
    const uint16_t v = rd16(p, le);
    return {normalized_color<4>((v >> 12) & 0xF), normalized_color<4>((v >> 8) & 0xF), normalized_color<4>((v >> 4) & 0xF), normalized_color<4>(v & 0xF)};
  }
  case GX_RGBA6: {
    const uint32_t v = rd24(p, le);
    return {normalized_color<6>((v >> 18) & 0x3F), normalized_color<6>((v >> 12) & 0x3F), normalized_color<6>((v >> 6) & 0x3F), normalized_color<6>(v & 0x3F)};
  }
  default:  // GX_RGBA8
    return {normalized_color<8>(p[0]), normalized_color<8>(p[1]), normalized_color<8>(p[2]), normalized_color<8>(p[3])};
  }
}

struct Decoded {
  uint32_t pnmtx = 0;
  V3 pos;
  V3 nrm{1.f, 0.f, 0.f};
  V3 binrm, tangent;
  V4 clr[2];
  float tex[8][2]{};
  uint8_t texmtx[8]{};
};

// What the draw needs from each vertex.
struct Needs {
  bool normal = false, nbt = false, color[2] = {false, false};
  uint8_t texMask = 0;  // GX_VA_TEX0.. used by texgens
};

class VertexStage {
public:
  VertexStage(const Pipeline& p, const aurora::gx::DrawData& d)
  : cfg(p.config.shaderConfig), pipe(p), draw(d) {
    vtxBase = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(d.immediateData.vtxStart));
    for (int i = 0; i < 12; ++i) {
      arrays[i] = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(d.immediateData.arrayStart[i]));
    }
    plan();
  }

  void decode_reference(uint32_t v, Decoded& out) const {
    const uint8_t* base = vtxBase + static_cast<size_t>(v) * cfg.vtxStride;
    out.pnmtx = draw.immediateData.currentPnMtx;
    for (int field = 0; field < activeCount; ++field) {
      const int attr = active[field];
      const AttrConfig& a = cfg.attrs[attr];
      bool le = false;
      const uint8_t* p = address(a, attr, base, 0, 0, le);
      if (attr == GX_VA_PNMTXIDX) {
        out.pnmtx = *p / 3u;
      } else if (attr <= GX_VA_TEX7MTXIDX) {
        out.texmtx[attr - GX_VA_TEX0MTXIDX] = *p;
      } else if (attr == GX_VA_POS) {
        float c[3] = {0.f, 0.f, 0.f};
        fetch(a, p, le, a.cnt == 2 ? 2 : 3, c);
        out.pos = {c[0], c[1], c[2]};
      } else if (attr == GX_VA_NRM) {
        float c[3];
        fetch(a, p, le, 3, c);
        out.nrm = {c[0], c[1], c[2]};
        if (needs.nbt && a.cnt == 9) {
          const uint32_t compSize = a.compType == GX_F32 ? 4 : (a.compType == GX_S16 || a.compType == GX_U16) ? 2 : 1;
          for (uint32_t slice = 1; slice <= 2; ++slice) {
            uint32_t dlExtra = 0;
            if (a.nbt3) dlExtra = a.attrType == GX_INDEX8 ? slice : a.attrType == GX_INDEX16 ? slice * 2 : 0;
            bool sle = false;
            const uint8_t* sp = address(a, attr, base, dlExtra, slice * 3 * compSize, sle);
            fetch(a, sp, sle, 3, c);
            (slice == 1 ? out.binrm : out.tangent) = {c[0], c[1], c[2]};
          }
        }
      } else if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        out.clr[attr - GX_VA_CLR0] = fetch_color(a, p, le);
      } else {
        float c[2] = {0.f, 0.f};
        fetch(a, p, le, a.cnt == 1 ? 1 : 2, c);
        out.tex[attr - GX_VA_TEX0][0] = c[0];
        out.tex[attr - GX_VA_TEX0][1] = c[1];
      }
    }
  }

  void decode(uint32_t v,Decoded& out) const {
#if !TWV_VITA_PLANNED_FETCH
    decode_reference(v,out);
#else
    const uint8_t* base=vtxBase+static_cast<size_t>(v)*cfg.vtxStride;
    out.pnmtx=draw.immediateData.currentPnMtx;
    for(int field=0;field<activeCount;++field) {
      const int attr=active[field];const auto& a=cfg.attrs[attr];
      bool le=false;const uint8_t* p=address(a,attr,base,0,0,le);
      if(attr==GX_VA_PNMTXIDX)out.pnmtx=*p/3u;
      else if(attr<=GX_VA_TEX7MTXIDX)out.texmtx[attr-GX_VA_TEX0MTXIDX]=*p;
      else if(attr==GX_VA_POS) {
        float c[3]={0.f,0.f,0.f};fetchPlans[field].read(p,c);out.pos={c[0],c[1],c[2]};
      } else if(attr==GX_VA_NRM) {
        float c[3];fetchPlans[field].read(p,c);out.nrm={c[0],c[1],c[2]};
        if(needs.nbt && a.cnt==9) {
          const uint32_t size=a.compType==GX_F32?4:(a.compType==GX_S16 || a.compType==GX_U16)?2:1;
          for(uint32_t slice=1;slice<=2;++slice) {
            const uint32_t extra=a.nbt3?(a.attrType==GX_INDEX8?slice:a.attrType==GX_INDEX16?slice*2:0):0;
            bool sle=false;const auto* sp=address(a,attr,base,extra,slice*3*size,sle);
            fetchPlans[field].read(sp,c);(slice==1?out.binrm:out.tangent)={c[0],c[1],c[2]};
          }
        }
      } else if(attr==GX_VA_CLR0 || attr==GX_VA_CLR1)out.clr[attr-GX_VA_CLR0]=fetch_color(a,p,le);
      else {
        float c[2]={0.f,0.f};fetchPlans[field].read(p,c);
        out.tex[attr-GX_VA_TEX0][0]=c[0];out.tex[attr-GX_VA_TEX0][1]=c[1];
      }
    }
#endif
  }

  bool can_pack_raw_streams(const TwvVitaVertexLayout& layout) const {
    const auto pn=cfg.attrs[GX_VA_PNMTXIDX].attrType;
    if(needs.nbt || (pn!=GX_NONE && pn!=GX_DIRECT) || !layout.stride || layout.stride>sizeof(TwvVitaVertex))return false;
    unsigned bytes=0;
    for(unsigned a=0;a<TwvVertexAttributeCount;++a)if(layout.offsets[a]!=TwvVertexAttributeAbsent) {
      const unsigned components=layout.components[a];
      if(components!=AttributeComponents[a] && !(a>=3 && a<=10 && components==2))return false;
      const unsigned n=components*4;
      if(layout.offsets[a]>layout.stride || n>layout.stride-layout.offsets[a])return false;
      bytes+=n;
    }
    if(bytes!=layout.stride)return false; // Only completely packed, no padding to compare.
    for(unsigned tc=0;tc<8;++tc)if(layout.offsets[3+tc]!=TwvVertexAttributeAbsent) {
      const auto src=cfg.tcgs[tc].src;
      if(src<GX_TG_TEX0 || src>GX_TG_TEX7)return false;
    }
    return true;
  }

  bool pack_raw_streams(const TwvVitaVertexLayout& layout,uint8_t* output,unsigned& pnSlots) const {
    pnSlots=1;if(!draw.vtxCount)return true;
    const auto pnOffset=layout.offsets[12];const auto& pn=cfg.attrs[GX_VA_PNMTXIDX];
    if(pn.attrType==GX_DIRECT) {
      const auto* p=vtxBase+pn.offset;
      for(uint32_t v=0;v<draw.vtxCount;++v,p+=cfg.vtxStride) {
        const unsigned index=*p/3u;if(index>=aurora::gx::MaxPnMtx)return false;
        pnSlots=std::max(pnSlots,index+1);const float value=float(index);
        if(pnOffset!=TwvVertexAttributeAbsent)std::memcpy(output+size_t(v)*layout.stride+pnOffset,&value,4);
      }
    } else {
      const auto index=draw.immediateData.currentPnMtx;
      if(index>=aurora::gx::MaxPnMtx)return false;
      pnSlots=index+1;const float value=float(index);
      if(pnOffset!=TwvVertexAttributeAbsent)default_attribute_stream(output+pnOffset,layout.stride,draw.vtxCount,&value,4);
    }
    const float zero[4]={},normal[3]={1.f,0.f,0.f},tex[3]={0.f,0.f,1.f};
    const auto numeric=[&](unsigned attr,unsigned slot,unsigned components,const float* fallback,float third=0.f) {
      const auto offset=layout.offsets[slot];if(offset==TwvVertexAttributeAbsent)return;
      const auto& a=cfg.attrs[attr];
      if(layout.components[slot]==2) {
        if(!wanted[attr] || a.attrType==GX_NONE)default_attribute_stream(output+offset,layout.stride,draw.vtxCount,fallback,8);
        else pack_numeric_attribute_st(AttributeStream<AttrConfig>{a,vtxBase,arrays[attr-GX_VA_POS],cfg.vtxStride},
            output+offset,layout.stride,draw.vtxCount,components);
      }
      else if(!wanted[attr] || a.attrType==GX_NONE)default_attribute_stream(output+offset,layout.stride,draw.vtxCount,fallback,12);
      else pack_numeric_attribute(AttributeStream<AttrConfig>{a,vtxBase,arrays[attr-GX_VA_POS],cfg.vtxStride},
          output+offset,layout.stride,draw.vtxCount,components,third);
    };
    numeric(GX_VA_POS,0,cfg.attrs[GX_VA_POS].cnt==2?2:3,zero);
    numeric(GX_VA_NRM,11,3,normal);
    for(unsigned ch=0;ch<2;++ch) {
      const auto offset=layout.offsets[1+ch];if(offset==TwvVertexAttributeAbsent)continue;
      const auto attr=GX_VA_CLR0+ch;const auto& a=cfg.attrs[attr];
      if(!wanted[attr] || a.attrType==GX_NONE)default_attribute_stream(output+offset,layout.stride,draw.vtxCount,zero,16);
      else pack_color_attribute(AttributeStream<AttrConfig>{a,vtxBase,arrays[attr-GX_VA_POS],cfg.vtxStride},
          output+offset,layout.stride,draw.vtxCount);
    }
    for(unsigned tc=0;tc<8;++tc)if(layout.offsets[3+tc]!=TwvVertexAttributeAbsent) {
      const unsigned attr=GX_VA_TEX0+(cfg.tcgs[tc].src-GX_TG_TEX0);
      numeric(attr,3+tc,cfg.attrs[attr].cnt==1?1:2,tex,1.f);
    }
    return true;
  }

  // Independent old vertex-major decoder/packing, bounded156B stack scratch.
  // No cached source values or additional heap/GXM allocation is needed.
  bool validate_raw_streams(const TwvVitaVertexLayout& layout,const uint8_t* packed,unsigned pnSlots) const {
    std::array<uint8_t,sizeof(TwvVitaVertex)> expected{};Decoded d{};unsigned slots=1;
    for(uint32_t v=0;v<draw.vtxCount;++v) {
      decode_reference(v,d);if(d.pnmtx>=aurora::gx::MaxPnMtx)return false;
      slots=std::max(slots,unsigned(d.pnmtx)+1);
      store_vertex_attribute(expected.data(),layout,0,&d.pos);
      store_vertex_attribute(expected.data(),layout,11,&d.nrm);
      const float index=float(d.pnmtx);store_vertex_attribute(expected.data(),layout,12,&index);
      for(unsigned ch=0;ch<2;++ch)store_vertex_attribute(expected.data(),layout,1+ch,&d.clr[ch]);
      for(unsigned tc=0;tc<8;++tc)if(layout.offsets[3+tc]!=TwvVertexAttributeAbsent) {
        const auto src=cfg.tcgs[tc].src-GX_TG_TEX0;
        const V3 t{d.tex[src][0],d.tex[src][1],1.f};store_vertex_attribute(expected.data(),layout,3+tc,&t);
      }
      if(std::memcmp(expected.data(),packed+size_t(v)*layout.stride,layout.stride))return false;
    }
    return slots==pnSlots;
  }

  // GX lighting_func for one channel config; alpha selects the ALPHA0/1 state.
  V4 light(const Decoded& d, const V3& mvPos, const V3& mvNrm, int ch, bool alpha) const {
    const auto& cc = cfg.colorChannels[alpha ? ch + 2 : ch];
    const auto& state = g_gxState.colorChannelState[alpha ? ch + 2 : ch];
    const V4 vtx = d.clr[ch];
    const auto v4 = [](const aurora::Vec4<float>& v) { return V4{v[0], v[1], v[2], v[3]}; };
    const V4 mat = cc.matSrc == GX_SRC_VTX ? vtx : v4(state.matColor);
    if (!cc.lightingEnabled) return mat;
    V4 acc = cc.ambSrc == GX_SRC_VTX ? vtx : v4(state.ambColor);
    const unsigned long mask = state.lightMask.to_ulong();
    for (int i = 0; i < 8; ++i) {
      if ((mask & (1u << i)) == 0) continue;
      const auto& light = g_gxState.lights[i];
      V3 ldir{light.pos[0] - mvPos.x, light.pos[1] - mvPos.y, light.pos[2] - mvPos.z};
      const float dist2 = dot3(ldir, ldir);
      const float dist = std::sqrt(dist2);
      if (dist > 0.f) ldir = {ldir.x / dist, ldir.y / dist, ldir.z / dist};
      const V3 ldirection{light.dir[0], light.dir[1], light.dir[2]};
      float attn = 1.f;
      if (cc.attnFn == GX_AF_SPOT) {
        const float cosine = std::max(0.f, dot3(ldir, ldirection));
        const float cosAttn = light.cosAtt[0] + light.cosAtt[1] * cosine + light.cosAtt[2] * cosine * cosine;
        const float distAttn = light.distAtt[0] + light.distAtt[1] * dist + light.distAtt[2] * dist2;
        attn = std::max(0.f, cosAttn / distAttn);
      } else if (cc.attnFn == GX_AF_SPEC) {
        attn = dot3(mvNrm, ldir) >= 0.f ? std::max(0.f, dot3(mvNrm, ldirection)) : 0.f;
        const float cosAttn = light.cosAtt[0] + light.cosAtt[1] * attn + light.cosAtt[2] * attn * attn;
        V3 k{light.distAtt[0], light.distAtt[1], light.distAtt[2]};
        if (cc.diffFn != GX_DF_NONE) k = normalize(k);
        const float distAttn = std::max(0.f, k.x + k.y * attn + k.z * attn * attn);
        attn = std::max(0.f, cosAttn / distAttn);
      }
      float diff = 1.f;
      if (cc.diffFn == GX_DF_SIGN) {
        diff = dot3(ldir, mvNrm);
      } else if (cc.diffFn == GX_DF_CLAMP) {
        diff = std::max(0.f, dot3(ldir, mvNrm));
      }
      const float f = attn * diff;
      acc.x += f * light.color[0];
      acc.y += f * light.color[1];
      acc.z += f * light.color[2];
      acc.w += f * light.color[3];
    }
    const auto sat = [](float v) { return std::clamp(v, 0.f, 1.f); };
    return {mat.x * sat(acc.x), mat.y * sat(acc.y), mat.z * sat(acc.z), mat.w * sat(acc.w)};
  }

  const aurora::Mat3x4<float>& postex(uint32_t index) const {
    return index < aurora::gx::MaxPnMtx ? g_gxState.pnMtx[index].pos : g_gxState.texMtxs[index - aurora::gx::MaxPnMtx];
  }

  // GX texgen for texcoord `coord` (shader.cpp 1213-1302); returns (s, t, q).
  V3 texgen(const Decoded& d, const V3& mvPos, const V3& mvNrm, uint32_t coord, int depth = 0) const {
    const auto& tcg = cfg.tcgs[coord];
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
      if (depth > 0) return {};
      const V3 base = texgen(d, mvPos, mvNrm, tcg.embossSrc, depth + 1);
      const auto& light = g_gxState.lights[tcg.type - GX_TG_BUMP0];
      const V3 ldir = normalize(V3{light.pos[0] - mvPos.x, light.pos[1] - mvPos.y, light.pos[2] - mvPos.z});
      const auto& nm = g_gxState.pnMtx[d.pnmtx].nrm;
      const V3 t = mul34(nm, d.tangent.x, d.tangent.y, d.tangent.z, 0.f);
      const V3 b = mul34(nm, d.binrm.x, d.binrm.y, d.binrm.z, 0.f);
      return {base.x / (base.z != 0.f ? base.z : 1.f) + dot3(ldir, t), base.y / (base.z != 0.f ? base.z : 1.f) + dot3(ldir, b), 1.f};
    }
    V4 src{0.f, 0.f, 1.f, 1.f};
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      const auto& uv = d.tex[tcg.src - GX_TG_TEX0];
      src = {uv[0], uv[1], 1.f, 1.f};
    } else if (tcg.src == GX_TG_POS) {
      src = {d.pos.x, d.pos.y, d.pos.z, 1.f};
    } else if (tcg.src == GX_TG_NRM) {
      src = {d.nrm.x, d.nrm.y, d.nrm.z, 1.f};
    } else if (tcg.src == GX_TG_BINRM) {
      src = {d.binrm.x, d.binrm.y, d.binrm.z, 1.f};
    } else if (tcg.src == GX_TG_TANGENT) {
      src = {d.tangent.x, d.tangent.y, d.tangent.z, 1.f};
    } else if (tcg.src == GX_TG_COLOR0 || tcg.src == GX_TG_COLOR1) {
      const V4& c = d.clr[tcg.src - GX_TG_COLOR0];
      src = c;
    }
    V3 t;
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      const AttrConfig& idx = cfg.attrs[GX_VA_TEX0MTXIDX + coord];
      if (idx.attrType == GX_DIRECT) {
        t = mul34(postex(d.texmtx[coord] / 3u), src.x, src.y, src.z, src.w);
      } else if (tcg.mtx == GX_IDENTITY) {
        t = {src.x, src.y, src.z};
      } else {
        t = mul34(postex(static_cast<uint32_t>(tcg.mtx) / 3u), src.x, src.y, src.z, src.w);
      }
      if (tcg.type == GX_TG_MTX2x4) t.z = 1.f;
    } else {  // SRTG
      t = {src.x, src.y, 1.f};
    }
    if (tcg.normalize) t = normalize(t);
    if (tcg.postMtx != GX_PTIDENTITY) {
      t = mul34(g_gxState.ptTexMtxs[(tcg.postMtx - GX_PTTEXMTX0) / 3], t.x, t.y, t.z, 1.f);
    }
    return t;
  }

  unsigned source_spans(std::array<SourceSpan,MaxGeometrySpans>& spans,bool sparse=false,size_t* broadBytes=nullptr) const {
    const size_t bytes=size_t(draw.vtxCount)*cfg.vtxStride;
    if(!bytes || bytes>GeometryEntryLimit)return 0;
    unsigned count=1;spans[0]={vtxBase,bytes};
    std::array<SourceSpan,MaxGeometrySpans> candidate{};unsigned candidateCount=1;
    std::array<uint16_t,MaxSparseIndices> indices;
    unsigned remaining=0;
    const bool compact=sparse && draw.vtxCount<=MaxSparseIndices;
    if(compact){
      candidate[0]=spans[0];
      for(int f=0;f<activeCount;++f){const auto attr=active[f];const auto type=cfg.attrs[attr].attrType;
        if(attr>=GX_VA_POS && (type==GX_INDEX8 || type==GX_INDEX16))++remaining;}
    }
    for(int field=0;field<activeCount;++field) {
      const int attr=active[field];const auto& a=cfg.attrs[attr];
      if(attr<GX_VA_POS || (a.attrType!=GX_INDEX8 && a.attrType!=GX_INDEX16))continue;
      uint32_t lo=UINT32_MAX,hi=0;
      for(uint32_t v=0;v<draw.vtxCount;++v) {
        const auto* p=vtxBase+size_t(v)*cfg.vtxStride+a.offset;
        const uint32_t index=a.attrType==GX_INDEX8 ? *p : rd16(p,false);
        lo=std::min(lo,index);hi=std::max(hi,index);
        if(compact)indices[v]=uint16_t(index);
      }
      size_t width;
      if(attr==GX_VA_CLR0 || attr==GX_VA_CLR1) {
        width=(a.compType==GX_RGB565 || a.compType==GX_RGBA4)?2:
              (a.compType==GX_RGB8 || a.compType==GX_RGBA6)?3:4;
      } else {
        const unsigned components=attr==GX_VA_POS ? (a.cnt==2?2:3):attr==GX_VA_NRM?3:(a.cnt==1?1:2);
        const unsigned scalar=(a.compType==GX_U8 || a.compType==GX_S8)?1:
                              (a.compType==GX_U16 || a.compType==GX_S16)?2:4;
        width=components*scalar;
      }
      const size_t span=size_t(hi-lo)*a.stride+width;
      if(span>GeometryEntryLimit || !arrays[attr-GX_VA_POS])return 0;
      spans[count++]={arrays[attr-GX_VA_POS]+size_t(lo)*a.stride,span};
      if(compact){
        --remaining;
        const unsigned capacity=MaxGeometrySpans-candidateCount-remaining;
        const unsigned n=indexed_source_runs(candidate.data()+candidateCount,capacity,arrays[attr-GX_VA_POS],a.stride,width,
                                             indices.data(),draw.vtxCount,lo,hi);
        if(!n)return 0;
        candidateCount+=n;
      }
    }
    // Validate this draw's raw indices before any previously referenced array
    // range; only array spans may be sorted/merged with one another.
    count=merge_source_spans(spans,count,1);
    size_t original=0;
    if(compact || broadBytes)for(unsigned i=0;i<count;++i)original+=spans[i].size;
    if(broadBytes)*broadBytes=original;
    if(compact){
      candidateCount=merge_source_spans(candidate,candidateCount,1);
      size_t selected=0;for(unsigned i=0;i<candidateCount;++i)selected+=candidate[i].size;
      // Include the unchanged root in the saving gate. Never increase source
      // payload or descriptor traversal merely for a negligible byte saving.
      if(selected<=original-original/4 && selected<original){
        std::copy_n(candidate.begin(),candidateCount,spans.begin());return candidateCount;
      }
    }
    return count;
  }

  uint32_t indexed_array_mask() const {
    uint32_t mask=0;
    for(int field=0;field<activeCount;++field) {
      const unsigned attr=active[field];const auto type=cfg.attrs[attr].attrType;
      if(attr>=GX_VA_POS && (type==GX_INDEX8 || type==GX_INDEX16))mask|=1u<<(attr-GX_VA_POS);
    }
    return mask;
  }

  const aurora::gx::ShaderConfig& cfg;
  const Pipeline& pipe;
  const aurora::gx::DrawData& draw;
  Needs needs;

private:
  void plan() {
    for (int ch=0; ch<2; ++ch) {
      const auto& cc = cfg.colorChannels[ch];
      const auto& cca = cfg.colorChannels[ch+2];
      needs.normal = needs.normal || cc.lightingEnabled || cca.lightingEnabled;
      needs.color[ch] = cc.matSrc == GX_SRC_VTX || cca.matSrc == GX_SRC_VTX ||
        (cc.lightingEnabled && cc.ambSrc == GX_SRC_VTX) ||
        (cca.lightingEnabled && cca.ambSrc == GX_SRC_VTX);
    }
    for (int i=0;i<8;++i) if (pipe.info.sampledTexCoords.test(i)) need_texgen(i,0);
    wanted.fill(false);
    wanted[GX_VA_PNMTXIDX] = true;
    for (int i = GX_VA_TEX0MTXIDX; i <= GX_VA_TEX7MTXIDX; ++i) wanted[i] = true;
    wanted[GX_VA_POS] = true;
    wanted[GX_VA_NRM] = needs.normal || needs.nbt;
    wanted[GX_VA_CLR0] = needs.color[0];
    wanted[GX_VA_CLR1] = needs.color[1];
    for (int i = 0; i < 8; ++i) wanted[GX_VA_TEX0 + i] = (needs.texMask & (1u << i)) != 0;
    activeCount=0;
    for(int attr=GX_VA_PNMTXIDX;attr<=GX_VA_TEX7;++attr) {
      const auto& a=cfg.attrs[attr];
      if(!wanted[attr] || a.attrType==GX_NONE)continue;
      active[activeCount]=attr;
#if TWV_VITA_PLANNED_FETCH
      if(attr>=GX_VA_POS && attr!=GX_VA_CLR0 && attr!=GX_VA_CLR1) {
        const int n=attr==GX_VA_POS?(a.cnt==2?2:3):attr==GX_VA_NRM?3:(a.cnt==1?1:2);
        fetchPlans[activeCount]=plan_fetch(a,a.attrType==GX_DIRECT?false:a.le,n);
      }
#endif
      ++activeCount;
    }
  }

  void need_texgen(uint32_t coord, int depth) {
    if (coord >= 8 || depth > 1) return;
    const auto& tcg = cfg.tcgs[coord];
    if (tcg.type >= GX_TG_BUMP0 && tcg.type <= GX_TG_BUMP7) {
      needs.nbt = needs.normal = true;
      need_texgen(tcg.embossSrc, depth + 1);
      return;
    }
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) needs.texMask |= static_cast<uint8_t>(1u << (tcg.src - GX_TG_TEX0));
    if (tcg.src == GX_TG_NRM) needs.normal = true;
    if (tcg.src == GX_TG_BINRM || tcg.src == GX_TG_TANGENT) needs.nbt = needs.normal = true;
    if (tcg.src == GX_TG_COLOR0) needs.color[0] = true;
    if (tcg.src == GX_TG_COLOR1) needs.color[1] = true;
  }

  const uint8_t* address(const AttrConfig& a, int attr, const uint8_t* base, uint32_t dlExtra, uint32_t within,
                         bool& le) const {
    const uint8_t* p = base + a.offset + dlExtra;
    if (a.attrType == GX_DIRECT) {
      le = false;
      return p + within;
    }
    const uint32_t index = a.attrType == GX_INDEX8 ? *p : rd16(p, false);
    le = a.le;
    return arrays[attr - GX_VA_POS] + static_cast<size_t>(index) * a.stride + within;
  }

  const uint8_t* vtxBase = nullptr;
  std::array<const uint8_t*, 12> arrays{};
  std::array<bool, GX_VA_TEX7 + 1> wanted{};
  std::array<int,GX_VA_TEX7+1> active{};
#if TWV_VITA_PLANNED_FETCH
  std::array<PlannedFetch,GX_VA_TEX7+1> fetchPlans{};
#endif
  int activeCount=0;
};
} // namespace

extern "C" int twv_vita_stream_decode_select(int enabled) {
  return streamDecodeControl.select(enabled);
}
extern "C" int twv_vita_small_packets_select(int enabled) {
  return smallPacketControl.select(enabled);
}
extern "C" int twv_vita_uncached_staging_select(int enabled) {
  return uncachedStagingControl.select(enabled);
}
extern "C" int twv_vita_vertex_cull_select(int enabled) {
  return vertexCullControl.select(enabled);
}
extern "C" int twv_vita_sparse_spans_select(int enabled) {
  return sparseSpansControl.select(enabled);
}
extern "C" int twv_vita_neon_compare_select(int enabled) {
  if(!neonCompareControl.select(enabled))return 0;
  geometryCache.set_neon_compare(bool(enabled));
  return 1;
}

TwvVitaVertex* build_vertices(const Pipeline& pipeline, const aurora::gx::DrawData& data) noexcept {
  smallPacketControl.begin();uncachedStagingControl.begin();
  streamDecodeControl.begin();
  neonCompareControl.begin();
  vertexCullControl.begin();
  sparseSpansControl.begin();
  const size_t bytes=size_t(data.vtxCount)*sizeof(TwvVitaVertex);
  VertexWorkSample sample(vertexWorkProbe,data.vtxCount,uint32_t(bytes),twv_vgl_time_us);
  auto* mapped=static_cast<TwvVitaVertex*>(twv_vgl_alloc(bytes));
  sample.mark(VertexWorkPhase::Allocate);
  if(!mapped) {count("vertex_allocation_failure");return nullptr;}
  auto* out=reinterpret_cast<TwvVitaVertex*>(stage_vertex_output(mapped,bytes));
  VertexStage stage(pipeline,data);
  Decoded d{};
#if TWV_VITA_PLANNED_FETCH
  count("vertex_planned_fetch_vertices",data.vtxCount);
#endif
  sample.mark(VertexWorkPhase::Setup);
  for (uint32_t v=0; v<data.vtxCount; ++v) {
    stage.decode(v,d);
    if (d.pnmtx >= aurora::gx::MaxPnMtx) { count("invalid_position_matrix"); return nullptr; }
    const auto& matrix = g_gxState.pnMtx[d.pnmtx];
    const V3 pos=mul34(matrix.pos,d.pos.x,d.pos.y,d.pos.z,1.f);
    const V3 normal=stage.needs.normal ? normalize(mul34(matrix.nrm,d.nrm.x,d.nrm.y,d.nrm.z,0.f)) : V3{};
    auto& vertex=out[v];
    vertex.position[0]=pos.x; vertex.position[1]=pos.y; vertex.position[2]=pos.z;
    for (int ch=0;ch<2;++ch) {
      const auto color=stage.light(d,pos,normal,ch,false);
      const auto alpha=stage.light(d,pos,normal,ch,true);
      vertex.color[ch][0]=color.x; vertex.color[ch][1]=color.y;
      vertex.color[ch][2]=color.z; vertex.color[ch][3]=alpha.w;
    }
    for (uint32_t coord=0;coord<8;++coord) {
      V3 t{0.f,0.f,1.f};
      if (pipeline.info.sampledTexCoords.test(coord)) t=stage.texgen(d,pos,normal,coord);
      vertex.texcoord[coord][0]=t.x; vertex.texcoord[coord][1]=t.y;
      vertex.texcoord[coord][2]=t.z;
    }
  }
  sample.mark(VertexWorkPhase::Decode);
  if(out!=mapped) {std::memcpy(mapped,out,bytes);sample.mark(VertexWorkPhase::StagingCopy);}
  sample.complete(VertexWorkRoute::CpuFallback);
  return mapped;
}

bool gpu_vertex_supported(const Pipeline& pipeline) noexcept {
#if !TWV_VITA_GPU_VERTEX
  return false;
#else
  // Bump/emboss depends on transformed tangents and view position. Keep that
  // complete draw on the existing CPU path, explicitly counted by the caller.
  for(unsigned tc=0;tc<8;++tc) if(pipeline.info.sampledTexCoords.test(tc)) {
    const auto type=pipeline.config.shaderConfig.tcgs[tc].type;
    if(type!=GX_TG_MTX2x4 && type!=GX_TG_MTX3x4 && type!=GX_TG_SRTG) return false;
  }
  return true;
#endif
}

TwvVitaVertexLayout gpu_vertex_layout(const Pipeline& pipeline) noexcept {
  const bool compactST=twv_vita_bundle_feature(TWV_BUNDLE_COMPACT_ST)!=0;
  unsigned colors=0;
  unsigned texMask=uint32_t(pipeline.info.sampledTexCoords.to_ulong());
  bool normal=pipeline.info.lightingEnabled;
  for(unsigned ch=0;ch<2;++ch) if(pipeline.info.sampledColorChannels.test(ch)) {
    const auto& rgb=pipeline.config.shaderConfig.colorChannels[ch];
    const auto& alpha=pipeline.config.shaderConfig.colorChannels[ch+2];
    if(rgb.matSrc==GX_SRC_VTX || alpha.matSrc==GX_SRC_VTX ||
       (rgb.lightingEnabled && rgb.ambSrc==GX_SRC_VTX) ||
       (alpha.lightingEnabled && alpha.ambSrc==GX_SRC_VTX)) colors|=1u<<ch;
  }
  if(pipeline.gpuTexgen) for(unsigned tc=0;tc<8;++tc) if(pipeline.info.sampledTexCoords.test(tc)) {
    const auto src=pipeline.config.shaderConfig.tcgs[tc].src;
    if(src==GX_TG_NRM)normal=true;
    if(src==GX_TG_COLOR0 || src==GX_TG_COLOR1)colors|=1u<<(src-GX_TG_COLOR0);
    if(src<GX_TG_TEX0 || src>GX_TG_TEX7)texMask&=~(1u<<tc);
  }
  return compact_vertex_layout(texMask,colors,normal,pipeline.gpuTexgen && compactST);
}

void configure_gpu_texgen(Pipeline& p) noexcept {
  p.gpuTexgen=false; p.texgenVectors=0;
  for(unsigned tc=0;tc<8;++tc)p.texgenRows[tc]=p.postRows[tc]=-1;
#if TWV_VITA_GPU_TEXGEN
  if(!p.gpuVertex)return;
  unsigned rows=0;
  for(unsigned tc=0;tc<8;++tc) if(p.info.sampledTexCoords.test(tc)) {
    const auto& g=p.config.shaderConfig.tcgs[tc];
    const bool uv=g.src>=GX_TG_TEX0 && g.src<=GX_TG_TEX7;
    if(!uv && g.src!=GX_TG_POS && g.src!=GX_TG_NRM && g.src!=GX_TG_COLOR0 && g.src!=GX_TG_COLOR1)return;
    if(g.type==GX_TG_MTX2x4 || g.type==GX_TG_MTX3x4) {
      // Per-vertex texture-matrix palettes need a larger uniform contract.
      // Keep the complete old CPU STQ calculation for those materials.
      if(p.config.shaderConfig.attrs[GX_VA_TEX0MTXIDX+tc].attrType==GX_DIRECT)return;
      if(g.mtx!=GX_IDENTITY) {
        if(unsigned(g.mtx)/3u>=aurora::gx::MaxPnMtx+aurora::gx::MaxTexMtx)return;
        p.texgenRows[tc]=int(rows); rows+=3;
      }
    } else if(g.type!=GX_TG_SRTG)return;
    if(g.postMtx!=GX_PTIDENTITY) {
      if(g.postMtx<GX_PTTEXMTX0 || (unsigned(g.postMtx)-GX_PTTEXMTX0)/3u>=aurora::gx::MaxPTTexMtx)return;
      p.postRows[tc]=int(rows); rows+=3;
    }
  }
  if(!texgen_uniforms_fit(p.info.lightingEnabled,rows))return;
  p.texgenVectors=rows; p.gpuTexgen=true;
#endif
}

void configure_gpu_cache_dependencies(Pipeline& p) noexcept {
  const aurora::gx::DrawData empty{};
  const VertexStage stage(p,empty); // Plan only: no vertex/array address is read.
  p.sourceArrayMask=stage.indexed_array_mask();
  p.usesCurrentPn=p.config.shaderConfig.attrs[GX_VA_PNMTXIDX].attrType!=GX_DIRECT;
}

void* build_gpu_vertices(const Pipeline& pipeline,const aurora::gx::DrawData& data,unsigned& pnSlots,bool& culled) noexcept {
  smallPacketControl.begin();uncachedStagingControl.begin();
  if(!residentAllocatorConfigured){
    if(twv_vita_bundle_feature(TWV_BUNDLE_RESIDENT_GEOMETRY) && twv_vgl_resident_available())
      geometryCache.set_output_allocator(twv_vgl_resident_alloc,twv_vgl_resident_release);
    residentAllocatorConfigured=true;
  }
  streamDecodeControl.begin();
  neonCompareControl.begin();
  vertexCullControl.begin();culled=false;
  sparseSpansControl.begin();
  pnSlots=1;
  const auto& layout=pipeline.vertexLayout;
  const size_t outputBytes=size_t(data.vtxCount)*layout.stride;
  VertexWorkSample sample(vertexWorkProbe,data.vtxCount,uint32_t(outputBytes),twv_vgl_time_us);
  unsigned char* out=nullptr;
  const bool cacheable=TWV_VITA_GEOMETRY_CACHE && geometryCacheEnabled && geometryCache.budget() && pipeline.gpuTexgen && data.vtxCount>=32;
  GeometryKey key{};uint32_t hash=0;
  GeometryCache::Lookup cached{nullptr,0,false};
  bool validateCached=false;
  SmallPacketKey smallKey{};
  uint32_t smallHash=0;
  const auto* smallSource=reinterpret_cast<const void*>(uintptr_t(data.immediateData.vtxStart));
  const auto& config=pipeline.config.shaderConfig;
  const bool smallCacheable=smallPacketControl.enabled() && small_packet_eligible(config,data.vtxCount,smallSource,layout,pipeline.gpuTexgen);
  const size_t smallSourceBytes=smallCacheable?size_t(data.vtxCount)*config.vtxStride:0;
  SmallPacketCache::Lookup smallHit;
  bool validateSmall=false;
  if(smallCacheable) {
    ++smallEligible;
    smallKey={uint32_t(data.pipeline),data.vtxCount,config.vtxStride,
        config.attrs[GX_VA_PNMTXIDX].attrType==GX_DIRECT?0u:data.immediateData.currentPnMtx};
    smallHash=XXH32(smallSource,smallSourceBytes,XXH32(&smallKey,sizeof smallKey,0));
    smallHit=smallPacketCache.lookup(smallHash,smallKey,smallSource,smallSourceBytes,outputBytes);
    sample.mark(VertexWorkPhase::Lookup);
    if(smallHit.data) {
      // This experimental cache always keeps its independent oracle, even in
      // builds that disable the older large-cache diagnostic option.
      validateSmall=(++smallValidationSerial%128==0);
      if(!validateSmall) {
        out=static_cast<unsigned char*>(twv_vgl_alloc(outputBytes));
        sample.mark(VertexWorkPhase::Allocate);
        if(!out){count("vertex_allocation_failure");return nullptr;}
        std::memcpy(out,smallHit.data,outputBytes);smallReplayBytes+=outputBytes;
        sample.mark(VertexWorkPhase::ReplayCopy);pnSlots=smallHit.pnSlots;
        sample.complete(VertexWorkRoute::Replay);return out;
      }
    }
  }
  if(cacheable) {
    key.pipeline=uint32_t(data.pipeline);key.vertexBase=data.immediateData.vtxStart;
    key.vertexCount=data.vtxCount;key.currentPnMtx=data.immediateData.currentPnMtx;
    static_assert(sizeof key.arrays==sizeof data.immediateData.arrayStart);
    std::memcpy(key.arrays,data.immediateData.arrayStart.data(),sizeof key.arrays);
#if TWV_VITA_CANONICAL_GEOMETRY_KEYS
    key=canonical_geometry_key(key,pipeline.sourceArrayMask,pipeline.usesCurrentPn);
#endif
    hash=XXH32(&key,sizeof key,0);
    cached=geometryCache.lookup(hash,key);
    sample.mark(VertexWorkPhase::Lookup);
    if(cached.data && cached.bytes==outputBytes) {
      validateCached=TWV_VITA_CACHE_VALIDATE && (++cacheValidationSerial%256==0);
      if(!validateCached) {
#if TWV_VITA_VERTEX_CULL
        if(vertexCullControl.enabled() && cached.pnSlots==1 && cached.bounds.valid){
          ++cullEligible;
          const unsigned plane=offscreen_plane(cached.bounds,g_gxState.pnMtx[0].pos,g_gxState.proj);
          if(plane){
            // Keep every256-hit independent decoder oracle on its original
            // path. Separately scan packed positions on every64th rejection.
            if(TWV_VITA_CACHE_VALIDATE && (cullRejected%64)==0){
              if(verify_offscreen_positions(cached.data,cached.bytes,data.vtxCount,layout.stride,layout.offsets[0],
                                            plane,g_gxState.pnMtx[0].pos,g_gxState.proj))++cullChecks;
              else{
                ++cullFailures;vertexCullControl.fail();
                twv::vita::telemetryEvent("vertex_cull_validation_failure",cullFailures);
              }
            }
            if(vertexCullControl.enabled()){
              ++cullRejected;cullVertices+=data.vtxCount;cullBytes+=outputBytes;
              culled=true;sample.complete(VertexWorkRoute::Culled);return nullptr;
            }
          }
        }
#endif
        if(cached.resident){
          out=const_cast<unsigned char*>(cached.data);
          ++residentReplayPackets;residentReplayBytes+=outputBytes;
        }else{
          out=static_cast<unsigned char*>(twv_vgl_alloc(outputBytes));
          sample.mark(VertexWorkPhase::Allocate);
          if(!out){count("vertex_allocation_failure");return nullptr;}
          std::memcpy(out,cached.data,outputBytes);
        }
        sample.mark(VertexWorkPhase::ReplayCopy);
        pnSlots=cached.pnSlots;
        count(CounterId::GeometryCacheHitVertices,data.vtxCount);
        sample.complete(VertexWorkRoute::Replay);
        return out;
      }
    }
  }
  if(!cacheable && !smallCacheable)sample.mark(VertexWorkPhase::Lookup);
  out=static_cast<unsigned char*>(twv_vgl_alloc(outputBytes));
  sample.mark(VertexWorkPhase::Allocate);
  if(!out){count("vertex_allocation_failure");return nullptr;}
  auto* decoded=stage_vertex_output(out,outputBytes);
  VertexStage stage(pipeline,data);
  unsigned texcoords[8],texCount=0;
  for(unsigned tc=0;tc<8;++tc) if(pipeline.info.sampledTexCoords.test(tc))texcoords[texCount++]=tc;
  Decoded d{};
#if TWV_VITA_PLANNED_FETCH
  count((validateCached || validateSmall)?"vertex_reference_fetch_vertices":"vertex_planned_fetch_vertices",data.vtxCount);
#endif
  sample.mark(VertexWorkPhase::Setup);
  bool streamed=false;
#if TWV_VITA_STREAM_DECODE
  if(streamDecodeControl.enabled() && !validateCached && !validateSmall && pipeline.gpuTexgen && stage.can_pack_raw_streams(layout)) {
    if(!stage.pack_raw_streams(layout,decoded,pnSlots)){count("invalid_position_matrix");return nullptr;}
    ++streamPackets;streamVertices+=data.vtxCount;streamed=true;
    if(TWV_VITA_CACHE_VALIDATE && ++streamValidationSerial%128==0) {
      if(stage.validate_raw_streams(layout,decoded,pnSlots))++streamValidationPass;
      else {
        ++streamValidationFailure;streamDecodeControl.fail();geometryCache.clear();smallPacketCache.clear();
        twv::vita::telemetryEvent("vertex_stream_validation_failure",streamValidationFailure);
        streamed=false;pnSlots=1; // Rebuild this entire packet with the old path below.
      }
    }
  }
#endif
  if(!streamed) {
  for(uint32_t v=0;v<data.vtxCount;++v) {
    // Keep the old vertex-major decoder independent of both optimized writers
    // when checking a cached packet's output and palette extent.
    if(validateCached || validateSmall)stage.decode_reference(v,d);
    else stage.decode(v,d);
    if(d.pnmtx>=aurora::gx::MaxPnMtx) {count("invalid_position_matrix");return nullptr;}
    pnSlots=std::max(pnSlots,unsigned(d.pnmtx)+1);
    auto* vertex=decoded+size_t(v)*layout.stride;
    store_vertex_attribute(vertex,layout,0,&d.pos);
    store_vertex_attribute(vertex,layout,11,&d.nrm);
    const float matrixIndex=float(d.pnmtx);
    store_vertex_attribute(vertex,layout,12,&matrixIndex);
    for(int ch=0;ch<2;++ch) store_vertex_attribute(vertex,layout,1+ch,&d.clr[ch]);
    // GPU texgens consume raw object-space attributes. Unsupported variants
    // retain the original CPU STQ calculation, including normalization/post math.
    for(unsigned i=0;i<texCount;++i) {
      const unsigned tc=texcoords[i];
      if(layout.offsets[3+tc]==TwvVertexAttributeAbsent)continue;
      V3 t;
      if(pipeline.gpuTexgen) {
        const unsigned src=pipeline.config.shaderConfig.tcgs[tc].src-GX_TG_TEX0;
        t={d.tex[src][0],d.tex[src][1],1.f};
      } else t=stage.texgen(d,{},{},tc);
      store_vertex_attribute(vertex,layout,3+tc,&t);
    }
  }
  }
  sample.mark(VertexWorkPhase::Decode);
  if(validateSmall) {
    if(pnSlots!=smallHit.pnSlots || std::memcmp(decoded,smallHit.data,outputBytes)!=0) {
      ++smallFailures;smallPacketControl.fail();streamDecodeControl.fail();
      // Current packet is already rebuilt by the independent original decoder.
      // No borrowed cache pointer is read after either cache is cleared.
      smallPacketCache.clear();geometryCache.clear();
      twv::vita::telemetryEvent("small_packet_validation_failure",smallFailures);
    } else ++smallChecks;
    sample.mark(VertexWorkPhase::Oracle);
  } else if(smallCacheable && smallPacketControl.enabled()) {
    smallPacketCache.store(smallHash,smallKey,smallSource,smallSourceBytes,outputBytes,decoded,pnSlots);
    sample.mark(VertexWorkPhase::Store);
  }
  if(validateCached) {
    if(pnSlots!=cached.pnSlots || std::memcmp(decoded,cached.data,outputBytes)!=0) {
      count("geometry_cache_validation_failure");
      neonCompareControl.fail();geometryCache.set_neon_compare(false);
      vertexCullControl.fail();
      sparseSpansControl.fail();
      geometryCacheEnabled=false;geometryCache.clear();smallPacketCache.clear();
    } else count(CounterId::GeometryCacheValidationPass);
    sample.mark(VertexWorkPhase::Oracle);
  } else if(cacheable && cached.mayStore) {
    std::array<SourceSpan,MaxGeometrySpans> spans{};
    size_t broadBytes=0;
    const unsigned n=stage.source_spans(spans,sparseSpansControl.enabled(),sparseSpansControl.enabled()?&broadBytes:nullptr);
    PositionBounds bounds{};
#if TWV_VITA_VERTEX_CULL
    if(vertexCullControl.enabled())bounds=position_bounds(decoded,outputBytes,data.vtxCount,layout.stride,layout.offsets[0],pnSlots);
#endif
    if(n){
      const bool stored=geometryCache.store(hash,key,spans.data(),n,decoded,outputBytes,pnSlots,bounds);
      if(stored && sparseSpansControl.enabled()){
        size_t selected=0;for(unsigned i=0;i<n;++i)selected+=spans[i].size;
        ++sparseAdmitted;sparseBroadBytes+=broadBytes;sparseSelectedBytes+=selected;
        if(selected<broadBytes)++sparseSelected;
      }
    }
    else geometryCache.defer(hash,key);
    sample.mark(VertexWorkPhase::Store);
  }
  if(decoded!=out) {std::memcpy(out,decoded,outputBytes);sample.mark(VertexWorkPhase::StagingCopy);}
  sample.complete(VertexWorkRoute::GpuDecode);
  return out;
}

void geometry_cache_end_frame() noexcept {
  geometryCache.end_frame();
#if TWV_VITA_GEOMETRY_CACHE
  // Resident entries retire through their captured provider; trimming cannot
  // recycle GPU storage until explicit completion. CPU entries keep ring replay.
  geometryCache.set_budget(twv_vgl_geometry_cache_budget(uint32_t(geometryCache.cpu_bytes())));
  if(!geometryCache.budget())vertexStaging.clear();
#endif
}
void geometry_cache_clear() noexcept {geometryCache.clear();smallPacketCache.clear();vertexStaging.clear();}
void geometry_cache_report() noexcept {
  twv::vita::telemetryEvent("gfx_small_packets_compiled",smallPacketControl.compiled());
  twv::vita::telemetryEvent("gfx_small_packets_explicit",smallPacketControl.selected());
  twv::vita::telemetryEvent("gfx_small_packets_enabled",smallPacketControl.enabled());
  twv::vita::telemetryEvent("gfx_small_packets_eligible",smallEligible);
  twv::vita::telemetryEvent("gfx_small_packets_validation_pass",smallChecks);
  twv::vita::telemetryEvent("gfx_small_packets_validation_failure",smallFailures);
  twv::vita::telemetryEvent("gfx_small_packets_replay_bytes",smallReplayBytes);
  const auto& small=smallPacketCache.stats;
  twv::vita::telemetryEvent("gfx_small_packets_hits",small.hits);
  twv::vita::telemetryEvent("gfx_small_packets_misses",small.misses);
  twv::vita::telemetryEvent("gfx_small_packets_collisions",small.collisions);
  twv::vita::telemetryEvent("gfx_small_packets_replacements",small.replacements);
  twv::vita::telemetryEvent("gfx_small_packets_rejected",small.rejected);
  twv::vita::telemetryEvent("gfx_small_packets_validation_bytes",small.validationBytes);
  twv::vita::telemetryEvent("gfx_small_packets_source_copy_bytes",small.sourceCopyBytes);
  twv::vita::telemetryEvent("gfx_small_packets_output_copy_bytes",small.outputCopyBytes);
  twv::vita::telemetryEvent("gfx_small_packets_footprint_bytes",small.dataFootprint);
  twv::vita::telemetryEvent("gfx_uncached_staging_compiled",uncachedStagingControl.compiled());
  twv::vita::telemetryEvent("gfx_uncached_staging_explicit",uncachedStagingControl.selected());
  twv::vita::telemetryEvent("gfx_uncached_staging_enabled",uncachedStagingControl.enabled());
  twv::vita::telemetryEvent("gfx_uncached_staged_bytes",uncachedStagedBytes);
  twv::vita::telemetryEvent("gfx_vram_staged_bytes",vramStagedBytes);
  if(uncachedStagingControl.enabled())twv_vgl_vertex_write_class_report();
  twv::vita::telemetryEvent("gfx_sparse_spans_compiled",sparseSpansControl.compiled());
  twv::vita::telemetryEvent("gfx_sparse_spans_explicit",sparseSpansControl.selected());
  twv::vita::telemetryEvent("gfx_sparse_spans_enabled",sparseSpansControl.enabled());
  twv::vita::telemetryEvent("gfx_sparse_spans_admitted",sparseAdmitted);
  twv::vita::telemetryEvent("gfx_sparse_spans_selected",sparseSelected);
  twv::vita::telemetryEvent("gfx_sparse_spans_broad_bytes",sparseBroadBytes);
  twv::vita::telemetryEvent("gfx_sparse_spans_selected_bytes",sparseSelectedBytes);
  twv::vita::telemetryEvent("gfx_geometry_validation_span_bytes",geometryCache.stats.validationSpanBytes);
  twv::vita::telemetryEvent("gfx_geometry_validation_spans",geometryCache.stats.validationSpans);
  twv::vita::telemetryEvent("gfx_geometry_cache_metadata_bytes",sizeof geometryCache);
  twv::vita::telemetryEvent("gfx_vertex_cull_compiled",vertexCullControl.compiled());
  twv::vita::telemetryEvent("gfx_vertex_cull_explicit",vertexCullControl.selected());
  twv::vita::telemetryEvent("gfx_vertex_cull_enabled",vertexCullControl.enabled());
  twv::vita::telemetryEvent("gfx_vertex_cull_eligible",cullEligible);
  twv::vita::telemetryEvent("gfx_vertex_cull_rejected",cullRejected);
  twv::vita::telemetryEvent("gfx_vertex_cull_vertices",cullVertices);
  twv::vita::telemetryEvent("gfx_vertex_cull_saved_bytes",cullBytes);
  twv::vita::telemetryEvent("gfx_vertex_cull_validation_pass",cullChecks);
  twv::vita::telemetryEvent("gfx_vertex_cull_validation_failure",cullFailures);
  twv::vita::telemetryEvent("gfx_geometry_compare_neon_enabled",neonCompareControl.enabled());
  twv::vita::telemetryEvent("gfx_geometry_compare_neon_compiled",neonCompareControl.compiled());
  twv::vita::telemetryEvent("gfx_geometry_compare_neon_explicit",neonCompareControl.selected());
  twv::vita::telemetryEvent("gfx_geometry_cache_neon_lookups",geometryCache.stats.neonLookups);
  twv::vita::telemetryEvent("gfx_vertex_stream_enabled",streamDecodeControl.enabled());
  twv::vita::telemetryEvent("gfx_vertex_stream_compiled",streamDecodeControl.compiled());
  twv::vita::telemetryEvent("gfx_vertex_stream_explicit",streamDecodeControl.selected());
  twv::vita::telemetryEvent("gfx_vertex_stream_packets",streamPackets);
  twv::vita::telemetryEvent("gfx_vertex_stream_vertices",streamVertices);
  twv::vita::telemetryEvent("gfx_vertex_stream_validation_pass",streamValidationPass);
  twv::vita::telemetryEvent("gfx_vertex_stream_validation_failure",streamValidationFailure);
  const auto& s=geometryCache.stats;
  twv::vita::telemetryEvent("gfx_resident_replay_packets",residentReplayPackets);
  twv::vita::telemetryEvent("gfx_resident_replay_bytes",residentReplayBytes);
  twv::vita::telemetryEvent("gfx_geometry_resident_bytes",s.residentBytes);
  twv::vita::telemetryEvent("gfx_geometry_resident_stores",s.residentStores);
  twv::vita::telemetryEvent("gfx_geometry_resident_fallbacks",s.residentFallbacks);
  twv::vita::telemetryEvent("gfx_geometry_resident_releases",s.residentReleases);
  twv::vita::telemetryEvent("gfx_geometry_resident_hit_bytes",s.residentHitBytes);
  twv::vita::telemetryEvent("gfx_geometry_cache_ways",GeometryCache::WayCount);
  twv::vita::telemetryEvent("gfx_geometry_cache_entries",GeometryCache::EntryCount);
  twv::vita::telemetryEvent("gfx_geometry_cache_hits",s.hits);
  twv::vita::telemetryEvent("gfx_geometry_cache_misses",s.misses);
  twv::vita::telemetryEvent("gfx_geometry_cache_invalidations",s.invalidations);
  twv::vita::telemetryEvent("gfx_geometry_cache_cooldowns",s.cooldowns);
  twv::vita::telemetryEvent("gfx_geometry_cache_stores",s.stores);
  twv::vita::telemetryEvent("gfx_geometry_cache_rejected",s.rejected);
  twv::vita::telemetryEvent("gfx_geometry_cache_bytes",geometryCache.bytes());
  twv::vita::telemetryEvent("gfx_geometry_cache_budget_deferrals",s.budgetDeferrals);
  twv::vita::telemetryEvent("gfx_geometry_cache_set_deferrals",s.setDeferrals);
  twv::vita::telemetryEvent("gfx_geometry_cache_admission_blocked",s.admissionBlocked);
  twv::vita::telemetryEvent("gfx_geometry_cache_budget_evictions",s.budgetEvictions);
  twv::vita::telemetryEvent("gfx_geometry_cache_budget_bytes",geometryCache.budget());
  twv::vita::telemetryEvent("gfx_geometry_cache_source_bytes",geometryCache.source_bytes());
  twv::vita::telemetryEvent("gfx_geometry_cache_output_bytes",geometryCache.bytes()-geometryCache.source_bytes());
  twv::vita::telemetryEvent("gfx_geometry_cache_memory_trims",s.memoryTrims);
  twv::vita::telemetryEvent("gfx_vertex_cpu_staging_capacity",vertexStaging.bytes());
#if TWV_VITA_VERTEX_WORK_PROBE
  vertexWorkProbe.report([](const char* name,uint64_t value){twv::vita::telemetryEvent(name,value);});
#endif
}

LightPlan gpu_light_plan(const Pipeline& p) noexcept {
  if(!p.gpuVertex || !p.info.lightingEnabled)return {};
  uint32_t masks[4]{};unsigned enabled=0;
  for(unsigned ch=0;ch<4;++ch) {
    masks[ch]=uint32_t(g_gxState.colorChannelState[ch].lightMask.to_ulong());
    if(p.info.sampledColorChannels.test(ch%2) && p.config.shaderConfig.colorChannels[ch].lightingEnabled)
      enabled|=1u<<ch;
  }
#if TWV_VITA_COMPACT_LIGHTS && TWV_VITA_ZERO_COLOR_LIGHTS
  remove_zero_color_lights(masks,g_gxState.lights);
#endif
  auto plan=compact_light_plan(masks,enabled);
#if !TWV_VITA_COMPACT_LIGHTS
  plan.count=8;
  for(unsigned i=0;i<8;++i)plan.indices[i]=i;
  for(unsigned ch=0;ch<4;++ch)plan.masks[ch]=(enabled&(1u<<ch))?masks[ch]:0;
#endif
  return plan;
}

void gpu_vertex_uniforms(TwvVitaDrawState& out,const Pipeline& p,unsigned pnSlots,const LightPlan& lights) noexcept {
  static_assert(aurora::gx::MaxPnMtx==10);
  out.pn_vectors=pnSlots*3;
  pack_matrix_palette(out.pn_pos,out.pn_nrm,g_gxState.pnMtx,pnSlots,p.info.lightingEnabled && lights.count);
  out.light_count=lights.count;
  for(unsigned i=0;i<lights.count;++i) for(unsigned k=0;k<4;++k) {
    const auto& light=g_gxState.lights[lights.indices[i]];
    out.light_pos[i*4+k]=light.pos[k]; out.light_dir[i*4+k]=light.dir[k];
    out.light_color[i*4+k]=light.color[k]; out.light_cos[i*4+k]=light.cosAtt[k];
    out.light_dist[i*4+k]=light.distAtt[k];
  }
  for(unsigned ch=0;ch<4;++ch) {
    const auto& channel=g_gxState.colorChannelState[ch];
    for(unsigned k=0;k<4;++k) {
      out.channel_mat[ch*4+k]=channel.matColor[k];
      if(p.info.lightingEnabled)out.channel_amb[ch*4+k]=channel.ambColor[k];
    }
    if(p.info.lightingEnabled && lights.count)pack_light_mask(out.light_masks+ch*8,lights.masks[ch]);
  }
  if(p.gpuTexgen) {
    out.texgen_vectors=p.texgenVectors;
    for(unsigned tc=0;tc<8;++tc) {
      const auto& g=p.config.shaderConfig.tcgs[tc];
      if(p.texgenRows[tc]>=0) {
        const unsigned index=unsigned(g.mtx)/3;
        const auto& matrix=index<aurora::gx::MaxPnMtx ? g_gxState.pnMtx[index].pos : g_gxState.texMtxs[index-aurora::gx::MaxPnMtx];
        pack_matrix_rows(out.texgen_rows+p.texgenRows[tc]*4,matrix);
      }
      if(p.postRows[tc]>=0)pack_matrix_rows(out.texgen_rows+p.postRows[tc]*4,g_gxState.ptTexMtxs[(g.postMtx-GX_PTTEXMTX0)/3]);
    }
  }
}
} // namespace twv::vgl
