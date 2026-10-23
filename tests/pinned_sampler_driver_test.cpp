// PRIVATE DRAFT. Production bodies come from pinned_sampler_driver.inc.
// The host widens pointer-valued GLuint handles; this is NOT a Vita ABI test.
// Native setters COPY descriptors into the recorder as an explicit mock only.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

using GLenum = uint32_t;
using GLint = int;
using GLsizei = int;
using GLuint = uintptr_t;
using GLboolean = uint8_t;
using GLfloat = float;
using SceGxmTextureFormat = uint32_t;
// Numeric constants from vita-headers e66ebe90, psp2/gxm.h:780-800.
enum SceGxmTextureFilter { SCE_GXM_TEXTURE_FILTER_POINT=0, SCE_GXM_TEXTURE_FILTER_LINEAR=1 };
enum SceGxmTextureMipFilter { SCE_GXM_TEXTURE_MIP_FILTER_DISABLED=0, SCE_GXM_TEXTURE_MIP_FILTER_ENABLED=0x200 };
enum SceGxmTextureAddrMode { SCE_GXM_TEXTURE_ADDR_REPEAT=0, SCE_GXM_TEXTURE_ADDR_MIRROR=1,
    SCE_GXM_TEXTURE_ADDR_CLAMP=2, SCE_GXM_TEXTURE_ADDR_MIRROR_CLAMP=3 };
struct SceGxmTexture { uint32_t control_words[4]{}; };
static_assert(sizeof(SceGxmTexture)==16);
using SceGxmTextureInternal = SceGxmTexture;
// Only fields used by the extracted loops; not production layout/enum proofs.
enum { UNIFORM_SAMPLER=1, UNIFORM_CUBE_SAMPLER=2 };
struct uniform { int type=UNIFORM_SAMPLER; uint8_t sampler_index=0; };
struct program { int max_frag_texunit_idx=0,max_vert_texunit_idx=0;
    uniform *frag_texunits[16]{},*vert_texunits[16]{}; };
struct texture_unit { GLuint tex_id[3]{}; };

#include "pinned_sampler_driver.inc"

static const char *case_name="startup";
static unsigned case_count=0,assertion_count=0,setter_count=0;
static void start_case(const char *name) { case_name=name; ++case_count; }
#define CHECK(condition) do { ++assertion_count; if (!(condition)) { \
    std::fprintf(stderr,"%s:%d case=%s failed: %s\n",__FILE__,__LINE__,case_name,#condition); \
    std::abort(); } } while (0)

static int fail_allocation_at=-1;
static unsigned allocation_calls=0,free_calls=0;
static std::unordered_set<void *> live_allocations;
static void *vglMalloc(size_t bytes) {
    const unsigned call=allocation_calls++;
    if (fail_allocation_at>=0 && call==unsigned(fail_allocation_at)) return nullptr;
    void *result=std::malloc(bytes);
    CHECK(result!=nullptr);
    CHECK(live_allocations.insert(result).second);
    return result;
}
static void vgl_free(void *pointer) {
    CHECK(pointer!=nullptr);
    CHECK(live_allocations.erase(pointer)==1);
    ++free_calls;
    std::free(pointer);
}
static void vgl_log(const char *format,...) { (void)format; }

struct Capture { bool vertex; unsigned slot; SceGxmTexture descriptor; };
static std::vector<Capture> captures;
static int sceGxmSetFragmentTexture(void *context,unsigned slot,const SceGxmTexture *descriptor) {
    (void)context;
    CHECK(descriptor!=nullptr && slot<16);
    ++setter_count;
    captures.push_back({false,slot,*descriptor}); // MOCK: copy now, not borrowed pointer.
    return 0;
}
static int sceGxmSetVertexTexture(void *context,unsigned slot,const SceGxmTexture *descriptor) {
    (void)context;
    CHECK(descriptor!=nullptr && slot<16);
    ++setter_count;
    captures.push_back({true,slot,*descriptor});
    return 0;
}

static void reset_state() {
    CHECK(live_allocations.empty());
    for (auto &binding:samplers) binding=nullptr;
    for (auto &unit:texture_units) unit=texture_unit{};
    for (auto &tex:texture_slots) tex=texture{};
    progs[0]=program{};
    captures.clear();
    vgl_error=GL_NO_ERROR;
    fail_allocation_at=-1;
    allocation_calls=free_calls=0;
    tex2d_override=0;
}
static std::array<GLuint,8> make_eight() {
    std::array<GLuint,8> names{};
    glGenSamplers(int(names.size()),names.data());
    CHECK(vgl_error==GL_NO_ERROR);
    for (unsigned i=0;i<names.size();++i) {
        CHECK(names[i]!=0);
        for (unsigned j=0;j<i;++j) CHECK(names[i]!=names[j]);
        const auto &created=*reinterpret_cast<sampler *>(names[i]);
        CHECK(created.min_filter==SCE_GXM_TEXTURE_FILTER_POINT);
        CHECK(created.mag_filter==SCE_GXM_TEXTURE_FILTER_LINEAR);
        CHECK(created.u_mode==SCE_GXM_TEXTURE_ADDR_REPEAT && created.v_mode==SCE_GXM_TEXTURE_ADDR_REPEAT);
        CHECK(created.mip_filter==SCE_GXM_TEXTURE_MIP_FILTER_ENABLED && created.use_mips==GL_TRUE);
        CHECK(created.lod_bias==GL_MAX_TEXTURE_LOD_BIAS);
    }
    return names;
}
static void destroy_eight(const std::array<GLuint,8> &names) {
    glDeleteSamplers(int(names.size()),names.data());
    CHECK(live_allocations.empty());
    for (auto *binding:samplers) CHECK(binding==nullptr);
}
static void allocation_cases() {
    start_case("Gen zero and invalid output");
    reset_state();
    glGenSamplers(0,nullptr);
    CHECK(vgl_error==GL_NO_ERROR && allocation_calls==0);
    glGenSamplers(1,nullptr);
    CHECK(vgl_error==GL_INVALID_VALUE && allocation_calls==0);
    vgl_error=GL_NO_ERROR;
    GLuint untouched=123;
    glGenSamplers(-1,&untouched);
    CHECK(untouched==123 && allocation_calls==0);
#ifndef SKIP_ERROR_HANDLING
    CHECK(vgl_error==GL_INVALID_VALUE);
#else
    CHECK(vgl_error==GL_NO_ERROR); // Existing optional argument validation contract.
#endif
    for (int failure=0;failure<8;++failure) {
        start_case("transactional allocation failure at each position");
        reset_state();
        GLuint prior=0;
        glGenSamplers(1,&prior);
        CHECK(prior!=0);
        glBindSampler(15,prior);
        auto *existing=samplers[15];
        allocation_calls=free_calls=0;
        fail_allocation_at=failure;
        std::array<GLuint,8> names;
        names.fill(UINTPTR_MAX);
        glGenSamplers(int(names.size()),names.data());
        CHECK(vgl_error==GL_OUT_OF_MEMORY);
        CHECK(allocation_calls==unsigned(failure+1));
        CHECK(free_calls==unsigned(failure));
        for (auto name:names) CHECK(name==0);
        CHECK(live_allocations.size()==1 && samplers[15]==existing);
        for (unsigned unit=0;unit<15;++unit) CHECK(samplers[unit]==nullptr);
        fail_allocation_at=-1;
        glDeleteSamplers(1,&prior);
        CHECK(live_allocations.empty() && samplers[15]==nullptr);
        // A later transaction must recover after an OOM; do not require GL error to vanish itself.
        vgl_error=GL_NO_ERROR;
        const auto recovered=make_eight();
        destroy_eight(recovered);
    }
    start_case("normal eight handles and delete clears every matching binding");
    reset_state();
    const auto names=make_eight();
    for (unsigned unit=0;unit<8;++unit) glBindSampler(unit,names[unit]);
    glBindSampler(15,names[3]);
    destroy_eight(names);
    CHECK(allocation_calls==8 && free_calls==8);
}

struct Params { int min,mag,s,t; };
static constexpr Params A{GL_LINEAR,GL_LINEAR,GL_REPEAT,GL_CLAMP_TO_EDGE};
static constexpr Params B{GL_NEAREST,GL_NEAREST,GL_MIRRORED_REPEAT,GL_REPEAT};
static void set_sampler(GLuint sampler_name,Params params) {
    glSamplerParameteri(sampler_name,GL_TEXTURE_MIN_FILTER,params.min);
    glSamplerParameteri(sampler_name,GL_TEXTURE_MAG_FILTER,params.mag);
    glSamplerParameteri(sampler_name,GL_TEXTURE_WRAP_S,params.s);
    glSamplerParameteri(sampler_name,GL_TEXTURE_WRAP_T,params.t);
    glSamplerParameteri(sampler_name,GL_TEXTURE_LOD_BIAS,0); // Existing integer API, neutral only.
    CHECK(vgl_error==GL_NO_ERROR);
}
static void set_legacy(unsigned id,Params params) {
    auto *tex=&texture_slots[id];
    _glTexParameteri(tex,GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,params.min);
    _glTexParameteri(tex,GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,params.mag);
    _glTexParameteri(tex,GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,params.s);
    _glTexParameteri(tex,GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,params.t);
    _glTexParameteri(tex,GL_TEXTURE_2D,GL_TEXTURE_LOD_BIAS,0);
    CHECK(vgl_error==GL_NO_ERROR);
}
static void init_texture(unsigned id,uint32_t data_word) {
    auto &tex=texture_slots[id];
    tex=texture{};
    tex.status=TEX_VALID;
    tex.mip_count=4;
    tex.gxm_tex.control_words[0]=0xA0008001u;
    tex.gxm_tex.control_words[1]=0x10203040u;
    tex.gxm_tex.control_words[2]=data_word;
    tex.gxm_tex.control_words[3]=0x55667788u;
    set_legacy(id,B);
}
static unsigned u_mode(const SceGxmTexture &descriptor) { return (descriptor.control_words[0]>>6)&7; }
static unsigned v_mode(const SceGxmTexture &descriptor) { return (descriptor.control_words[0]>>3)&7; }
static unsigned min_filter(const SceGxmTexture &descriptor) { return (descriptor.control_words[0]>>10)&3; }
static unsigned mag_filter(const SceGxmTexture &descriptor) { return (descriptor.control_words[0]>>12)&3; }
static unsigned mip_filter(const SceGxmTexture &descriptor) { return descriptor.control_words[0]&0x200; }
static unsigned count_bits(const SceGxmTexture &descriptor) { return (descriptor.control_words[0]>>17)&15; }
static void untouched_storage(const SceGxmTexture &descriptor,uint32_t data_word) {
    CHECK(descriptor.control_words[1]==0x10203040u);
    CHECK(descriptor.control_words[2]==data_word);
    CHECK(descriptor.control_words[3]==0x55667788u);
}
static void check_AB(const Capture &capture,bool is_A,uint32_t data_word=0x00112233u) {
    CHECK(min_filter(capture.descriptor)==unsigned(is_A?SCE_GXM_TEXTURE_FILTER_LINEAR:SCE_GXM_TEXTURE_FILTER_POINT));
    CHECK(mag_filter(capture.descriptor)==unsigned(is_A?SCE_GXM_TEXTURE_FILTER_LINEAR:SCE_GXM_TEXTURE_FILTER_POINT));
    CHECK(u_mode(capture.descriptor)==unsigned(is_A?SCE_GXM_TEXTURE_ADDR_REPEAT:SCE_GXM_TEXTURE_ADDR_MIRROR));
    CHECK(v_mode(capture.descriptor)==unsigned(is_A?SCE_GXM_TEXTURE_ADDR_CLAMP:SCE_GXM_TEXTURE_ADDR_REPEAT));
    CHECK(mip_filter(capture.descriptor)==0);
    CHECK(count_bits(capture.descriptor)==0); // 2D no-mip: count=1, encoded count-1.
    untouched_storage(capture.descriptor,data_word);
}
using Apply=void (*)();
static constexpr std::array<Apply,3> routes{fixture_multidraw,fixture_arrays,fixture_elements};
static void install_program(uniform (&mapping)[4]) {
    mapping[0]={UNIFORM_SAMPLER,3};
    mapping[1]={UNIFORM_SAMPLER,7};
    mapping[2]={UNIFORM_SAMPLER,7};
    mapping[3]={UNIFORM_SAMPLER,3};
    auto &p=progs[0];
    p=program{};
    p.max_frag_texunit_idx=5;
    p.frag_texunits[0]=&mapping[0];
    p.frag_texunits[4]=&mapping[1];
    p.max_vert_texunit_idx=6;
    p.vert_texunits[1]=&mapping[2];
    p.vert_texunits[5]=&mapping[3];
}
static void check_mapping() {
    CHECK(captures.size()==4);
    CHECK(!captures[0].vertex && captures[0].slot==0);
    CHECK(!captures[1].vertex && captures[1].slot==4);
    CHECK(captures[2].vertex && captures[2].slot==1);
    CHECK(captures[3].vertex && captures[3].slot==5);
}
static void mapped_units_cases() {
    for (const auto apply:routes) {
        start_case("same texture; compacted resources and independent GL units");
        reset_state();
        const auto names=make_eight();
        init_texture(1,0x00112233u);
        uniform mapping[4];
        install_program(mapping);
        texture_units[3].tex_id[0]=1;
        texture_units[7].tex_id[0]=1;
        set_sampler(names[3],A);
        set_sampler(names[7],B);
        glBindSampler(3,names[3]);
        glBindSampler(7,names[7]);
        apply();
        check_mapping();
        check_AB(captures[0],true);
        check_AB(captures[1],false);
        check_AB(captures[2],false);
        check_AB(captures[3],true);
        // Recorder values must not track the mutable final CPU descriptor.
        const auto saved=captures[1].descriptor;
        vglSetTexUMode(&texture_slots[1].gxm_tex,SCE_GXM_TEXTURE_ADDR_CLAMP);
        CHECK(std::memcmp(&saved,&captures[1].descriptor,sizeof saved)==0);

        start_case("sampler to zero restores legacy fields and no-mip count");
        glBindSampler(3,0);
        glBindSampler(7,0);
        captures.clear();
        apply();
        check_mapping();
        for (const auto &capture:captures) check_AB(capture,false);

        start_case("identical sampler requests on a shared texture");
        set_sampler(names[7],A);
        glBindSampler(3,names[3]);
        glBindSampler(7,names[7]);
        captures.clear();
        apply();
        for (const auto &capture:captures) check_AB(capture,true);

        start_case("two distinct texture data descriptors");
        init_texture(2,0x00445566u);
        texture_units[7].tex_id[0]=2;
        set_sampler(names[7],B);
        captures.clear();
        apply();
        check_AB(captures[0],true);
        check_AB(captures[1],false,0x00445566u);
        check_AB(captures[2],false,0x00445566u);
        check_AB(captures[3],true);

        start_case("original no-object route reproduces shared last-wins");
        glBindSampler(3,0);
        glBindSampler(7,0);
        texture_units[7].tex_id[0]=1;
        set_legacy(1,A);
        set_legacy(1,B);
        captures.clear();
        apply();
        for (const auto &capture:captures) check_AB(capture,false);
        destroy_eight(names);
    }
}
static void min_filter_cases() {
    struct Row { int gl; unsigned minimum,mip,count; };
    const Row rows[]={{GL_NEAREST,0,0,0},{GL_LINEAR,1,0,0},
        {GL_NEAREST_MIPMAP_NEAREST,0,0,3},{GL_LINEAR_MIPMAP_NEAREST,1,0,3},
        {GL_NEAREST_MIPMAP_LINEAR,0,0x200,3},{GL_LINEAR_MIPMAP_LINEAR,1,0x200,3}};
    for (const auto apply:routes) for (const auto row:rows) {
        start_case("all min filters match legacy 2D descriptor count and bits");
        reset_state();
        const auto names=make_eight();
        init_texture(1,0x00112233u);
        uniform mapping[4];
        install_program(mapping);
        texture_units[3].tex_id[0]=texture_units[7].tex_id[0]=1;
        Params params=A;
        params.min=row.gl;
        set_legacy(1,params);
        const auto legacy=texture_slots[1].gxm_tex;
        set_sampler(names[3],params);
        set_sampler(names[7],params);
        glBindSampler(3,names[3]);
        glBindSampler(7,names[7]);
        apply();
        check_mapping();
        for (const auto &capture:captures) {
            CHECK(min_filter(capture.descriptor)==row.minimum);
            CHECK(mip_filter(capture.descriptor)==row.mip);
            CHECK(count_bits(capture.descriptor)==row.count);
            CHECK(std::memcmp(&legacy,&capture.descriptor,sizeof legacy)==0);
        }
        destroy_eight(names);
    }
}
static void isolated_stage_restore_cases() {
    // The earlier mixed-stage case restores through the first fragment loop.
    // Isolate each stage so every one of the six real loops executes both arms.
    for (const auto apply:routes) for (const bool vertex:{false,true}) {
        start_case(vertex?"isolated vertex override then restore":"isolated fragment override then restore");
        reset_state();
        const auto names=make_eight();
        init_texture(1,0x00112233u);
        const auto legacy=texture_slots[1].gxm_tex;
        uniform mapping[4];
        install_program(mapping);
        if (vertex) progs[0].max_frag_texunit_idx=0;
        else progs[0].max_vert_texunit_idx=0;
        texture_units[3].tex_id[0]=texture_units[7].tex_id[0]=1;
        set_sampler(names[3],A);
        set_sampler(names[7],A);
        glBindSampler(3,names[3]);
        glBindSampler(7,names[7]);
        CHECK(texture_slots[1].overridden==GL_FALSE);
        apply();
        CHECK(captures.size()==2 && texture_slots[1].overridden==GL_TRUE);
        for (const auto &capture:captures) {
            CHECK(capture.vertex==vertex);
            check_AB(capture,true);
        }
        glBindSampler(3,0);
        glBindSampler(7,0);
        captures.clear();
        apply();
        CHECK(captures.size()==2 && texture_slots[1].overridden==GL_FALSE);
        for (const auto &capture:captures) {
            CHECK(capture.vertex==vertex);
            check_AB(capture,false);
            CHECK(std::memcmp(&legacy,&capture.descriptor,sizeof legacy)==0);
        }
        destroy_eight(names);
    }
}
static void cube_unchanged_cases() {
    for (const auto apply:routes) {
        start_case("cube non-mip behavior unchanged by 2D-only patch");
        reset_state();
        const auto names=make_eight();
        init_texture(1,0x00112233u);
        uniform mapping[4];
        install_program(mapping);
        for (auto &item:mapping) item.type=UNIFORM_CUBE_SAMPLER;
        texture_units[3].tex_id[2]=texture_units[7].tex_id[2]=1;
        set_sampler(names[3],A);
        set_sampler(names[7],B);
        glBindSampler(3,names[3]);
        glBindSampler(7,names[7]);
        apply();
        CHECK(captures.size()==4);
        for (const auto &capture:captures) CHECK(count_bits(capture.descriptor)==15);
        destroy_eight(names);
    }
}
static void checked_error_cases() {
    start_case("parameter errors and optional unit checks");
    reset_state();
    const auto names=make_eight();
    const auto initial=*reinterpret_cast<sampler *>(names[0]);
    glSamplerParameteri(names[0],GL_TEXTURE_MIN_FILTER,-1);
    CHECK(vgl_error==GL_INVALID_ENUM);
    const auto &after=*reinterpret_cast<sampler *>(names[0]);
    CHECK(initial.min_filter==after.min_filter && initial.mag_filter==after.mag_filter);
    CHECK(initial.u_mode==after.u_mode && initial.v_mode==after.v_mode);
    CHECK(initial.mip_filter==after.mip_filter && initial.use_mips==after.use_mips);
    CHECK(initial.lod_bias==after.lod_bias); // Never compare sampler struct padding.
    vgl_error=GL_NO_ERROR;
    glSamplerParameterf(names[0],GL_TEXTURE_LOD_BIAS,0.0f);
    CHECK(vgl_error==GL_NO_ERROR);
#ifndef SKIP_ERROR_HANDLING
    glBindSampler(COMBINED_TEXTURE_IMAGE_UNITS_NUM,names[0]);
    CHECK(vgl_error==GL_INVALID_VALUE);
#endif
    // No call with a fabricated/destroyed pointer: the API has no ID registry.
    destroy_eight(names);
}
int main(int argc,char **argv) {
    if (argc==2 && std::strcmp(argv[1],"--oom-only")==0) {
        // Negative original-source control: ASan/UBSan must catch its NULL dereference.
        CHECK(TWV_SAMPLER_FIXTURE_CANDIDATE==0);
        start_case("original unchecked sampler allocation");
        reset_state();
        fail_allocation_at=0;
        std::array<GLuint,8> names{};
        glGenSamplers(int(names.size()),names.data());
        CHECK(false); // Reaching this would invalidate the expected negative control.
    }
    const bool non_mip_only=argc==2 && std::strcmp(argv[1],"--non-mip-only")==0;
    if (!non_mip_only) {
        CHECK(TWV_SAMPLER_FIXTURE_CANDIDATE==1);
        allocation_cases();
    }
    min_filter_cases();
    if (!non_mip_only) {
        mapped_units_cases();
        isolated_stage_restore_cases();
        cube_unchanged_cases();
        checked_error_cases();
    }
    CHECK(live_allocations.empty());
    std::printf("pinned sampler driver seam passed: cases=%u assertions=%u native_setter_copies=%u; mock SetTexture, NOT GPU proof\n",
                case_count,assertion_count,setter_count);
}
