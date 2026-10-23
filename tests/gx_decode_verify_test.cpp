// Independent differential host oracle: every decoder/parser body is generated
// verbatim from pinned real Aurora sources, never reproduced in this test.
#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#include <dolphin/gx.h>
#include <dolphin/gd/GDGeometry.h>
#include <aurora/math.hpp>

namespace oracle {
std::vector<std::string> effects;
void (*fatalReceipt)() = nullptr;
template<class... A> void effect(const char* text, const A&... args) {
 std::ostringstream out; out << text;
 [[maybe_unused]] auto item=[&](const auto& arg) {out << '|';if constexpr(std::is_arithmetic_v<std::decay_t<decltype(arg)>> || std::is_enum_v<std::decay_t<decltype(arg)>>) out << +arg;else out << arg;};
 (item(args), ...);
 effects.push_back(out.str());
}
}
namespace aurora {
struct Module {
 const char* name;
 template<class... A> void debug(const char* text,const A&... args) const { oracle::effect(text,args...); }
 template<class... A> void warn(const char* text,const A&... args) const { oracle::effect(text,args...); }
 template<class... A> void error(const char* text,const A&... args) const { oracle::effect(text,args...); }
 template<class... A> [[noreturn]] void fatal(const char* text,const A&... args) const {
  oracle::effect(text,args...); if(oracle::fatalReceipt) oracle::fatalReceipt(); std::_Exit(86);
 }
};
}
// fmt is used only for the original parser's unsupported-opcode diagnostic.
namespace fmt { template<class A> std::string format(const char* text,const A& value) {
 std::ostringstream out;if(std::string_view(text)=="[{:02x}]") out << '[';else out << ' ';
 out << std::hex << std::setw(2) << std::setfill('0') << +value;
 if(std::string_view(text)=="[{:02x}]") out << ']';
 return out.str();
} }
#define AURORA_ASSERT(cond, ...) do { if(!(cond)) Log.fatal(__VA_ARGS__); } while(false)
#define CHECK(cond, ...) AURORA_ASSERT(cond, __VA_ARGS__)
#define FATAL(...) Log.fatal(__VA_ARGS__)
#define LIKELY [[likely]]
#define UNLIKELY [[unlikely]]
#define ZoneScoped
#define ZoneScopedN(x)
#include "gx_decode_types.inc"

namespace aurora::gx { GXState g_gxState{},g_gxStateRef{},g_gxStateGuard{}; }
namespace oracle {
aurora::gx::GXState* active = &aurora::gx::g_gxState;
bool fastEnabled = true;
unsigned comparisons = 0, deaths = 0;
}
extern "C" int twv_vita_gx_fast_enabled() noexcept { return oracle::fastEnabled; }

// Only external/platform effects are seams. View-state mapping intentionally
// uses a fixed identity target; mapping correctness is outside this oracle.
namespace aurora::gx {
void set_render_viewport(const gfx::Viewport& v) noexcept {
 auto& s=*oracle::active;
 if(v.left!=s.renderViewport.left || v.width!=s.renderViewport.width || v.height!=s.renderViewport.height) s.dirty|=DirtyUniform;
 s.renderViewport=v; oracle::effect("viewport");
}
void set_logical_viewport(const gfx::Viewport& v) noexcept {
 auto& s=*oracle::active;
 if(v.left!=s.logicalViewport.left || v.width!=s.logicalViewport.width || v.height!=s.logicalViewport.height) s.dirty|=DirtyUniform;
 s.logicalViewport=v; set_render_viewport(v);
}
void set_render_scissor(const gfx::ClipRect& v) noexcept { oracle::active->renderScissor=v;oracle::effect("scissor"); }
void set_logical_scissor(const gfx::ClipRect& v) noexcept { oracle::active->logicalScissor=v;set_render_scissor(v); }
void copy_tex(const void* dest,GXBool clear) noexcept { oracle::effect("copy_tex",reinterpret_cast<uintptr_t>(dest),clear); }
void evict_texture_object(u32 id) noexcept { oracle::effect("evict_tex",id); }
void evict_tlut_object(u32 id) noexcept { oracle::effect("evict_tlut",id); }
void evict_copy_texture(const void* ptr) noexcept { oracle::effect("evict_copy",reinterpret_cast<uintptr_t>(ptr)); }
void pop_debug_group() noexcept { oracle::effect("debug_pop"); }
namespace texture { void invalidate_bindings() noexcept { oracle::effect("invalidate_bindings"); } }
}
namespace aurora::gfx {
namespace depth_peek { void request_snapshot() noexcept { oracle::effect("depth_snapshot"); } }
void begin_offscreen(u32 w,u32 h) noexcept { oracle::effect("begin_offscreen",w,h); }
void end_offscreen() noexcept { oracle::effect("end_offscreen"); }
Range push_indices(const u8*,size_t size,u32 align) noexcept { oracle::effect("push_indices",size,align);return {0,static_cast<u32>(size)}; }
Range push_verts(const u8*,size_t size,u32 align) noexcept { oracle::effect("push_verts",size,align);return {0,static_cast<u32>(size)}; }
void push_debug_group(std::string) noexcept { oracle::effect("debug_push"); }
void insert_debug_marker(std::string) noexcept { oracle::effect("debug_marker"); }
}

// This boundary is deliberately fatal if a fixture accidentally tries to
// validate a draw using a dummy geometry/shader implementation.
#define PROCESS_SEAMS(ns) \
 namespace aurora::gx::ns { \
 struct ProcessResult { uint32_t bytesProcessed; bool drawDone; }; \
 static void handle_draw(u8,ByteReader&) noexcept { std::_Exit(87); } \
 static u32 calc_vtx_size(GXVtxFmt) noexcept { std::_Exit(87); } \
 static void draw_prim(GXPrimitive,GXVtxFmt,u16,ByteReader&) noexcept { std::_Exit(87); } \
 static void push_gx_draw(GXPrimitive,GXVtxFmt,u16,gfx::Range,gfx::Range,u32) noexcept { std::_Exit(87); } \
 }
PROCESS_SEAMS(fifo)
PROCESS_SEAMS(fifo_reference)
PROCESS_SEAMS(fifo_guard)

#define TWV_GX_DECODE_REF 0
#include "gx_decode_regs.inc"
#include "gx_decode_process.inc"
#undef TWV_GX_DECODE_REF

#define fifo fifo_reference
#define g_gxState g_gxStateRef
#include "gx_decode_regs_pristine.inc"
#include "gx_decode_process_pristine.inc"
#undef g_gxState
#undef fifo

// Third decoder checks that TWV_GX_DECODE_REF really emits original behavior.
#define TWV_GX_DECODE_REF 1
#define fifo fifo_guard
#define g_gxState g_gxStateGuard
#include "gx_decode_regs.inc"
#include "gx_decode_process.inc"
#undef g_gxState
#undef fifo
#undef TWV_GX_DECODE_REF

#include "gx_decode_semantics.inc"
template<class A,A Default,class F> void semantic(const aurora::gx::TevPass<A,Default>& x,F&& f) { f("a",x.a);f("b",x.b);f("c",x.c);f("d",x.d); }
template<class T,class F> void semantic(const aurora::Vec2<T>& x,F&& f) { f("x",x.x);f("y",x.y); }
template<class T,class F> void semantic(const aurora::Vec4<T>& x,F&& f) { for(unsigned i=0;i<4;++i) f(std::to_string(i).c_str(),x[i]); }
template<class T,class F> void semantic(const aurora::Mat3x2<T>& x,F&& f) { f("m0",x.m0);f("m1",x.m1);f("m2",x.m2); }
template<class T,class F> void semantic(const aurora::Mat3x4<T>& x,F&& f) { f("m0",x.m0);f("m1",x.m1);f("m2",x.m2); }
template<class T,class F> void semantic(const aurora::Mat4x4<T>& x,F&& f) { f("m0",x.m0);f("m1",x.m1);f("m2",x.m2);f("m3",x.m3); }
template<class F> void semantic(const aurora::gfx::Viewport& x,F&& f) { f("left",x.left);f("top",x.top);f("width",x.width);f("height",x.height);f("znear",x.znear);f("zfar",x.zfar); }
template<class F> void semantic(const aurora::gfx::ClipRect& x,F&& f) { f("x",x.x);f("y",x.y);f("width",x.width);f("height",x.height); }
template<class F> void semantic(const aurora::gfx::Range& x,F&& f) { f("offset",x.offset);f("size",x.size); }
template<class F> void semantic(const GXTexObj_& x,F&& f) {
 f("mode0",x.mode0);f("mode1",x.mode1);f("image0",x.image0);f("image3",x.image3);f("userData",x.userData);f("data",x.data);f("mWidth",x.mWidth);f("mHeight",x.mHeight);f("mFormat",x.mFormat);f("tlut",x.tlut);f("texObjId",x.texObjId);f("texDataVersion",x.texDataVersion);f("flags",x.flags);
}
template<class F> void semantic(const GXTlutObj_& x,F&& f) { f("tlut",x.tlut);f("loadTlut0",x.loadTlut0);f("numEntries",x.numEntries);f("data",x.data);f("format",x.format);f("tlutObjId",x.tlutObjId);f("tlutDataVersion",x.tlutDataVersion);f("flags",x.flags); }

namespace oracle {
using Snapshot=std::vector<std::pair<std::string,uint64_t>>;
std::vector<std::string> semanticNames;
template<class T> void capture(Snapshot& out,const std::string& path,const T& value);
template<class T,size_t N> void capture(Snapshot& out,const std::string& path,const std::array<T,N>& value) { for(size_t i=0;i<N;++i) capture(out,path.empty()?std::string{}:path+"["+std::to_string(i)+"]",value[i]); }
template<size_t N> void capture(Snapshot& out,const std::string& path,const std::bitset<N>& value) { for(size_t i=0;i<N;++i) out.emplace_back(path.empty()?std::string{}:path+"["+std::to_string(i)+"]",value[i]); }
template<class T> void capture(Snapshot& out,const std::string& path,const T& value) {
 if constexpr(std::is_same_v<T,float>) out.emplace_back(path,std::bit_cast<uint32_t>(value));
 else if constexpr(std::is_pointer_v<T>) out.emplace_back(path,reinterpret_cast<uintptr_t>(value));
 // Hardware registers can contain reserved enum encodings. Compare their
 // integer representation without evaluating an invalid C++ enum value.
 else if constexpr(std::is_enum_v<T>) {std::underlying_type_t<T> bits;std::memcpy(&bits,&value,sizeof(bits));out.emplace_back(path,static_cast<uint64_t>(bits));}
 else if constexpr(std::is_arithmetic_v<T>) out.emplace_back(path,static_cast<uint64_t>(value));
 else semantic(value,[&](const char* field,const auto& item){capture(out,path.empty()?std::string{}:path+"."+field,item);});
}
Snapshot snapshot(const aurora::gx::GXState& s) {
 Snapshot out;out.reserve(8192);capture(out,semanticNames.empty()?"GXState":"",s);
 if(semanticNames.empty()) for(auto& [name,value]:out) {semanticNames.push_back(name);name.clear();(void)value;}
 return out;
}
void equal(const Snapshot& a,const Snapshot& b) {
 if(a!=b) {
  for(size_t i=0;i<std::min(a.size(),b.size());++i) if(a[i]!=b[i]) {std::fprintf(stderr,"GX semantic mismatch: %s %llx != %llx\n",semanticNames[i].c_str(),(unsigned long long)a[i].second,(unsigned long long)b[i].second);break;}
  std::abort();
 }
}
void reset() {
 aurora::gx::g_gxState=aurora::gx::GXState{};
 aurora::gx::g_gxStateRef=aurora::gx::GXState{};
 aurora::gx::g_gxStateGuard=aurora::gx::GXState{};
}
template<class F> auto execute(aurora::gx::GXState& state,F&& f) {
 active=&state;effects.clear();f();return std::make_pair(snapshot(state),effects);
}
template<class Fast,class Ref,class Guard> void compare(Fast&& fast,Ref&& ref,Guard&& guard) {
 // Clear only the draw-owned dirty receipt equally. Otherwise DirtyAll from
 // construction would hide a missing or spurious dirty transition forever.
 aurora::gx::g_gxState.dirty=aurora::gx::g_gxStateRef.dirty=aurora::gx::g_gxStateGuard.dirty=0;
 const auto a=execute(aurora::gx::g_gxState,fast),b=execute(aurora::gx::g_gxStateRef,ref),c=execute(aurora::gx::g_gxStateGuard,guard);
 equal(a.first,b.first);equal(c.first,b.first);assert(a.second==b.second && c.second==b.second);++comparisons;
}
void stream(const std::vector<u8>& data);
void append(std::vector<u8>& out,uint64_t value,unsigned bytes);
void bp(u32 word) { std::vector<u8> data{GX_LOAD_BP_REG};append(data,word,4);stream(data); }
void cp(u8 addr,u32 word) { std::vector<u8> data{GX_LOAD_CP_REG,addr};append(data,word,4);stream(data); }
void xf(u16 addr,const std::vector<u8>& payload) { assert(!payload.empty() && payload.size()%4==0);std::vector<u8> data{GX_LOAD_XF_REG};append(data,((payload.size()/4-1)<<16)|addr,4);data.insert(data.end(),payload.begin(),payload.end());stream(data); }
void copy(u32 addr,const std::vector<u8>& data,std::endian endian) { compare([&]{assert(aurora::gx::fifo::copy_xf_data(addr,data.data(),data.size()/4,endian));},[&]{assert(aurora::gx::fifo_reference::copy_xf_data(addr,data.data(),data.size()/4,endian));},[&]{assert(aurora::gx::fifo_guard::copy_xf_data(addr,data.data(),data.size()/4,endian));}); }
void append(std::vector<u8>& out,uint64_t value,unsigned bytes) { for(unsigned i=bytes;i;--i) out.push_back(static_cast<u8>(value>>((i-1)*8))); }
std::vector<u8> words(std::span<const u32> values,std::endian endian=std::endian::big) {std::vector<u8> out;for(auto v:values) {if(endian==std::endian::little) v=bswap(v);append(out,v,4);}return out;}
void stream(const std::vector<u8>& data) {
 std::array<std::pair<u32,bool>,3> result{};
 compare([&]{auto r=aurora::gx::fifo::process(data.data(),data.size());result[0]={r.bytesProcessed,r.drawDone};},[&]{auto r=aurora::gx::fifo_reference::process(data.data(),data.size());result[1]={r.bytesProcessed,r.drawDone};},[&]{auto r=aurora::gx::fifo_guard::process(data.data(),data.size());result[2]={r.bytesProcessed,r.drawDone};});
 assert(result[0]==result[1] && result[2]==result[1]);
}

int receiptFd=-1;
void write_receipt() {
 std::vector<u8> receipt;
 for(const auto& [name,v]:snapshot(*active)) {append(receipt,name.size(),8);receipt.insert(receipt.end(),name.begin(),name.end());append(receipt,v,8);}
 for(const auto& e:effects) {append(receipt,e.size(),8);receipt.insert(receipt.end(),e.begin(),e.end());}
 size_t done=0;while(done<receipt.size()) {const auto n=write(receiptFd,receipt.data()+done,receipt.size()-done);if(n<=0) std::_Exit(88);done+=n;}
}
std::pair<int,std::vector<u8>> death_run(const std::vector<u8>& bytes,unsigned mode) {
 int fds[2];assert(pipe(fds)==0);const pid_t pid=fork();assert(pid>=0);
 if(pid==0) {
  close(fds[0]);receiptFd=fds[1];fatalReceipt=write_receipt;reset();effects.clear();
  if(mode==1) {active=&aurora::gx::g_gxStateRef;aurora::gx::fifo_reference::process(bytes.data(),bytes.size());}
  else {active=&aurora::gx::g_gxState;fastEnabled=mode==2;aurora::gx::fifo::process(bytes.data(),bytes.size());}
  write_receipt();std::_Exit(0);
 }
 close(fds[1]);std::vector<u8> receipt;std::array<u8,4096> chunk{};
 for(;;) {const auto n=read(fds[0],chunk.data(),chunk.size());assert(n>=0);if(!n) break;receipt.insert(receipt.end(),chunk.begin(),chunk.begin()+n);}
 close(fds[0]);int status=0;assert(waitpid(pid,&status,0)==pid && WIFEXITED(status));assert(!receipt.empty());return {WEXITSTATUS(status),receipt};
}
void truncated(const std::vector<u8>& command) {
 // Prefix a real state write so fatal diagnostics must retain absolute offsets.
 const std::vector<u8> prefix{GX_LOAD_BP_REG,0xe0,0,0,7};
 for(size_t n=1;n<command.size();++n) {
  auto data=prefix;data.insert(data.end(),command.begin(),command.begin()+n);
  const auto ref=death_run(data,1),off=death_run(data,0),on=death_run(data,2);
  assert(ref.first==86 && off==ref && on==ref);++deaths;
 }
}
}

int main() {
 using namespace oracle;using namespace aurora::gx;
 static_assert(sizeof(u32)==4 && sizeof(aurora::Mat3x4<float>)==48);
 for(bool enabled:{false,true}) {
  fastEnabled=enabled;reset();
  // Every BP/CP register, valid-bit first write, unchanged repeat, and mask
  // merging. All 8-bit normalized colours and signed 11-bit TEV values.
  for(unsigned reg=0;reg<256;++reg) for(u32 v:{0u,0x00ffffffu,0x0055aa11u}) {bp((reg<<24)|v);bp((reg<<24)|v);}
  for(unsigned v=0;v<256;++v) {bp(0x4f000000u|v|(v<<8));bp(0xe0800000u|v|(v<<12));xf(0x100a,words(std::array<u32,1>{v*0x01010101u}));}
  for(unsigned v=0;v<2048;++v) bp(0xe0000000u|v|(v<<12));
  for(unsigned reg=0;reg<256;++reg) for(u32 v:{0u,0xffffffffu,0x31415926u}) {cp(reg,v);cp(reg,v);}
  for(u8 reg:{0x50,0x60,0x70,0x77,0x80,0x87,0x90,0x97}) {
   cp(reg,0x12345678u);
   g_gxState.lastVtxFmt=g_gxStateRef.lastVtxFmt=g_gxStateGuard.lastVtxFmt=GX_VTXFMT0;
   g_gxState.lastVtxSize=g_gxStateRef.lastVtxSize=g_gxStateGuard.lastVtxSize=91;
   cp(reg,0x12345678u);assert(g_gxState.lastVtxFmt==GX_VTXFMT0 && g_gxState.dirty==0);
   cp(reg,0x12345679u);assert(g_gxState.lastVtxFmt==GX_MAX_VTXFMT);
  }
  bp(0xfe00000fu);bp(0x410abcdeu);assert(g_gxState.bpRegCache[0xfe]==0xffffffu);
  // TLUT trigger must refresh slot state even after the register repeats.
  bp(0x64001234u);bp(0x65001400u);bp(0x64005678u);bp(0x65001400u);assert(g_gxState.loadedTluts[0].loadTlut0==0x64005678u);
  bp(0x52000800u);bp(0x52000800u);assert(effects.size()==1 && effects[0].find("copy_tex")==0);
  bp(0x52004800u);assert(effects.size()==1 && effects[0].find("STUB: display copy")==0);
  // XF registers with valid safe texture-matrix indices, invalidation aliases,
  // full/partial projection/viewport and out-of-range shadows.
  for(unsigned reg=0;reg<0x70;++reg) for(u32 v:{0u,0x11223344u}) {
   if(reg==0x18 || reg==0x19) v=0;
   xf(0x1000+reg,words(std::array<u32,1>{v}));xf(0x1000+reg,words(std::array<u32,1>{v}));
  }
  cp(0x30,7);xf(0x1018,words(std::array<u32,1>{0}));assert(!g_gxState.cpRegValid[0x30]);
  bp(0);xf(0x1009,words(std::array<u32,1>{2}));assert(!g_gxState.bpRegValid[0]);
  const std::array<u32,12> bits{0u,0x80000000u,0x3f800000u,0xbf800000u,0x7f800000u,0xff800000u,0x7fc12345u,0x7fa54321u,1u,0x807fffffu,0x7fffffffu,0xffffffffu};
  for(auto endian:{std::endian::little,std::endian::big}) for(unsigned slot=0;slot<10;++slot) {
   copy(slot*12,words(bits,endian),endian);copy(slot*12,words(bits,endian),endian);
   copy(0x78+slot*12,words(bits,endian),endian);copy(0x78+slot*12,words(std::span(bits).first(8),endian),endian);
   copy(0x400+slot*9,words(std::span(bits).first(9),endian),endian);
   copy(0x500+slot*12,words(bits,endian),endian);
  }
  for(unsigned slot=10;slot<20;++slot) copy(0x500+slot*12,words(bits),std::endian::big);
  for(auto endian:{std::endian::little,std::endian::big}) for(unsigned slot=0;slot<8;++slot) for(unsigned offset=0;offset<16;++offset) {
   const auto block=words(bits,endian);std::vector<u8> light((16-offset)*4);
   for(size_t i=0;i<light.size();++i) light[i]=block[i%block.size()];
   copy(0x600+slot*16+offset,light,endian);copy(0x600+slot*16+offset,light,endian);
  }
  for(unsigned offset=0;offset<2;++offset) {
   auto view=std::vector<u32>{0x43a00000u,0xc3700000u,0x4b7fffffu,0x43aa0000u,0x43aa0000u,0x4b7fffffu};
   if(offset) view.insert(view.begin(),3);
   xf(0x101a-offset,words(view));xf(0x101a-offset,words(view));
   for(u32 type:{0u,1u}) {auto projection=std::vector<u32>(bits.begin(),bits.begin()+6);projection.push_back(type);xf(0x1020,words(projection));xf(0x1020,words(projection));}
  }
  // Finite viewport/projection checks include block routing within longer XF
  // writes and preserve the original behavior for unsupported partial blocks.
  xf(0x1020,words(std::array<u32,7>{0x3f800000u,0,0x3f800000u,0,0x3f800000u,0,1}));
  assert(g_gxState.proj.m3[3]==1.f && g_gxState.proj.m3[2]==0.f);
  xf(0x1020,words(std::array<u32,7>{0x3f800000u,0,0x3f800000u,0,0x3f800000u,0,1}));assert(g_gxState.dirty==0);
  for(u32 pattern:bits) {
   const std::array<u32,6> view{pattern,pattern,pattern,pattern,pattern,pattern};
   xf(0x101a,words(view));xf(0x101a,words(view));
   for(u32 type:{0u,1u}) {auto projection=std::vector<u32>(view.begin(),view.end());projection.push_back(type);xf(0x1020,words(projection));xf(0x1020,words(projection));}
  }
  // ARRAYBASE aliases, same-pointer size/endian changes, stale upload-range
  // reset and no-op preservation; no cachedRange exclusions in host proof.
  for(unsigned alias=0;alias<16;++alias) for(u32 size:{0u,16u,0xffffffffu}) for(u8 le:{0u,1u,2u}) {
   const auto idx=GX_VA_POS+alias;g_gxState.arrays[idx].cachedRange=g_gxStateRef.arrays[idx].cachedRange=g_gxStateGuard.arrays[idx].cachedRange={17,31};
   std::vector<u8> command{GX_AURORA};append(command,GX_AURORA_LOAD_ARRAYBASE+alias,2);append(command,0x12345678u+alias,8);append(command,size,4);append(command,le,1);stream(command);
   g_gxState.arrays[idx].cachedRange=g_gxStateRef.arrays[idx].cachedRange=g_gxStateGuard.arrays[idx].cachedRange={19,37};
   stream(command);assert(g_gxState.arrays[idx].cachedRange.offset==19 && g_gxState.arrays[idx].cachedRange.size==37 && g_gxState.dirty==0);
  }
  // Real indexed-load command path reads actual live source bytes.
  const auto source=words(bits);for(unsigned kind=0;kind<4;++kind) for(bool le:{false,true}) {
   std::vector<u8> base{GX_AURORA};append(base,GX_AURORA_LOAD_ARRAYBASE+GX_POS_MTX_ARRAY-GX_VA_POS+kind,2);append(base,reinterpret_cast<uintptr_t>(source.data()),8);append(base,source.size(),4);append(base,le,1);stream(base);
   cp(0xb0+GX_POS_MTX_ARRAY-GX_VA_POS+kind,0);
   std::vector<u8> cmd{static_cast<u8>(GX_LOAD_INDX_A+kind*8)};append(cmd,0,2);append(cmd,(11u<<12),2);stream(cmd);
  }
  // DRAWDONE must terminate at exactly its own word, even when repeated.
  stream({GX_LOAD_BP_REG,GX_BP_REG_DRAWDONE,0,0,0,0xff});stream({GX_LOAD_BP_REG,GX_BP_REG_DRAWDONE,0,0,0,0xff});
  stream({0,1,2,3,4,5,6,7});stream({GX_CMD_INVL_VC});stream({GX_CMD_CALL_DL,0,0,0,0,0,0,0,0});
  for(unsigned alias=0;alias<8;++alias) {
   std::vector<u8> mixed{static_cast<u8>(0x60+alias)};append(mixed,0xfe0000ffu,4);
   mixed.push_back(0x61);append(mixed,0xe000007fu,4);
   mixed.push_back(static_cast<u8>(GX_LOAD_CP_REG+alias));mixed.push_back(0x70);append(mixed,0x31415926u,4);
   mixed.push_back(static_cast<u8>(GX_LOAD_XF_REG+alias));append(mixed,0x100au,4);append(mixed,0x89abcdefu,4);
   mixed.push_back(GX_CMD_INVL_VC);mixed.push_back(0);stream(mixed);
  }
 }
 reset();
 // Comparator self-tests: semantic mutation detected; padding mutation ignored.
 auto before=snapshot(g_gxState);g_gxState.tevStages[0].colorOp._p1=99;equal(before,snapshot(g_gxState));g_gxState.dirty^=DirtyUniform;assert(before!=snapshot(g_gxState));
 g_gxState.proj.m0[0]=std::bit_cast<float>(0x80000000u);assert(snapshot(g_gxState)!=snapshot(g_gxStateRef));
 g_gxState.proj.m0[0]=std::bit_cast<float>(0x7fc12345u);g_gxStateRef.proj.m0[0]=std::bit_cast<float>(0x7fc12346u);assert(snapshot(g_gxState)!=snapshot(g_gxStateRef));
 truncated({GX_LOAD_BP_REG,0x52,0,8,0});truncated({GX_LOAD_CP_REG,0x70,0,0,0,0});
 std::vector<u8> xfCommand{GX_LOAD_XF_REG,0,11,0,0};auto payload=words(std::array<u32,12>{});xfCommand.insert(xfCommand.end(),payload.begin(),payload.end());truncated(xfCommand);
 std::vector<u8> arrayCommand{GX_AURORA};append(arrayCommand,GX_AURORA_LOAD_ARRAYBASE,2);append(arrayCommand,0,8);append(arrayCommand,48,4);append(arrayCommand,1,1);truncated(arrayCommand);
 for(u8 cmd:{GX_LOAD_INDX_A,GX_LOAD_INDX_B,GX_LOAD_INDX_C,GX_LOAD_INDX_D}) {
  const std::vector<u8> data{cmd,0,0,0xb0,0};truncated(data);
  const auto ref=death_run(data,1);assert(ref.first==86 && death_run(data,0)==ref && death_run(data,2)==ref);++deaths;
 }
 std::printf("gx_decode_verify_test: compiled=%d, %u semantic three-way comparisons, %u truncation/death equivalence cases; pristine/runtime OFF/runtime ON/REF; recorded I/O; no draw or GPU claims\n",TWV_VITA_GX_FAST_DECODE,comparisons,deaths);
}
