#include "system_probe.hpp"
#include "system_usage.hpp"
#include "telemetry.hpp"
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <cstdio>
#include <cinttypes>

// Verified against pinned SDK image8227da1b596d981f4513cd9b36bda1e43857f6dae5bcf4312c1cd40167f7d9bb:
// threadmgr/thread.h:206 GetThreadInfo;269–292 SystemInfo/idleClock;types.h SysClock=u64.
// psp2common/kernel/threadmgr.h:runClocks documented as clock cycles, not us.
// Public user imports provide error channels but no guarantee of permission.
// Reference upstream: https://github.com/vitasdk/vita-headers/tree/master/include/psp2
namespace twv::vita {
namespace {
UsageSampleGate gate;
UsageCounter idle[4],switches[4],wakeups[4],threads[2];
void event(const char* prefix,const char* field,long long value) {
    char name[80];std::snprintf(name,sizeof name,"%s_%s",prefix,field);telemetryEvent(name,value);
}
void wide(const char* prefix,const char* field,uint64_t value) {
    char name[80],text[24];
    std::snprintf(name,sizeof name,"%s_%s",prefix,field);
    std::snprintf(text,sizeof text,"%" PRIu64,value);telemetryText(name,text);
}
void counter(const char* prefix,const char* field,UsageCounter& tracking,uint64_t now,
             uint64_t value,bool valid,uint64_t identity) {
    char name[80];std::snprintf(name,sizeof name,"%s_%s",prefix,field);
    const auto d=tracking.sample(now,value,valid,identity);
    event(name,"state",int(d.state));
    if(valid)wide(name,"raw",value);
    if(d.state==UsageState::Delta){wide(name,"delta",d.clocks);wide(name,"interval_us",d.elapsedUs);}
}
void thread(const char* prefix,int32_t uid,unsigned index,uint64_t now) {
    event(prefix,"uid",uid);
    if(uid<=0){
        event(prefix,"available",0);
        counter(prefix,"run_clocks",threads[index],now,0,false,0);return;
    }
    SceKernelThreadInfo info{};info.size=sizeof info;
    const int rc=sceKernelGetThreadInfo(uid,&info);
    event(prefix,"result",rc);event(prefix,"available",rc==0);
    counter(prefix,"run_clocks",threads[index],now,info.runClocks,rc==0,uint32_t(uid));
    if(rc!=0)return;
    event(prefix,"status",info.status);event(prefix,"affinity",info.currentCpuAffinityMask);
    event(prefix,"current_core",info.currentCpuId);event(prefix,"last_core",info.lastExecutedCpuId);
}
}
void system_probe_sample(int32_t mainThreadId,int32_t audioThreadId) {
    const uint64_t now=processTimeUs();
    if(!gate.due(now))return;
    static bool unitsReported=false;
    if(!unitsReported){
        telemetryText("sys_counter_units","native clocks; time conversion unverified; no utilization percentage");
        unitsReported=true;
    }
    wide("sys","sample_us",now);
    telemetryEvent("sys_caller_core",sceKernelGetCpuId());
    SceKernelSystemInfo info{};info.size=sizeof info;
    const int rc=sceKernelGetSystemInfo(&info);
    telemetryEvent("sys_info_result",rc);
    if(rc==0)telemetryEvent("sys_active_cpu_mask",info.activeCpuMask);
    // Retain all four hardware slots and raw mask; do not assume its encoding
    // or call the reserved fourth core application-available.
    for(unsigned i=0;i<4;++i) {
        char prefix[20];std::snprintf(prefix,sizeof prefix,"sys_core%u",i);
        counter(prefix,"idle_clocks",idle[i],now,info.cpuInfo[i].idleClock,rc==0,info.activeCpuMask);
        counter(prefix,"switches",switches[i],now,info.cpuInfo[i].threadSwitchCount,rc==0,info.activeCpuMask);
        counter(prefix,"wakeups",wakeups[i],now,info.cpuInfo[i].comesOutOfIdleCount,rc==0,info.activeCpuMask);
    }
    thread("sys_main",mainThreadId,0,now);thread("sys_audio",audioThreadId,1,now);
    SceKernelFreeMemorySizeInfo memory{};memory.size=sizeof memory;
    const int memoryRc=sceKernelGetFreeMemorySize(&memory);
    telemetryEvent("sys_memory_result",memoryRc);
    if(memoryRc==0){
        telemetryEvent("sys_free_user_bytes",memory.size_user);
        telemetryEvent("sys_free_cdram_bytes",memory.size_cdram);
        telemetryEvent("sys_free_phycont_bytes",memory.size_phycont);
    }
    // Signed raw result retains any error; positive values are MHz per power.h.
    telemetryEvent("sys_arm_mhz_result",scePowerGetArmClockFrequency());
    telemetryEvent("sys_bus_mhz_result",scePowerGetBusClockFrequency());
    telemetryEvent("sys_gpu_mhz_result",scePowerGetGpuClockFrequency());
    telemetryEvent("sys_gpu_xbar_mhz_result",scePowerGetGpuXbarClockFrequency());
}
}
