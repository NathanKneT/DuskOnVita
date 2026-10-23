// GX material -> Cg. State comes from the original engine, no scene-specific shaders.
#include "internal.hpp"
#include "gx/shader_info.hpp"
#include <fmt/format.h>
#include <array>

namespace twv::vgl {
namespace {
using namespace aurora::gx;
constexpr const char* components[] = {"r","g","b","a"};
std::string swizzle(const TevSwap& s) {
    return fmt::format("{}{}{}{}",components[s.red],components[s.green],components[s.blue],components[s.alpha]);
}
std::string konst_value(unsigned selection,bool alpha) {
    if (selection<8) return fmt::format("({}.0/8.0)",8-selection);
    if (!alpha && selection>=12 && selection<16) return fmt::format("konst[{}].rgb",selection-12);
    if (selection>=16 && selection<32) {
        return fmt::format("konst[{}].{}",(selection-16)%4,components[(selection-16)/4]);
    }
    count("unsupported_konst_selector"); return "0.0";
}
std::string color_arg(GXTevColorArg arg,const TevStage& s,bool wrap) {
    std::string value;
    switch(arg) {
    case GX_CC_CPREV: value="r0.rgb"; break;
    case GX_CC_APREV: value="r0.aaa"; break;
    case GX_CC_C0: value="r1.rgb"; break;
    case GX_CC_A0: value="r1.aaa"; break;
    case GX_CC_C1: value="r2.rgb"; break;
    case GX_CC_A1: value="r2.aaa"; break;
    case GX_CC_C2: value="r3.rgb"; break;
    case GX_CC_A2: value="r3.aaa"; break;
    case GX_CC_TEXC: return "texval.rgb";
    case GX_CC_TEXA: return "texval.aaa";
    case GX_CC_RASC: return "rasval.rgb";
    case GX_CC_RASA: return "rasval.aaa";
    case GX_CC_ONE: return "float3(1.0,1.0,1.0)";
    case GX_CC_HALF: return "float3(0.5,0.5,0.5)";
    case GX_CC_KONST: return fmt::format("float3({})",konst_value(s.kcSel,false));
    default: return "float3(0.0,0.0,0.0)";
    }
    return wrap ? fmt::format("overflow3({})",value) : value;
}
std::string alpha_arg(GXTevAlphaArg arg,const TevStage& s,bool wrap) {
    std::string value;
    switch(arg) {
    case GX_CA_APREV: value="r0.a"; break;
    case GX_CA_A0: value="r1.a"; break;
    case GX_CA_A1: value="r2.a"; break;
    case GX_CA_A2: value="r3.a"; break;
    case GX_CA_TEXA: return "texval.a";
    case GX_CA_RASA: return "rasval.a";
    case GX_CA_KONST: return konst_value(s.kaSel,true);
    default: return "0.0";
    }
    return wrap ? fmt::format("overflow1({})",value) : value;
}
std::string operation(const TevOp& op,std::string a,std::string b,std::string c,std::string d,bool alpha) {
    std::string expression;
    if (op.op == GX_TEV_ADD || op.op == GX_TEV_SUB) {
        const char* bias=op.bias == GX_TB_ADDHALF ? "+0.5" : op.bias == GX_TB_SUBHALF ? "-0.5" : "";
        const float scale=op.scale == GX_CS_SCALE_2 ? 2.f : op.scale == GX_CS_SCALE_4 ? 4.f : op.scale == GX_CS_DIVIDE_2 ? .5f : 1.f;
        expression=fmt::format("(({} {} lerp({}, {}, {}) {}) * {:.1f})",d,op.op == GX_TEV_SUB ? "-" : "+",a,b,c,bias,scale);
    } else {
        const bool eq=(static_cast<int>(op.op)&1)!=0;
        const char* compare=eq ? "==" : ">";
        if (alpha) expression=fmt::format("({}+((floor({}*255.0+0.5) {} floor({}*255.0+0.5)) ? {} : 0.0))",d,a,compare,b,c);
        else if (op.op == GX_TEV_COMP_RGB8_GT || op.op == GX_TEV_COMP_RGB8_EQ) {
            expression=d+"+float3(";
            for(int i=0;i<3;++i) expression+=fmt::format("{}(floor(({}).{}*255.0+0.5) {} floor(({}).{}*255.0+0.5) ? ({}).{} : 0.0)",i?",":"",a,components[i],compare,b,components[i],c,components[i]);
            expression+=")";
        } else {
            const auto packed=[&](const std::string& v) {
                if (op.op == GX_TEV_COMP_R8_GT || op.op == GX_TEV_COMP_R8_EQ) return fmt::format("floor(({}).r*255.0+0.5)",v);
                if (op.op == GX_TEV_COMP_GR16_GT || op.op == GX_TEV_COMP_GR16_EQ) return fmt::format("floor(dot(({}).rg*255.0,float2(1.0,256.0))+0.5)",v);
                return fmt::format("floor(dot(({}).rgb*255.0,float3(1.0,256.0,65536.0))+0.5)",v);
            };
            expression=fmt::format("({}+(({} {} {}) ? {} : float3(0.0,0.0,0.0)))",d,packed(a),compare,packed(b),c);
        }
    }
    return fmt::format("clamp(({}), {:.1f}, {:.1f})",expression,op.clamp?0.f:-4.f,op.clamp?1.f:4.f);
}
std::string alpha_test(GXCompare compare,uint32_t reference) {
    const char* operators[]={"", "<", "==", "<=", ">", "!=", ">=", ""};
    if (compare == GX_NEVER) return "false";
    if (compare == GX_ALWAYS) return "true";
    return fmt::format("(floor(result.a*255.0+0.5) {} {}.0)",operators[compare],reference);
}
std::string io_fields(const Pipeline& p,bool position) {
    std::string result=position ? "float4 pos : POSITION;\n" : "";
    result+="float4 color0 : COLOR0; float4 color1 : COLOR1;\n";
    for(int i=0;i<8;++i) if(p.info.sampledTexCoords.test(i)) result+=fmt::format("float3 tc{} : TEXCOORD{};\n",i,i);
    // Actual window depth is interpolated from the homogeneous projection by hardware.
    return result;
}
}

std::string vertex_shader(const Pipeline& p,unsigned lightCount) {
    if (p.gpuVertex) return gpu_vertex_shader(p,lightCount);
    std::string code="struct Out {"+io_fields(p,true)+"};\nOut main(float3 position : POSITION, float4 color0 : COLOR0, float4 color1 : COLOR1";
    for(int i=0;i<8;++i) if(p.info.sampledTexCoords.test(i)) code+=fmt::format(",float3 tc{} : TEXCOORD{}",i,i);
    code+=",uniform float4 proj_rows[4]) { Out o; float4 v=float4(position,1.0); o.pos=float4(dot(proj_rows[0],v),dot(proj_rows[1],v),dot(proj_rows[2],v),dot(proj_rows[3],v)); o.color0=color0; o.color1=color1;\n";
    for(int i=0;i<8;++i) if(p.info.sampledTexCoords.test(i)) code+=fmt::format("o.tc{}=tc{};\n",i,i);
    return code+"return o; }\n";
}

std::string fragment_shader(const Pipeline& p) {
    const auto& cfg=p.config.shaderConfig;
    std::string code="float overflow1(float v){float b=v*255.0;return (b-floor(b/256.0)*256.0)/255.0;}\nfloat3 overflow3(float3 v){float3 b=v*255.0;return (b-floor(b/256.0)*256.0)/255.0;}\n";
    code+="struct In {"+io_fields(p,false)+"};\nfloat4 main(In i, float4 pixel : WPOS, uniform float4 regs[4], uniform float4 konst[4], uniform float4 fog[2]";
    for(int n=0;n<8;++n) if(p.info.sampledTextures.test(n)) code+=fmt::format(",uniform sampler2D tex{} : TEXUNIT{}",n,n);
    code+=") : COLOR { float4 r0=regs[0],r1=regs[1],r2=regs[2],r3=regs[3];\n";
    for(unsigned n=0;n<cfg.tevStageCount;++n) {
        const auto& s=cfg.tevStages[n];
        code+="{ float4 texval=float4(1.0,1.0,1.0,1.0); float4 rasval=float4(0.0,0.0,0.0,0.0);\n";
        if(s.texMapId<GX_MAX_TEXMAP && s.texCoordId<GX_MAX_TEXCOORD && p.info.sampledTextures.test(s.texMapId)) {
            code+=fmt::format("texval=tex2D(tex{},i.tc{}.xy/i.tc{}.z).{};\n",static_cast<int>(s.texMapId),static_cast<int>(s.texCoordId),static_cast<int>(s.texCoordId),swizzle(cfg.tevSwapTable[s.tevSwapTex]));
        }
        if(s.channelId!=GX_COLOR_NULL && s.channelId!=GX_COLOR_ZERO && s.channelId!=GX_ALPHA_BUMP && s.channelId!=GX_ALPHA_BUMPN) {
            code+=fmt::format("rasval=i.color{}.{};\n",static_cast<int>(color_channel(s.channelId)),swizzle(cfg.tevSwapTable[s.tevSwapRas]));
        }
        code+=fmt::format("float3 color_out={};\nfloat alpha_out={};\nr{}.rgb=color_out; r{}.a=alpha_out;\n}}\n",
            operation(s.colorOp,color_arg(s.colorPass.a,s,true),color_arg(s.colorPass.b,s,true),color_arg(s.colorPass.c,s,true),color_arg(s.colorPass.d,s,false),false),
            operation(s.alphaOp,alpha_arg(s.alphaPass.a,s,true),alpha_arg(s.alphaPass.b,s,true),alpha_arg(s.alphaPass.c,s,true),alpha_arg(s.alphaPass.d,s,false),true),
            static_cast<int>(s.colorOp.outReg),static_cast<int>(s.alphaOp.outReg));
    }
    if(cfg.tevStageCount) {
        const auto& last=cfg.tevStages[cfg.tevStageCount-1];
        code+=fmt::format("float4 result=float4(r{}.rgb,r{}.a);\n",static_cast<int>(last.colorOp.outReg),static_cast<int>(last.alphaOp.outReg));
        if(!last.colorOp.clamp) code+="result.rgb=overflow3(result.rgb);\n";
        if(!last.alphaOp.clamp) code+="result.a=overflow1(result.a);\n";
    } else code+="float4 result=r0;\n";
    const auto& ac=cfg.alphaCompare;
    const std::string a=alpha_test(ac.comp0,ac.ref0), b=alpha_test(ac.comp1,ac.ref1);
    std::string test;
    switch(ac.op) {
    case GX_AOP_OR: test=fmt::format("({}||{})",a,b); break;
    case GX_AOP_XOR: test=fmt::format("({}!={})",a,b); break;
    case GX_AOP_XNOR: test=fmt::format("({}=={})",a,b); break;
    default: test=fmt::format("({}&&{})",a,b); break;
    }
    code+=fmt::format("if(!{}) discard;\n",test);
    if(cfg.fogType!=GX_FOG_NONE) {
        code+=(cfg.fogType&8) ? "float base=fog[1].x*pixel.z;\n" : "float base=fog[1].x/(fog[1].y-pixel.z);\n";
        code+="float f=clamp(base-fog[1].z,0.0,1.0);\n";
        switch(cfg.fogType&7) {
        case 4: code+="f=1.0-exp2(-8.0*f);\n"; break;
        case 5: code+="f=1.0-exp2(-8.0*f*f);\n"; break;
        case 6: code+="f=exp2(-8.0*(1.0-f));\n"; break;
        case 7: code+="f=1.0-f; f=exp2(-8.0*f*f);\n"; break;
        default: break;
        }
        code+="result.rgb=lerp(result.rgb,fog[0].rgb,clamp(f,0.0,1.0));\n";
    }
    if(p.config.dstAlpha!=UINT32_MAX) code+=fmt::format("result.a={}.0/255.0;\n",p.config.dstAlpha);
    return code+"return result; }\n";
}
}
