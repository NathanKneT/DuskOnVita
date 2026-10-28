// Game/Aurora domain only. Link wrappers observe original calls, unchanged order
// and return values. Inclusive timers overlap graphics stage timers: do not sum.
#include "gx/command_processor.hpp"
#include "profile_scope.hpp"
#include "runtime/simulation_probe.hpp"
#include "runtime/telemetry.hpp"
#if TWV_VITA_DECODE_PROBE
#include "decode_probe.hpp"
#include "gfx/recording.hpp"
#include "gx/gx.hpp"
#include "gx/pipeline.hpp"
#include "runtime/telemetry.hpp"
#include <bit>
#include <span>
#include <string>
#include <utility>
namespace {
twv::vgl::DecodeProbe sDecode;
// FIFO-driven calls only: RmlUi also pushes vertices, from the game thread between frames.
inline void decode_mark(twv::vgl::DecodeClass c) noexcept {
    if(!sDecode.inFifo)return;
    ++sDecode.calls[c];
    if(sDecode.wants(c))sDecode.mark_at(c,twv_vgl_time_us());
}
}
#endif

using aurora::gx::fifo::ProcessResult;
extern "C" ProcessResult __real__ZN6aurora2gx4fifo7processEPKhj(const uint8_t*,uint32_t) noexcept;
extern "C" ProcessResult __wrap__ZN6aurora2gx4fifo7processEPKhj(const uint8_t* data,uint32_t size) noexcept {
    twv::vgl::ProfileScope timer(TWV_PROFILE_FIFO);
#if TWV_VITA_DECODE_PROBE
    sDecode.inFifo=true; // the outermost call: static lists are processed by an unwrapped inner call
    if(sDecode.sampled) {
        const uint64_t start=twv_vgl_time_us();
        sDecode.fifo_begin(start);
        const ProcessResult result=__real__ZN6aurora2gx4fifo7processEPKhj(data,size);
        const uint64_t end=twv_vgl_time_us();
        sDecode.fifo_end(end);
        sDecode.fifo(end-start);
        sDecode.inFifo=false;
        return result;
    }
    const ProcessResult result=__real__ZN6aurora2gx4fifo7processEPKhj(data,size);
    sDecode.inFifo=false;
    return result;
#else
    return __real__ZN6aurora2gx4fifo7processEPKhj(data,size);
#endif
}
#if TWV_VITA_DECODE_PROBE
// GX decode breakdown (decode_probe.hpp): every wrap below runs on the FIFO's thread during process().
extern "C" void __real__ZN6aurora3gfx17push_draw_commandINS_2gx8DrawDataEEEvT_(aurora::gx::DrawData);
extern "C" void __wrap__ZN6aurora3gfx17push_draw_commandINS_2gx8DrawDataEEEvT_(aurora::gx::DrawData data) {
    if(!sDecode.timing()) {
        if(sDecode.inFifo)++sDecode.calls[twv::vgl::DecBackend];
        __real__ZN6aurora3gfx17push_draw_commandINS_2gx8DrawDataEEEvT_(std::move(data));
        return;
    }
    ++sDecode.calls[twv::vgl::DecBackend];
    const uint64_t start=twv_vgl_time_us();
    sDecode.mark_at(twv::vgl::DecBackend,start);
    __real__ZN6aurora3gfx17push_draw_commandINS_2gx8DrawDataEEEvT_(std::move(data));
    const uint64_t end=twv_vgl_time_us();
    sDecode.draw(end-start);
    sDecode.mark_at(twv::vgl::DecDrawParse,end); // back in the draw's parsing
}
extern "C" void __real_twv_gfx_static_dl(const uint8_t*,uint32_t,uint32_t) noexcept;
extern "C" void __wrap_twv_gfx_static_dl(const uint8_t* list,uint32_t size,uint32_t id) noexcept {
    const bool timing=sDecode.timing();
    const uint64_t now=timing?twv_vgl_time_us():0;
    if(list) {
        if(sDecode.inFifo)++sDecode.calls[twv::vgl::DecList];
        sDecode.list_enter(size,now);
        if(timing)sDecode.mark_at(twv::vgl::DecList,now);
    } else {
        sDecode.list_leave(now);
        if(timing)sDecode.mark_at(twv::vgl::DecOther,now); // back in the command loop
    }
    __real_twv_gfx_static_dl(list,size,id);
}
extern "C" void __real__ZN6aurora2gx4fifo9handle_bpEj(uint32_t) noexcept;
extern "C" void __wrap__ZN6aurora2gx4fifo9handle_bpEj(uint32_t value) noexcept {
    decode_mark(twv::vgl::DecBp);
    __real__ZN6aurora2gx4fifo9handle_bpEj(value);
}
extern "C" void __real__ZN6aurora2gx4fifo9handle_cpEhj(uint8_t,uint32_t) noexcept;
extern "C" void __wrap__ZN6aurora2gx4fifo9handle_cpEhj(uint8_t addr,uint32_t value) noexcept {
    decode_mark(twv::vgl::DecCp);
    __real__ZN6aurora2gx4fifo9handle_cpEhj(addr,value);
}
extern "C" void __real__ZN6aurora2gx4fifo9handle_xfEtSt4spanIKhLj4294967295EE(uint16_t,std::span<const uint8_t>) noexcept;
extern "C" void __wrap__ZN6aurora2gx4fifo9handle_xfEtSt4spanIKhLj4294967295EE(uint16_t addr,std::span<const uint8_t> data) noexcept {
    decode_mark(twv::vgl::DecXf);
    __real__ZN6aurora2gx4fifo9handle_xfEtSt4spanIKhLj4294967295EE(addr,data);
}
extern "C" bool __real__ZN6aurora2gx4fifo12copy_xf_dataEjPKhjSt6endian(uint32_t,const uint8_t*,uint32_t,std::endian) noexcept;
extern "C" bool __wrap__ZN6aurora2gx4fifo12copy_xf_dataEjPKhjSt6endian(uint32_t addr,const uint8_t* data,uint32_t len,std::endian e) noexcept {
    decode_mark(twv::vgl::DecXf); // indexed matrix loads (LOAD_INDX); handle_xf's own calls are not wrapped
    return __real__ZN6aurora2gx4fifo12copy_xf_dataEjPKhjSt6endian(addr,data,len,e);
}
extern "C" void __real_twv_gfx_array_base(uint32_t) noexcept;
extern "C" void __wrap_twv_gfx_array_base(uint32_t attr) noexcept {
    decode_mark(twv::vgl::DecCp);
    __real_twv_gfx_array_base(attr);
}
extern "C" aurora::gfx::Range __real__ZN6aurora3gfx10push_vertsEPKhjj(const uint8_t*,size_t,size_t);
extern "C" aurora::gfx::Range __wrap__ZN6aurora3gfx10push_vertsEPKhjj(const uint8_t* data,size_t size,size_t align) {
    decode_mark(twv::vgl::DecDrawParse);
    return __real__ZN6aurora3gfx10push_vertsEPKhjj(data,size,align);
}
extern "C" aurora::gfx::Range __real__ZN6aurora3gfx12push_indicesEPKhjj(const uint8_t*,size_t,size_t);
extern "C" aurora::gfx::Range __wrap__ZN6aurora3gfx12push_indicesEPKhjj(const uint8_t* data,size_t size,size_t align) {
    decode_mark(twv::vgl::DecDrawParse);
    return __real__ZN6aurora3gfx12push_indicesEPKhjj(data,size,align);
}
// Every draw asks for the bind generation right after its pipeline block: texture resolution, bind
// groups and uniforms follow, up to the backend draw.
extern "C" uint64_t __real__ZN6aurora2gx7texture23current_bind_generationEv() noexcept;
extern "C" uint64_t __wrap__ZN6aurora2gx7texture23current_bind_generationEv() noexcept {
    decode_mark(twv::vgl::DecDrawSetup);
    return __real__ZN6aurora2gx7texture23current_bind_generationEv();
}
// populate_pipeline_config and pipeline_ref are wrapped by pipeline_fast.cpp, which marks them here
// and, on timed frames, also times the two calls themselves.
extern "C" void twv_decode_probe_mark(unsigned kind) noexcept {
    decode_mark(static_cast<twv::vgl::DecodeClass>(kind));
}
extern "C" bool twv_decode_probe_timing() noexcept {return sDecode.timing();}
// Game thread, after the FIFO drain (aurora_vita.cpp end_frame).
extern "C" void twv_decode_probe_frame_end() noexcept {
    sDecode.frame_end();
    if(sDecode.frames%60)return;
    static long long clockNs=-1; // cost of one clock read: each timed class change pays one
    if(clockNs<0) {
        constexpr int Reads=256;
        const uint64_t start=twv_vgl_time_us();
        for(int i=0;i<Reads;++i)(void)twv_vgl_time_us();
        clockNs=(long long)((twv_vgl_time_us()-start)*1000/(Reads+1));
    }
    const std::pair<const char*,uint64_t> receipts[]={
        {"gfx_decode_probe_frames",sDecode.frames},{"gfx_decode_probe_sampled_frames",sDecode.sampledFrames},
        {"gfx_decode_probe_fifo_us",sDecode.fifoUs},{"gfx_decode_probe_draw_us",sDecode.drawUs},
        {"gfx_decode_probe_static_list_us",sDecode.listUs},{"gfx_decode_probe_static_lists",sDecode.lists},
        {"gfx_decode_probe_static_list_bytes",sDecode.listBytes},{"gfx_decode_probe_nested_lists",sDecode.nestedLists},
        {"gfx_decode_probe_switches",sDecode.switches}};
    for(const auto& [name,value]:receipts)twv::vita::telemetryEvent(name,(long long)value);
    twv::vita::telemetryEvent("gfx_decode_probe_clock_ns",clockNs);
    for(unsigned c=0;c<twv::vgl::DecClassCount;++c) {
        const std::string base=std::string("gfx_decode_probe_")+twv::vgl::DecodeClassNames[c];
        twv::vita::telemetryEvent((base+"_us").c_str(),(long long)sDecode.classUs[c]);
        twv::vita::telemetryEvent((base+"_calls").c_str(),(long long)sDecode.calls[c]);
    }
}
#endif
extern "C" void __real__Z13fapGm_Executev();
extern "C" void __wrap__Z13fapGm_Executev() {
    ++twv::vita::simulationExecuteCalls;
    twv::vgl::ProfileScope timer(TWV_PROFILE_GAME_EXECUTE);
    __real__Z13fapGm_Executev();
}
// Inside the tick (fpcM_Management): actor Execute and Draw handlers, 3D particle calc, real-shadow
// ground polygon collection (cBgS::ShdwDraw, called only by d_drawlist.cpp realPolygonCheck, inside
// actor Draw) and the cutscene update (inside the scene's Execute). Pointer arguments as void*:
// same AAPCS registers and return values as the original declarations.
// The main-thread actor profile (runtime/actor_profile.cpp, game ABI) learns the tick's execute and
// draw phases and the painter from these wrappers, with the times they read anyway (end 0: begin).
extern "C" void twv_actprof_phase(unsigned phase,unsigned long long start,unsigned long long end)
    __attribute__((weak));
namespace {
struct PhaseScope {
    PhaseScope(uint32_t category,unsigned phase):category(category),phase(phase),start(twv_vgl_time_us()) {
        if(twv_actprof_phase)twv_actprof_phase(phase,start,0);
    }
    ~PhaseScope() {
        const uint64_t end=twv_vgl_time_us();
        twv_vgl_profile_add(category,end-start);
        if(twv_actprof_phase)twv_actprof_phase(phase,start,end);
    }
    PhaseScope(const PhaseScope&)=delete;
    PhaseScope& operator=(const PhaseScope&)=delete;
    uint32_t category;
    unsigned phase;
    uint64_t start;
};
}
extern "C" void __real__Z13fpcEx_HandlerPFiPvS_E(void*);
extern "C" void __wrap__Z13fpcEx_HandlerPFiPvS_E(void* queueFunc) {
    PhaseScope timer(TWV_PROFILE_TICK_EXECUTE,0);
    __real__Z13fpcEx_HandlerPFiPvS_E(queueFunc);
}
extern "C" int __real__Z13fpcDw_HandlerPFiPFiPvS_EES1_(void*,void*);
extern "C" int __wrap__Z13fpcDw_HandlerPFiPFiPvS_EES1_(void* iterHandler,void* func) {
    PhaseScope timer(TWV_PROFILE_TICK_DRAW,1);
    return __real__Z13fpcDw_HandlerPFiPFiPvS_EES1_(iterHandler,func);
}
extern "C" void __real__ZN13dPa_control_c6calc3DEv(void*);
extern "C" void __wrap__ZN13dPa_control_c6calc3DEv(void* self) {
    twv::vgl::ProfileScope timer(TWV_PROFILE_TICK_PARTICLE3D);
    __real__ZN13dPa_control_c6calc3DEv(self);
}
extern "C" void __real__ZN4cBgS8ShdwDrawEP13cBgS_ShdwDraw(void*,void*);
extern "C" void __wrap__ZN4cBgS8ShdwDrawEP13cBgS_ShdwDraw(void* self,void* shdw) {
    twv_vgl_profile_add(TWV_PROFILE_TICK_SHADOW_CALLS,1000);
    twv::vgl::ProfileScope timer(TWV_PROFILE_TICK_SHADOW_COLLECT);
    __real__ZN4cBgS8ShdwDrawEP13cBgS_ShdwDraw(self,shdw);
}
extern "C" int __real__ZN7dDemo_c6updateEv();
extern "C" int __wrap__ZN7dDemo_c6updateEv() {
    twv::vgl::ProfileScope timer(TWV_PROFILE_TICK_DEMO);
    return __real__ZN7dDemo_c6updateEv();
}
// Collision detail, timed only with full receipts (diagnostic runs): ground and line queries come
// by the hundred per tick, so free play (quiet receipts) pays one flag test per call instead.
extern "C" void __real__ZN9dBgS_Acch6CrrPosER4dBgS(void*,void*);
extern "C" void __wrap__ZN9dBgS_Acch6CrrPosER4dBgS(void* self,void* bgs) {
    if(twv::vita::telemetryIsQuiet()) {__real__ZN9dBgS_Acch6CrrPosER4dBgS(self,bgs);return;}
    twv::vgl::ProfileScope timer(TWV_PROFILE_TICK_BG_ACCH);
    __real__ZN9dBgS_Acch6CrrPosER4dBgS(self,bgs);
}
extern "C" float __real__ZN4cBgS11GroundCrossEP11cBgS_GndChk(void*,void*);
extern "C" float __wrap__ZN4cBgS11GroundCrossEP11cBgS_GndChk(void* self,void* chk) {
    if(twv::vita::telemetryIsQuiet())return __real__ZN4cBgS11GroundCrossEP11cBgS_GndChk(self,chk);
    twv_vgl_profile_add(TWV_PROFILE_TICK_BG_QUERIES,1);
    twv::vgl::ProfileScope timer(TWV_PROFILE_TICK_BG_GROUND);
    return __real__ZN4cBgS11GroundCrossEP11cBgS_GndChk(self,chk);
}
extern "C" bool __real__ZN4cBgS9LineCrossEP11cBgS_LinChk(void*,void*);
extern "C" bool __wrap__ZN4cBgS9LineCrossEP11cBgS_LinChk(void* self,void* chk) {
    if(twv::vita::telemetryIsQuiet())return __real__ZN4cBgS9LineCrossEP11cBgS_LinChk(self,chk);
    twv_vgl_profile_add(TWV_PROFILE_TICK_BG_QUERIES,1);
    twv::vgl::ProfileScope timer(TWV_PROFILE_TICK_BG_LINE);
    return __real__ZN4cBgS9LineCrossEP11cBgS_LinChk(self,chk);
}
extern "C" void __real__ZN4dCcS4MoveEv(void*);
extern "C" void __wrap__ZN4dCcS4MoveEv(void* self) {
    twv::vgl::ProfileScope timer(TWV_PROFILE_TICK_CC_MOVE);
    __real__ZN4dCcS4MoveEv(self);
}
extern "C" void __real__Z14mDoAud_Executev();
extern "C" void __wrap__Z14mDoAud_Executev() {
    twv::vgl::ProfileScope timer(TWV_PROFILE_GAME_AUDIO);
    __real__Z14mDoAud_Executev();
}
extern "C" void __real__Z15cAPIGph_Painterv();
extern "C" void __wrap__Z15cAPIGph_Painterv() {
    PhaseScope timer(TWV_PROFILE_PAINTER,2);
    __real__Z15cAPIGph_Painterv();
}
extern "C" void __real_VIWaitForRetrace();
extern "C" void __wrap_VIWaitForRetrace() {
    twv::vgl::ProfileScope timer(TWV_PROFILE_VI_CALLBACKS);
    __real_VIWaitForRetrace();
}
