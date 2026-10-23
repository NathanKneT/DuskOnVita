#include "gfx_vgl/vertex_decoder_control.hpp"
#include "gfx_vgl/geometry_cache.hpp"
#include "runtime/renderer_options.hpp"
#include <array>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace twv::vgl {
VertexDecoderControl streamDecodeControl{bool(TWV_TEST_STREAM_COMPILED)};
VertexDecoderControl neonCompareControl{bool(TWV_TEST_STREAM_COMPILED)};
VertexDecoderControl vertexCullControl{bool(TWV_TEST_STREAM_COMPILED)};
VertexDecoderControl sparseSpansControl{bool(TWV_TEST_STREAM_COMPILED)};
VertexDecoderControl smallPacketControl{bool(TWV_TEST_STREAM_COMPILED)};
VertexDecoderControl uncachedStagingControl{bool(TWV_TEST_STREAM_COMPILED)};
GeometryCache geometryCache;
#include "vertex_decoder_selector.inc"
}

int main(){
    using twv::vgl::VertexDecoderControl;using twv::vita::RendererOptions;
    for(bool compiled:{false,true}){
        VertexDecoderControl noArg(compiled);
        assert(!noArg.enabled() && !noArg.selected() && noArg.compiled()==compiled);
        for(int invalid:{-1,2,INT_MIN,INT_MAX})assert(!noArg.select(invalid));
        assert(!noArg.selected());noArg.begin();assert(!noArg.select(0) && !noArg.select(1));
        VertexDecoderControl off(compiled);assert(off.select(0));
        assert(off.selected() && !off.enabled() && !off.select(0) && !off.select(1));
        VertexDecoderControl on(compiled);
        assert(on.select(1)==compiled);
        if(!compiled){assert(!on.selected() && !on.enabled());assert(on.select(0));}
        else {assert(on.enabled());on.fail();assert(!on.enabled() && !on.select(1));}
        VertexDecoderControl failed(compiled);failed.fail();assert(!failed.select(0) && !failed.select(1));
    }
    for(const char* value:{"on","off"}){
        RendererOptions options;
        assert(!options.selected && !options.consume("--stage"));
        const std::string arg=std::string("--vita-stream-decode=")+value;
        assert(options.consume(arg.c_str())==1 && options.selected);
        assert(options.streamDecode==int(std::strcmp(value,"on")==0));
        assert(options.consume(arg.c_str())==-1 && options.consume("--vita-stream-decode=off")==-1);
    }
    for(const char* bad:{"--vita-stream-decode","--vita-stream-decode=","--vita-stream-decode=ON",
                        "--vita-stream-decode=1","--vita-stream-decode=onx"}){
        RendererOptions options;assert(options.consume(bad)==-1 && !options.selected && !options.streamDecode);
    }
    for(const char* other:{"","-","--vita-stream-decodex=on","--vita-stream-decoder=off","--cvar","on"}){
        RendererOptions options;assert(!options.consume(other) && !options.selected);
    }
    for(const char* value:{"on","off"}){
        RendererOptions options;
        const std::string arg=std::string("--vita-neon-compare=")+value;
        assert(options.consume(arg.c_str())==2 && options.neonSelected && !options.selected);
        assert(options.neonCompare==int(std::strcmp(value,"on")==0));
        assert(options.consume("--vita-stream-decode=off")==1 && options.selected);
        assert(options.consume(arg.c_str())==-1);
    }
    for(const char* bad:{"--vita-neon-compare","--vita-neon-compare=","--vita-neon-compare=ON","--vita-neon-compare=onx"}){
        RendererOptions options;assert(options.consume(bad)==-1 && !options.neonSelected && !options.neonCompare);
    }
    RendererOptions unknown;assert(!unknown.consume("--vita-neon-comparex=on") && !unknown.neonSelected);
    for(const char* value:{"on","off"}){
        RendererOptions options;const std::string arg=std::string("--vita-shadow-image-skip=")+value;
        assert(options.consume(arg.c_str())==3 && options.shadowSelected && !options.selected && !options.neonSelected);
        assert(options.shadowImageSkip==int(std::strcmp(value,"on")==0));
        assert(options.consume(arg.c_str())==-1);
    }
    for(const char* bad:{"--vita-shadow-image-skip","--vita-shadow-image-skip=","--vita-shadow-image-skip=ON","--vita-shadow-image-skip=onx"}){
        RendererOptions options;assert(options.consume(bad)==-1 && !options.shadowSelected && !options.shadowImageSkip);
    }
    assert(!unknown.consume("--vita-shadow-image-skipx=on") && !unknown.shadowSelected);
    for(const char* value:{"on","off"}){
        RendererOptions options;const std::string arg=std::string("--vita-vertex-cull=")+value;
        assert(options.consume(arg.c_str())==4 && options.cullSelected && !options.shadowSelected && !options.neonSelected && !options.selected);
        assert(options.vertexCull==int(std::strcmp(value,"on")==0));assert(options.consume(arg.c_str())==-1);
    }
    for(const char* bad:{"--vita-vertex-cull","--vita-vertex-cull=","--vita-vertex-cull=ON","--vita-vertex-cull=onx"}){
        RendererOptions options;assert(options.consume(bad)==-1 && !options.cullSelected && !options.vertexCull);
    }
    assert(!unknown.consume("--vita-vertex-cullx=on") && !unknown.cullSelected);
    for(const char* value:{"on","off"}){
        RendererOptions options;const std::string arg=std::string("--vita-sparse-spans=")+value;
        assert(options.consume(arg.c_str())==5 && options.sparseSelected && !options.cullSelected && !options.shadowSelected);
        assert(options.sparseSpans==int(std::strcmp(value,"on")==0));assert(options.consume(arg.c_str())==-1);
    }
    for(const char* bad:{"--vita-sparse-spans","--vita-sparse-spans=","--vita-sparse-spans=ON","--vita-sparse-spans=onx"}){
        RendererOptions options;assert(options.consume(bad)==-1 && !options.sparseSelected && !options.sparseSpans);
    }
    assert(!unknown.consume("--vita-sparse-spansx=on") && !unknown.sparseSelected);
    for(const auto& choice:std::array<std::pair<const char*,int>,2>{{{"--vita-small-packets",12},{"--vita-uncached-staging",13}}}) {
        for(const char* value:{"on","off"}) {
            RendererOptions options;const std::string arg=std::string(choice.first)+"="+value;
            assert(options.consume(arg.c_str())==choice.second);
            assert((choice.second==12?options.smallSelected:options.uncachedSelected));
            assert((choice.second==12?options.smallPackets:options.uncachedStaging)==int(std::strcmp(value,"on")==0));
            assert(options.consume(arg.c_str())==-1);
            assert(options.consume("--vita-opt-bundle=off")==6);
        }
        for(const char* suffix:{"","=","=ON","=1","=onx"}) {
            RendererOptions options;const auto token=std::string(choice.first)+suffix;
            assert(options.consume(token.c_str())==-1 && !options.smallSelected && !options.uncachedSelected);
        }
        RendererOptions options;const auto unrelated=std::string(choice.first)+"x=on";
        assert(!options.consume(unrelated.c_str()));
    }
    std::vector<std::string> baseline{"--stage","F_SP108,6,1,0","--develop","--vita-benchmark=C1-FIGHT",
                                     "--cvar","video.maxFrameRate=60"};
    for(const char* stream:{"on","off"})for(const char* neon:{"on","off"})for(const char* shadow:{"on","off"})for(const char* cull:{"on","off"})for(const char* sparse:{"on","off"}){
        auto input=baseline;input.push_back(std::string("--vita-stream-decode=")+stream);input.push_back(std::string("--vita-neon-compare=")+neon);
        input.push_back(std::string("--vita-shadow-image-skip=")+shadow);
        input.push_back(std::string("--vita-vertex-cull=")+cull);
        input.push_back(std::string("--vita-sparse-spans=")+sparse);
        input.push_back(std::string("--vita-small-packets=")+stream);
        input.push_back(std::string("--vita-uncached-staging=")+neon);
        RendererOptions options;std::vector<std::string> forwarded;
        for(const auto& token:input){const auto result=options.consume(token.c_str());assert(result>=0);if(!result)forwarded.push_back(token);}
        assert(forwarded==baseline); // No extra flag reaches the original game CLI.
        assert(options.streamDecode==int(std::strcmp(stream,"on")==0) && options.neonCompare==int(std::strcmp(neon,"on")==0));
        assert(options.shadowImageSkip==int(std::strcmp(shadow,"on")==0));
        assert(options.vertexCull==int(std::strcmp(cull,"on")==0));
        assert(options.sparseSpans==int(std::strcmp(sparse,"on")==0));
        assert(options.smallPackets==int(std::strcmp(stream,"on")==0));
        assert(options.uncachedStaging==int(std::strcmp(neon,"on")==0));
    }
    using namespace twv::vgl;
    assert(!smallPacketControl.enabled() && !twv_vita_small_packets_select(-1));
    assert(twv_vita_small_packets_select(1)==int(bool(TWV_TEST_STREAM_COMPILED)));
    if(!TWV_TEST_STREAM_COMPILED)assert(twv_vita_small_packets_select(0)==1);
    smallPacketControl.begin();assert(!twv_vita_small_packets_select(0) && !twv_vita_small_packets_select(1));
    assert(!uncachedStagingControl.enabled() && !twv_vita_uncached_staging_select(-1));
    assert(twv_vita_uncached_staging_select(1)==int(bool(TWV_TEST_STREAM_COMPILED)));
    if(!TWV_TEST_STREAM_COMPILED)assert(twv_vita_uncached_staging_select(0)==1);
    uncachedStagingControl.begin();assert(!twv_vita_uncached_staging_select(0) && !twv_vita_uncached_staging_select(1));
    assert(!sparseSpansControl.enabled() && !twv_vita_sparse_spans_select(-1));
    assert(twv_vita_sparse_spans_select(1)==int(bool(TWV_TEST_STREAM_COMPILED)));
    if(!TWV_TEST_STREAM_COMPILED)assert(twv_vita_sparse_spans_select(0)==1);
    sparseSpansControl.begin();assert(!twv_vita_sparse_spans_select(0) && !twv_vita_sparse_spans_select(1));
    assert(!vertexCullControl.enabled() && !twv_vita_vertex_cull_select(-1));
    assert(twv_vita_vertex_cull_select(1)==int(bool(TWV_TEST_STREAM_COMPILED)));
    if(!TWV_TEST_STREAM_COMPILED)assert(twv_vita_vertex_cull_select(0)==1);
    vertexCullControl.begin();assert(!twv_vita_vertex_cull_select(0) && !twv_vita_vertex_cull_select(1));
    assert(!neonCompareControl.enabled() && !twv_vita_neon_compare_select(-1));
    assert(twv_vita_neon_compare_select(1)==int(bool(TWV_TEST_STREAM_COMPILED)));
    if(!TWV_TEST_STREAM_COMPILED)assert(twv_vita_neon_compare_select(0)==1);
    assert(neonCompareControl.selected() && geometryCache.neon_compare_enabled()==bool(TWV_TEST_STREAM_COMPILED));
    neonCompareControl.begin();assert(!twv_vita_neon_compare_select(0) && !twv_vita_neon_compare_select(1));
    assert(!streamDecodeControl.enabled() && !twv_vita_stream_decode_select(-1));
    assert(twv_vita_stream_decode_select(1)==int(bool(TWV_TEST_STREAM_COMPILED)));
    if(!TWV_TEST_STREAM_COMPILED)assert(twv_vita_stream_decode_select(0)==1);
    assert(streamDecodeControl.selected());streamDecodeControl.fail();
    assert(!streamDecodeControl.enabled() && !twv_vita_stream_decode_select(1));
    std::printf("vertex_decoder_control_test: compiled%d, six real integer C selectors, defaultOFF, startup-only, fail-closed, independent parser choices and unchanged forwarded CLI passed\n",TWV_TEST_STREAM_COMPILED);
}
