#!/usr/bin/env python3
"""DRAFT: extract real pinned sampler code; never reads or mutates the build mirror.

Run only after the coordinator releases the native window. Applies patches only
inside a new TemporaryDirectory. Does not compile/run the generated test.
Native copy-at-setter is a test assumption, not a GPU or driver-implementation proof.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

PIN = '7635e3ceb3c52e34ca2e51d2281628d6c756a35b'
ARCHIVE_SHA = 'cf0849e18c3a692c9aec91854c8ca62da0598c8a6f7a1e17336f7d67ba092e23'
RESERVATION_SHA = '0f1572904982d3e27bace4df71105c341a9f3e8752c50d72652ef9a74e1e9f29'
CANDIDATE_SHA = '9acf4217048cf4286637640cd2106bb195d65d264880df973ea099bbb7c66d28'
BASE = {
    'textures.c': 'b1c26610c15af421b9b12350d477fe8f3106f56a7d8aa3874163a3c9b70f4f3e',
    'custom_shaders.c': 'cdb3d943b265f56fb0affa58f52423c333177149abc25fc0072c11f6d3c0de2f',
    'shared.h': 'b93ba0c9d687aa3f1613041c2783b6753abcd22719994343aa134164dc2809f9',
    'vitaGL.h': '5da33f7b11eadce8e8d4174ff63d94ec6e3460e03c193b4c7b722a5afb8b14d7',
    'utils/gxm_utils.h': '10ff357b2e9db3ba2160df53138018bfaaeb1a8fc862518eb652890074f33361',
}
ROUTES = (
    ('multidraw', '_glMultiDrawArrays_CustomShadersIMPL'),
    ('arrays', '_glDrawArrays_CustomShadersIMPL'),
    ('elements', '_glDrawElements_CustomShadersIMPL'),
)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def require_hash(path, expected):
    if sha(path.read_bytes()) != expected:
        raise ValueError('Hash mismatch: ' + str(path))


def balanced_end(source, opening):
    """C brace scanner skipping comments and string/character literals."""
    depth, i = 0, opening
    while i < len(source):
        if source.startswith('//', i):
            end = source.find('\n', i)
            i = len(source) if end < 0 else end + 1
            continue
        if source.startswith('/*', i):
            end = source.find('*/', i + 2)
            if end < 0:
                raise ValueError('Unclosed C comment')
            i = end + 2
            continue
        char = source[i]
        if char in ('"', "'"):
            quote = char
            i += 1
            while i < len(source):
                if source[i] == '\\':
                    i += 2
                elif source[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
            continue
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise ValueError('Unclosed C braces')


def function(source, name):
    pattern = r'^[^\n]*\b' + re.escape(name) + r'\([^;{}]*\)\s*\{'
    matches = list(re.finditer(pattern, source, re.M))
    if len(matches) != 1:
        raise ValueError('Function anchor count for ' + name + ': ' + str(len(matches)))
    match = matches[0]
    return source[match.start():balanced_end(source, match.end() - 1)] + '\n'


def macro(source, name):
    lines = source.splitlines(keepends=True)
    matches = [i for i, line in enumerate(lines) if re.match(r'^#define ' + name + r'\b', line)]
    if len(matches) != 1:
        raise ValueError('Macro anchor: ' + name)
    start = end = matches[0]
    while lines[end].rstrip().endswith('\\'):
        end += 1
    return ''.join(lines[start:end + 1])


def typedef(source, start, end):
    begin = source.index(start)
    finish = source.index(end, begin) + len(end)
    return source[begin:finish] + '\n'


def draw_loop(body, stage):
    marker = '// Uploading ' + stage + ' textures on relative texture units'
    if body.count(marker) != 1:
        raise ValueError('Draw loop marker: ' + marker)
    begin = body.index('\n\tfor (int i = 0;', body.index(marker)) + 1
    opening = body.index('{', begin)
    loop = body[begin:balanced_end(body, opening)] + '\n'
    expected = 'sceGxmSet' + stage.capitalize() + 'Texture(gxm_context, i, &tex->gxm_tex);'
    if loop.count(expected) != 1 or '->sampler_index' not in loop:
        raise ValueError('Unexpected native bind loop')
    return loop


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--reservation-patch', type=Path, required=True)
    parser.add_argument('--candidate-patch', type=Path,
                        default=Path(__file__).resolve().parents[1] / 'patches/deps/vitagl/0002-checked-sampler-objects.patch')
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true',
                        help='For negative controls only; original allocation failure dereferences NULL')
    args = parser.parse_args()
    require_hash(args.archive, ARCHIVE_SHA)
    require_hash(args.reservation_patch, RESERVATION_SHA)
    require_hash(args.candidate_patch, CANDIDATE_SHA)
    with tempfile.TemporaryDirectory(prefix='twv-sampler-fixture-') as temp:
        target = Path(temp)
        with tarfile.open(args.archive) as archive:
            for member in archive.getmembers():
                parts = Path(member.name).parts
                if not member.isfile() or len(parts) < 3 or parts[0] != 'vitaGL-' + PIN or parts[1] != 'source':
                    continue
                relative = Path(*parts[1:])
                if relative.is_absolute() or '..' in relative.parts:
                    raise ValueError('Unsafe source archive member')
                output = target / relative
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(archive.extractfile(member).read())
        subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(args.reservation_patch.resolve())],
                       cwd=target, check=True)
        for name, expected in BASE.items():
            require_hash(target / 'source' / name, expected)
        baseline = {name: (target / 'source' / name).read_text() for name in BASE}
        if not args.baseline:
            subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-i', str(args.candidate_patch.resolve())],
                           cwd=target, check=True)
        sources = {name: (target / 'source' / name).read_text() for name in BASE}
        textures, custom = sources['textures.c'], sources['custom_shaders.c']
        shared, header, helpers = sources['shared.h'], sources['vitaGL.h'], sources['utils/gxm_utils.h']
        if baseline['custom_shaders.c'].count('->use_mips ? tex->mip_count : 0);') != 12:
            raise ValueError('Expected exactly twelve baseline custom mip sites')
        chunks = ['/* Extracted production sampler code; host ABI/GXM seams are explicit. */\n']
        chunks.append('#define TWV_SAMPLER_FIXTURE_CANDIDATE ' + str(int(not args.baseline)) + '\n')
        bodies = [function(textures, name) for name in (
            '_glTexParameteri', 'glGenSamplers', 'glDeleteSamplers', 'glBindSampler',
            'glSamplerParameteri', 'glSamplerParameterf')]
        if not args.baseline:
            gen = bodies[1]
            if 'if (!smp)' not in gen or 'SET_GL_ERROR(GL_OUT_OF_MEMORY)' not in gen:
                raise ValueError('Missing candidate OOM guard')
            for line in custom.splitlines():
                if 'vglSetTexMipmapCount(&tex->gxm_tex,' in line and 'use_mips ?' in line:
                    if ': (tex_type == 0 ? 1 : 0));' not in line:
                        raise ValueError('Uncovered custom sampler mip site: ' + line)
            expected = baseline['custom_shaders.c'].replace(
                '->use_mips ? tex->mip_count : 0);',
                '->use_mips ? tex->mip_count : (tex_type == 0 ? 1 : 0));')
            if expected != custom:
                raise ValueError('Candidate changed more than the twelve count sites')
        error_macros = macro(shared, 'SET_GL_ERROR') + macro(shared, 'SET_GL_ERROR_WITH_VALUE')
        if 'return;' not in macro(shared, 'SET_GL_ERROR'):
            raise ValueError('SET_GL_ERROR must return independent of SKIP_ERROR_HANDLING')
        helper_names = ('vglSetTexUMode', 'vglSetTexVMode', 'vglSetTexMinFilter',
                        'vglSetTexMagFilter', 'vglSetTexMipFilter', 'vglSetTexLodBias', 'vglSetTexMipmapCount')
        helper_bodies = [function(helpers, name) for name in helper_names]
        native_chunks = []
        manifest_routes = {}
        for label, route in ROUTES:
            body = function(custom, route)
            frag, vert = draw_loop(body, 'fragment'), draw_loop(body, 'vertex')
            native_chunks.append('static void fixture_' + label + '() {\nprogram *p = &progs[cur_program - 1];\n' + frag + vert + '}\n')
            manifest_routes[label] = {'function': route, 'fragment_sha256': sha(frag.encode()), 'vertex_sha256': sha(vert.encode())}
        required = set(re.findall(r'\bGL_[A-Z0-9_]+\b', ''.join(bodies))) | {'GL_NO_ERROR', 'GL_OUT_OF_MEMORY'}
        defines = dict(re.findall(r'^#define (GL_[A-Z0-9_]+)\s+([^\n]+)', header, re.M))
        pending = list(required)
        while pending:
            name = pending.pop()
            if name not in defines:
                raise ValueError('Missing GL constant ' + name)
            for dependency in re.findall(r'\bGL_[A-Z0-9_]+\b', defines[name]):
                if dependency not in required:
                    required.add(dependency)
                    pending.append(dependency)
        chunks.extend('#define ' + name + ' ' + defines[name] + '\n' for name in sorted(required))
        for name in ('COMBINED_TEXTURE_IMAGE_UNITS_NUM',):
            chunks.append(macro(shared, name))
        enum_end = shared.index('TEX_VALID')
        enum_start = shared.rfind('enum {', 0, enum_end)
        chunks.append(shared[enum_start:shared.index(';', enum_end) + 1] + '\n')
        chunks.append(typedef(shared, 'typedef struct texture {', '} texture;'))
        chunks.append(typedef(shared, '// Sampler object struct\ntypedef struct {', '} sampler;'))
        chunks.append('''
static texture texture_slots[16]{};
static texture_unit texture_units[COMBINED_TEXTURE_IMAGE_UNITS_NUM]{};
static sampler *samplers[COMBINED_TEXTURE_IMAGE_UNITS_NUM]{};
static program progs[1]{};
static unsigned cur_program = 1;
static uint8_t tex2d_override = 0;
static uint32_t vgl_framecount = 1;
static void *gxm_context = nullptr;
static GLenum vgl_error = 0;
static void *vglMalloc(size_t);
static void vgl_free(void *);
static void vgl_log(const char *, ...);
static int sceGxmSetFragmentTexture(void *, unsigned, const SceGxmTexture *);
static int sceGxmSetVertexTexture(void *, unsigned, const SceGxmTexture *);
#define THREAD_SAFE() /* mock: this host seam is single-threaded */
''')
        chunks.append(error_macros)
        chunks.extend(helper_bodies)
        chunks.extend(bodies)
        chunks.extend(native_chunks)
        output_dir = args.output_dir.resolve()
        output_dir.mkdir(parents=True, exist_ok=True)
        output = output_dir / 'pinned_sampler_driver.inc'
        output.write_text('\n'.join(chunks), newline='\n')
        manifest = {'pin': PIN, 'archive_sha256': ARCHIVE_SHA, 'reservation_patch_sha256': RESERVATION_SHA,
                    'candidate_patch_sha256': None if args.baseline else CANDIDATE_SHA,
                    'baseline': args.baseline, 'baseline_sources': BASE,
                    'post_patch_sources': {name: sha((target / 'source' / name).read_bytes()) for name in BASE},
                    'routes': manifest_routes, 'fixture_sha256': sha(output.read_bytes()),
                    'limits': ['actual sampler functions, structs and six native bind loops only',
                               'host GLuint is uintptr_t; not a 32-bit ABI proof',
                               'native copy-at-setter is mocked, not hardware evidence',
                               'no vertex/index submission, shader compilation or GPU retirement proof']}
        (output_dir / 'sampler-driver-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', newline='\n')
        print('Prepared actual-source fixture:', output)


if __name__ == '__main__':
    main()
