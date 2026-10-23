#!/usr/bin/env python3
"""Verify pinned vitaGL archive, apply reservation patch, extract production C.

Native API: https://docs.vitasdk.org/group__SceGxmUser.html
No SDK internals are simulated as evidence of hardware correctness.
"""
import argparse
import hashlib
import re
import subprocess
import tarfile
import tempfile
from pathlib import Path

PIN = '7635e3ceb3c52e34ca2e51d2281628d6c756a35b'
ARCHIVE_SHA = 'cf0849e18c3a692c9aec91854c8ca62da0598c8a6f7a1e17336f7d67ba092e23'
ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / 'patches/deps/vitagl/0001-sdk-managed-uniform-reservations.patch'

def function(source, name):
    match = re.search(r'^[^\n]*\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', source, re.M)
    if not match:
        raise ValueError('Missing function ' + name)
    start = match.start()
    same_line = source[match.end():].split('\n', 1)[0]
    if '}' in same_line:
        return source[start:match.end() + same_line.index('}') + 1] + '\n'
    end = re.search(r'^}', source[match.end():], re.M)
    if not end: raise ValueError('Missing function end ' + name)
    pos = match.end() + end.end()
    return source[start:pos] + '\n'

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--archive', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if hashlib.sha256(a.archive.read_bytes()).hexdigest() != ARCHIVE_SHA:
        raise SystemExit('Wrong vitaGL archive; expected source pin ' + PIN)
    with tempfile.TemporaryDirectory(prefix='twv-uniform-fixture-') as temp:
        target = Path(temp)
        # Extract source files only, no links or archive-provided output paths.
        with tarfile.open(a.archive) as archive:
            for item in archive.getmembers():
                parts = Path(item.name).parts
                if item.isfile() and len(parts) > 2 and parts[1] == 'source':
                    relative = Path(*parts[1:])
                    if '..' in relative.parts: raise ValueError('Unsafe source path')
                    dest = target / relative
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(archive.extractfile(item).read())
        subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(PATCH.resolve())], cwd=target, check=True, stdout=subprocess.DEVNULL)
        sources = {name: (target / ('source/' + name)).read_text() for name in
                   ('custom_shaders.c', 'ffp.c', 'draw.c', 'tests.c', 'misc.c', 'framebuffers.c', 'gxm.c', 'utils/gxm_utils.c', 'utils/gxm_utils.h')}
        custom = sources['custom_shaders.c']
        # Every setup snapshots both stages, even when CPU uniform shadows hit.
        assert 'p->vert_uniforms && dirty_shader_vert_unifs' not in custom
        assert 'p->frag_uniforms && dirty_shader_frag_unifs' not in custom
        assert 'upload_uniforms();' not in custom
        for stage, letter in (('vert', 'v'), ('frag', 'f')):
            assert custom.count('if (p->' + stage + '_uniforms && p->' + letter + 'shader->unif_buf_size) { \\') == 2
            assert custom.count('else if (!p->' + letter + 'shader->unif_buf_size) { \\') == 2
        # Reflection lists include sampler records independently of default bytes.
        link = function(custom, 'glLinkProgram')
        assert 'cat == SCE_GXM_PARAMETER_CATEGORY_SAMPLER ||' in link
        assert 'if (p->fshader->unif_buf_size)' in link
        assert 'p->frag_uniforms_num++' in link
        assert 'vglBeginUniformUpload();' in function(sources['gxm.c'], 'scene_reset')
        assert 'UNIFORM_CIRCULAR_POOL_SIZE' not in sources['utils/gxm_utils.c']
        for name in ('draw.c', 'ffp.c', 'custom_shaders.c'):
            for line in sources[name].splitlines():
                if re.search(r'\bsceGxmDraw(?:Instanced)?\(', line):
                    assert 'if (!vgl_uniform_reservation_failed)' in line, (name, line)
        for name, fn, label in (('misc.c', 'glClear', 'clear_uniform_cleanup'),
                                ('tests.c', 'update_scissor_test', 'scissor_uniform_cleanup'),
                                ('framebuffers.c', 'glBlitNamedFramebuffer', 'blit_uniform_cleanup')):
            body = function(sources[name], fn)
            assert label + ':' in body
            for match in re.finditer(r'\w+ = vglReserve(?:Vertex|Fragment)UniformBuffer\(0\);', body):
                tail = body[match.end():]
                assert re.match(r'\s*if \(!\w+\) goto ' + label + ';', tail)
            assert 'sceGxmReserveVertexDefaultUniformBuffer' not in body
            assert 'sceGxmReserveFragmentDefaultUniformBuffer' not in body
        ffp = function(sources['ffp.c'], 'reload_ffp_shaders')
        assert ffp.count('if (!buffer) return 0;') == 2
        assert 'dirty_vert_unifs = 0xFFFF' in ffp and 'dirty_frag_unifs = 0xFFFFFFFF' in ffp
        # Extract actual common reserve path, restoration wrappers and upload macro.
        chunks = ['/* pin=' + PIN + ' patch_sha256=' + hashlib.sha256(PATCH.read_bytes()).hexdigest() + ' */\n']
        chunks.append('\n'.join(re.findall(r'^static (?:uint64_t|uint32_t|int32_t) uniform_[^\n]*', sources['utils/gxm_utils.c'], re.M)) + '\n')
        for name in ('vglSetupUniformCircularPool', 'vglBeginUniformUpload', 'vglReserveNativeUniformBuffer', 'vglSkipEmptyUniformStage', 'vglGetUniformReservationStats', 'vglGetUniformReservationDiagnostics'):
            chunks.append(function(sources['utils/gxm_utils.c'], name))
        for name in ('vglRestoreFragmentUniformBuffer', 'vglRestoreVertexUniformBuffer',
                     'vglReserveFragmentUniformBuffer', 'vglReserveVertexUniformBuffer'):
            chunks.append(function(sources['utils/gxm_utils.h'], name))
        start = custom.index('#ifndef HAVE_FFP_SHADER_SUPPORT\n#define upload_uniforms(failure)')
        end = custom.index('\n#endif', start) + len('\n#endif')
        chunks.append(custom[start:end] + '\n')
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text('\n'.join(chunks))
        print('Pinned production fixture:', a.output)

if __name__ == '__main__':
    main()
