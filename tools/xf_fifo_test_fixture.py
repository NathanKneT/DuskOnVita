"""Extract real upstream/patched matrix functions and FIFO stores for host QA."""
from pathlib import Path
import shutil
import subprocess
import sys

repo,out=map(Path,sys.argv[1:])
def function(source,signature):
    start=source.index(signature);brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

game='libs/JSystem/src/J3DGraphBase/J3DGD.cpp'
aurora='lib/dolphin/gx/GXVert.cpp'
for module,rel,patch in [('dusklight',game,'0011-vita-j3d-matrix-blocks.patch'),
                         ('aurora',aurora,'0002-vita-matrix-block-api.patch')]:
    root=repo/'external/dusklight'
    if module=='aurora':root=root/'extern/aurora'
    target=out/module;target.mkdir()
    paths=[rel]+(['include/dolphin/gx/GXVert.h'] if module=='aurora' else [])
    for name in paths:
        dest=target/name;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/name,dest)
    subprocess.run(['patch','--batch','-s','-p1','-d',str(target),'-i',str(repo/'patches'/module/patch)],check=True)

source=(out/'dusklight'/game).read_text()
names=['J3DFifoLoadPosMtxImm','J3DFifoLoadNrmMtxImm','J3DFifoLoadNrmMtxImm3x3',
       'J3DFifoLoadNrmMtxToTexMtx','J3DFifoLoadNrmMtxToTexMtx3x3']
(out/'matrix_functions.inc').write_text('\n'.join(function(source,'void '+name+'(') for name in names))
source=(out/'aurora'/aurora).read_text()
(out/'try_block.inc').write_text(function(source,'bool GXTryCmdBlock('))
source=(repo/'external/dusklight/extern/aurora/lib/gx/fifo.hpp').read_text()
start=source.index('inline void write_data(');end=source.index('inline void write_f32(',start)
(out/'fifo_stores.inc').write_text(source[start:end])
