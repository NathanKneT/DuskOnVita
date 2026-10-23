// Reuse the SDK-free Aurora types AND actual extracted VertexStage methods.
#define main previous_decoder_fixture_main
#include "vertex_stage_decode_test.cpp"
#undef main

bool covers(const SourceSpan* spans,unsigned count,const uint8_t* address){
    const auto value=reinterpret_cast<uintptr_t>(address);
    for(unsigned i=0;i<count;++i){const auto begin=reinterpret_cast<uintptr_t>(spans[i].data);if(value>=begin && value-begin<spans[i].size)return true;}
    return false;
}
int main(){
    uint64_t covered=0,mutations=0,holeTests=0,selected=0;
    for(unsigned trial=0;trial<144;++trial){
        Pipeline pipeline{};auto& cfg=pipeline.config.shaderConfig;
        const bool index16=trial%2;const unsigned count=64+(trial%4)*32;
        cfg.vtxStride=index16?4:2;
        cfg.colorChannels[0].matSrc=GX_SRC_VTX;
        auto& pos=cfg.attrs[GX_VA_POS];pos.attrType=index16?GX_INDEX16:GX_INDEX8;
        pos.offset=0;pos.compType=GX_F32;pos.cnt=3;pos.stride=12+(trial%3)*4;pos.le=bool(trial%2);
        auto& color=cfg.attrs[GX_VA_CLR0];color.attrType=pos.attrType;color.offset=index16?2:1;
        color.compType=uint8_t(trial%6);color.stride=8;color.cnt=4;
        const unsigned range=index16?32000:256;
        std::vector<uint8_t> positions(size_t(range)*pos.stride+12),colors(size_t(range)*color.stride+4),raw(size_t(count)*cfg.vtxStride);
        for(auto& v:positions)v=uint8_t(random_value());
        for(auto& v:colors)v=uint8_t(random_value());
        std::vector<uint16_t> pi(count),ci(count);
        for(unsigned v=0;v<count;++v){
            pi[v]=uint16_t(random_value()%range);ci[v]=uint16_t(random_value()%range);
            auto* bytes=raw.data()+size_t(v)*cfg.vtxStride;
            if(index16){bytes[0]=pi[v]>>8;bytes[1]=pi[v]&255;bytes[2]=ci[v]>>8;bytes[3]=ci[v]&255;}
            else{bytes[0]=uint8_t(pi[v]);bytes[1]=uint8_t(ci[v]);}
        }
        aurora::gx::DrawData draw{};draw.vtxCount=count;draw.immediateData.vtxStart=reinterpret_cast<uintptr_t>(raw.data());
        draw.immediateData.arrayStart[GX_VA_POS-GX_VA_POS]=reinterpret_cast<uintptr_t>(positions.data());
        draw.immediateData.arrayStart[GX_VA_CLR0-GX_VA_POS]=reinterpret_cast<uintptr_t>(colors.data());
        VertexStage stage(pipeline,draw);
        std::array<SourceSpan,MaxGeometrySpans> broad{},sparse{};
        const unsigned bn=stage.source_spans(broad);size_t original=0;
        const unsigned sn=stage.source_spans(sparse,true,&original);
        assert(bn && sn && sn<=MaxGeometrySpans);
        size_t broadBytes=0,sparseBytes=0;for(unsigned i=0;i<bn;++i)broadBytes+=broad[i].size;for(unsigned i=0;i<sn;++i)sparseBytes+=sparse[i].size;
        assert(original==broadBytes && sparseBytes<=broadBytes);
        assert(sparse[0].data==raw.data() && sparse[0].size==raw.size());
        if(sparseBytes<broadBytes){++selected;assert(sparseBytes<=broadBytes-broadBytes/4);}
        for(unsigned v=0;v<count;++v){
            for(unsigned b=0;b<12;++b){assert(covers(sparse.data(),sn,positions.data()+size_t(pi[v])*pos.stride+b));++covered;}
            for(unsigned b=0;b<width(color,GX_VA_CLR0);++b){assert(covers(sparse.data(),sn,colors.data()+size_t(ci[v])*color.stride+b));++covered;}
        }
        const auto pack=[&](){std::vector<float> result(size_t(count)*7);for(unsigned v=0;v<count;++v){Decoded d{};stage.decode_reference(v,d);std::memcpy(result.data()+v*7,&d.pos,12);std::memcpy(result.data()+v*7+3,&d.clr[0],16);}return result;};
        auto expected=pack();GeometryCache cache(2*1024*1024);GeometryKey key{};key.pipeline=trial;
        assert(cache.store(1,key,sparse.data(),sn,expected.data(),expected.size()*4,1));
        assert(cache.lookup(1,key).data);
        // Referenced byte changes always invalidate; restore/re-admit before
        // each mutation so every test starts from valid unchanged root indices.
        for(unsigned i=0;i<16;++i){
            auto* byte=positions.data()+size_t(pi[i])*pos.stride+(i%12);*byte^=1;
            assert(!cache.lookup(1,key).data);*byte^=1;cache.clear();
            assert(cache.store(1,key,sparse.data(),sn,expected.data(),expected.size()*4,1));++mutations;
        }
        // An excluded hole must not change original decoding; cache replay can
        // remain valid even if that unused byte changes. This is the saving.
        if(sparseBytes<broadBytes){
            bool found=false;
            for(auto* buffer:{&positions,&colors}){
                for(size_t i=0;i<buffer->size();++i)if(covers(broad.data(),bn,buffer->data()+i) && !covers(sparse.data(),sn,buffer->data()+i)){
                    (*buffer)[i]^=1;auto actual=pack();auto hit=cache.lookup(1,key);
                    assert(hit.data && actual.size()==expected.size() && std::memcmp(actual.data(),expected.data(),expected.size()*4)==0);
                    (*buffer)[i]^=1;++holeTests;found=true;break;
                }
                if(found)break;
            }
            assert(found);
        }
        // Index-root lifetime rule: changed raw data is rejected before ranges
        // derived from the previous indices are dereferenced.
        raw[0]^=1;assert(!cache.lookup(1,key).data);raw[0]^=1;
    }
    assert(selected>10 && holeTests==selected && mutations==144*16);
    // Dense/small/capacity/oversize fallbacks and bounded adaptive coalescing.
    uint8_t array[100000]{};uint16_t ids[MaxSparseIndices]{};SourceSpan spans[4]{};
    for(unsigned i=0;i<128;++i)ids[i]=uint16_t(i);
    assert(indexed_source_runs(spans,4,array,12,12,ids,128,0,127)==1 && spans[0].size==128*12);
    for(unsigned i=0;i<128;++i)ids[i]=uint16_t(i*32);
    unsigned n=indexed_source_runs(spans,1,array,12,12,ids,128,0,127*32);assert(n==1);
    assert(!indexed_source_runs(spans,0,array,12,12,ids,128,0,127*32));
    assert(!indexed_source_runs(spans,4,nullptr,12,12,ids,128,0,127*32));
    std::printf("source_span_coverage_test:144 actual production index8/index16/color/endian/stride scenarios;%llu referenced bytes covered,%llu mutations invalidate,%llu excluded holes preserve independent old decoding;raw root first and capacity/dense fallback passed\n",(unsigned long long)covered,(unsigned long long)mutations,(unsigned long long)holeTests);
}
