#include "gfx_vgl/attribute_stream.hpp"
#include <array>
#include <vector>
#include <cassert>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
using namespace twv::vgl;
struct Attribute {uint32_t attrType,compType,frac,offset,stride;bool le;};
static constexpr uint32_t Patterns[]={0,1,0x80000000,0x7fffffff,0x8000,0x7fff,0xffff,0x80,0x7f,0xff,0x3f800000,0x007fffff,0x7f800000,0xff800000,0x7fc12345,0x7fa12345,0xffffffff};
static unsigned width(unsigned type){return type<=GX_S8?1:type<=GX_S16?2:4;}
static void wire(uint8_t*p,uint32_t value,unsigned bytes,bool little){for(unsigned i=0;i<bytes;++i)p[little?i:bytes-1-i]=uint8_t(value>>(8*i));}
static void compare(const AttributeStream<Attribute>& input,uint32_t vertices,unsigned components,unsigned outputStride,unsigned skew){
 std::vector<uint8_t> expected(size_t(vertices)*12+16,0x6d),actual(size_t(vertices)*outputStride+skew+16,0xa5);
 pack_numeric_attribute(input,expected.data(),12,vertices,components,1.f);
 pack_numeric_attribute_st(input,actual.data()+skew,outputStride,vertices,components);
 for(unsigned v=0;v<vertices;++v)assert(!std::memcmp(expected.data()+size_t(v)*12,actual.data()+skew+size_t(v)*outputStride,8));
 for(size_t i=0;i<actual.size();++i){
  const bool written=i>=skew && (i-skew)/outputStride<vertices && (i-skew)%outputStride<8;
  if(!written)assert(actual[i]==0xa5);
 }
}
int main(){
 for(unsigned kind:{GX_DIRECT,GX_INDEX8,GX_INDEX16})for(bool le:{false,true})
  for(unsigned type=GX_U8;type<=GX_F32;++type)for(unsigned components:{1u,2u,3u})
   for(unsigned frac:{0u,1u,6u,14u,31u}){
    Attribute a{kind,type,frac,3,components*width(type)+3,le};
    constexpr unsigned Entries=384,Vertices=128;
    const unsigned packetStride=kind==GX_DIRECT?a.offset+components*width(type)+3:7;
    std::vector<uint8_t> packet(size_t(Vertices)*packetStride+16,0xb3),array(size_t(Entries)*a.stride+3,0xb7);
    auto* sourceArray=array.data()+1; // Every source entry can be unaligned.
    for(unsigned v=0;v<Entries;++v)for(unsigned c=0;c<components;++c)
     wire(sourceArray+size_t(v)*a.stride+c*width(type),Patterns[(v+c)%std::size(Patterns)],width(type),le);
    for(unsigned v=0;v<Vertices;++v){
     auto* p=packet.data()+size_t(v)*packetStride+a.offset;
     if(kind==GX_DIRECT){for(unsigned c=0;c<components;++c)wire(p+c*width(type),Patterns[(v+c)%std::size(Patterns)],width(type),false);}
     else {const unsigned index=(v*37+257)%(kind==GX_INDEX8?256:Entries);wire(p,index,kind==GX_INDEX8?1:2,false);}
    }
    const AttributeStream<Attribute> input{a,packet.data(),sourceArray,packetStride};
    for(unsigned count=0;count<=Vertices;++count)for(unsigned stride:{8u,13u,31u})compare(input,count,components,stride,count%4);
   }
 // Source, wire index and output each end immediately before an inaccessible
 // page: accidental 3F output stores or wide source/index reads must fault.
 const size_t page=size_t(sysconf(_SC_PAGESIZE));
 auto* pages=static_cast<uint8_t*>(mmap(nullptr,page*6,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
 assert(pages!=MAP_FAILED);
 for(unsigned p:{1u,3u,5u})assert(mprotect(pages+page*p,page,PROT_NONE)==0);
 for(unsigned kind:{GX_DIRECT,GX_INDEX8,GX_INDEX16})for(bool le:{false,true})
  for(unsigned type=GX_U8;type<=GX_F32;++type)for(unsigned components:{1u,2u,3u}){
   Attribute a{kind,type,14,3,components*width(type),le};
   auto* source=pages+page-components*width(type);
   auto* packet=kind==GX_DIRECT?source-a.offset:pages+3*page-(kind==GX_INDEX8?1:2)-a.offset;
   if(kind!=GX_DIRECT)wire(packet+a.offset,0,kind==GX_INDEX8?1:2,false);
   for(unsigned pattern=0;pattern<std::size(Patterns);++pattern){
    for(unsigned c=0;c<components;++c)wire(source+c*width(type),Patterns[(pattern+c)%std::size(Patterns)],width(type),kind==GX_DIRECT?false:le);
    AttributeStream<Attribute> in{a,packet,source,a.offset+components*width(type)};
    float oracle[3];pack_numeric_attribute(in,reinterpret_cast<uint8_t*>(oracle),12,1,components);
    auto* output=pages+5*page-8;std::memset(output-8,0xa5,16);
    pack_numeric_attribute_st(in,output,8,1,components);
    assert(!std::memcmp(output,oracle,8));
    for(unsigned i=0;i<8;++i)assert(output[-8+int(i)]==0xa5);
   }
  }
 assert(munmap(pages,page*6)==0);
 const float fallback[3]={-0.f,1.f,9.f};
 for(unsigned count=0;count<=128;++count){
  std::vector<uint8_t> output(size_t(count)*13+9,0xa5);
  default_attribute_stream(output.data()+1,13,count,fallback,8);
  for(size_t i=0;i<output.size();++i){
   const bool written=i>=1 && (i-1)/13<count && (i-1)%13<8;
   if(written)assert(output[i]==reinterpret_cast<const uint8_t*>(fallback)[(i-1)%13]);
   else assert(output[i]==0xa5);
  }
 }
 Attribute a{GX_DIRECT,GX_F32,0,0,0,false};
 pack_numeric_attribute_st(AttributeStream<Attribute>{a,nullptr,nullptr,0},nullptr,8,0,2);
}
