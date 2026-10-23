#!/usr/bin/env python3
"""Extract actual SDK classifier/report and shutdown cache reset for host mocks."""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from vgl_backend_source import exported, function  # noqa: E402


def routine(text: str, name: str) -> str:
    # Complete original body, including native SDK tests and all failure handling,
    # under its exported name (the harness calls twv_vgl_*).
    return exported(function(text, name)).rstrip() + '\n'


def extract(source: Path, output: Path) -> None:
    text = source.read_text(encoding="utf-8")
    shutdown = routine(text, "shutdown")
    reset = re.search(r'vertexWriteClassCache\.clear\(\);\s*\+\+vertexWriteOwner;', shutdown)
    if reset is None:
        raise ValueError("Production shutdown no longer resets classifier identity; review test")
    output.write_text(
        '// Generated from actual gpu_api.cpp; never edit this fixture.\n'
        + routine(text, "vertex_write_class")
        + routine(text, "vertex_write_class_report")
        + 'static void extracted_shutdown_cache_reset(){' + reset.group() + '}\n',
        encoding="utf-8",
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    extract(args.source, args.output)
