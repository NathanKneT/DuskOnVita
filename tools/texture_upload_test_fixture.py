"""Extract the production upload/factory lifecycle for recording callback tests."""
from pathlib import Path
import sys
repo,out=map(Path,sys.argv[1:])
source=(repo/'platform/vita/gfx_vgl/textures.cpp').read_text()
start=source.index('void upload(Texture&');end=source.index('\n}\n}\nextern',start)+2
parts=['namespace twv::vgl {\n'+source[start:end]+'\n}\n']
start=source.index('TextureHandle new_static_texture_2d');end=source.index('\nvoid write_texture(',start)
parts.append('namespace aurora::gfx {\n'+source[start:end]+'\n}\n')
(out/'texture_upload_functions.inc').write_text('\n'.join(parts))
