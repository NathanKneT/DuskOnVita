#!/usr/bin/env python3
"""Extract actual SDK classifier/report and shutdown cache reset for host mocks."""
import argparse
import re
from pathlib import Path


def routine(text: str, name: str) -> str:
    match = re.search(r'extern "C"[^\n]*\b' + re.escape(name) + r'\(', text)
    if match is None:
        raise ValueError(f"Production routine missing: {name}")
    # These functions are adjacent top-level C exports. Retain their complete
    # original body, including native SDK tests and all failure handling.
    end = text.find('\nextern "C"', match.end())
    if end < 0:
        raise ValueError(f"Cannot establish production boundary after {name}")
    return text[match.start():end].rstrip() + '\n'


def extract(source: Path, output: Path) -> None:
    text = source.read_text(encoding="utf-8")
    shutdown = routine(text, "twv_vgl_shutdown")
    reset = re.search(r'vertexWriteClassCache\.clear\(\);\s*\+\+vertexWriteOwner;', shutdown)
    if reset is None:
        raise ValueError("Production shutdown no longer resets classifier identity; review test")
    output.write_text(
        '// Generated from actual gpu_api.cpp; never edit this fixture.\n'
        + routine(text, "twv_vgl_vertex_write_class")
        + routine(text, "twv_vgl_vertex_write_class_report")
        + 'static void extracted_shutdown_cache_reset(){' + reset.group() + '}\n',
        encoding="utf-8",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    extract(args.source, args.output)
