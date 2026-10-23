#pragma once
// G1 GXP disk cache: format and key only (no SDK; host-tested in tests/gxm_shader_cache_test.cpp).
// Compiled programs are produced on the user's console by the user's own libshacccg.suprx
// and stay on that memory card; nothing compiled ships with the port.
// File: ux0:data/twilight-vita/gxp_cache/<v|f>/<xx>/<key>.gxp = 40-byte header + GXP blob.
// The key covers the format version, the stage, every compile option we set, a fingerprint
// of the compiler module, and the exact source. The header repeats the key and the source
// hash; the backend also runs sceGxmProgramCheck on the blob before registering it.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace twv::gxm {
constexpr uint32_t StageVertex=1,StageFragment=2; // = gfx_vgl VertexSourceTag/FragmentSourceTag
constexpr uint32_t CacheVersion=1;
constexpr char CacheMagic[8]={'T','W','V','G','X','P','0','1'};
// The options gxm_backend.cpp passes to sceShaccCgCompileProgram, in the order of its
// assignments: optimisation 3, fastmath 1, fastprecision 0, fastint 1, useFx 1, locale 0,
// warning level 3, performance warnings 1, pedantic 0, SceShaccCgExt extensions on.
// These are the settings vitaGL uses (vitaShaRK SHARK_OPT_FAST, LOG_ERRORS warning level).
constexpr uint32_t CompileOptionWords[10]={3,1,0,1,1,0,3,1,0,1};
struct CacheHeader {
    char magic[8];
    uint32_t version,stage;
    uint64_t key,sourceHash;
    uint32_t gxpBytes,reserved;
};
static_assert(sizeof(CacheHeader)==40);

inline uint64_t source_hash(const char* source) {return XXH3_64bits(source,std::strlen(source));}
inline uint64_t cache_key(uint32_t stage,uint64_t compilerFingerprint,const char* source) {
    XXH3_state_t s;XXH3_64bits_reset(&s);
    XXH3_64bits_update(&s,CacheMagic,sizeof CacheMagic);
    const uint32_t head[2]={CacheVersion,stage};
    XXH3_64bits_update(&s,head,sizeof head);
    XXH3_64bits_update(&s,CompileOptionWords,sizeof CompileOptionWords);
    XXH3_64bits_update(&s,&compilerFingerprint,sizeof compilerFingerprint);
    XXH3_64bits_update(&s,source,std::strlen(source));
    return XXH3_64bits_digest(&s);
}
inline void cache_path(char* out,size_t size,const char* root,uint32_t stage,uint64_t key) {
    std::snprintf(out,size,"%s/%s/%02x/%016llx.gxp",root,stage==StageVertex?"v":"f",
                  unsigned(key>>56),static_cast<unsigned long long>(key));
}
inline CacheHeader make_header(uint32_t stage,uint64_t key,uint64_t sourceHash,uint32_t gxpBytes) {
    CacheHeader h{};std::memcpy(h.magic,CacheMagic,sizeof CacheMagic);
    h.version=CacheVersion;h.stage=stage;h.key=key;h.sourceHash=sourceHash;h.gxpBytes=gxpBytes;
    return h;
}
// Returns the GXP bytes inside a cache file, or null when anything disagrees.
inline const unsigned char* parse_cache_file(const unsigned char* bytes,size_t size,uint32_t stage,
                                             uint64_t key,uint64_t sourceHash,uint32_t& gxpBytes) {
    gxpBytes=0;
    if(!bytes || size<sizeof(CacheHeader))return nullptr;
    CacheHeader h;std::memcpy(&h,bytes,sizeof h);
    if(std::memcmp(h.magic,CacheMagic,sizeof CacheMagic) || h.version!=CacheVersion || h.stage!=stage ||
       h.key!=key || h.sourceHash!=sourceHash || h.reserved || !h.gxpBytes || h.gxpBytes!=size-sizeof h)
        return nullptr;
    gxpBytes=h.gxpBytes;return bytes+sizeof h;
}
// Compiler fingerprint: the module's own bytes (any two different builds differ).
inline uint64_t compiler_fingerprint(const void* module,size_t size) {return XXH3_64bits(module,size);}
}
