#include "bundle_control.hpp"
#ifndef TWV_VITA_RENDER_BUNDLE
#define TWV_VITA_RENDER_BUNDLE 0
#endif
namespace {twv::vgl::BundleControl control{TWV_VITA_RENDER_BUNDLE!=0};}
extern "C" int twv_vita_bundle_feature(uint32_t feature){return control.enabled(feature);}
extern "C" uint32_t twv_vita_bundle_memory_profile(void){return control.memory_profile();}
extern "C" int twv_vita_bundle_select(uint32_t features,uint32_t profile){return control.select(features,profile);}
extern "C" uint32_t twv_vita_bundle_features(void){return control.mask();}
extern "C" uint32_t twv_vita_bundle_compiled(void){return control.compiled();}
