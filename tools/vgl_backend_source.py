"""Extract vitaGL backend functions from gpu_api.cpp for the host harnesses.

Since G0 the vitaGL entry points are functions of the backend table (vgl_X) and
gpu_dispatch.cpp exports twv_vgl_X. The recording-GL harnesses still call the
exported names, so an extracted body gets its exported signature back.
"""
from __future__ import annotations

import re


def exported(text: str) -> str:
    """vgl_X( at the start of a top-level definition line -> extern "C" twv_vgl_X(."""
    return re.sub(r'^([A-Za-z_][\w:<>\* ]*?)\bvgl_(\w+)\(', r'extern "C" \1twv_vgl_\2(', text, flags=re.M)


def function(source: str, name: str) -> str:
    """The complete top-level definition of vgl_<name>, by brace matching."""
    match = re.search(r'^[A-Za-z_][^\n]*\bvgl_' + re.escape(name) + r'\s*\(', source, flags=re.M)
    if match is None:
        raise SystemExit('Missing production function: vgl_' + name)
    body = source.index('{', match.end())
    depth, end = 1, body + 1
    while depth:
        if source[end] == '{':
            depth += 1
        elif source[end] == '}':
            depth -= 1
        end += 1
    return source[match.start():end]
