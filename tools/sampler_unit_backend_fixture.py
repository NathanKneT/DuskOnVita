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

def audited(original):
    """The baseline with the C03 replacement, the G2 insertions (each after its anchor line) and the
    off-screen edits."""
    expected = original.replace(OLD, NEW, 1)
    for anchor, inserted in G2_INSERTIONS:
        if expected.count(anchor) != 1:
            raise SystemExit('G2 anchor not unique in the baseline; re-audit: ' + anchor.strip())
        expected = expected.replace(anchor, anchor + inserted, 1)
    for old, new in OFFSCREEN:
        if expected.count(old) != 1:
            raise SystemExit('Audited off-screen edit no longer matches the baseline: ' + old[:60])
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
