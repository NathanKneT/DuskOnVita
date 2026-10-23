// Game/Aurora domain only. Link wrappers observe original calls, unchanged order
// and return values. Inclusive timers overlap graphics stage timers: do not sum.
#include "gx/command_processor.hpp"
#include "profile_scope.hpp"
#include "runtime/simulation_probe.hpp"

using aurora::gx::fifo::ProcessResult;
extern "C" ProcessResult __real__ZN6aurora2gx4fifo7processEPKhj(const uint8_t*,uint32_t) noexcept;
extern "C" ProcessResult __wrap__ZN6aurora2gx4fifo7processEPKhj(const uint8_t* data,uint32_t size) noexcept {
    twv::vgl::ProfileScope timer(TWV_PROFILE_FIFO);
    return __real__ZN6aurora2gx4fifo7processEPKhj(data,size);
}
extern "C" void __real__Z13fapGm_Executev();
extern "C" void __wrap__Z13fapGm_Executev() {
    ++twv::vita::simulationExecuteCalls;
    twv::vgl::ProfileScope timer(TWV_PROFILE_GAME_EXECUTE);
    __real__Z13fapGm_Executev();
}
extern "C" void __real__Z14mDoAud_Executev();
extern "C" void __wrap__Z14mDoAud_Executev() {
    twv::vgl::ProfileScope timer(TWV_PROFILE_GAME_AUDIO);
    __real__Z14mDoAud_Executev();
}
extern "C" void __real__Z15cAPIGph_Painterv();
extern "C" void __wrap__Z15cAPIGph_Painterv() {
    twv::vgl::ProfileScope timer(TWV_PROFILE_PAINTER);
    __real__Z15cAPIGph_Painterv();
}
extern "C" void __real_VIWaitForRetrace();
extern "C" void __wrap_VIWaitForRetrace() {
    twv::vgl::ProfileScope timer(TWV_PROFILE_VI_CALLBACKS);
    __real_VIWaitForRetrace();
}
