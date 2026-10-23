#!/usr/bin/env python3
"""Extract the unchanged pinned driver routine for the asynchronous mip oracle."""
import argparse
import hashlib
from pathlib import Path

PINNED_SHA256='b51e8644e00fe8fc77e1ccf169e02538b912c2872958d9cdd8b5415431a3766c'
def extract(source: Path,output: Path):
    data=source.read_bytes()
    if hashlib.sha256(data).hexdigest()!=PINNED_SHA256:
        raise ValueError('Pinned vitaGL gpu_utils.c changed; review the source fixture before updating its hash')
    text=data.decode('utf-8');start=text.index('void gpu_alloc_mipmaps(')
    end=text.index('\nvoid gpu_free_palette(',start)
    output.write_text(text[start:end],encoding='utf-8')
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();extract(args.source,args.output)
