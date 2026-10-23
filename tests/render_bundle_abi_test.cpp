#include "gfx_vgl/bundle_options.h"
#include <cassert>
int main(){
    assert(twv_vita_bundle_compiled()==TWV_VITA_RENDER_BUNDLE);
#if TWV_VITA_RENDER_BUNDLE
    assert(twv_vita_bundle_select(TWV_BUNDLE_CORE,2));
    assert(twv_vita_bundle_features()==TWV_BUNDLE_CORE);
    assert(twv_vita_bundle_memory_profile()==2);
    assert(twv_vita_bundle_feature(TWV_BUNDLE_COMPACT_ST));
    assert(!twv_vita_bundle_feature(TWV_BUNDLE_SUBMIT_OBSERVER));
#else
    assert(!twv_vita_bundle_select(TWV_BUNDLE_CORE,2));
    assert(twv_vita_bundle_select(0,0));
    assert(!twv_vita_bundle_features() && !twv_vita_bundle_memory_profile());
#endif
    assert(!twv_vita_bundle_select(0,0));
}
