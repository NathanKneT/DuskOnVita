#pragma once
#include <cstdint>
namespace twv::vita {
constexpr uint64_t MemoryMiB = 1024u * 1024u;
constexpr uint64_t ResidentReserveBytes = 8u * MemoryMiB;
struct MemoryPolicy {
    uint64_t ram_bytes=0, cdram_bytes=0, resident_bytes=0;
    uint64_t total_bytes=0, scratch_bytes=48u*MemoryMiB, newlib_bytes=160u*MemoryMiB;
};
// Output is reset on failure. Reserve must be 4 KiB aligned and fit the RAM pool.
inline bool memory_policy(uint32_t profile, uint64_t reserve, MemoryPolicy& out) {
    out = {};
    if (profile>2 || reserve%4096u) return false;
    const uint64_t ram=(32u+16u*profile)*MemoryMiB;
    if (reserve>ram) return false;
    out.ram_bytes=ram-reserve;
    out.cdram_bytes=(96u-16u*profile)*MemoryMiB;
    out.resident_bytes=reserve;
    out.total_bytes=out.ram_bytes+out.cdram_bytes+reserve;
    return true;
}
}
