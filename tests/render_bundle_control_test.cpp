#include "gfx_vgl/bundle_control.hpp"
#include "runtime/renderer_options.hpp"
#include <cassert>
#include <cstdio>
#include <string>

int main(){
    using twv::vgl::BundleControl;using twv::vita::RendererOptions;
    for(uint32_t profile=0;profile<3;++profile)for(uint32_t mask=0;mask<=TWV_BUNDLE_ALL;++mask){
        BundleControl c(true);assert(c.select(mask,profile));assert(!c.select(mask,profile));
        assert(c.mask()==mask && c.memory_profile()==profile);
        for(uint32_t bit=1;bit<=8;bit*=2)assert(c.enabled(bit)==bool(mask&bit));
        assert(!c.enabled(0) && !c.enabled(3) && !c.enabled(16));
    }
    for(uint32_t mask:{0u,1u,15u,16u,UINT32_MAX}){
        BundleControl c(false);const bool valid=mask==0;assert(c.select(mask,0)==valid);
        assert(!c.enabled(1) && !c.mask() && !c.select(0,0));
    }
    BundleControl unavailable(false);assert(!unavailable.select(0,1));assert(unavailable.select(0,0));
    BundleControl invalid(true);assert(!invalid.select(16,0) && !invalid.select(0,3));assert(invalid.select(0,0));
    BundleControl early(true);assert(!early.enabled(1));assert(!early.select(1,0));
    BundleControl poolEarly(true);assert(poolEarly.memory_profile()==0);assert(!poolEarly.select(1,1));
    for(bool bundleFirst:{false,true}){
        RendererOptions r;
        if(bundleFirst)assert(r.consume("--vita-opt-bundle=on")==6);
        assert(r.consume("--vita-compact-st=off")==7);
        assert(r.consume("--vita-submit-observer=on")==10);
        if(!bundleFirst)assert(r.consume("--vita-opt-bundle=on")==6);
        assert(r.bundle_features()==14);
        assert(r.consume("--vita-memory-pools=ram64")==11 && r.memoryProfile==2);
        assert(r.consume("--vita-memory-pools=legacy")==-1);
        assert(r.consume("--vita-resident-geometry=off")==8 && r.bundle_features()==12);
        assert(r.consume("--vita-texture-upload=off")==9 && r.bundle_features()==8);
        assert(r.consume("--vita-compact-st=on")==-1);
    }
    RendererOptions defaults;assert(defaults.bundle_features()==0 && defaults.memoryProfile==0);
    assert(defaults.consume("--vita-opt-bundle=on")==6 && defaults.bundle_features()==TWV_BUNDLE_CORE);
    assert(!(defaults.bundle_features()&TWV_BUNDLE_SUBMIT_OBSERVER));
    for(const char* token:{"--vita-opt-bundle","--vita-opt-bundle=","--vita-opt-bundle=yes","--vita-compact-st=1",
                          "--vita-memory-pools=ram128","--vita-memory-pools=","--vita-memory-pools"}){
        RendererOptions r;assert(r.consume(token)==-1);assert(!r.bundleSelected && !r.memorySelected && !r.featureSelected);
    }
    RendererOptions prefix;assert(prefix.consume("--vita-memory-pools-x=ram64")==0);
    assert(prefix.consume("--vita-opt-bundlex=on")==0 && prefix.consume("--stage")==0);
    assert(prefix.consume(nullptr)==-1);
    std::puts("render_bundle_control_test: defaults, full mask/profile matrix, late/invalid/duplicate rejection, order-independent overrides and diagnostic default passed");
}
