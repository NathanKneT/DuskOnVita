"""Exercise the actual patched retail shadow body and actual backend C controls."""
from pathlib import Path
import shutil
import subprocess
import sys

repo,out=map(Path,sys.argv[1:])
relative='src/d/d_drawlist.cpp'
target=out/'shadow-fixture'
dest=target/relative
dest.parent.mkdir(parents=True)
shutil.copyfile(repo/'external/dusklight'/relative,dest)
original=dest.read_text()
subprocess.run(['patch','--batch','-s','-p1','-d',str(target),'-i',str(repo/'patches/dusklight/0013-vita-unsupported-shadow-image-skip.patch')],check=True)
patched=dest.read_text()

def function(source,signature):
    start=source.index(signature);brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

signature='void dDlst_shadowReal_c::imageDraw('
body=function(patched,signature)
begin=body.index('#if TARGET_VITA');end=body.index('#endif',begin)+len('#endif\n')
assert body[:begin]+body[end:]==function(original,signature)
# This patch must not touch outer FBO/copy/state flow, visible projections or
# bloom/monochrome/minimap. The whole rest of the original TU is byte-identical.
assert patched.replace(body,function(original,signature)).replace(
    '#if TARGET_VITA\nextern "C" int twv_vita_shadow_image_skip();\n'
    'extern "C" void twv_vita_shadow_image_skipped(unsigned models, unsigned shapes);\n#endif\n','')==original
(out/'shadow_image_body.inc').write_text(body+'\n'+function(original,signature).replace('::imageDraw(','::legacyImageDraw(',1))
backend=(repo/'platform/vita/gfx_vgl/backend.cpp').read_text()
parts=[]
for signature in ['extern "C" int twv_vita_shadow_image_skip_select(',
                  'extern "C" int twv_vita_shadow_image_skip(',
                  'extern "C" void twv_vita_shadow_image_skipped(']:
    parts.append(function(backend,signature))
assert 'if(offscreen && !OffscreenDrawSupported) {count("unsupported_offscreen_draw");return;}' in backend
assert 'constexpr bool OffscreenDrawSupported=false;' in backend
(out/'shadow_image_control.inc').write_text('\n'.join(parts))
