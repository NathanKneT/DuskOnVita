#include "gfx_vgl/exact_bytes.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

using twv::vgl::exact_bytes_equal;

int main() {
    assert(exact_bytes_equal(nullptr,nullptr,0));
    std::array<uint8_t,288> left{},right{};
    // Packed GX arrays can start at any byte alignment. Every changed byte
    // must invalidate a snapshot, including the block boundary and short tail.
    for(size_t a=0;a<16;++a)for(size_t b=0;b<16;++b) {
        for(size_t n=0;n<=257;++n) {
            for(size_t i=0;i<n;++i)left[a+i]=right[b+i]=uint8_t(i*73+19);
            assert(exact_bytes_equal(left.data()+a,right.data()+b,n));
            for(size_t i=0;i<n;++i) {
                right[b+i]^=uint8_t(1u<<(i%8));
                assert(!exact_bytes_equal(left.data()+a,right.data()+b,n));
                assert(std::memcmp(left.data()+a,right.data()+b,n)!=0);
                right[b+i]^=uint8_t(1u<<(i%8));
            }
        }
    }
    for(size_t n:{4096u,65537u,2097152u}) {
        std::vector<uint8_t> a(n+16),b(n+16);
        for(size_t i=0;i<n;++i)a[i+1]=b[i+7]=uint8_t(i*31+(i>>8));
        assert(exact_bytes_equal(a.data()+1,b.data()+7,n));
        for(size_t i:{size_t(0),n/2,n-1}) {
            b[i+7]^=0x80;
            assert(!exact_bytes_equal(a.data()+1,b.data()+7,n));
            b[i+7]^=0x80;
        }
    }
    // A requested span may end at an inaccessible page. Wider loads must not
    // cross it even for zero bytes, unaligned blocks or a short final tail.
    const auto page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* mapping=static_cast<uint8_t*>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,
                                            MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
    assert(mapping!=MAP_FAILED);
    assert(mprotect(mapping+page,page,PROT_NONE)==0);
    std::vector<uint8_t> other(544);
    for(size_t n=0;n<=512;++n)for(size_t offset=0;offset<16;++offset) {
        auto* end=mapping+page-n;
        for(size_t i=0;i<n;++i)end[i]=other[i+offset]=uint8_t(i*53+7);
        assert(exact_bytes_equal(end,other.data()+offset,n));
        assert(exact_bytes_equal(other.data()+offset,end,n));
        if(n) {
            end[n-1]^=1;
            assert(!exact_bytes_equal(end,other.data()+offset,n));
            assert(!exact_bytes_equal(other.data()+offset,end,n));
        }
    }
    assert(munmap(mapping,page*2)==0);
    std::puts("exact_bytes_test: every alignment, changed byte, block/tail boundary, large span and protected-page end passed");
}
