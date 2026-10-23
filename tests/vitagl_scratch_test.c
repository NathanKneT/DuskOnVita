// Compile the actual patched vitaGL scratch function with isolated pool state.
// Omitting the dependency patch must fail this test, even if our bridge aligns
// its own returned pointer: uniforms and internal vertex copies share the cursor.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

static uint8_t *circular_data_pool_ptr[3], *circular_data_pool_limit[3];
static unsigned vgl_circular_idx;
static void *fallback;
static unsigned fallback_count;
static void *gpu_alloc_mapped_for_cpu(uint32_t size) {
    ++fallback_count;
    return fallback = malloc(size);
}
static void mark_as_dirty(void *p) { assert(p == fallback); }

#include "vitagl_scratch_function.inc"

int main(void) {
    _Alignas(16) uint8_t arena[3][16384];
    for (unsigned frame = 0; frame < 3; ++frame) {
        vgl_circular_idx = frame;
        circular_data_pool_ptr[frame] = arena[frame];
        circular_data_pool_limit[frame] = arena[frame] + sizeof arena[frame];
        for (unsigned draw = 0; draw < 16; ++draw) {
            // Include the bridge's 15-byte over-reservation, odd triangle index
            // counts, and vitaGL's own uniform/vertex byte allocations.
            const uint32_t sizes[] = {6 + 15, 3 * 140 + 15, 3 * 140, 4, 7, 64};
            for (unsigned k = 0; k < sizeof sizes / sizeof sizes[0]; ++k) {
                uint8_t *before = circular_data_pool_ptr[frame];
                uint8_t *p = vgl_reserve_data_pool(sizes[k]);
                assert((uintptr_t)p % 16 == 0);
                assert(p == before && p + sizes[k] <= circular_data_pool_ptr[frame]);
                assert((uintptr_t)circular_data_pool_ptr[frame] % 16 == 0);
                *(float*)p = 1.0f; // typed store checked by UBSan alignment
            }
        }
    }
    assert(fallback_count == 0);
    uint8_t *before = circular_data_pool_ptr[vgl_circular_idx];
    assert(vgl_reserve_data_pool(UINT32_MAX) == NULL);
    assert(circular_data_pool_ptr[vgl_circular_idx] == before);
    circular_data_pool_limit[vgl_circular_idx] = before;
    void *p = vgl_reserve_data_pool(65);
    assert(p && (uintptr_t)p % 16 == 0 && fallback_count == 1);
    free(fallback);
    puts("vitagl_scratch_test: actual dependency cursor, mixed uploads, fallback and overflow passed");
}
