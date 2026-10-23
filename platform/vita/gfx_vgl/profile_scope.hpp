#pragma once
#include "gpu_api.h"

#ifndef TWV_VITA_DRAW_TIMERS
#define TWV_VITA_DRAW_TIMERS 1
#endif

namespace twv::vgl {
// Only frequent diagnostic draw timers are optional. Shader, frame, present
// and original game clocks stay active. The publisher uses the same predicate.
constexpr bool profile_category_enabled(uint32_t category) {
    return TWV_VITA_DRAW_TIMERS || (category != TWV_PROFILE_VERTEX &&
        category != TWV_PROFILE_SUBMIT && category != TWV_PROFILE_STATE &&
        category != TWV_PROFILE_PIPELINE);
}

template<uint32_t Category> struct DrawProfileScope {
    static_assert(Category < TWV_PROFILE_COUNT);
    DrawProfileScope() {
        if constexpr(profile_category_enabled(Category)) start = twv_vgl_time_us();
    }
    ~DrawProfileScope() { finish(); }
    DrawProfileScope(const DrawProfileScope&) = delete;
    DrawProfileScope& operator=(const DrawProfileScope&) = delete;
    void finish() {
        if constexpr(profile_category_enabled(Category)) {
            if(active) {
                twv_vgl_profile_add(Category, twv_vgl_time_us() - start);
                active = false;
            }
        }
    }
private:
    uint64_t start = 0;
    bool active = true;
};

struct ProfileScope {
    explicit ProfileScope(uint32_t category) : category(category), start(twv_vgl_time_us()) {}
    ~ProfileScope() { twv_vgl_profile_add(category, twv_vgl_time_us() - start); }
    uint32_t category;
    uint64_t start;
};
}
