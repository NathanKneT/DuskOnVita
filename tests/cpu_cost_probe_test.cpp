#include "gfx_vgl/cpu_cost_probe.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

uint64_t tick=0,clockCalls=0,profileCalls=0,profileUs=0;
extern "C" uint64_t twv_vgl_time_us(){++clockCalls;return tick;}
extern "C" void twv_vgl_profile_add(uint32_t category,uint64_t us){
    assert(category==TWV_PROFILE_PIPELINE);++profileCalls;profileUs+=us;
}

int main(int argc,char** argv){
    assert(argc==1 || (argc==3 && std::strcmp(argv[1],"--emit-receipts")==0));
    using namespace twv::vgl;
    static_assert(sizeof(CpuCostProbe)<=768);
    CpuCostProbe probe;
    constexpr unsigned Blocks=256,Calls=64*Blocks;
    unsigned descriptorSamples=0,indexSamples=0,sampledFailures=0,sampledSuccesses=0;
    uint64_t sampledFailedBytes=0,sampledCopiedBytes=0,requestedBytes=0,copiedBytes=0;
    unsigned failures=0,blockDescriptor=0,blockIndex=0;
    std::set<unsigned> descriptorOffsets,indexOffsets;
    for(unsigned i=0;i<Calls;++i){
        if(!(i%64)){
            if(i && TWV_VITA_DRAW_TIMERS)assert(blockDescriptor==1 && blockIndex==1);
            blockDescriptor=blockIndex=0;
        }
        const uint64_t bytes=2*(i%37);
        {
            CpuDescriptorScope scope(probe,173);
            if(scope.is_selected()){
                ++descriptorSamples;++blockDescriptor;descriptorOffsets.insert(i%64);
            }
            tick+=7;
        }
        const bool success=i%3!=0;
        CpuIndexScope scope(probe,bytes);
        requestedBytes+=bytes;
        if(scope.is_selected()){
            ++indexSamples;++blockIndex;indexOffsets.insert(i%64);
            if(success){++sampledSuccesses;sampledCopiedBytes+=bytes;}
            else {++sampledFailures;sampledFailedBytes+=bytes;}
        }
        tick+=3;scope.allocated(success);
        if(success){tick+=11;scope.copied();copiedBytes+=bytes;}
        else ++failures;
    }
    assert(probe.descriptorCalls==Calls && probe.descriptorOutputBytes==173*Calls);
    assert(probe.indexCalls==Calls && probe.indexRequestedBytes==requestedBytes);
    assert(probe.indexAllocationSuccess==Calls-failures && probe.indexAllocationFailure==failures);
    assert(probe.indexCopies==Calls-failures && probe.indexCopiedBytes==copiedBytes);
#if TWV_VITA_DRAW_TIMERS
    assert(descriptorSamples==Blocks && indexSamples==Blocks && blockDescriptor==1 && blockIndex==1);
    assert(descriptorOffsets.size()>40 && indexOffsets.size()>40);
    assert(probe.descriptorSampled.calls==Blocks && probe.descriptorSampled.bytes==Blocks*173);
    assert(probe.descriptorSampled.wallUs==Blocks*7 && !probe.descriptorSampled.rejectedClocks);
    assert(sampledSuccesses && sampledFailures);
    assert(probe.indexSampledCalls==Blocks);
    assert(probe.indexSampledRequestedBytes==sampledFailedBytes+sampledCopiedBytes);
    assert(probe.indexAllocateSuccessSampled.calls==sampledSuccesses);
    assert(probe.indexAllocateFailureSampled.calls==sampledFailures);
    assert(probe.indexAllocateSuccessSampled.wallUs==sampledSuccesses*3);
    assert(probe.indexAllocateFailureSampled.wallUs==sampledFailures*3);
    assert(probe.indexAllocateSuccessSampled.bytes==sampledCopiedBytes);
    assert(probe.indexAllocateFailureSampled.bytes==sampledFailedBytes);
    assert(probe.indexCopySampled.calls==sampledSuccesses);
    assert(probe.indexCopySampled.bytes==sampledCopiedBytes && probe.indexCopySampled.wallUs==sampledSuccesses*11);
    assert(clockCalls==Blocks*2+sampledSuccesses*3+sampledFailures*2);
#else
    assert(!descriptorSamples && !indexSamples && !clockCalls && !probe.indexSampledCalls);
    assert(!probe.descriptorSampled.calls && !probe.indexAllocateSuccessSampled.calls);
    assert(!probe.indexAllocateFailureSampled.calls && !probe.indexCopySampled.calls);
#endif
    // A hit reuses the two existing profile clocks. Miss-only phase clocks
    // partition the same inclusive elapsed duration without nested totals.
    const auto oldClock=clockCalls,oldProfile=profileUs,oldCalls=profileCalls;
    for(unsigned i=0;i<1000;++i){
        CpuPipelineScope scope(probe,191);tick+=13;scope.found();
    }
    for(unsigned i=0;i<100;++i){
        CpuPipelineScope scope(probe,191);
        tick+=3;scope.missing();tick+=5;scope.shader_info_done();
        tick+=7;scope.layout_done();tick+=11;scope.created();
    }
    assert(probe.pipelineCalls==1100 && probe.pipelineHashInputBytes==1100*191);
    assert(probe.pipelineHits==1000 && probe.pipelineMisses==100 && probe.pipelineCreated==100);
#if TWV_VITA_DRAW_TIMERS
    assert(clockCalls-oldClock==2500 && profileCalls-oldCalls==1100);
    assert(profileUs-oldProfile==1000*13+100*26);
    assert(probe.pipelineHit.calls==1000 && probe.pipelineHit.wallUs==1000*13);
    assert(probe.pipelineMiss.calls==100 && probe.pipelineMiss.wallUs==100*26);
    assert(probe.pipelineMissLookup.wallUs==100*3 && probe.pipelineMissShaderInfo.wallUs==100*5);
    assert(probe.pipelineMissLayout.wallUs==100*7 && probe.pipelineMissInsert.wallUs==100*11);
    assert(probe.pipelineMissLookup.wallUs+probe.pipelineMissShaderInfo.wallUs+
        probe.pipelineMissLayout.wallUs+probe.pipelineMissInsert.wallUs==probe.pipelineMiss.wallUs);
#else
    assert(clockCalls==oldClock && profileCalls==oldCalls && profileUs==oldProfile);
    assert(!probe.pipelineHit.calls && !probe.pipelineMiss.calls);
#endif
    assert(probe.clockCalls==clockCalls);
    // Timestamp zero is valid; a regressing span is counted and rejected by the
    // attribution while the pre-existing profile retains its modulo arithmetic.
    CpuCostProbe invalid;
    for(unsigned i=0;i<64;++i){
        tick=100;
        CpuDescriptorScope scope(invalid,4);tick=90;
    }
    tick=100;
    {CpuPipelineScope scope(invalid,4);scope.found();tick=90;}
    tick=0;
    {CpuPipelineScope scope(invalid,4);scope.found();}
#if TWV_VITA_DRAW_TIMERS
    assert(invalid.descriptorSampled.calls==1 && invalid.descriptorSampled.rejectedClocks==1);
    assert(!invalid.descriptorSampled.wallUs);
    assert(invalid.pipelineHit.calls==2 && invalid.pipelineHit.rejectedClocks==1 && !invalid.pipelineHit.wallUs);
#else
    assert(!invalid.descriptorSampled.calls && !invalid.pipelineHit.calls && !invalid.clockCalls);
#endif
    std::map<std::string,uint64_t> receipt;
    std::vector<std::string> order;
    probe.report(120,[&](const char* name,uint64_t value){
        assert(receipt.emplace(name,value).second);order.emplace_back(name);
    });
    assert(order.front()=="gfx_cpu_cost_report_begin" && order.back()=="gfx_cpu_cost_report_end");
    assert(receipt.at(order.front())==120 && receipt.at(order.back())==120);
    assert(receipt.at("gfx_cpu_cost_report_version")==1 && receipt.at("gfx_cpu_cost_sample_period")==64);
    assert(receipt.at("gfx_cpu_cost_timers_enabled")==TWV_VITA_DRAW_TIMERS);
    assert(receipt.at("gfx_cpu_cost_clock_calls")==probe.clockCalls);
    assert(!receipt.at("gfx_cpu_cost_index_primitive_known") && !receipt.at("gfx_cpu_cost_index_generation_measured"));
    assert(receipt.at("gfx_cpu_cost_pipeline_hit_inclusive_wall_us")==probe.pipelineHit.wallUs);
    assert(receipt.at("gfx_cpu_cost_index_copy_sampled_bytes")==sampledCopiedBytes);
    assert(receipt.size()==63);
    if(argc==3){
        auto* output=std::fopen(argv[2],"wb");assert(output);
        probe.report(120,[&](const char* name,uint64_t value){
            assert(std::fprintf(output,"{\"ms\":4000,\"event\":\"%s\",\"value\":%llu}\n",name,
                static_cast<unsigned long long>(value))>0);
        });
        assert(std::fclose(output)==0);
    }
    std::printf("cpu_cost_probe_test: timers%d, fixed63-field receipt, all-call bytes/routes, changing1/64 samples, failure/zero/backwards clocks and unchanged pipeline clocks passed\n",TWV_VITA_DRAW_TIMERS);
}
