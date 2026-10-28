#!/usr/bin/env python3
"""Extract the actual C03 sampled-texture loop and original negative control.

Backend ABI/dispatch test only: no substitute GPU, actor, or game decoder.
The full candidate must equal the SHA-bound baseline plus the one audited call
replacement and the three audited G2 insertions outside the loop. Generated wrappers omit unrelated push_draw setup, retain its
sampled-texture loop verbatim, and execute its exact draw statement afterward.
"""
import argparse
import hashlib
import json
from pathlib import Path

RELATIVE = 'platform/vita/gfx_vgl/backend.cpp'
BASELINE_REVISION = '964d278cd1190f659c23bc68be5f62b32a7da245'
BASELINE_LF_SHA256 = '030d89f724bdf2c179da3f27ac77c72a59b83b30b083796b161eea64c02aa10c'
LOOP = ' for(int i=0;i<8;++i) if(p.info.sampledTextures.test(i)) {'
OLD = '  twv_vgl_texture_sampler(texture->gpu,obj.min_filter(),obj.mag_filter()==GX_LINEAR,obj.wrap_s(),obj.wrap_t());'
NEW = '''  if(!twv_vgl_texture_sampler_unit(i,obj.min_filter(),obj.mag_filter()==GX_LINEAR,obj.wrap_s(),obj.wrap_t())) {
   twv::vita::telemetryText("gfx_bundle_failure","per-unit sampler setup failed before draw");
   twv::vita::telemetryFlush();std::abort();
  }'''
DRAW = 'twv_vgl_draw(&state,vertices,data.vtxCount,indices,data.indexCount);'
# Later audited backend change (the off-screen skip): game-thread shadow query and counters,
# discarded off-screen draws tallied per target size. Applied after the C03 replacement.
OFFSCREEN = [
    ('VertexDecoderControl shadowImageSkipControl{bool(TWV_VITA_SHADOW_IMAGE_SKIP)};\n',
     'VertexDecoderControl shadowImageSkipControl{bool(TWV_VITA_SHADOW_IMAGE_SKIP)};\n'
     "// Counted by the game thread's skip queries, merged at end_frame after the FIFO drain:\n"
     '// with the GX worker on, hotCounters belong to the worker.\n'
     'CounterBatch gameThreadCounters;\n'),
    ('uint32_t frame_count=0, off_w=0, off_h=0;\n',
     'uint32_t frame_count=0, off_w=0, off_h=0;\n'
     '// Draws dropped because their off-screen target cannot be drawn, by target size (one size\n'
     '// per pass: shadow, bloom, depth of field, minimap), so a run shows what each pass costs.\n'
     'struct OffscreenDiscards {uint32_t w,h,draws;};\n'
     'std::array<OffscreenDiscards,8> offscreenDiscards{};\n'
     'void tally_offscreen_discard() noexcept {\n'
     ' for(auto& t:offscreenDiscards) if(!t.draws || (t.w==off_w && t.h==off_h)) {t.w=off_w;t.h=off_h;++t.draws;return;}\n'
     '}\n'),
    ('void flush_hot_counters() noexcept {\n#if TWV_VITA_BATCH_COUNTERS\n'
     ' hotCounters.flush([](const char* name,uint64_t delta) {\n'
     '  const auto it=counters.find(name);\n  if(it!=counters.end())it->second+=delta;\n'
     '  else counters.emplace(name,delta);\n });\n#endif\n}\n',
     'void count_game(CounterId id,uint32_t amount) noexcept {gameThreadCounters.add(id,amount);}\n'
     'void flush_hot_counters() noexcept {\n const auto merge=[](const char* name,uint64_t delta) {\n'
     '  const auto it=counters.find(name);\n  if(it!=counters.end())it->second+=delta;\n'
     '  else counters.emplace(name,delta);\n };\n#if TWV_VITA_BATCH_COUNTERS\n'
     ' hotCounters.flush(merge);\n#endif\n gameThreadCounters.flush(merge);\n}\n'),
    ('extern "C" int twv_vita_shadow_image_skip() {\n vgl::shadowImageSkipControl.begin();\n'
     ' vgl::count(vgl::CounterId::ShadowImageCalls);\n'
     ' return vgl::shadowImageSkipControl.enabled() && vgl::offscreen && !vgl::OffscreenDrawSupported;\n}\n'
     'extern "C" void twv_vita_shadow_image_skipped(unsigned models,unsigned shapes) {\n'
     ' vgl::count(vgl::CounterId::ShadowImageSkippedCalls);\n'
     ' vgl::count(vgl::CounterId::ShadowImageSkippedModels,models);\n'
     ' vgl::count(vgl::CounterId::ShadowImageRestoredPackets,shapes);\n}\n',
     '// Game thread. Reads only start-up state, never vgl::offscreen: with the GX worker on, the\n'
     "// worker may not have processed the pass's BEGIN_OFFSCREEN yet. dDlst_shadowReal_c::imageDraw\n"
     "// is only called inside the shadow pass's GXCreateFrameBuffer (TARGET_PC), so it is off-screen.\n"
     'extern "C" int twv_vita_shadow_image_skip() {\n vgl::shadowImageSkipControl.begin();\n'
     ' vgl::count_game(vgl::CounterId::ShadowImageCalls);\n'
     ' return vgl::shadowImageSkipControl.enabled() && !vgl::OffscreenDrawSupported;\n}\n'
     'extern "C" void twv_vita_shadow_image_skipped(unsigned models,unsigned shapes) {\n'
     ' vgl::count_game(vgl::CounterId::ShadowImageSkippedCalls);\n'
     ' vgl::count_game(vgl::CounterId::ShadowImageSkippedModels,models);\n'
     ' vgl::count_game(vgl::CounterId::ShadowImageRestoredPackets,shapes);\n}\n'),
    ('  for(const auto& [key,value]:vgl::counters) twv::vita::telemetryEvent(("gfx_"+key).c_str(),value);\n',
     '  for(const auto& [key,value]:vgl::counters) twv::vita::telemetryEvent(("gfx_"+key).c_str(),value);\n'
     '  for(const auto& t:vgl::offscreenDiscards) if(t.draws)\n'
     '   twv::vita::telemetryEvent(("gfx_offscreen_discard_"+std::to_string(t.w)+"x"+std::to_string(t.h)).c_str(),t.draws);\n'),
    ('{count("unsupported_offscreen_draw");return;}',
     '{count("unsupported_offscreen_draw");tally_offscreen_discard();return;}'),
]
TEXTURE_PIN = '8ba4b22ab4a4ba1db58f77a7c0bd04994624ae63ae58c12d3b545bf30083f9fc'
# GXM G2 (docs/ARCHITECTURE.md): the only other audited backend changes, three insertions outside
# the sampled-texture loop (resident static indices, packed formats, native primitive).
G2_INSERTIONS = [
    (' (void)alignment; sFrameStats.lastIndexSize+=size;\n',
     ' // G2 (--vita-gxm-resident): indices inside the static list being processed stay in GPU memory.\n'
     ' if(const void* resident=twv::vgl::static_resident_indices(data,size))\n'
     '  return {.offset=uint32_t(reinterpret_cast<uintptr_t>(resident)),.size=uint32_t(size)};\n'),
    (' const auto lights=gpu_light_plan(p);\n',
     ' // G2 (GXM, --vita-gxm-compact): packed formats are fixed before the first decode and shader.\n'
     ' if(!p.packedConfigured) configure_packed_formats(p);\n'),
    (' state.vertex_layout=p.vertexLayout;\n',
     ' // G2: native primitive (patches/aurora/0008, only when the GXM backend draws them) and packed formats.\n'
     ' state.primitive=data.primitive==GX_TRIANGLESTRIP?TwvPrimitiveStrip:data.primitive==GX_TRIANGLEFAN?TwvPrimitiveFan:\n'
     '     data.primitive==GX_QUADS?TwvPrimitiveQuads:TwvPrimitiveTriangles;\n'
     ' std::memcpy(state.vertex_formats,p.packed.format.data(),TwvVertexAttributeCount);\n'
     ' std::memcpy(state.vertex_counts,p.packed.count.data(),TwvVertexAttributeCount);\n'
     ' std::memcpy(state.vertex_fracs,p.packed.frac.data(),TwvVertexAttributeCount);\n'),
]

# Track D (docs/PERFORMANCE.md, --vita-gxm-pipeline-memo): the shader-info check compares the
# config with a hot copy of the one last checked for the same pipeline before the stored one.
# Outside the sampled-texture loop.
PIPELINE_MEMO = [
    (' uint64_t requests=0,reused=0,built=0,fallback=0,oracleChecks=0,oracleFailures=0,invalid=0;\n',
     ' uint64_t requests=0,reused=0,built=0,fallback=0,oracleChecks=0,oracleFailures=0,invalid=0,memoHits=0;\n'
     ' // --vita-gxm-pipeline-memo (twv_vita_pipeline_info_copy): -1 until read at the first call, then the\n'
     ' // pipeline last checked there, with copies of its config and info that stay in cache.\n'
     ' int memoOn=-1;\n uint32_t memoPipeline=UINT32_MAX;\n aurora::gx::PipelineConfig memoConfig{};\n'
     ' aurora::gx::ShaderInfo memoInfo{};\n'),
    ('  twv::vita::telemetryEvent("gfx_shader_info_invalid",sPipelineInfoReuse.invalid);\n',
     '  twv::vita::telemetryEvent("gfx_shader_info_invalid",sPipelineInfoReuse.invalid);\n'
     '  if(twv_gpu_features()&TwvGpuFeaturePipelineMemo)\n'
     '   twv::vita::telemetryEvent("gfx_shader_info_memo_hits",sPipelineInfoReuse.memoHits);\n'),
    (' vgl::pipelineIndex.clear();\n',
     " vgl::pipelineIndex.clear();\n sPipelineInfoReuse.memoPipeline=UINT32_MAX; // the memo's pipeline is gone\n"),
    (' if(pipeline>=twv::vgl::pipelines.size()) {\n  ++sPipelineInfoReuse.invalid;pipeline_info_failure(3);\n }\n'
     ' const auto& cached=twv::vgl::pipelines[pipeline];\n'
     ' if(std::memcmp(&cached.config,expected,sizeof *expected)!=0) {\n'
     '  ++sPipelineInfoReuse.invalid;pipeline_info_failure(3);\n }\n',
     " // --vita-gxm-pipeline-memo: a pipeline's stored config never changes (pipelines are cleared only at\n"
     ' // shutdown), so a config equal to the copy of the one last checked for that pipeline passes the\n'
     ' // check against the stored one: the whole-config comparison runs against the hot copy instead of\n'
     " // the pipeline's own, rarely cached, config.\n"
     ' auto& memo=sPipelineInfoReuse;\n'
     ' if(memo.memoOn<0)memo.memoOn=(twv_gpu_features()&TwvGpuFeaturePipelineMemo)!=0;\n'
     ' const bool remembered=memo.memoOn && pipeline==memo.memoPipeline &&\n'
     '  std::memcmp(&memo.memoConfig,expected,sizeof *expected)==0;\n'
     ' if(!remembered) {\n  if(pipeline>=twv::vgl::pipelines.size()) {\n'
     '   ++sPipelineInfoReuse.invalid;pipeline_info_failure(3);\n  }\n'
     '  if(std::memcmp(&twv::vgl::pipelines[pipeline].config,expected,sizeof *expected)!=0) {\n'
     '   ++sPipelineInfoReuse.invalid;pipeline_info_failure(3);\n  }\n }\n'
     ' const auto& cached=twv::vgl::pipelines[pipeline];\n'),
    (' *out=cached.info;++sPipelineInfoReuse.reused;return 1;\n',
     ' if(remembered) {*out=memo.memoInfo;++sPipelineInfoReuse.memoHits;++sPipelineInfoReuse.reused;return 1;}\n'
     ' *out=cached.info;\n'
     ' if(memo.memoOn) {memo.memoPipeline=pipeline;memo.memoConfig=*expected;memo.memoInfo=cached.info;}\n'
     ' ++sPipelineInfoReuse.reused;return 1;\n'),
]

# g4 console fixes (28 Oct): the game drawn full width (it is laid out for 960x544), and draws that
# sample an EFB copy nothing wrote skipped (black water). Outside the sampled-texture loop: the
# helper has its own copy of the loop header, in a separate function.
CONSOLE_FIXES = [
    ('constexpr bool OffscreenDrawSupported=false;\n',
     'constexpr bool OffscreenDrawSupported=false;\n'
     '// EFB copies (resolve_pass_into) are not implemented either. A draw that samples a copy would\n'
     '// read storage nothing wrote and paint it over the scene: retail water (fbtex_dummy) shows the\n'
     '// copied scene behind it, and here it came out opaque black (F_SP104 opening, village, Faron).\n'
     '// Leaving the destination untouched is the closest picture until copies exist; implementing\n'
     '// them must change this, so that no supported copy is skipped.\n'
     'constexpr bool CopyResolveSupported=false;\n'
     'bool samples_unresolved_copy(const Pipeline& p) noexcept {\n'
     ' const auto& textures=aurora::gx::g_gxState.textures;\n'
     ' for(int i=0;i<8;++i) if(p.info.sampledTextures.test(i)) {\n'
     '  const auto* texture=static_cast<const Texture*>(textures[i].ref.get());\n'
     '  if(texture && texture->efbCopy) return true;\n'
     ' }\n'
     ' return false;\n'
     '}\n'),
    ('aurora::gfx::Viewport viewport{0,0,724,544,0,1};\n'
     'aurora::gfx::ClipRect scissor{0,0,724,544};\n'
     'constexpr int GameLeft=118, GameWidth=724, GameHeight=544;\n',
     '// The game lays itself out for the full 960x544 screen (Dusklight widens its logical 2D space and\n'
     '// camera aspect to the window), so it is drawn full width: a 724-wide box squeezed it to 75%.\n'
     'aurora::gfx::Viewport viewport{0,0,960,544,0,1};\n'
     'aurora::gfx::ClipRect scissor{0,0,960,544};\n'
     'constexpr int GameLeft=0, GameWidth=960, GameHeight=544;\n'),
    (' return twv::vgl::offscreen ? Vec2<uint32_t>{twv::vgl::off_w,twv::vgl::off_h} : Vec2<uint32_t>{724,544};\n',
     ' return twv::vgl::offscreen ? Vec2<uint32_t>{twv::vgl::off_w,twv::vgl::off_h} :\n'
     '                               Vec2<uint32_t>{twv::vgl::GameWidth,twv::vgl::GameHeight};\n'),
    (' if(c.shaderConfig.lineMode) {count("unsupported_line_point_draw");return;}\n',
     ' if(c.shaderConfig.lineMode) {count("unsupported_line_point_draw");return;}\n'
     ' if(!CopyResolveSupported && samples_unresolved_copy(p)) {count("unresolved_copy_draw");return;}\n'),
    (' const int x1=std::min({r.x+r.width,int(std::ceil(vp.left+vp.width)),724});\n',
     ' const int x1=std::min({r.x+r.width,int(std::ceil(vp.left+vp.width)),GameWidth});\n'),
]

def audited(original):
    """The baseline with the C03 replacement, the G2 insertions (each after its anchor line), the
    off-screen edits, the pipeline memo and the g4 console fixes."""
    expected = original.replace(OLD, NEW, 1)
    for anchor, inserted in G2_INSERTIONS:
        if expected.count(anchor) != 1:
            raise SystemExit('G2 anchor not unique in the baseline; re-audit: ' + anchor.strip())
        expected = expected.replace(anchor, anchor + inserted, 1)
    for old, new in OFFSCREEN:
        if expected.count(old) != 1:
            raise SystemExit('Audited off-screen edit no longer matches the baseline: ' + old[:60])
        expected = expected.replace(old, new, 1)
    for old, new in PIPELINE_MEMO:
        if expected.count(old) != 1:
            raise SystemExit('Audited pipeline-memo edit no longer matches the baseline: ' + old[:60])
        expected = expected.replace(old, new, 1)
    for old, new in CONSOLE_FIXES:
        if expected.count(old) != 1:
            raise SystemExit('Audited console-fix edit no longer matches the baseline: ' + old[:60])
        expected = expected.replace(old, new, 1)
    return expected

def sha(data):
    return hashlib.sha256(data).hexdigest()

def declaration(source, name):
    start = source.index('struct ' + name + ' {')
    return source[start:source.index('\n};', start) + 3]

def loop_and_draw(source, name):
    signature = 'template<> void push_draw_command<gx::DrawData>(gx::DrawData data) {'
    assert source.count(signature) == 1
    start = source.index(signature)
    end = source.index('\n}', start) + 2
    body = source[start:end]
    assert body.count(LOOP) == body.count(DRAW) == 1
    first = body.index(LOOP)
    last = body.index(' stateTimer.finish();', first)
    assert body.index(DRAW) > last
    loop = body[first:last]
    # The marker proves this is the draw's existing bind stage, not an unused
    # helper or comment that happens to contain a similar loop.
    assert body[:first].endswith(' if(trace) twv::vita::telemetryEvent("gfx_bind_enter",data.pipeline);\n')
    assert loop.endswith(' }\n')
    wrapper = ('void ' + name + '(const Pipeline& p,const GxBindings& gx,TwvVitaDrawState& state,\n'
               ' const DrawData& data,const void* vertices,const uint16_t* indices) {\n'
               + loop + ' ' + DRAW + '\n}\n')
    return wrapper, body, loop

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo-root', type=Path, required=True)
    parser.add_argument('--candidate-source', type=Path)
    parser.add_argument('--gpu-header', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    baseline_bytes = (args.repo_root / 'tests/fixtures/baseline' / BASELINE_REVISION[:7] /
                      RELATIVE).read_bytes().replace(b'\r\n', b'\n')
    if sha(baseline_bytes) != BASELINE_LF_SHA256:
        raise SystemExit('Pinned Git backend baseline changed; re-audit C03 replacement')
    original = baseline_bytes.decode()
    candidate_path = args.candidate_source or args.repo_root / RELATIVE
    candidate = candidate_path.read_text()
    if original.count(OLD) != 1 or candidate != audited(original):
        raise SystemExit('Candidate differs from the audited C03 replacement, G2 insertions and off-screen edits')
    output = args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    candidate_wrapper, candidate_body, candidate_loop = loop_and_draw(candidate, 'dispatch_candidate')
    original_wrapper, original_body, original_loop = loop_and_draw(original, 'dispatch_original')
    assert original_loop.replace(OLD, NEW, 1) == candidate_loop
    (output / 'sampler_unit_backend_loop.inc').write_text(candidate_wrapper + '\n' + original_wrapper)
    texture_path = args.repo_root / 'external/dusklight/extern/aurora/lib/gfx/texture.hpp'
    texture = texture_path.read_text()
    if sha(texture.encode()) != TEXTURE_PIN:
        raise SystemExit('Actual GX texture declarations changed; re-audit raw sampler conversion')
    types = ('namespace aurora::gfx {constexpr u32 InvalidTextureFormat = -1;}\n'
             + declaration(texture, 'GXTexObj_') + '\n'
             + 'namespace twv::vgl {struct Texture {uint32_t gpu=0;};}\n'
             + 'namespace wgpu {struct SamplerDescriptor;}\n'
             + 'namespace aurora::gfx {\nusing TextureHandle=std::shared_ptr<twv::vgl::Texture>;\n'
             + declaration(texture, 'TextureBind') + '\n}\n')
    (output / 'sampler_unit_backend_types.inc').write_text(types)
    header_path = args.gpu_header or args.repo_root / 'platform/vita/gfx_vgl/gpu_api.h'
    header = header_path.read_bytes()
    if b'int twv_vgl_texture_sampler_unit(' not in header:
        raise SystemExit('Actual modified bridge header must declare the unit API')
    (output / 'gfx_vgl').mkdir(exist_ok=True)
    (output / 'gfx_vgl/gpu_api.h').write_bytes(header)
    manifest = {
        'scope': 'actual backend loop + exact following draw statement; recorded C ABI only; no GPU or actor claim',
        'baseline': {'repository': str(args.repo_root.resolve()), 'revision': BASELINE_REVISION,
                     'path': RELATIVE, 'normalized_lf_sha256': BASELINE_LF_SHA256},
        'candidate': {'path': str(candidate_path.resolve()), 'sha256': sha(candidate_path.read_bytes())},
        'body_sha256': {'original': sha(original_body.encode()), 'candidate': sha(candidate_body.encode())},
        'loop_sha256': {'original': sha(original_loop.encode()), 'candidate': sha(candidate_loop.encode())},
        'actual_texture_header_normalized_sha256': TEXTURE_PIN,
        'actual_gpu_header': {'path': str(header_path.resolve()), 'sha256': sha(header)},
        'recorded_seams': ['opaque Texture gpu field/resource handle', 'count', 'C ABI sampler calls', 'telemetry', 'draw'],
        'failure_contract': 'real std::abort after actual telemetryText/telemetryFlush; child checks SIGABRT and no draw',
        'negative_control': 'SHA-bound original object-only caller must fail the logical-unit dispatch contract',
    }
    (output / 'sampler_unit_backend_manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print('sampler backend fixture: full-source anchor, actual GX object + bridge header, candidate/original loops')

if __name__ == '__main__':
    main()
