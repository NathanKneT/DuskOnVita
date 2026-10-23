#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
namespace twv::vgl {
// One entry: exact equality has no hash collision or pointer identity shortcut.
class TextureConversionCache {
    void* payload_=nullptr;
    size_t sourceSize_=0,decodedSize_=0;
    uint32_t format_=0,width_=0,height_=0,levels_=0;
    bool arbitrary_=false;
public:
    TextureConversionCache()=default;
    TextureConversionCache(const TextureConversionCache&)=delete;
    TextureConversionCache& operator=(const TextureConversionCache&)=delete;
    static constexpr size_t limit=2*1024*1024;
    ~TextureConversionCache(){clear();}
    void clear() noexcept {std::free(payload_);payload_=nullptr;sourceSize_=decodedSize_=0;}
    size_t used() const noexcept {return sizeof(*this)+(payload_?sourceSize_+decodedSize_:0);}
    const uint8_t* find(uint32_t f,uint32_t w,uint32_t h,uint32_t m,const void* src,size_t n,bool& arbitrary,size_t& decoded) const noexcept {
        if(!payload_||!src||f!=format_||w!=width_||h!=height_||m!=levels_||n!=sourceSize_||std::memcmp(src,payload_,n))return nullptr;
        arbitrary=arbitrary_;decoded=decodedSize_;
        return static_cast<const uint8_t*>(payload_)+sourceSize_;
    }
    bool put(uint32_t f,uint32_t w,uint32_t h,uint32_t m,const void* src,size_t n,const void* rgba,size_t bytes,bool arbitrary,size_t budget) noexcept {
        clear();
        if(budget>limit)budget=limit;
        if(!src||!rgba||!n||!bytes||budget<sizeof(*this)||n>budget-sizeof(*this)||bytes>budget-sizeof(*this)-n)return false;
        payload_=std::malloc(n+bytes);if(!payload_)return false;
        std::memcpy(payload_,src,n);std::memcpy(static_cast<uint8_t*>(payload_)+n,rgba,bytes);
        sourceSize_=n;decodedSize_=bytes;format_=f;width_=w;height_=h;levels_=m;arbitrary_=arbitrary;return true;
    }
};
}
