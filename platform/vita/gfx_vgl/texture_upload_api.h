#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Validate the full RGBA8 chain before allocation; synchronous copy, unchanged
 * sampler parameters/invalidation. Return 0 without creating a resource on error. */
uint32_t twv_vgl_texture_static_rgba(uint32_t width,uint32_t height,uint32_t levels,const void* bytes,size_t size);
/* Total permitted cache bytes including metadata, capped at 2 MiB. */
uint32_t twv_vgl_texture_cache_budget(uint32_t currently_used);
void twv_vgl_texture_conversion_cache_clear(void);
#ifdef __cplusplus
}
#endif
