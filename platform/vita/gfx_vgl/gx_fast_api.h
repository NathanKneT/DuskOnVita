#pragma once
// Plain C types only across game/int-enum and SDK/short-enum translation units.
// Serialized main/FIFO context: select before game_main; first use locks choice.
// Default OFF. A fault disables the group permanently for this process.
#ifdef __cplusplus
extern "C" {
#define TWV_GX_NOEXCEPT noexcept
#else
#define TWV_GX_NOEXCEPT
#endif
int twv_vita_gx_fast_select(int enabled) TWV_GX_NOEXCEPT;
int twv_vita_gx_fast_enabled(void) TWV_GX_NOEXCEPT;
void twv_vita_gx_fast_disable(unsigned reason) TWV_GX_NOEXCEPT;
void twv_vita_gx_fast_report(void) TWV_GX_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#undef TWV_GX_NOEXCEPT
