#pragma once
#include <cstdint>
#include <string>
constexpr unsigned PAD_MAX_CONTROLLERS=4;
constexpr unsigned PAD_BUTTON_A=0x100,PAD_BUTTON_B=0x200,PAD_TRIGGER_L=0x40,PAD_TRIGGER_R=0x20;
constexpr int PAD_ERR_NONE=0,PAD_ERR_NO_CONTROLLER=-1,fpcNm_ALINK_e=1;
struct PADStatus {uint16_t button{};int8_t stickX{},stickY{},substickX{},substickY{};
    uint8_t triggerLeft{},triggerRight{};int err{};};
struct Capsule {bool ChkAtHit(){return false;}void* GetAtHitAc(){return nullptr;}bool ChkAtShieldHit(){return false;}};
struct daAlink_c {enum {PROC_WAIT=0};int mProcID=PROC_WAIT;Capsule mAtCps[3];};
inline daAlink_c* dComIfGp_getPlayer(int){static daAlink_c actor;return &actor;}
inline int fopAcM_GetProfName(daAlink_c*){return fpcNm_ALINK_e;}
inline int fopAcM_GetID(daAlink_c*){return 1;}
inline bool fpcM_IsCreating(int){return false;}
inline bool fopOvlpM_IsDoingReq(){return false;}
inline bool dComIfGp_event_runCheck(){return false;}
inline const char* dComIfGp_getStartStageName(){return "F_SP108";}
inline int dComIfGp_roomControl_getStayNo(){return 6;}
inline int dComIfGs_getLife(){return 12;}
struct JUTFader {enum {Wait};int getStatus() const{return Wait;}};
struct mDoGph_gInf_c {static JUTFader* getFader(){return nullptr;}};
namespace dusk {struct Requested {bool set=true;std::string stage="F_SP108";int room=6,point=1,layer=0;};inline Requested StageRequested;}
void item_func_SWORD();void item_func_WOOD_SHIELD();void item_func_WEAR_KOKIRI();void dComIfGs_setTime(float);
