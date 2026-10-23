// Read-only main-thread snapshot after FIFO drain, before scene-probe throttling.
// Build in the original game's int-enum domain, never with Vita SDK headers.
#include "target_probe.hpp"
#include "telemetry.hpp"
#include "d/d_com_inf_game.h"
#include "d/d_attention.h"
#include "f_op/f_op_actor_mng.h"
#include "JSystem/J3DGraphAnimator/J3DModel.h"
#include "JSystem/JUtility/JUTVideo.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_lib.h"
#include <cmath>
#include <cstdio>

namespace {
unsigned actor_id(const fopAc_ac_c* actor) {
    return actor?unsigned(fopAcM_GetID(actor)):~0u;
}
void actor_snapshot(const char* name,const fopAc_ac_c* actor) {
    if(!actor){twv::vita::telemetryText(name,"present=0");return;}
    char text[384];
    std::snprintf(text,sizeof text,
        "present=1 id=%u profile=%d world=%.6f,%.6f,%.6f attention=%.6f,%.6f,%.6f flags=%08x",
        actor_id(actor),int(fopAcM_GetProfName(actor)),
        double(actor->current.pos.x),double(actor->current.pos.y),double(actor->current.pos.z),
        double(actor->attention_info.position.x),double(actor->attention_info.position.y),
        double(actor->attention_info.position.z),unsigned(actor->attention_info.flags));
    twv::vita::telemetryText(name,text);
}
template<class Matrix>
void matrix_snapshot(const char* name,const Matrix& matrix,unsigned rows) {
    for(unsigned row=0;row<rows;++row) {
        char text[192];
        std::snprintf(text,sizeof text,"row=%u values=%.9g,%.9g,%.9g,%.9g",row,
            double(matrix[row][0]),double(matrix[row][1]),double(matrix[row][2]),double(matrix[row][3]));
        twv::vita::telemetryText(name,text);
    }
}
void project_snapshot(const char* name,const cXyz& point,const view_class& view,
                      const view_port_class& viewport,unsigned fbWidth,unsigned fbHeight) {
    double clip[4]{};
    for(unsigned row=0;row<4;++row)
        clip[row]=view.projViewMtx[row][0]*double(point.x)+view.projViewMtx[row][1]*double(point.y)+
                  view.projViewMtx[row][2]*double(point.z)+view.projViewMtx[row][3];
    const bool valid=std::isfinite(clip[0]) && std::isfinite(clip[1]) && std::isfinite(clip[2]) &&
        std::isfinite(clip[3]) && clip[3]>0.000001 && std::isfinite(viewport.x_orig) &&
        std::isfinite(viewport.y_orig) && std::isfinite(viewport.width) && std::isfinite(viewport.height) &&
        viewport.width>0 && viewport.height>0;
    // EventLog::text has a 254-byte payload limit. Bounded significant-digit
    // fields and separate records preserve every point label and derived value.
    char text[240];
    std::snprintf(text,sizeof text,"point=%s valid=%u world=%.9g,%.9g,%.9g clip=%.9g,%.9g,%.9g,%.9g",
        name,unsigned(valid),double(point.x),double(point.y),double(point.z),clip[0],clip[1],clip[2],clip[3]);
    twv::vita::telemetryText("target_probe_point_clip",text);
    if(valid) {
        const double x=viewport.x_orig+viewport.width*(1+clip[0]/clip[3])*0.5;
        const double y=viewport.y_orig+viewport.height*(1-clip[1]/clip[3])*0.5;
        // Diagnostic prediction only: assumes normal scaled onscreen policy.
        // These are not measured GPU positions or feedback from the GL viewport.
        const bool mapped=fbWidth && fbHeight;
        std::snprintf(text,sizeof text,
            "point=%s logical=%.9g,%.9g derived_fit_valid=%u derived_fit724x544_left118=%.9g,%.9g",
            name,x,y,
            unsigned(mapped),mapped?118+724*x/fbWidth:0,mapped?544*y/fbHeight:0);
        twv::vita::telemetryText("target_probe_point_screen",text);
    }
    // Original game helper, separately labelled: includes mirror/subviewport policy.
    Vec source=point,projected{};
    mDoLib_project(&source,&projected);
    std::snprintf(text,sizeof text,"point=%s x=%.9g y=%.9g z=%.9g finite=%u",name,double(projected.x),
        double(projected.y),double(projected.z),unsigned(std::isfinite(projected.x) && std::isfinite(projected.y) && std::isfinite(projected.z)));
    twv::vita::telemetryText("target_probe_original_project",text);
}
}

extern "C" void twv_vita_target_probe(unsigned frame) {
    auto* attention=dComIfGp_getPlayer(0)?dComIfGp_getAttention():nullptr;
    const bool locked=attention && attention->LockonTruth();
    auto* eligible=attention?attention->LockonTarget(0):nullptr;
    auto* list=attention?attention->GetLockonList(0):nullptr;
    auto* candidate=list?list->getActor():nullptr;
    static bool seen=false,lastLocked=false;
    static unsigned lastEligible=~0u,lastCandidate=~0u;
    const unsigned eligibleID=actor_id(eligible),candidateID=actor_id(candidate);
    const bool changed=!seen || locked!=lastLocked || eligibleID!=lastEligible || candidateID!=lastCandidate;
    if(!changed && frame%30)return;
    seen=true;lastLocked=locked;lastEligible=eligibleID;lastCandidate=candidateID;
    twv::vita::telemetryEvent("target_probe_frame",frame);
    char text[384];
    std::snprintf(text,sizeof text,"active=%u locked=%u eligible_id=%u candidate_id=%u changed=%u",
        unsigned(attention && (eligible || candidate)),unsigned(locked),eligibleID,candidateID,unsigned(changed));
    twv::vita::telemetryText("target_probe_state",text);
    actor_snapshot("target_probe_locked",locked?eligible:nullptr);
    actor_snapshot("target_probe_eligible",eligible);
    actor_snapshot("target_probe_candidate",candidate);
    if(!attention)return;
    const auto& draw=attention->draw[0];
    auto* model=draw.mDrawType<2?draw.mModel[draw.mDrawType]:nullptr;
    std::snprintf(text,sizeof text,
        "cursor_y=%.6f draw_type=%u draw_state=%u model_present=%u remembered_id=%u remembered_world=%.6f,%.6f,%.6f fade=%d",
        double(draw.mCursorOffsetY),unsigned(draw.mDrawType),unsigned(draw.field_0x173),unsigned(model!=nullptr),
        unsigned(attention->mTargetActorID),double(attention->mDrawAttnPos.x),double(attention->mDrawAttnPos.y),
        double(attention->mDrawAttnPos.z),int(attention->field_0x328));
    twv::vita::telemetryText("target_probe_cursor",text);
    if(model)matrix_snapshot("target_probe_marker_base",model->getBaseTRMtx(),3);
    const auto* view=dComIfGd_getView();const auto* viewport=dComIfGd_getViewport();
    if(!view || !viewport){twv::vita::telemetryText("target_probe_view","valid=0");return;}
    unsigned fbWidth=0,fbHeight=0;
    if(const auto* video=JUTVideo::getManager())if(const auto* mode=video->getRenderMode()) {
        fbWidth=mode->fbWidth;fbHeight=mode->efbHeight;
    }
    std::snprintf(text,sizeof text,
        "valid=1 eye=%.6f,%.6f,%.6f center=%.6f,%.6f,%.6f fovy=%.6f aspect=%.6f near=%.6f far=%.6f",
        double(view->lookat.eye.x),double(view->lookat.eye.y),double(view->lookat.eye.z),
        double(view->lookat.center.x),double(view->lookat.center.y),double(view->lookat.center.z),
        double(view->fovy),double(view->aspect),double(view->near_),double(view->far_));
    twv::vita::telemetryText("target_probe_view",text);
    std::snprintf(text,sizeof text,"logical=%.6f,%.6f,%.6f,%.6f depth=%.6f,%.6f fb=%u,%u ortho=%.6f,%.6f,%.6f,%.6f",
        double(viewport->x_orig),double(viewport->y_orig),double(viewport->width),double(viewport->height),
        double(viewport->near_z),double(viewport->far_z),fbWidth,fbHeight,
        double(mDoGph_gInf_c::getMinXF()),double(mDoGph_gInf_c::getMinYF()),
        double(mDoGph_gInf_c::getWidthF()),double(mDoGph_gInf_c::getHeightF()));
    twv::vita::telemetryText("target_probe_viewport",text);
    matrix_snapshot("target_probe_view_matrix",view->viewMtx,3);
    matrix_snapshot("target_probe_projection",view->projMtx,4);
    matrix_snapshot("target_probe_projview",view->projViewMtx,4);
    const auto* selected=eligible?eligible:candidate;
    if(selected) {
        project_snapshot("target_probe_actor_screen",selected->current.pos,*view,*viewport,fbWidth,fbHeight);
        project_snapshot("target_probe_attention_screen",selected->attention_info.position,*view,*viewport,fbWidth,fbHeight);
        cXyz cursor=selected->attention_info.position;cursor.y+=draw.mCursorOffsetY;
        project_snapshot("target_probe_cursor_screen",cursor,*view,*viewport,fbWidth,fbHeight);
    }
    if(model) {
        const auto& base=model->getBaseTRMtx();
        const cXyz origin(base[0][3],base[1][3],base[2][3]);
        project_snapshot("target_probe_marker_base_screen",origin,*view,*viewport,fbWidth,fbHeight);
    }
}
