#include "gfx_vgl/gx_fast_api.h"
#include "runtime/renderer_options.hpp"
#include <cassert>
#include <climits>
#include <cstdio>
#include <map>
#include <string>

namespace {
std::map<std::string,long long> values;
unsigned flushes=0;
}
namespace twv::vita {
void telemetryEvent(const char* key,long long value){values[key]=value;}
void telemetryFlush(){++flushes;}
}

int main(int argc,char** argv) {
    assert(argc==2);
    const std::string mode=argv[1];
    constexpr bool compiled=bool(TWV_VITA_GX_FAST_DECODE);
    twv_vita_gx_fast_report();
    assert(values.at("gx_fast_compiled")==compiled);
    assert(values.at("gx_fast_enabled")==0 && values.at("gx_fast_explicit")==0);
    if(mode=="default") {
        assert(!twv_vita_gx_fast_enabled());
        assert(!twv_vita_gx_fast_select(0) && !twv_vita_gx_fast_select(1));
    } else if(mode=="off") {
        assert(twv_vita_gx_fast_select(0));
        assert(!twv_vita_gx_fast_select(0) && !twv_vita_gx_fast_select(1));
        assert(!twv_vita_gx_fast_enabled());
    } else if(mode=="on") {
        assert(twv_vita_gx_fast_select(1)==compiled);
        if(!compiled)assert(twv_vita_gx_fast_select(0));
        assert(twv_vita_gx_fast_enabled()==compiled);
        assert(!twv_vita_gx_fast_select(0));
    } else if(mode=="invalid") {
        for(int bad:{-1,2,INT_MIN,INT_MAX})assert(!twv_vita_gx_fast_select(bad));
        assert(twv_vita_gx_fast_select(0));
    } else if(mode=="failed") {
        twv_vita_gx_fast_disable(2);
        assert(!twv_vita_gx_fast_enabled());
        assert(!twv_vita_gx_fast_select(0) && !twv_vita_gx_fast_select(1));
        twv_vita_gx_fast_disable(3);twv_vita_gx_fast_report();
        assert(values.at("gx_fast_failures")==2 && values.at("gx_fast_first_failure_reason")==2);
        assert(flushes==1);
    } else if(mode=="late") {
        assert(twv_vita_gx_fast_select(compiled));
        assert(twv_vita_gx_fast_enabled()==compiled);
        twv_vita_gx_fast_disable(1);
        assert(!twv_vita_gx_fast_enabled() && !twv_vita_gx_fast_select(1));
    } else assert(false);
    for(const char* value:{"on","off"}) {
        twv::vita::RendererOptions options;
        const std::string arg=std::string("--vita-gx-decode=")+value;
        assert(options.consume(arg.c_str())==14 && options.gxSelected);
        assert(options.gxDecode==int(std::string(value)=="on"));
        assert(options.consume(arg.c_str())==-1);
        assert(options.consume("--vita-clocks=system")==0);
    }
    for(const char* token:{"--vita-gx-decode","--vita-gx-decode=","--vita-gx-decode=ON",
                          "--vita-gx-decode=1","--vita-gx-decode=onx"}) {
        twv::vita::RendererOptions options;
        assert(options.consume(token)==-1 && !options.gxSelected && !options.gxDecode);
    }
    twv::vita::RendererOptions unrelated;
    assert(!unrelated.consume("--vita-gx-decodex=on") && !unrelated.gxSelected);
    std::printf("gx_fast_control_test: compiled%d %s, real ABI lifecycle/parser/failure receipts passed\n",compiled,argv[1]);
}
