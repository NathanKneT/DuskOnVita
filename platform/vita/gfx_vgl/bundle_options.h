#pragma once
// Startup-only options, fixed-width C ABI shared by the SDK and game domains.
#include <stdint.h>
#define TWV_BUNDLE_COMPACT_ST 1u
#define TWV_BUNDLE_RESIDENT_GEOMETRY 2u
#define TWV_BUNDLE_TEXTURE_UPLOAD 4u
#define TWV_BUNDLE_SUBMIT_OBSERVER 8u
#define TWV_BUNDLE_ALL 15u
#define TWV_BUNDLE_CORE 7u
#ifdef __cplusplus
extern "C" {
#endif
// First query freezes all choices for this boot. Parent implements selection.
int twv_vita_bundle_feature(uint32_t feature);
uint32_t twv_vita_bundle_memory_profile(void);
int twv_vita_bundle_select(uint32_t features,uint32_t memory_profile);
uint32_t twv_vita_bundle_features(void);
uint32_t twv_vita_bundle_compiled(void);
#ifdef __cplusplus
}
#endif
