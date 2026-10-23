// Recording SDK test of patched production C, not a model of Vita driver internals.
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
typedef int GLboolean;
enum {GL_FALSE=0,GL_TRUE=1};
typedef struct {size_t vertex_size,fragment_size;unsigned epoch;} Context;
static Context context={0};
static Context *gxm_context=&context;
static void *vgl_def_vert_buf=NULL,*vgl_def_frag_buf=NULL;
static GLboolean vgl_uniform_reservation_failed=GL_FALSE;
static GLboolean dirty_shader_vert_unifs=GL_TRUE,dirty_shader_frag_unifs=GL_TRUE;
static unsigned reservations=0,fail_at=0,null_at=0,logs=0,draws=0;
static size_t native_bytes=0;
struct Snapshot {void* ptr;size_t size;unsigned epoch;};
static struct Snapshot snapshots[10000];
static unsigned snapshots_count=0;
#define vgl_log(...) (++logs)
static int reserve_native(Context *c,void **out,int vertex){
 ++reservations;*out=NULL;
 if(reservations==fail_at)return -17;
 if(reservations==null_at)return 0;
 const size_t size=vertex?c->vertex_size:c->fragment_size;
 if(!size)return 0; // Native empty container may report success without storage.
 assert(snapshots_count<10000);
 void* ptr=malloc(size);assert(ptr);memset(ptr,0xcc,size);
 // The test backend owns each snapshot. Epoch crossings model SDK ring management;
 // the application must neither wrap nor rebind an older epoch's pointer.
 if(native_bytes/65536!=(native_bytes+size)/65536)++c->epoch;
 native_bytes+=size;
 snapshots[snapshots_count++]=(struct Snapshot){ptr,size,c->epoch};*out=ptr;return 0;
}
static int sceGxmReserveVertexDefaultUniformBuffer(Context*c,void**out){return reserve_native(c,out,1);}
static int sceGxmReserveFragmentDefaultUniformBuffer(Context*c,void**out){return reserve_native(c,out,0);}
#define vgl_fast_memcpy memcpy
struct Shader{uint32_t unif_buf_size;};
typedef struct ubo {struct ubo *alias;void*chain;unsigned idx,bind;} ubo;
struct UboBuf {void*ptr;unsigned last_frame;};
static struct UboBuf *ubo_buf[1];static unsigned ubo_offset[1],vgl_framecount=1;
static void sceGxmSetVertexUniformBuffer(Context*c,unsigned i,void*p){(void)c;(void)i;(void)p;assert(0);}
static void sceGxmSetFragmentUniformBuffer(Context*c,unsigned i,void*p){(void)c;(void)i;(void)p;assert(0);}
#ifdef HAVE_FFP_SHADER_SUPPORT
struct FfpUniform {uint8_t *vptr;};
enum {FFP_MVP_MATRIX,FFP_MV_MATRIX,FFP_NORMAL_MATRIX,SCE_GXM_PARAMETER_TYPE_F32};
static uint16_t dirty_vert_unifs=0;
static GLboolean mvp_modified=GL_FALSE;
static float vgl_mvp_matrix[16],projection_matrix[16],modelview_matrix[16],normal_matrix[16];
static void matrix4x4_multiply(float*a,const float*b,const float*c){(void)a;(void)b;(void)c;assert(0);}
static void recalculate_normal_matrix(void){assert(0);}
static void vglSetUniformData(uint8_t*a,int b,int c,unsigned d,unsigned e,const float*f,int g){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;assert(0);}
#endif
struct Program{void *vert_uniforms,*frag_uniforms;struct Shader *vshader,*fshader;void *unif_vbuffer,*unif_fbuffer;ubo *vert_ubos,*frag_ubos;
#ifdef HAVE_FFP_SHADER_SUPPORT
struct FfpUniform *ffp_binds[3];
#endif
};
#include "vitagl_uniform_fixture.inc"
// This is the production upload macro invoked with its production return ABI.
static int submit(struct Program*p){upload_uniforms(return 0);if(!vgl_uniform_reservation_failed)++draws;return !vgl_uniform_reservation_failed;}
static void next_draw(void){vglBeginUniformUpload();}
int main(void){
 unsigned char vs[4096],fs[512];memset(vs,0x35,sizeof vs);memset(fs,0xa7,sizeof fs);
 struct Shader v={sizeof vs},f={sizeof fs};struct Program p={.vert_uniforms=vs,.frag_uniforms=fs,.vshader=&v,.fshader=&f,.unif_vbuffer=vs,.unif_fbuffer=fs};
 context.vertex_size=v.unif_buf_size;context.fragment_size=f.unif_buf_size;
 vglSetupUniformCircularPool();next_draw();assert(submit(&p));
 assert(snapshots_count==2&&snapshots[0].size==sizeof vs&&snapshots[1].size==sizeof fs);
 assert(!memcmp(snapshots[0].ptr,vs,sizeof vs)&&!memcmp(snapshots[1].ptr,fs,sizeof fs));
 // CPU-cache hits must still produce fresh native buffers after helper draws/scenes.
 assert(!dirty_shader_vert_unifs&&!dirty_shader_frag_unifs);
 const unsigned before=snapshots_count;next_draw();assert(submit(&p));assert(snapshots_count==before+2);
 // A sampler list is nonempty while the reflected default container and CPU
 // buffer are empty. Native success(NULL) would have dropped this draw in V54.
 unsigned before_empty=reservations,before_empty_draws=draws;
 f.unif_buf_size=0;p.unif_fbuffer=NULL;context.fragment_size=0;
 next_draw();assert(submit(&p));assert(reservations==before_empty+1&&draws==before_empty_draws+1);
 assert(!vgl_uniform_reservation_failed);
 // Vertex sampler-only/UBO-only default container has the same rule.
 f.unif_buf_size=sizeof fs;p.unif_fbuffer=fs;context.fragment_size=sizeof fs;
 v.unif_buf_size=0;p.unif_vbuffer=NULL;context.vertex_size=0;
 before_empty=reservations;next_draw();assert(submit(&p));assert(reservations==before_empty+1);
 f.unif_buf_size=0;p.unif_fbuffer=NULL;context.fragment_size=0;
 before_empty=reservations;next_draw();assert(submit(&p));assert(reservations==before_empty);
 v.unif_buf_size=sizeof vs;p.unif_vbuffer=vs;context.vertex_size=sizeof vs;
 f.unif_buf_size=sizeof fs;p.unif_fbuffer=fs;context.fragment_size=sizeof fs;
 // Cross the former 2 MiB limit repeatedly. No application-owned address wraps.
 for(unsigned i=0;i<700;++i){vs[0]=(unsigned char)i;next_draw();assert(submit(&p));}
 assert(native_bytes>2*1024*1024&&context.epoch>0);
 assert(((unsigned char*)snapshots[0].ptr)[0]==0x35);
 assert(((unsigned char*)snapshots[1].ptr)[0]==0xa7);
 for(unsigned i=1;i<snapshots_count;++i)assert(snapshots[i].ptr!=snapshots[i-1].ptr);
 // Vertex failure: no copy/draw and no attempt to reserve the second stage.
 unsigned old_draws=draws,old_reservations=reservations;fail_at=reservations+1;next_draw();assert(!submit(&p));
 assert(draws==old_draws&&reservations==old_reservations+1&&logs==0);
 // Failure remains sticky within this transaction (e.g. failed scissor setup).
 assert(!submit(&p)&&reservations==old_reservations+1&&draws==old_draws);
 fail_at=0;next_draw();assert(submit(&p)&&draws==old_draws+1);
 // Fragment failure after successful vertex reserve: never draw partial state.
 old_draws=draws;fail_at=reservations+2;next_draw();assert(!submit(&p)&&draws==old_draws);
 fail_at=0;next_draw();assert(submit(&p)&&draws==old_draws+1);
 // Defensive success-with-null validation must fail closed as well.
 null_at=reservations+1;next_draw();assert(!submit(&p));null_at=0;
 next_draw();assert(submit(&p));
 // Clear/scissor restores invalidate rather than rebind old native pointers.
 assert(vgl_def_vert_buf&&vgl_def_frag_buf);vglRestoreVertexUniformBuffer();vglRestoreFragmentUniformBuffer();
 assert(!vgl_def_vert_buf&&!vgl_def_frag_buf);next_draw();assert(submit(&p));
 uint64_t nulls=0,errors=0,empty_v=0,empty_f=0;
 uint32_t first_stage=0,first_bytes=0,last_stage=0,last_bytes=0;
 int32_t first_result=0,last_result=0;
 vglGetUniformReservationDiagnostics(&nulls,&errors,&empty_v,&empty_f,&first_stage,&first_result,&first_bytes,&last_stage,&last_result,&last_bytes);
 assert(nulls==1&&errors==2&&empty_v==2&&empty_f==2);
 assert(first_stage==1&&first_result==-17&&first_bytes==sizeof vs);
 assert(last_stage==1&&last_result==0&&last_bytes==sizeof vs);
 // Helper size=0 is an accounting sentinel, not an empty reflected program.
 before_empty=reservations;next_draw();assert(vglReserveVertexUniformBuffer(0));
 assert(reservations==before_empty+1&&!vgl_uniform_reservation_failed);
 context.vertex_size=0;next_draw();assert(!vglReserveVertexUniformBuffer(0));
 assert(vgl_uniform_reservation_failed);context.vertex_size=sizeof vs;
 next_draw();assert(submit(&p));
 vglGetUniformReservationDiagnostics(&nulls,&errors,&empty_v,&empty_f,&first_stage,&first_result,&first_bytes,&last_stage,&last_result,&last_bytes);
 assert(nulls==2&&errors==2&&empty_v==2&&empty_f==2);
 assert(first_stage==1&&first_result==-17&&first_bytes==sizeof vs);
 assert(last_stage==1&&last_result==0&&last_bytes==0);
 vglGetUniformReservationDiagnostics(NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
 uint64_t vb=0,fb=0,vc=0,fc=0,failed=0;
 vglGetUniformReservationStats(&vb,&fb,&vc,&fc,&failed);
 assert(vc+fc==reservations&&failed==4);
 assert(vb+fb==native_bytes-sizeof vs&&vb>2*1024*1024);
 assert(vc>fc); // Failed VS prevents FS attempt; failed FS leaves one successful VS.
 vglGetUniformReservationStats(NULL,NULL,NULL,NULL,NULL);
 for(unsigned i=0;i<snapshots_count;++i)free(snapshots[i].ptr);
 return 0;
}
