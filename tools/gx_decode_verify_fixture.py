#!/usr/bin/env python3
"""Build a pinned, real-source GX decoder host fixture, with recorded GPU I/O.

No decoder body is transcribed. The full regs.cpp, process family, ByteReader and
handle_aurora are compiled from the patched temporary source. See the contract.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
AURORA = ROOT / 'external/dusklight/extern/aurora'
PINS = {
    'lib/gx/regs.cpp': 'f2f005b4e7937db643feb21d8d3279114ac01f87bfcdedf65c83bbbf315a15d3',
    'lib/gx/regs.hpp': 'cf415430db58a8e9a5da89e2d85bc04ea213f234421f0e70158c05790888b073',
    'lib/gx/command_processor.cpp': 'ff3eeb1c06562c0cbbeb5cbad0daa45aeb4370733dd073ccd34f10f6303ba5ab',
    'lib/gx/gx.hpp': '7cc8b98806ebabd340f66926940a126e6b97f0c42d7f8b9c825c094a7ee8ac62',
    'lib/internal.hpp': '0ca805251b1b7d6d513834cd2c46ecc1bfc411588fad130babf71b3448439c94',
    'lib/gfx/texture.hpp': '8ba4b22ab4a4ba1db58f77a7c0bd04994624ae63ae58c12d3b545bf30083f9fc',
    'lib/gfx/types.hpp': '462bcdfc42c06939d46606607d99b227dfb93ea66e0ef63947510b48fd8f96d3',
    'lib/webgpu/gpu.hpp': '5b3aaee8ed95e86b52eb1b5c7eaa7dce67495257c8ef0a255a84059e2a302182',
    'include/aurora/gfx.hpp': '20575915b089845d6927d3f27d220119be8364cdf8373146c413a68bb014816f',
    'include/aurora/math.hpp': '981a58838bf952614788f8f01e10c48a92e8cbe171f4c9bea2f41a1fab2fe9ff',
}

def digest(data):
    return hashlib.sha256(data).hexdigest()

def function(source, signature):
    start = source.index(signature)
    # These production functions end with a column-zero closing brace. This
    # avoids interpreting braces in diagnostic strings/comments as C++ syntax.
    end = source.index('\n}', start) + 2
    return source[start:end]

def declaration(source, signature):
    return function(source, signature) + ';'

def no_includes(source):
    return re.sub(r'^\s*#(?:include|pragma once)[^\n]*\n', '', source, flags=re.M)

# Explicit semantic inventory. Padding names are intentionally absent. Every
# input declaring these members is hash pinned above; changes require re-audit.
FIELDS = {
 'TevOp': 'op bias scale outReg clamp',
 'TevStage': 'colorPass alphaPass colorOp alphaOp kcSel kaSel texCoordId texMapId channelId tevSwapRas tevSwapTex indTexStage indTexFormat indTexBiasSel indTexAlphaSel indTexMtxId indTexWrapS indTexWrapT indTexUseOrigLOD indTexAddPrev',
 'IndStage': 'texCoordId texMapId scaleS scaleT',
 'ColorChannelConfig': 'matSrc ambSrc diffFn attnFn lightingEnabled',
 'ColorChannelState': 'matColor ambColor lightMask',
 'TcgConfig': 'type src mtx postMtx normalize embossSrc',
 'FogState': 'type a b c color rangeK rangeCenter rangeEnabled fog0Raw fog1Raw fog2Raw',
 'TevSwap': 'red green blue alpha',
 'AlphaCompare': 'comp0 ref0 op comp1 ref1',
 'IndTexMtxInfo': 'mtx scaleExp adjScaleRaw',
 'TexCoordScale': 'scaleS scaleT biasS biasT cylWrapS cylWrapT lineOffset pointOffset',
 'VtxAttrFmt': 'cnt type frac',
 'VtxFmt': 'attrs',
 'PnMtx': 'pos nrm',
 'Light': 'pos dir color cosAtt distAtt',
 'AttrArray': 'data size stride le cachedRange',
 'GXState': 'pnMtx currentPnMtx proj projType fog cullMode lineWidth pointSize lineTexOffset pointTexOffset lineHalfAspect blendMode blendFacSrc blendFacDst blendOp depthFunc clearColor clearDepth pixelFmt zFmt zCompLocBeforeTex dstAlpha alphaCompare colorRegs kcolors colorChannelConfig colorChannelState lights tevStages loadedTextures loadedTluts texMtxs ptTexMtxs tcgs texCoordScales vtxDesc vtxFmts tevSwapTable indStages indTexMtxs arrays depthCompare depthUpdate colorUpdate alphaUpdate numChans numIndStages numTevStages numTexGens frontOffset frontScale backOffset backScale clamp viewportPolicy logicalViewport renderViewport logicalScissor renderScissor texCopySrc texCopyFmt texCopyDstWidth texCopyDstHeight texCopyDstWide texCopyDest lastVtxSize lastVtxFmt dirty bpRegCache bpRegValid cpRegCache cpRegValid xfRegCache xfRegValid',
}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--patch', type=Path, default=ROOT/'patches/aurora/0003-vita-fast-register-decode.patch')
    parser.add_argument('--pristine-self-test', action='store_true', help='fixture plumbing only; not candidate evidence')
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    original = {}
    for name, pin in PINS.items():
        source = subprocess.check_output(['git', '-C', str(AURORA), 'show', 'HEAD:'+name], text=True)
        if digest(source.encode()) != pin:
            raise SystemExit('Unexpected pinned Aurora '+name+'; re-audit the fixture')
        original[name] = source
    manifest = {'aurora_head': subprocess.check_output(['git','-C',str(AURORA),'rev-parse','HEAD'],text=True).strip(), 'normalized_source_sha256': PINS, 'patches': [], 'plumbing_only': args.pristine_self_test}
    # Compile the real pinned include bytes, rather than assuming a clean
    # dependency worktree. The archive contains headers only, with no execution.
    archive = subprocess.check_output(['git','-C',str(AURORA),'archive','HEAD','include/dolphin'])
    manifest['dolphin_header_archive_sha256'] = digest(archive)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
        for item in tree.getmembers():
            if not item.isfile():
                continue
            target = out/item.name
            target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes(tree.extractfile(item).read())
    (out/'include/aurora').mkdir(parents=True,exist_ok=True)
    (out/'include/aurora/math.hpp').write_text(original['include/aurora/math.hpp'])
    with tempfile.TemporaryDirectory(prefix='twv-gx-oracle-') as temp:
        temp = Path(temp)
        for name, source in original.items():
            target = temp/name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(source)
        patches = [] if args.pristine_self_test else list(dict.fromkeys(
            [*(p.resolve() for p in sorted((ROOT/'patches/aurora').glob('*.patch'))), args.patch.resolve()]))
        for patch in patches:
            # Existing patches only target files present in the fixture when
            # relevant; copy the pristine target files needed by each patch.
            for name in re.findall(r'^--- a/([^\n\r]+)', patch.read_text(), re.M):
                target = temp/name
                if not target.exists():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(subprocess.check_output(['git','-C',str(AURORA),'show','HEAD:'+name]))
            subprocess.run(['patch','--batch','--fuzz=0','-s','-p1','-i',str(patch)], cwd=temp, check=True)
            manifest['patches'].append({'path':str(patch.relative_to(ROOT)), 'sha256':digest(patch.read_bytes())})
        regs = (temp/'lib/gx/regs.cpp').read_text()
        cp = (temp/'lib/gx/command_processor.cpp').read_text()
        header = (temp/'lib/gx/regs.hpp').read_text()
        # Real math and Dolphin types/enums are included directly from the
        # pinned tree. Register-owned types are extracted verbatim below.
        gx = original['lib/gx/gx.hpp']
        state = gx[gx.index('namespace GX {'):gx.index('extern GXState g_gxState;')]
        state = state.replace('extern wgpu::BindGroup g_emptyTextureBindGroup;', '')
        start = state.index('  struct CopyTextureRef {')
        end = state.index('  // Decoded state', start)
        state = state[:start]+state[end:]
        for line in ('  absl::flat_hash_map<const void*, CopyTextureRef> copyTextures;\n',
                     '  absl::flat_hash_map<CopyTextureKey, CopyTextureRef> copyTextureCache;\n',
                     '  std::array<gfx::TextureBind, MaxTextures> textures;\n'):
            assert line in state
            state = state.replace(line, '')
        state += 'extern GXState g_gxState; extern GXState g_gxStateRef;\n}\n'
        texture = original['lib/gfx/texture.hpp']
        objects = texture[texture.index('struct GXTexObj_ {'):texture.index('namespace aurora::gfx {\nstruct TextureBind')]
        gfx = 'namespace aurora::gfx {\n'+declaration(original['lib/webgpu/gpu.hpp'],'struct Viewport {')+'\n'+declaration(original['lib/gfx/types.hpp'],'struct ClipRect {')+'\n'+declaration(original['include/aurora/gfx.hpp'],'struct Range {')+'\nconstexpr u32 InvalidTextureFormat = -1;\n}\n'
        internal = original['lib/internal.hpp']
        bits = internal[internal.index('#ifndef ALWAYS_INLINE'):internal.index('#define AURORA_ALIGN')]
        reader = 'namespace aurora {\n'+declaration(internal,'class ByteReader {')+'\n}\n'
        (out/'gx_decode_types.inc').write_text(gfx+objects+state+'\n'+bits+'\n'+reader)
        (out/'gx_decode_regs.inc').write_text(no_includes(header)+'\n'+no_includes(regs))
        # A separate baseline is the pristine source, independent of REF guard
        # coverage in the patch. Candidate REF is tested as an additional mode.
        (out/'gx_decode_regs_pristine.inc').write_text(no_includes(original['lib/gx/regs.hpp'])+'\n'+no_includes(original['lib/gx/regs.cpp']))
        constants = original['lib/gx/command_processor.cpp']
        constants = constants[constants.index('// GX FIFO opcodes'):constants.index('struct FogRangeLutKey')]
        aurora = function(cp,'void handle_aurora(ByteReader& reader) noexcept {')
        # Keep the whole process family, including compile guards and helper
        # fallback, rather than reconstructing a tested parser.
        start = cp.index('ProcessResult process(') if args.pristine_self_test else cp.index('static bool process_command(')
        end = cp.index('[[noreturn]] static void handle_draw_overrun', start)
        family = cp[start:end]
        pristine = function(original['lib/gx/command_processor.cpp'],'ProcessResult process(')
        (out/'gx_decode_process.inc').write_text('namespace aurora::gx::fifo {\n'+constants+'\n'+aurora+'\n'+family+'\n}\n')
        (out/'gx_decode_process_pristine.inc').write_text('namespace aurora::gx::fifo {\n'+constants+'\n'+function(original['lib/gx/command_processor.cpp'],'void handle_aurora(ByteReader& reader) noexcept {')+'\n'+pristine+'\n}\n')
        visitors = []
        for name, fields in FIELDS.items():
            visitors.append('template<class F> void semantic(const aurora::gx::'+name+'& x, F&& f) {\n'+''.join(' f("'+field+'", x.'+field+');\n' for field in fields.split())+'}\n')
        (out/'gx_decode_semantics.inc').write_text(''.join(visitors))
        manifest['compiled_source_sha256'] = {'regs.cpp':digest(regs.encode()),'regs.hpp':digest(header.encode()),'command_processor.cpp':digest(cp.encode())}
    (out/'gx_decode_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print('GX fixture: real regs.cpp + process family + ByteReader; manifest:',out/'gx_decode_manifest.json')

if __name__ == '__main__':
    main()
