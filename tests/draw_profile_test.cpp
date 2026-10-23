#include "gfx_vgl/profile_scope.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <utility>

uint64_t now=0,clockCalls=0;
std::array<uint64_t,TWV_PROFILE_COUNT> totals{},adds{};
extern "C" uint64_t twv_vgl_time_us() { ++clockCalls;return now; }
extern "C" void twv_vgl_profile_add(uint32_t category,uint64_t us) {
    assert(category<TWV_PROFILE_COUNT);totals[category]+=us;++adds[category];
}

template<uint32_t Category> void check_category() {
    constexpr bool hot=Category==TWV_PROFILE_VERTEX || Category==TWV_PROFILE_SUBMIT ||
        Category==TWV_PROFILE_STATE || Category==TWV_PROFILE_PIPELINE;
    constexpr bool enabled=TWV_VITA_DRAW_TIMERS || !hot;
    const auto before=clockCalls,oldTotal=totals[Category],oldAdds=adds[Category];
    now=0; // Zero is a valid start, not an inactive sentinel.
    { twv::vgl::DrawProfileScope<Category> scope;now+=17; }
    assert(clockCalls-before==(enabled?2:0));
    assert(totals[Category]-oldTotal==(enabled?17:0));
    assert(adds[Category]-oldAdds==(enabled?1:0));
    const auto prior=clockCalls,total=totals[Category],add=adds[Category];
    now=UINT64_MAX-9;
    {
        twv::vgl::DrawProfileScope<Category> scope;
        now+=17;scope.finish();scope.finish();now+=1000;
    }
    assert(clockCalls-prior==(enabled?2:0));
    assert(totals[Category]-total==(enabled?17:0));
    assert(adds[Category]-add==(enabled?1:0));
}

template<size_t... Category> void check_all(std::index_sequence<Category...>) {
    (check_category<Category>(),...);
}

int main() {
    check_all(std::make_index_sequence<TWV_PROFILE_COUNT>{});
    const auto old=clockCalls,shader=totals[TWV_PROFILE_SHADER];
    { twv::vgl::ProfileScope shaderTimer(TWV_PROFILE_SHADER);now+=23; }
    assert(clockCalls-old==2 && totals[TWV_PROFILE_SHADER]-shader==23);
    const auto clocks=clockCalls,addsBefore=adds[TWV_PROFILE_VERTEX];
    unsigned work=0;
    for(unsigned i=0;i<1000000;++i) {
        twv::vgl::DrawProfileScope<TWV_PROFILE_VERTEX> scope;
        ++work;++now;
    }
    assert(work==1000000);
    assert(clockCalls-clocks==(TWV_VITA_DRAW_TIMERS?2000000:0));
    assert(adds[TWV_PROFILE_VERTEX]-addsBefore==(TWV_VITA_DRAW_TIMERS?1000000:0));
    std::printf("draw_profile_test: mode%d, all12 categories, clock0/wrap, explicit finish, retained shader and million scopes passed\n",TWV_VITA_DRAW_TIMERS);
}
