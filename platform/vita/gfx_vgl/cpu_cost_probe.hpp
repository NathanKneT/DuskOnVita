#pragma once
// Bounded CPU wall-time attribution. These are neither GPU durations nor active
// CPU time. Counts/bytes cover all calls; sampled times must not be extrapolated
// into exact frame costs. The Aurora index generator/primitive are not visible
// at push_indices, so its measurements cover every index allocation/copy.
#include "profile_scope.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace twv::vgl {
struct CpuCostTiming {
    uint64_t calls=0,bytes=0,wallUs=0,rejectedClocks=0;
    void add(uint64_t begin,uint64_t end,uint64_t size=0) noexcept {
        ++calls;bytes+=size;
        if(end>=begin)wallUs+=end-begin;
        else ++rejectedClocks;
    }
};

class CpuCostProbe {
    struct Selector {
        uint32_t seed=0x923fde17,offset=0;
        bool select(uint64_t serial) noexcept {
            const unsigned index=unsigned(serial&(SamplePeriod-1));
            if(!index) {
                seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
                offset=seed&(SamplePeriod-1);
            }
            return index==offset;
        }
    } descriptorSelector,indexSelector;
    uint64_t now() noexcept {++clockCalls;return twv_vgl_time_us();}
public:
    static constexpr unsigned SamplePeriod=64,ReportVersion=1;
    uint64_t clockCalls=0;
    // Object/hash footprints only; no claim about compared bytes or transfers.
    uint64_t descriptorCalls=0,descriptorOutputBytes=0;
    uint64_t pipelineCalls=0,pipelineHashInputBytes=0,pipelineHits=0,pipelineMisses=0,pipelineCreated=0;
    uint64_t indexCalls=0,indexRequestedBytes=0,indexAllocationSuccess=0,indexAllocationFailure=0;
    uint64_t indexCopies=0,indexCopiedBytes=0,indexSampledCalls=0,indexSampledRequestedBytes=0;
    CpuCostTiming descriptorSampled;
    CpuCostTiming pipelineHit,pipelineMiss,pipelineMissLookup,pipelineMissShaderInfo,pipelineMissLayout,pipelineMissInsert;
    CpuCostTiming indexAllocateSuccessSampled,indexAllocateFailureSampled,indexCopySampled;

    template<bool Timers> class DescriptorScope {
        CpuCostProbe& probe;
        uint64_t begin=0,bytes;
        bool selected=false;
    public:
        DescriptorScope(CpuCostProbe& value,size_t outputBytes) noexcept:probe(value),bytes(outputBytes) {
            const auto serial=probe.descriptorCalls++;
            probe.descriptorOutputBytes+=bytes;
            if constexpr(Timers) {
                selected=probe.descriptorSelector.select(serial);
                if(selected)begin=probe.now();
            }
        }
        DescriptorScope(const DescriptorScope&)=delete;
        DescriptorScope& operator=(const DescriptorScope&)=delete;
        ~DescriptorScope() {
            if constexpr(Timers)if(selected)probe.descriptorSampled.add(begin,probe.now(),bytes);
        }
        bool is_selected()const noexcept{return selected;}
    };

    // Replaces the existing pipeline DrawProfileScope: a hit still makes exactly
    // its original two clock reads and publishes the same inclusive profile delta.
    // Only misses add clocks at the three phase boundaries. Exclusive spans
    // partition the inclusive miss duration and include diagnostic bookkeeping.
    template<bool Timers> class PipelineScope {
        CpuCostProbe& probe;
        uint64_t begin=0,last=0;
        bool hit=false,miss=false;
        unsigned phase=0;
        void mark(CpuCostTiming& timing) noexcept {
            if constexpr(Timers) {
                const auto end=probe.now();timing.add(last,end);last=end;
            }
        }
    public:
        PipelineScope(CpuCostProbe& value,size_t keyBytes) noexcept:probe(value) {
            if constexpr(Timers)begin=last=probe.now();
            ++probe.pipelineCalls;probe.pipelineHashInputBytes+=keyBytes;
        }
        PipelineScope(const PipelineScope&)=delete;
        PipelineScope& operator=(const PipelineScope&)=delete;
        void found() noexcept {hit=true;++probe.pipelineHits;}
        void missing() noexcept {miss=true;++probe.pipelineMisses;mark(probe.pipelineMissLookup);phase=1;}
        void shader_info_done() noexcept {mark(probe.pipelineMissShaderInfo);phase=2;}
        void layout_done() noexcept {mark(probe.pipelineMissLayout);phase=3;}
        void created() noexcept {++probe.pipelineCreated;}
        ~PipelineScope() {
            if constexpr(Timers) {
                const auto end=probe.now();
                // Keep the pre-existing profile clock arithmetic (including
                // modulo wrap). Attribution rejects regressing clock spans.
                twv_vgl_profile_add(TWV_PROFILE_PIPELINE,end-begin);
                if(hit)probe.pipelineHit.add(begin,end);
                else if(miss) {
                    probe.pipelineMiss.add(begin,end);
                    auto& unfinished=phase==1?probe.pipelineMissShaderInfo:
                        phase==2?probe.pipelineMissLayout:probe.pipelineMissInsert;
                    unfinished.add(last,end);
                }
            }
        }
    };

    template<bool Timers> class IndexScope {
        CpuCostProbe& probe;
        uint64_t begin=0,bytes;
        bool selected=false;
    public:
        IndexScope(CpuCostProbe& value,size_t requestedBytes) noexcept:probe(value),bytes(requestedBytes) {
            const auto serial=probe.indexCalls++;
            probe.indexRequestedBytes+=bytes;
            if constexpr(Timers) {
                selected=probe.indexSelector.select(serial);
                if(selected) {
                    ++probe.indexSampledCalls;probe.indexSampledRequestedBytes+=bytes;
                    begin=probe.now();
                }
            }
        }
        IndexScope(const IndexScope&)=delete;
        IndexScope& operator=(const IndexScope&)=delete;
        void allocated(bool success) noexcept {
            if(success)++probe.indexAllocationSuccess;else ++probe.indexAllocationFailure;
            if constexpr(Timers)if(selected) {
                const auto end=probe.now();
                auto& timing=success?probe.indexAllocateSuccessSampled:probe.indexAllocateFailureSampled;
                timing.add(begin,end,bytes);begin=end;
            }
        }
        void copied() noexcept {
            ++probe.indexCopies;probe.indexCopiedBytes+=bytes;
            if constexpr(Timers)if(selected)probe.indexCopySampled.add(begin,probe.now(),bytes);
        }
        bool is_selected()const noexcept{return selected;}
    };

    // Fixed-size cumulative receipt at the backend's existing 60-frame cadence.
    // A consumer must find matching frame markers/version before using a group.
    template<class Emit> void report(uint64_t frame,Emit emit)const {
        emit("gfx_cpu_cost_report_begin",frame);
        emit("gfx_cpu_cost_report_version",ReportVersion);
        emit("gfx_cpu_cost_timers_enabled",uint64_t(bool(TWV_VITA_DRAW_TIMERS)));
        emit("gfx_cpu_cost_sample_period",SamplePeriod);
        emit("gfx_cpu_cost_clock_calls",clockCalls);
        emit("gfx_cpu_cost_index_primitive_known",0);
        emit("gfx_cpu_cost_index_generation_measured",0);
        emit("gfx_cpu_cost_descriptor_calls",descriptorCalls);
        emit("gfx_cpu_cost_descriptor_output_bytes",descriptorOutputBytes);
        emit("gfx_cpu_cost_pipeline_calls",pipelineCalls);
        emit("gfx_cpu_cost_pipeline_hash_input_bytes",pipelineHashInputBytes);
        emit("gfx_cpu_cost_pipeline_hit_calls",pipelineHits);
        emit("gfx_cpu_cost_pipeline_miss_calls",pipelineMisses);
        emit("gfx_cpu_cost_pipeline_created_calls",pipelineCreated);
        emit("gfx_cpu_cost_index_calls",indexCalls);
        emit("gfx_cpu_cost_index_requested_bytes",indexRequestedBytes);
        emit("gfx_cpu_cost_index_allocate_success_calls",indexAllocationSuccess);
        emit("gfx_cpu_cost_index_allocate_failure_calls",indexAllocationFailure);
        emit("gfx_cpu_cost_index_copy_calls",indexCopies);
        emit("gfx_cpu_cost_index_copied_bytes",indexCopiedBytes);
        emit("gfx_cpu_cost_index_sampled_calls",indexSampledCalls);
        emit("gfx_cpu_cost_index_sampled_requested_bytes",indexSampledRequestedBytes);
        const auto timing=[&](const char* name,const CpuCostTiming& value) {
            char key[112];
            const auto field=[&](const char* suffix,uint64_t amount) {
                std::snprintf(key,sizeof key,"gfx_cpu_cost_%s_%s",name,suffix);emit(key,amount);
            };
            field("calls",value.calls);field("bytes",value.bytes);
            field("wall_us",value.wallUs);field("rejected_clocks",value.rejectedClocks);
        };
        timing("descriptor_sampled",descriptorSampled);
        timing("pipeline_hit_inclusive",pipelineHit);timing("pipeline_miss_inclusive",pipelineMiss);
        timing("pipeline_miss_lookup_exclusive",pipelineMissLookup);
        timing("pipeline_miss_shader_info_exclusive",pipelineMissShaderInfo);
        timing("pipeline_miss_layout_exclusive",pipelineMissLayout);
        timing("pipeline_miss_insert_exclusive",pipelineMissInsert);
        timing("index_allocate_success_sampled",indexAllocateSuccessSampled);
        timing("index_allocate_failure_sampled",indexAllocateFailureSampled);
        timing("index_copy_sampled",indexCopySampled);
        emit("gfx_cpu_cost_report_end",frame);
    }
};
using CpuDescriptorScope=CpuCostProbe::DescriptorScope<bool(TWV_VITA_DRAW_TIMERS)>;
using CpuPipelineScope=CpuCostProbe::PipelineScope<bool(TWV_VITA_DRAW_TIMERS)>;
using CpuIndexScope=CpuCostProbe::IndexScope<bool(TWV_VITA_DRAW_TIMERS)>;
} // namespace twv::vgl
