#!/usr/bin/env python3
"""Extract the actual C03 sampled-texture loop and original negative control.

Backend ABI/dispatch test only: no substitute GPU, actor, or game decoder.
The full candidate must equal the SHA-bound baseline plus the one audited call
replacement. Generated wrappers omit unrelated push_draw setup, retain its
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
TEXTURE_PIN = '8ba4b22ab4a4ba1db58f77a7c0bd04994624ae63ae58c12d3b545bf30083f9fc'

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
    if original.count(OLD) != 1 or candidate != original.replace(OLD, NEW, 1):
        raise SystemExit('Candidate differs from the audited single-call C03 replacement')
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
