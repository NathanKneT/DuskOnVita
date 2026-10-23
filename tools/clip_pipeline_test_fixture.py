"""Extract the actual production cache-hit branch (including allocation and rollback)."""
from pathlib import Path
import sys
repo,out=map(Path,sys.argv[1:])
source=(repo/'platform/vita/gfx_vgl/vertex.cpp').read_text()
start=source.index('    if(cached.data && cached.bytes==outputBytes) {')
brace=source.index('{',start);depth=1;end=brace+1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
(out/'clip_cache_hit.inc').write_text(source[start:end])
assert source.index('cached=geometryCache.lookup(hash,key,staticCert,static_verify());',source.index('void* build_gpu_vertices('))<start
assert 'geometry_cache_validation_failure' in source and 'vertexCullControl.fail();' in source
