#include "gfx_vgl/vertex_decoder_control.hpp"
#include "gfx_vgl/counter_batch.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

namespace twv::vgl {
VertexDecoderControl shadowImageSkipControl{bool(TWV_TEST_SHADOW_COMPILED)};
bool offscreen=false,OffscreenDrawSupported=false;
std::map<std::string,unsigned> counters;
void count(const char* name,unsigned amount=1){counters[name]+=amount;}
void count(CounterId id,unsigned amount=1){count(CounterNames[size_t(id)],amount);}
}
namespace twv::gfx_vgl {
#include "shadow_image_control.inc"
}
using twv::gfx_vgl::twv_vita_shadow_image_skip;
using twv::gfx_vgl::twv_vita_shadow_image_skipped;
using twv::gfx_vgl::twv_vita_shadow_image_skip_select;
using u8=std::uint8_t;using u16=std::uint16_t;
using Mtx=float[3][4];
constexpr int GX_ORTHOGRAPHIC=1;
unsigned projectionCalls=0,preDrawCalls=0,drawCalls=0;
float lastProjection=0;
void GXSetProjection(const float (*matrix)[4],int mode){assert(mode==GX_ORTHOGRAPHIC);++projectionCalls;lastProjection=matrix[0][0];}
#define JUT_ASSERT(line,condition) assert(condition)
bool interpolate=false;
const void* getInterpKey(const void* base,int index){return reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(base)^index);}
namespace dusk::interp {
bool lookup_replacement(const void*,Mtx matrix){if(interpolate)matrix[0][0]=42;return interpolate;}
}
struct J3DShape {
    unsigned flags=0;
    bool checkFlag(unsigned flag)const{return (flags&flag)!=0;}
    void loadPreDrawSetting(){++preDrawCalls;}
};
struct J3DShapePacket {
    Mtx* base=nullptr;bool hidden=false;
    void setBaseMtxPtr(Mtx* matrix){base=matrix;}
    void drawFast(){if(!hidden)++drawCalls;}
};
struct J3DModelData {
    std::array<J3DShape,4> shapes{};u16 count=4;
    J3DShape* getShapeNodePointer(u16 n){return &shapes.at(n);}
    u16 getShapeNum()const{return count;}
};
struct J3DModel {
    J3DModelData data;std::array<J3DShapePacket,4> packets{};
    J3DModelData* getModelData(){return &data;}
    J3DShapePacket* getShapePacket(u16 n){return &packets.at(n);}
};
struct dDlst_shadowReal_c {
    Mtx mRenderProjMtx{},mViewMtx{};
    u8 mModelNum=2;J3DModel* mpModels[2]{};
    void imageDraw(Mtx);void legacyImageDraw(Mtx);
};
#include "shadow_image_body.inc"

int main(){
    using namespace twv::vgl;
    for(int choice:{-1,2,0,1})for(bool supported:{false,true})for(bool off:{false,true})for(bool interp:{false,true}){
        shadowImageSkipControl=VertexDecoderControl(bool(TWV_TEST_SHADOW_COMPILED));
        offscreen=off;OffscreenDrawSupported=supported;counters.clear();
        assert(twv_vita_shadow_image_skip_select(choice)==int(choice==0 || (choice==1 && TWV_TEST_SHADOW_COMPILED)));
        const bool selected=shadowImageSkipControl.selected();
        if(selected)assert(!twv_vita_shadow_image_skip_select(0));
        interpolate=interp;Mtx camera{},old{};
        J3DModel models[2];models[0].data.count=4;models[1].data.count=3;
        models[0].data.shapes[1].flags=1;models[1].data.shapes[0].flags=1;
        models[0].data.shapes[2].flags=2; // other flags do not suppress restoration
        models[0].packets[2].hidden=true; // packet-hidden != shape-hidden
        for(auto& model:models)for(auto& packet:model.packets)packet.base=&old;
        dDlst_shadowReal_c shadow;shadow.mpModels[0]=&models[0];shadow.mpModels[1]=&models[1];
        shadow.mRenderProjMtx[0][0]=13;
        projectionCalls=preDrawCalls=drawCalls=0;
        shadow.legacyImageDraw(camera);
        assert(projectionCalls==1 && preDrawCalls==2 && drawCalls==4);
        std::array<Mtx*,8> expected{};unsigned n=0;
        for(auto& model:models)for(auto& packet:model.packets){expected[n++]=packet.base;packet.base=&old;}
        projectionCalls=preDrawCalls=drawCalls=0;
        shadow.imageDraw(camera);
        assert(projectionCalls==1 && lastProjection==(interp?42:13));
        n=0;for(const auto& model:models)for(const auto& packet:model.packets)assert(expected[n++]==packet.base);
        assert(shadow.mModelNum==2 && shadow.mRenderProjMtx[0][0]==13 && models[0].data.shapes[2].flags==2);
        const bool skip=TARGET_VITA && TWV_TEST_SHADOW_COMPILED && choice==1 && off && !supported;
        assert(preDrawCalls==(skip?0:2) && drawCalls==(skip?0:4));
        assert(counters["shadow_image_skipped_calls"]==unsigned(skip));
        assert(counters["shadow_image_skipped_models"]==(skip?2u:0u));
        assert(counters["shadow_image_restored_packets"]==(skip?5u:0u));
        assert(counters["shadow_image_calls"]==unsigned(bool(TARGET_VITA)));
        if(TARGET_VITA)assert(!twv_vita_shadow_image_skip_select(0) && !twv_vita_shadow_image_skip_select(1));
    }
    shadowImageSkipControl=VertexDecoderControl(bool(TWV_TEST_SHADOW_COMPILED));
    offscreen=true;OffscreenDrawSupported=false;
    assert(!twv_vita_shadow_image_skip()); // default OFF even on an unsupported pass
    assert(!twv_vita_shadow_image_skip_select(1));
    std::printf("shadow_image_test: Vita%d compiled%d: actual patched/original bodies, backend selector, supported/onscreen/default negative controls, interpolation and packet restoration passed\n",TARGET_VITA,TWV_TEST_SHADOW_COMPILED);
}
