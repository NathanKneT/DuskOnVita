// Original GX transforms, lighting and supported ordinary texgens on the GPU.
// Palette/emboss/resource-limit texgens retain the explicitly counted CPU path.
#include "internal.hpp"
#include <fmt/format.h>
#include <cmath>

namespace twv::vgl {
namespace {
using namespace aurora::gx;
const char* component[]={"x","y","z","w"};

std::string mask(unsigned channel,unsigned light) {
    return fmt::format("light_masks[{}].{}",channel*2+light/4,component[light%4]);
}

std::string factor(const ColorChannelConfig& cc,unsigned light) {
    std::string s=fmt::format(
        "float3 delta=light_pos[{}].xyz-mv; float d2=dot(delta,delta); float dist=sqrt(d2); "
        "float3 ld=dist>0.0 ? delta/dist : delta; float attenuation=1.0;\n",light);
    if(cc.attnFn==GX_AF_SPOT) {
        s+=fmt::format(
            "float c=max(0.0,dot(ld,light_dir[{0}].xyz)); "
            "float num=light_cos[{0}].x+light_cos[{0}].y*c+light_cos[{0}].z*c*c; "
            "float den=light_dist[{0}].x+light_dist[{0}].y*dist+light_dist[{0}].z*d2; "
            "attenuation=max(0.0,num/den);\n",light);
    } else if(cc.attnFn==GX_AF_SPEC) {
        s+=fmt::format(
            "float a=dot(nn,ld)>=0.0 ? max(0.0,dot(nn,light_dir[{0}].xyz)) : 0.0; "
            "float num=light_cos[{0}].x+light_cos[{0}].y*a+light_cos[{0}].z*a*a; "
            "float3 k=light_dist[{0}].xyz;\n",light);
        if(cc.diffFn!=GX_DF_NONE) s+="k=norm3(k);\n";
        s+="float den=max(0.0,k.x+k.y*a+k.z*a*a); attenuation=max(0.0,num/den);\n";
    }
    if(cc.diffFn==GX_DF_SIGN) s+="float f=attenuation*dot(ld,nn);\n";
    else if(cc.diffFn==GX_DF_CLAMP) s+="float f=attenuation*max(0.0,dot(ld,nn));\n";
    else s+="float f=attenuation;\n";
    return s;
}

std::string channel(const Pipeline& p,unsigned ch,unsigned lightCount) {
    const auto& rgb=p.config.shaderConfig.colorChannels[ch];
    const auto& alpha=p.config.shaderConfig.colorChannels[ch+2];
    const auto source=[&](GXColorSrc src,unsigned state,const char* kind,const char* swizzle) {
        return src==GX_SRC_VTX ? fmt::format("color{}.{}",ch,swizzle)
            : fmt::format("channel_{}[{}].{}",kind,state,swizzle);
    };
    std::string s="{\n";
    s+=fmt::format("float3 mr={}; float ma={};\n",
        source(rgb.matSrc,ch,"mat","rgb"),source(alpha.matSrc,ch+2,"mat","a"));
    if(rgb.lightingEnabled)s+=fmt::format("float3 ar={};\n",source(rgb.ambSrc,ch,"amb","rgb"));
    if(alpha.lightingEnabled)s+=fmt::format("float aa={};\n",source(alpha.ambSrc,ch+2,"amb","a"));
    const bool shared=rgb.lightingEnabled && alpha.lightingEnabled &&
        rgb.diffFn==alpha.diffFn && rgb.attnFn==alpha.attnFn;
    for(unsigned i=0;i<lightCount;++i) {
        const auto r=mask(ch,i),a=mask(ch+2,i);
        if(shared) {
            s+=fmt::format("if({}>0.0 || {}>0.0) {{\n",r,a)+factor(rgb,i);
            s+=fmt::format("if({}>0.0) ar+=f*light_color[{}].rgb; "
                "if({}>0.0) aa+=f*light_color[{}].a; }}\n",r,i,a,i);
        } else {
            if(rgb.lightingEnabled) s+=fmt::format("if({}>0.0) {{\n",r)+factor(rgb,i)+
                fmt::format("ar+=f*light_color[{}].rgb; }}\n",i);
            if(alpha.lightingEnabled) s+=fmt::format("if({}>0.0) {{\n",a)+factor(alpha,i)+
                fmt::format("aa+=f*light_color[{}].a; }}\n",i);
        }
    }
    s+=fmt::format("o.color{}=float4({},{});\n}}\n",ch,
        rgb.lightingEnabled ? "mr*clamp(ar,0.0,1.0)" : "mr",
        alpha.lightingEnabled ? "ma*clamp(aa,0.0,1.0)" : "ma");
    return s;
}

std::string texgen(const Pipeline& p,unsigned tc) {
    const auto& g=p.config.shaderConfig.tcgs[tc];
    std::string source;
    if(g.src>=GX_TG_TEX0 && g.src<=GX_TG_TEX7)source=fmt::format("float4(tc{}.xy,1.0,1.0)",tc);
    else if(g.src==GX_TG_POS)source="float4(position,1.0)";
    else if(g.src==GX_TG_NRM)source="float4(normal,1.0)";
    else source=fmt::format("color{}",unsigned(g.src)-GX_TG_COLOR0);
    const auto transform=[](int row,const std::string& v) {
        return fmt::format("float3(dot(texgen_rows[{}],{}),dot(texgen_rows[{}],{}),dot(texgen_rows[{}],{}))",
                           row,v,row+1,v,row+2,v);
    };
    std::string s=fmt::format("{{float4 src={}; float3 t;\n",source);
    if(g.type==GX_TG_SRTG)s+="t=float3(src.xy,1.0);\n";
    else {
        s+=p.texgenRows[tc]>=0 ? "t="+transform(p.texgenRows[tc],"src")+";\n" : "t=src.xyz;\n";
        if(g.type==GX_TG_MTX2x4)s+="t.z=1.0;\n";
    }
    if(g.normalize)s+="t=norm3(t);\n";
    if(p.postRows[tc]>=0)s+="float4 post=float4(t,1.0);t="+transform(p.postRows[tc],"post")+";\n";
    return s+fmt::format("o.tc{}=t;}}\n",tc);
}
}

std::string gpu_vertex_shader(const Pipeline& p,unsigned lightCount) {
    std::string s="float3 norm3(float3 v){float q=dot(v,v);return q<=1e-10 ? v : v/sqrt(q);}\n";
    s+="struct Out {float4 pos:POSITION;float4 color0:COLOR0;float4 color1:COLOR1;\n";
    for(unsigned tc=0;tc<8;++tc) if(p.info.sampledTexCoords.test(tc))
        s+=fmt::format("float3 tc{}:TEXCOORD{};\n",tc,tc);
    s+="};\nOut main(float3 position:POSITION,float3 normal:NORMAL,float pn_index:BLENDINDICES,"
       "float4 color0:COLOR0,float4 color1:COLOR1";
    for(unsigned tc=0;tc<8;++tc) if(p.info.sampledTexCoords.test(tc))
        s+=fmt::format(",float3 tc{}:TEXCOORD{}",tc,tc);
    // G3 (--vita-gxm-uniform-split, GXM only): the lighting uniforms (channel registers, lights, light
    // masks) in vertex uniform buffer 1, which the backend uploads only when they change. The binding
    // sits between /*G3{*/ and /*}G3*/: the draw digest hashes the source without it.
    const std::string b1=(twv_gpu_features()&TwvGpuFeatureUniformSplit) ? "/*G3{*/ : BUFFER[1]/*}G3*/" : "";
    s+=",uniform float4 proj_rows[4],uniform float4 pn_pos[30],uniform float4 channel_mat[4]"+b1;
    if(p.info.lightingEnabled)s+=",uniform float4 channel_amb[4]"+b1;
    if(p.info.lightingEnabled && lightCount)s+=fmt::format(
       ",uniform float4 pn_nrm[30],uniform float4 light_pos[{0}]{1},uniform float4 light_dir[{0}]{1},"
       "uniform float4 light_color[{0}]{1},uniform float4 light_cos[{0}]{1},uniform float4 light_dist[{0}]{1},"
       "uniform float4 light_masks[8]{1}",lightCount,b1);
    if(p.gpuTexgen && p.texgenVectors)s+=fmt::format(",uniform float4 texgen_rows[{}]",p.texgenVectors);
    s+=") {Out o;";
    // G2 packed formats (GXM): integer inputs arrive unscaled; multiply by the GX shift, an exact
    // power of two written with all its digits, so every value equals the F32 path's exactly.
    // The statements sit between /*G2{*/ and /*}G2*/: the draw digest hashes the source without
    // them (the G1 text of the same program).
    std::string scales;
    const auto scale=[&](unsigned slot,const std::string& name) {
        const auto format=p.packed.format[slot];
        if(format==TwvFormatF32 || format==TwvFormatU8N || p.packed.frac[slot]<=0)return;
        scales+=fmt::format("{0}={0}*{1:.17g};",name,std::ldexp(1.0,-int(p.packed.frac[slot])));
    };
    scale(0,"position");scale(11,"normal");
    for(unsigned tc=0;tc<8;++tc) if(p.info.sampledTexCoords.test(tc))scale(3+tc,fmt::format("tc{}",tc));
    if(!scales.empty())s+="/*G2{*/"+scales+"/*}G2*/";
    s+="int m=int(pn_index)*3;float4 v=float4(position,1.0);"
       "float3 mv=float3(dot(pn_pos[m],v),dot(pn_pos[m+1],v),dot(pn_pos[m+2],v));"
       "float4 view=float4(mv,1.0);"
       "o.pos=float4(dot(proj_rows[0],view),dot(proj_rows[1],view),"
       "dot(proj_rows[2],view),dot(proj_rows[3],view));\n";
    if(p.info.lightingEnabled && lightCount) s+="float3 nn=norm3(float3(dot(pn_nrm[m].xyz,normal),"
        "dot(pn_nrm[m+1].xyz,normal),dot(pn_nrm[m+2].xyz,normal)));\n";
    else s+="float3 nn=float3(0.0,0.0,0.0);\n";
    for(unsigned ch=0;ch<2;++ch) {
        if(p.info.sampledColorChannels.test(ch)) s+=channel(p,ch,lightCount);
        else s+=fmt::format("o.color{}=float4(0.0,0.0,0.0,0.0);\n",ch);
    }
    for(unsigned tc=0;tc<8;++tc) if(p.info.sampledTexCoords.test(tc))
        s+=p.gpuTexgen ? texgen(p,tc) : fmt::format("o.tc{}=tc{};\n",tc,tc);
    return s+"return o;}\n";
}
} // namespace twv::vgl
