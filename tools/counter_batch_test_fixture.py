#!/usr/bin/env python3
"""Extract actual counter producers and the frozen V59 string-counter oracle.

The reference snippets come from the pinned preceding backend, not a rewrite of
the newly batched conditions or names. Output is normalized to LF on every host.
"""
import argparse
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKEND = 'platform/vita/gfx_vgl/backend.cpp'
REFERENCE_COMMIT = 'bb52f1cf0b0f336c2db1e1da0d3283573da5ac45'
REFERENCE_SHA = 'b701e74c1a15b8cea2bcd84be964d0a59c4ed304a8e929ad656bd1f9af41aa34'


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


def fallback_calls(source):
    start = source.index(' if(c.shaderConfig.numIndStages) count(')
    return source[start:source.index('\n const auto lights=', start)]


def cull_call(source):
    start = source.index(' if(vertexCulled){count(')
    return source[start:source.index('\n', start)]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    source = (ROOT / BACKEND).read_text()
    original = (ROOT / 'tests/fixtures/baseline' / REFERENCE_COMMIT[:7] / BACKEND).read_text()
    if hashlib.sha256(original.encode()).hexdigest() != REFERENCE_SHA:
        raise SystemExit('Unexpected frozen V59 backend; re-audit counter reference')

    def write(name, text):
        (args.output_dir / name).write_bytes((text + '\n').encode())

    start = source.index('void count(const char* name,')
    end = source.index('\n}\nnamespace twv::gfx_vgl', start)
    write('counter_functions.inc', source[start:end])
    start = source.index(' if(p.gpuVertex) {\n  count(CounterId::GpuMatrixSlots')
    end = source.index('\n const auto* indices=', start)
    parts = [source[start:end]]
    start = source.index(' count(CounterId::DrawsSubmitted)', end)
    end = source.index('\n}\nvoid resolve_pass_into', start)
    parts.append(source[start:end])
    write('draw_counter_calls.inc', '\n'.join(parts))

    # Original full callback bodies keep selection/return behavior in the oracle.
    signatures = ['extern "C" int twv_vita_shadow_image_skip(',
                  'extern "C" void twv_vita_shadow_image_skipped(']
    current_shadow = '\n'.join(function(source, signature) for signature in signatures)
    original_shadow = '\n'.join(function(original, signature) for signature in signatures)
    original_shadow = original_shadow.replace('twv_vita_shadow_image_skip(', 'shadow_image_skip_reference(')
    original_shadow = original_shadow.replace('twv_vita_shadow_image_skipped(', 'shadow_image_skipped_reference(')
    original_shadow = original_shadow.replace('vgl::count(', '::reference_count(')
    write('backend_counter_shadow.inc', 'namespace twv::gfx_vgl {\n' + current_shadow + '\n' + original_shadow + '\n}')
    write('backend_counter_fallback.inc', fallback_calls(source))
    write('backend_counter_fallback_reference.inc', fallback_calls(original).replace('count(', 'reference_count('))
    write('backend_counter_cull.inc', cull_call(source))
    write('backend_counter_cull_reference.inc', cull_call(original).replace('count(', 'reference_count('))

    # The only intended backend changes are these eight count-call substitutions.
    substitutions = {
        '"fallback_indirect_draw"': 'CounterId::FallbackIndirectDraw',
        '"fallback_fog_range_draw"': 'CounterId::FallbackFogRangeDraw',
        '"fallback_logic_blend_draw"': 'CounterId::FallbackLogicBlendDraw',
        '"vertex_cull_draws"': 'CounterId::VertexCullDraws',
        '"shadow_image_calls"': 'vgl::CounterId::ShadowImageCalls',
        '"shadow_image_skipped_calls"': 'vgl::CounterId::ShadowImageSkippedCalls',
        '"shadow_image_skipped_models"': 'vgl::CounterId::ShadowImageSkippedModels',
        '"shadow_image_restored_packets"': 'vgl::CounterId::ShadowImageRestoredPackets',
    }
    expected = original
    for old, new in substitutions.items():
        if expected.count(old) != 1:
            raise SystemExit('Ambiguous V59 counter name: ' + old)
        expected = expected.replace(old, new, 1)
    # Off-screen skip: the shadow query runs on the game thread, so it
    # counts into the game-thread batch and no longer reads the FIFO's offscreen flag.
    for old, new, times in (('vgl::count(vgl::CounterId::ShadowImage', 'vgl::count_game(vgl::CounterId::ShadowImage', 4),
                            ('.enabled() && vgl::offscreen && !vgl::OffscreenDrawSupported',
                             '.enabled() && !vgl::OffscreenDrawSupported', 1)):
        if expected.count(old) != times:
            raise SystemExit('Unexpected V59 shadow counter text: ' + old)
        expected = expected.replace(old, new)
    if source != expected:
        # Root may add independent backend work after this agent's freeze. Keep
        # this local audit strict for the producer blocks owned by this fixture.
        if (fallback_calls(source) != fallback_calls(expected) or cull_call(source) != cull_call(expected)
                or current_shadow != '\n'.join(function(expected, signature) for signature in signatures)):
            raise SystemExit('Backend counter producer changed beyond the audited ID substitutions')
    print('Extracted current counter producers and pinned V59 reference into', args.output_dir)


if __name__ == '__main__':
    main()
