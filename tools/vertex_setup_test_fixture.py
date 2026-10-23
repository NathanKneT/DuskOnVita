#!/usr/bin/env python3
"""Compile pinned V59 and current real vertex builders for deferred-setup QA.

Only a logical plan_fetch call counter is injected. Source-bearing decoder,
stream writer, source span, address resolver and oracle bodies must be identical.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
BASE='24e9250c5413d4c48a5b50d3fe4b04b8c001f26a'
PATH='platform/vita/gfx_vgl/vertex.cpp'
PIN='3043f0802797196f65e9ee086c8dce9503ac534af3027fd595fcf65d04fa52cd'
AURORA=ROOT/'external/dusklight/extern/aurora'

def sha(source):return hashlib.sha256(source.encode()).hexdigest()
def function(source,signature,indent=''):
 start=source.index(signature);return source[start:source.index('\n'+indent+'}',start)+len(indent)+2]
def struct(source,name):
 start=source.index('struct '+name+' {');return source[start:source.index('\n};',start)+3]
def pristine(name):return subprocess.check_output(['git','-C',str(AURORA),'show','HEAD:'+name],text=True)

def extract(source):
 parts=[source[source.index('struct V3 {'):source.index('inline float dot3(')]]
 parts.append(source[source.index('inline uint16_t rd16('):source.index('struct Decoded {')])
 start=source.index('struct Decoded {');end=source.index('  // GX lighting_func',start);parts.append(source[start:end])
 start=source.index('  unsigned source_spans(');end=source.index('  const aurora::gx::ShaderConfig& cfg;',start);parts.append(source[start:end])
 start=end;end=source.index('};\n} // namespace',start)+2
 fields=source[start:end]
 # CPU texgen math is not a substitute decoder: accidental use aborts.
 parts.append(fields[:-2]+'\npublic:\n V3 texgen(const Decoded&,V3,V3,unsigned) const {std::abort();}\n};')
 stage='\n'.join(parts)
 needle='inline PlannedFetch plan_fetch(const AttrConfig& a,bool le,int n) {'
 assert stage.count(needle)==1
 stage=stage.replace(needle,needle+'\n  ++planCalls;',1)
 start=source.index('GeometryCache geometryCache;');end=source.index('\nstruct V3 {',start)
 globals_=source[start:end]
 build=function(source,'void* build_gpu_vertices(')
 dependencies=function(source,'void configure_gpu_cache_dependencies(')
 selector=function(source,'extern "C" int twv_vita_lazy_vertex_plan_select(') if 'twv_vita_lazy_vertex_plan_select' in source else ''
 return 'unsigned planCalls=0;\n'+stage+'\n'+globals_+'\n'+selector+'\n'+build+'\n'+dependencies+'\n'

def main():
 p=argparse.ArgumentParser();p.add_argument('--output-dir',type=Path,required=True);a=p.parse_args();a.output_dir.mkdir(parents=True,exist_ok=True)
 original=(ROOT/'tests/fixtures/baseline'/BASE[:7]/PATH).read_text()
 if sha(original)!=PIN:raise SystemExit('Unexpected V59 vertex source; re-audit fixture')
 current=(ROOT/PATH).read_text()
 unchanged={}
 for signature in ('inline uint16_t rd16(','inline uint32_t rd24(','inline uint32_t rd32(',
                   'inline void fetch(','inline PlannedFetch plan_fetch(','inline V4 fetch_color(',
                   'template<int Type,int N,bool Little> void fetch_specialized(',
                   'template<int Type,bool Little> inline PlannedFetch::Function select_fetch_count(',
                   '  void decode_reference(','  void decode(','  bool can_pack_raw_streams(',
                   '  bool pack_raw_streams(','  bool validate_raw_streams(',
                   '  unsigned source_spans(','  uint32_t indexed_array_mask(',
                   '  void need_texgen(','  const uint8_t* address('):
  indent='  ' if signature.startswith('  ') else ''
  left=function(original,signature,indent);right=function(current,signature,indent)
  if left!=right:raise SystemExit('Source-bearing function changed: '+signature)
  unchanged[signature]=sha(left)
 # The moved plan selector must be the original selector for every format,
 # not merely for the finite formats exercised by the integration cases.
 marker='if(attr>=GX_VA_POS && attr!=GX_VA_CLR0 && attr!=GX_VA_CLR1) {'
 old_start=original.index(marker,original.index('  void plan()'))
 old_end=original.index('\n      }',old_start)+8
 new_start=current.index(marker,current.index('  void prepare_fetch_field('))
 new_end=current.index('\n    }',new_start)+6
 normalize=lambda text:re.sub(r'\s+','',text.replace('activeCount','field'))
 if normalize(original[old_start:old_end])!=normalize(current[new_start:new_end]):raise SystemExit('Moved fetch-plan selection changed')
 gx=pristine('lib/gx/gx.hpp');pipe=pristine('lib/gx/pipeline.hpp')
 if sha(gx)!='7cc8b98806ebabd340f66926940a126e6b97f0c42d7f8b9c825c094a7ee8ac62':raise SystemExit('GX declarations changed; re-audit fixture')
 constants=gx[gx.index('constexpr u32 MaxTextures'):gx.index('extern wgpu::BindGroup')]
 tev=gx[gx.index('template <typename Arg, Arg Default>'):gx.index('// For uniform generation')]
 types=constants+tev+'\n'+'\n'.join(struct(gx,n) for n in ('TcgConfig','TevSwap','AlphaCompare','AttrConfig','ShaderConfig','ShaderInfo','GXBindGroups'))
 types+='\nconstexpr uint32_t GXPipelineConfigVersion = 13;\n'+struct(pipe,'PipelineConfig')+'\n'+struct(pipe,'DrawData')
 gfx=pristine('include/aurora/gfx.hpp')
 types='namespace aurora::gfx {\nusing PipelineRef=uint64_t;using BindGroupRef=uint64_t;\n'+struct(gfx,'Range')+'\n}\nnamespace aurora::gx {\n'+types+'\n}\n'
 internal=(ROOT/'platform/vita/gfx_vgl/internal.hpp').read_text()
 types+='namespace twv::vgl {\n'+struct(internal,'Pipeline')+'\n}\n'
 (a.output_dir/'vertex_setup_types.inc').write_text(types)
 for name,source in (('original',original),('candidate',current)):
  (a.output_dir/('vertex_setup_'+name+'.inc')).write_text(extract(source))
 manifest={'base':BASE,'original_sha256':PIN,'candidate_sha256':sha(current),'unchanged_source_bodies':unchanged,
           'gx_types_sha256':sha(gx),'pipeline_types_sha256':sha(pipe),
           'included_header_sha256':{name:sha((ROOT/name).read_text()) for name in (
             'platform/vita/gfx_vgl/attribute_stream.hpp','platform/vita/gfx_vgl/geometry_cache.hpp',
             'platform/vita/gfx_vgl/counter_batch.hpp','external/dusklight/extern/aurora/include/aurora/math.hpp',
             'external/dusklight/extern/aurora/include/dolphin/gx/GXEnum.h')},
           'instrumentation':'one ++planCalls at unchanged real plan_fetch entry; no source-read substitution',
           'host_seams':'allocation, memory class, resident provider, hash collisions, counters/telemetry; CPU texgen aborts'}
 (a.output_dir/'vertex_setup_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
 print('vertex setup fixture: pinned original/current full builders; unchanged source-bearing bodies verified')

if __name__=='__main__':main()
