#pragma once
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <unordered_map>

namespace twv::vgl {
// Hashing narrows candidates only. A complete key comparison remains mandatory,
// including on 32-bit targets where Aurora deliberately truncates its hash.
template<class Key> class ExactKeyIndex {
    static_assert(std::has_unique_object_representations_v<Key>);
    struct Entry { Key key; uint32_t id; };
    std::unordered_multimap<uint64_t,Entry> entries;
public:
    uint32_t find(uint64_t hash,const Key& key) const {
        const auto range=entries.equal_range(hash);
        for(auto it=range.first;it!=range.second;++it)
            if(std::memcmp(&it->second.key,&key,sizeof key)==0) return it->second.id;
        return UINT32_MAX;
    }
    void insert(uint64_t hash,const Key& key,uint32_t id) { entries.emplace(hash,Entry{key,id}); }
    void clear() { entries.clear(); }
};
}
