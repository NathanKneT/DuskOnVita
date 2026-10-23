#include "../platform/vita/gfx_vgl/texture_conversion_cache.hpp"
#include <cassert>
int main(){
    using twv::vgl::TextureConversionCache;
    TextureConversionCache cache;
    unsigned char src[]={1,2,3,4},other[]={1,2,3,4},rgba[16]{};
    bool arbitrary=false;size_t bytes=0;
    assert(cache.put(1,2,2,1,src,4,rgba,16,true,1024));
    assert(cache.find(1,2,2,1,other,4,arbitrary,bytes));
    assert(arbitrary&&bytes==16);
    src[0]=9;
    assert(!cache.find(1,2,2,1,src,4,arbitrary,bytes));
    assert(!cache.find(2,2,2,1,other,4,arbitrary,bytes));
    assert(!cache.find(1,3,2,1,other,4,arbitrary,bytes));
    assert(!cache.find(1,2,2,2,other,4,arbitrary,bytes));
    assert(!cache.find(1,2,2,1,other,3,arbitrary,bytes));
    assert(cache.used()<=TextureConversionCache::limit);
    cache.clear();assert(!cache.find(1,2,2,1,other,4,arbitrary,bytes));
    assert(!cache.put(1,2,2,1,other,4,rgba,16,false,0));
    assert(!cache.put(1,2,2,1,other,SIZE_MAX,rgba,16,false,SIZE_MAX));
    assert(!cache.put(1,2,2,1,nullptr,4,rgba,16,false,1024));
    assert(cache.put(1,2,2,1,other,4,rgba,16,false,1024));
    assert(!cache.put(1,2,2,1,other,4,rgba,16,false,sizeof(cache)+19));
    assert(!cache.find(1,2,2,1,other,4,arbitrary,bytes));
}
