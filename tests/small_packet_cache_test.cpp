#include "gfx_vgl/small_packet_cache.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace twv::vgl;

int main() {
    SmallPacketCache cache;
    static_assert(SmallPacketCache::Capacity==64);
    static_assert(SmallPacketCache::data_footprint()<180*1024);
    assert(cache.stats.dataFootprint==sizeof cache);
    SmallPacketKey key{7,4,16,9};
    std::array<uint8_t,512> source{},relocated{};
    std::array<uint8_t,2048> output{};
    for(unsigned i=0;i<source.size();++i)source[i]=uint8_t(i*17+3);
    for(unsigned i=0;i<output.size();++i)output[i]=uint8_t(i*19+7);
    constexpr uint32_t hash=0x12345678;
    assert(!cache.lookup(hash,key,source.data(),64,128).data);
    assert(cache.store(hash,key,source.data(),64,128,output.data(),10));
    relocated=source;
    auto hit=cache.lookup(hash,key,relocated.data(),64,128);
    assert(hit.data && hit.data!=output.data() && hit.bytes==128 && hit.pnSlots==10);
    assert(!std::memcmp(hit.data,output.data(),128));
    output[0]^=1;assert(hit.data[0]!=output[0]);output[0]^=1;
    // Hash is intentionally held constant: every byte must be compared exactly.
    for(unsigned i=0;i<64;++i) {
        relocated[i]^=1;
        assert(!cache.lookup(hash,key,relocated.data(),64,128).data);
        relocated[i]^=1;
        assert(cache.lookup(hash,key,relocated.data(),64,128).data);
    }
    for(unsigned field=0;field<4;++field) {
        auto changed=key;
        if(field==0)++changed.pipeline;
        if(field==1)changed.vertexCount=2;
        if(field==2)changed.inputStride=8;
        if(field==3)--changed.currentPnMtx;
        const size_t bytes=size_t(changed.vertexCount)*changed.inputStride;
        assert(!cache.lookup(hash,changed,source.data(),bytes,128).data);
    }
    assert(!cache.lookup(hash,key,source.data(),64,124).data);
    assert(!cache.lookup(hash+16,key,source.data(),64,128).data); // Same bucket, unequal hash.
    // Exact-hash collision with another source can coexist with the first one.
    relocated[31]^=1;
    assert(cache.store(hash,key,relocated.data(),64,128,output.data(),9));
    assert(cache.lookup(hash,key,source.data(),64,128).pnSlots==10);
    assert(cache.lookup(hash,key,relocated.data(),64,128).pnSlots==9);
    const auto priorHits=cache.stats.hits;cache.clear();
    assert(!cache.lookup(hash,key,source.data(),64,128).data && cache.stats.hits==priorHits);

    // Fill all sets, touch one entry and replace the actual least-recent entry.
    for(unsigned set=0;set<16;++set)for(unsigned way=0;way<4;++way) {
        auto entryKey=key;entryKey.pipeline=100+set*4+way;
        assert(cache.store(set,entryKey,source.data(),64,128,output.data(),1));
    }
    auto oldest=key;oldest.pipeline=100;
    auto second=key;second.pipeline=101;
    assert(cache.lookup(0,oldest,source.data(),64,128).data);
    auto replacement=key;replacement.pipeline=999;
    assert(cache.store(0,replacement,source.data(),64,128,output.data(),2));
    assert(!cache.lookup(0,second,source.data(),64,128).data);
    assert(cache.lookup(0,oldest,source.data(),64,128).data);
    assert(cache.lookup(0,replacement,source.data(),64,128).pnSlots==2);
    assert(cache.stats.replacements==1);
    // Updating an existing entry changes its CPU payload without evicting one.
    output[0]^=1;
    assert(cache.store(0,replacement,source.data(),64,128,output.data(),3));
    assert(cache.lookup(0,replacement,source.data(),64,128).data[0]==output[0]);
    assert(cache.stats.replacements==1);

    auto rejected=[&](SmallPacketKey bad,const void* data,size_t bytes,size_t outBytes) {
        assert(!cache.lookup(hash,bad,data,bytes,outBytes).data);
        assert(!cache.store(hash,bad,data,bytes,outBytes,output.data(),1));
    };
    auto bad=key;bad.vertexCount=0;rejected(bad,source.data(),0,128);
    bad=key;bad.vertexCount=32;rejected(bad,source.data(),512,128);
    bad=key;bad.inputStride=0;rejected(bad,source.data(),0,128);
    bad=key;bad.inputStride=UINT32_MAX;rejected(bad,source.data(),64,128);
    rejected(key,nullptr,64,128);rejected(key,source.data(),63,128);
    rejected(key,source.data(),65,128);rejected(key,source.data(),513,128);
    rejected(key,source.data(),64,0);rejected(key,source.data(),64,2049);
    const auto* overflow=reinterpret_cast<const void*>(UINTPTR_MAX-31);
    rejected(key,overflow,64,128);
    assert(!cache.store(hash,key,source.data(),64,128,nullptr,1));
    assert(!cache.store(hash,key,source.data(),64,128,overflow,1));
    for(unsigned pn:{0u,11u,UINT32_MAX})assert(!cache.store(hash,key,source.data(),64,128,output.data(),pn));

    // Exact maximum source/output bounds and complete source-byte mutation scan.
    cache.clear();key={2,16,32,0};
    assert(cache.store(hash,key,source.data(),512,2048,output.data(),10));
    for(unsigned byte=0;byte<512;++byte) {
        source[byte]^=1;assert(!cache.lookup(hash,key,source.data(),512,2048).data);source[byte]^=1;
    }
    hit=cache.lookup(hash,key,source.data(),512,2048);
    assert(hit.bytes==2048 && !std::memcmp(hit.data,output.data(),2048));
    // Returned payload can safely be an input to a later store, including victim reuse.
    assert(cache.store(hash,key,source.data(),512,2048,hit.data,10));
    assert(cache.lookup(hash,key,source.data(),512,2048).data);
    assert(cache.stats.hits && cache.stats.misses && cache.stats.collisions && cache.stats.rejected);
    assert(cache.stats.sourceCopyBytes && cache.stats.outputCopyBytes && cache.stats.validationBytes);
    assert(cache.stats.lookupCalls==cache.stats.hits+cache.stats.misses);
    std::printf("small_packet_cache_test: exact relocated/mutated bytes, collisions, 64-entry LRU, bounds, PN, owned copies and clear passed; footprint=%zu bytes\n",sizeof cache);
}
