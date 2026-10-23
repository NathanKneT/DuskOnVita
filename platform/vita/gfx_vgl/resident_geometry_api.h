#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Reserve exactly this amount from vitaGL RAM before its initialization.
#define TWV_RESIDENT_GEOMETRY_RESERVE (8u*1024u*1024u)
int twv_vgl_resident_init(void);
int twv_vgl_resident_available(void);
int twv_vgl_resident_shutdown(void);
void* twv_vgl_resident_alloc(size_t bytes);
void twv_vgl_resident_release(void* ptr);
struct twv_resident_geometry_stats {
    uint64_t allocations,fallbacks,releases,collections,completionFailures;
    size_t liveBytes,retiredBytes,peakBytes;
    uint64_t scannedUnits,completedWaits,waitTimeUs;
};
void twv_vgl_resident_get_stats(struct twv_resident_geometry_stats* out);
#ifdef __cplusplus
}
#endif
