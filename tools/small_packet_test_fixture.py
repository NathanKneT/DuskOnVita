#!/usr/bin/env python3
"""Extract actual build_gpu_vertices and VertexStage; no cache algorithm copy.
Run after/instead of the existing vertex_stage fixture extraction. The only added
method is an aborting stub for CPU texgen, unreachable in raw-GPU test cases.
"""
import argparse
import hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser()
p.add_argument('--output-dir',type=Path,required=True)
a=p.parse_args()
a.output_dir.mkdir(parents=True,exist_ok=True)
source=(ROOT/'platform/vita/gfx_vgl/vertex.cpp').read_text()
gx=(ROOT/'external/dusklight/extern/aurora/lib/gx/gx.hpp').read_text()
types=[]
for name in ('AttrConfig','ColorChannelConfig','TcgConfig'):
 start=gx.index('struct '+name+' {');end=gx.index('\n};',start)+3;types.append(gx[start:end])
(a.output_dir/'small_packet_types.inc').write_text('\n'.join(types))
start=source.index('struct V3 {');end=source.index('inline float dot3(',start);parts=[source[start:end]]
start=source.index('inline uint16_t rd16(');end=source.index('struct Decoded {',start);parts.append(source[start:end])
start=end;end=source.index('class VertexStage {',start);parts.append(source[start:end])
start=end;end=source.index('  // GX lighting_func',start);parts.append(source[start:end])
start=source.index('  unsigned source_spans(');end=source.index('  uint32_t indexed_array_mask(',start);parts.append(source[start:end])
start=source.index('  const aurora::gx::ShaderConfig& cfg;',end);end=source.index('};\n} // namespace',start)+2
# Keep every production decode/plan/address/writer/oracle method unchanged.
fields=source[start:end]
fields=fields[:-2]+'\npublic:\n  V3 texgen(const Decoded&,V3,V3,unsigned) const {std::abort();}\n};'
parts.append(fields)
(a.output_dir/'small_packet_vertex_stage.inc').write_text('\n'.join(parts))
start=source.index('GeometryCache geometryCache;');end=source.index('\nstruct V3 {',start)
globals_=source[start:end]
start=source.index('void* build_gpu_vertices(');end=source.index('\nvoid geometry_cache_end_frame()',start)
build=source[start:end]
start=source.index('void geometry_cache_clear()');end=source.index('\n',start)
clear=source[start:end]
(a.output_dir/'small_packet_build.inc').write_text('// Actual vertex.cpp SHA256 '+hashlib.sha256(source.encode()).hexdigest()+'\n'+globals_+'\n'+build+'\n'+clear+'\n')
print('Extracted real VertexStage/build_gpu_vertices into',a.output_dir)
