#include "gfx_vgl/geometry_cache.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

int main() {
    using namespace twv::vgl;
    GeometryCache cache(128);
    uint8_t raw[8]={1,2,3},array[8]={4,5,6},packet[16]={7,8,9};
    SourceSpan spans[]={{raw,sizeof raw},{array,sizeof array}};
    GeometryKey key{};key.pipeline=7;key.vertexCount=4;key.vertexBase=42;
    assert(cache.lookup(2,key).mayStore);
    assert(cache.store(2,key,spans,2,packet,sizeof packet,1));
    assert(cache.bytes()==32);
    auto hit=cache.lookup(2,key);
    assert(hit.data && hit.bytes==16 && !hit.mayStore && hit.pnSlots==1 && std::memcmp(hit.data,packet,16)==0);
    // Mutable vertex arrays must invalidate even when every address/index stays
    // the same. A later identical address can never replay the earlier geometry.
    array[3]=99;
    auto changed=cache.lookup(2,key);
    assert(!changed.data && !changed.mayStore && cache.bytes()==0);
    assert(!cache.store(2,key,spans,2,packet,16));
    cache.clear();packet[0]=44;
    assert(cache.store(2,key,spans,2,packet,16,10));
    assert(cache.lookup(2,key).data[0]==44 && cache.lookup(2,key).pnSlots==10);
    raw[1]=17;assert(!cache.lookup(2,key).data && cache.bytes()==0);
    cache.clear();

    // Unused register-array bindings and a PN default overridden in every raw
    // vertex must not create false cache identities. Used dependencies remain.
    GeometryKey allBindings{};allBindings.pipeline=10;allBindings.vertexBase=12;
    allBindings.currentPnMtx=8;allBindings.arrays[0]=100;allBindings.arrays[11]=987;
    auto unrelated=allBindings;unrelated.arrays[11]=222;unrelated.currentPnMtx=2;
    const auto canonical=canonical_geometry_key(allBindings,1,false);
    auto same=canonical_geometry_key(unrelated,1,false);
    assert(std::memcmp(&canonical,&same,sizeof same)==0);
    assert(cache.store(8,canonical,spans,2,packet,16));
    assert(cache.lookup(8,same).data);
    unrelated.arrays[0]=101;same=canonical_geometry_key(unrelated,1,false);
    assert(!cache.lookup(8,same).data);
    same=canonical_geometry_key(allBindings,1,true);
    assert(!cache.lookup(8,same).data);
    cache.clear();

    // Forced hash/set collision: full keys, not pointers or hashes alone, decide.
    auto second=key;second.pipeline=8;
    auto third=key;third.currentPnMtx=9;
    GeometryCache collision(1024);
    std::array<GeometryKey,GeometryCache::WayCount+1> colliding{};
    for(unsigned i=0;i<colliding.size();++i) {
        colliding[i]=key;colliding[i].pipeline=100+i;packet[0]=44+i;
        assert(collision.store(3,colliding[i],spans,2,packet,16)==(i<GeometryCache::WayCount));
    }
    for(unsigned i=0;i<GeometryCache::WayCount;++i)
        assert(collision.lookup(3,colliding[i]).data[0]==44+i);
    assert(!collision.lookup(3,colliding.back()).data && collision.stats.setDeferrals>0);
    assert(collision.bytes()==GeometryCache::WayCount*32);
    collision.end_frame(); // Entries used this frame remain available to next frame.
    assert(collision.lookup(3,colliding[0]).data);
    collision.end_frame();
    collision.end_frame(); // No draw used the entries: release their CPU allocations.
    assert(collision.bytes()==0);
    packet[0]=66;assert(collision.store(3,third,spans,2,packet,16));
    assert(collision.lookup(3,third).data[0]==66);

    // Same 512 slots/payload in both policies. A recurring cluster of four
    // distinct full keys sharing a hash must stay resident in the four-way
    // policy; two-way must render misses normally instead of displacing hot
    // entries. No data aliasing, packet loss or memory growth is allowed.
    GeometryCacheImpl<2> twoWay(1024);GeometryCacheImpl<4> fourWay(1024);
    static_assert(decltype(twoWay)::EntryCount==decltype(fourWay)::EntryCount);
    uint8_t clusterInput[4][8]{},clusterOutput[4][16]{};
    GeometryKey clusterKeys[4]{};
    for(unsigned i=0;i<4;++i){clusterKeys[i]=key;clusterKeys[i].pipeline=200+i;}
    for(unsigned frame=0;frame<50;++frame) {
        for(unsigned i=0;i<4;++i) {
            clusterInput[i][0]=i;clusterOutput[i][0]=44+i;
            SourceSpan input{clusterInput[i],8};
            const auto visit=[&](auto& c) {
                auto found=c.lookup(13,clusterKeys[i]);
                if(found.data)assert(found.data[0]==44+i);
                else if(found.mayStore)assert(c.store(13,clusterKeys[i],&input,1,clusterOutput[i],16));
                assert(c.bytes()<=1024);
            };
            visit(twoWay);visit(fourWay);
        }
        twoWay.end_frame();fourWay.end_frame();
    }
    assert(twoWay.stats.hits==2*49 && fourWay.stats.hits==4*49);
    assert(twoWay.stats.setDeferrals>0 && fourWay.stats.setDeferrals==0);

    GeometryCache tiny(31);
    assert(!tiny.store(0,key,spans,2,packet,16) && tiny.bytes()==0);
    assert(!tiny.lookup(0,key).mayStore); // Rejected spans must not be rescanned every frame.
    GeometryCache bounded(32);
    assert(bounded.store(0,key,spans,2,packet,16));
    assert(!bounded.store(1,second,spans,2,packet,16));
    assert(bounded.bytes()==32 && bounded.lookup(0,key).data && !bounded.lookup(1,second).mayStore);
    bounded.clear();assert(bounded.bytes()==0);
    assert(!bounded.store(0,key,spans,2,packet,16,0));
    assert(!bounded.store(1,second,spans,2,packet,16,11));

    // A scene larger than the cache must keep a warm subset, not cyclically
    // rebuild every entry. Sixteen of24 packets fit: expect16 hits each later
    // frame, with full original decoding left to the caller on every miss.
    GeometryCache overBudget(1024);
    uint8_t sourceSet[24][32]{},packetSet[24][32]{};
    GeometryKey keys[24]{};
    for(unsigned i=0;i<24;++i){keys[i].pipeline=i;sourceSet[i][0]=i;packetSet[i][0]=i;}
    for(unsigned frame=0;frame<100;++frame) {
        for(unsigned i=0;i<24;++i) {
            auto result=overBudget.lookup(i,keys[i]);
            if(result.data)assert(result.data[0]==i);
            else if(result.mayStore) {
                SourceSpan source{sourceSet[i],32};
                overBudget.store(i,keys[i],&source,1,packetSet[i],32,1);
            }
        }
        assert(overBudget.bytes()<=1024);
        overBudget.end_frame();
    }
    assert(overBudget.stats.hits==16*99 && overBudget.stats.budgetDeferrals==1);
    assert(overBudget.stats.budgetEvictions==0 && overBudget.stats.admissionBlocked>0);
    overBudget.end_frame(); // A different scene can reclaim the now-unused subset.
    assert(overBudget.bytes()==0 && overBudget.lookup(23,keys[23]).mayStore);

    uint8_t continuous[64]{};
    std::array<SourceSpan,MaxGeometrySpans> overlap{};
    overlap[0]={continuous+8,16};overlap[1]={continuous+4,12};overlap[2]={continuous+32,8};
    assert(merge_source_spans(overlap,3)==2);
    assert(overlap[0].data==continuous+4 && overlap[0].size==20 && overlap[1].data==continuous+32);
    // The live raw packet must be checked first even when it has a higher
    // address than its array. A new lower index may make the old array tail
    // inaccessible after an allocation is reused/shrunk at that same address.
    alignas(16) uint8_t reused[64]{};
    std::array<SourceSpan,MaxGeometrySpans> indexed{};
    indexed[0]={reused+32,8};indexed[1]={reused,16};indexed[2]={reused+8,16};
    assert(merge_source_spans(indexed,3,1)==2);
    assert(indexed[0].data==reused+32 && indexed[0].size==8);
    assert(indexed[1].data==reused && indexed[1].size==24);
    GeometryCache reusedCache(256);
    assert(reusedCache.store(0,key,indexed.data(),2,packet,16,1));
    reused[32]=1;
#if defined(__SANITIZE_ADDRESS__)
    __asan_poison_memory_region(reused+8,16);
#endif
    assert(!reusedCache.lookup(0,key).data); // Must never read the old array tail.
#if defined(__SANITIZE_ADDRESS__)
    __asan_unpoison_memory_region(reused+8,16);
#endif
    std::puts("geometry_cache_test: index-before-array lifetime checks, exact replay, mutable bytes, address reuse, collisions, bounds, pruning and overlap passed");
}
