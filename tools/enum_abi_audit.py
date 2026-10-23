#!/usr/bin/env python3
"""Enum-ABI audit for prebuilt SDK headers (docs/ARCHITECTURE.md, "Enum ABI").

arm-vita-eabi defaults to short enums; everything built from source here uses
-fno-short-enums. A prebuilt library (SDL3, built with short enums) and our code
therefore disagree on the size of every enum whose values fit in 1 or 2 bytes.
Passing such an enum by value is safe (AAPCS widens arguments and return values
to a word), but memory shared through a pointer is not. This tool compiles a
header twice, once per enum mode, with all types kept in the debug info, and
diffs the DWARF layouts:

  LAYOUT   size or member offsets differ: the struct must never cross the boundary
  WIDTH    offsets match but an enum member is narrower in the library: our reads
           of that member see 1-3 padding bytes the library may not have written
  ENUMPTR  a function exchanges an enum through a pointer (the library writes or
           reads fewer bytes than our int-sized object)

Functions whose parameters or return values involve an affected type are listed,
and the identifiers are searched for in the given source trees so the report says
which ones the Vita build can actually reach. Writes a text report; exit code 0.
"""
import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile

DIE_RE = re.compile(r"^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: (\d+)(?: \((DW_TAG_\w+)\))?")
ATTR_RE = re.compile(r"^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$")
REF_RE = re.compile(r"<0x([0-9a-f]+)>")
TRANSPARENT = {"DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type", "DW_TAG_atomic_type",
               "DW_TAG_restrict_type"}


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def attr_value(raw):
    # "(indirect string, offset: 0x1234): name" -> "name"
    if raw.startswith("(indirect string") or raw.startswith("(indexed string"):
        return raw.split("): ", 1)[1] if "): " in raw else raw
    return raw.strip()


def parse_dwarf(readelf, obj):
    dies, stack = {}, []
    for line in run([readelf, "--debug-dump=info", "-wi", obj]).splitlines():
        m = DIE_RE.match(line)
        if m:
            depth, off, abbrev, tag = int(m.group(1)), int(m.group(2), 16), m.group(3), m.group(4)
            del stack[depth:]
            if abbrev == "0" or tag is None:
                continue
            die = {"tag": tag, "attrs": {}, "children": [], "off": off}
            dies[off] = die
            if stack:
                stack[-1]["children"].append(die)
            stack.append(die)
            continue
        a = ATTR_RE.match(line)
        if a and stack:
            stack[-1]["attrs"][a.group(1)] = attr_value(a.group(2))
    return dies


def ref(die, name="DW_AT_type"):
    v = die["attrs"].get(name)
    m = REF_RE.search(v) if v else None
    return int(m.group(1), 16) if m else None


def num(v):
    v = v.split()[0] if v else v
    try:
        return int(v, 0)
    except (TypeError, ValueError):
        return None


class Types:
    def __init__(self, dies):
        self.dies = dies

    def strip(self, off):
        seen = 0
        while off is not None and off in self.dies and self.dies[off]["tag"] in TRANSPARENT and seen < 64:
            off, seen = ref(self.dies[off]), seen + 1
        return self.dies.get(off) if off is not None else None

    def size(self, off):
        d = self.strip(off)
        if d is None:
            return None
        s = num(d["attrs"].get("DW_AT_byte_size"))
        if s is not None:
            return s
        if d["tag"] == "DW_TAG_array_type":
            elem, count = self.size(ref(d)), 1
            for sub in d["children"]:
                if sub["tag"] == "DW_TAG_subrange_type":
                    ub = num(sub["attrs"].get("DW_AT_upper_bound"))
                    cnt = num(sub["attrs"].get("DW_AT_count"))
                    count *= cnt if cnt is not None else (ub + 1 if ub is not None else 0)
            return elem * count if elem is not None else None
        return None

    def is_enum(self, off):
        d = self.strip(off)
        while d is not None and d["tag"] == "DW_TAG_array_type":
            d = self.strip(ref(d))
        return d is not None and d["tag"] == "DW_TAG_enumeration_type"

    def name_of(self, die):
        return die["attrs"].get("DW_AT_name")


def member_paths(types, affected):
    """affected struct -> member names that embed it in another aggregate (SDL_Event.key, ...)."""
    out = collections.defaultdict(set)
    for d in types.dies.values():
        if d["tag"] not in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            continue
        for m in d["children"]:
            t = types.strip(ref(m)) if m["tag"] == "DW_TAG_member" else None
            if t is not None and types.name_of(t) in affected and types.name_of(m):
                out[types.name_of(t)].add(types.name_of(m))
    return out


def aggregates(types):
    """name -> (size, [(member, offset, size, is_enum)]) for every defined struct/union."""
    typedef_names = collections.defaultdict(list)
    for d in types.dies.values():
        if d["tag"] == "DW_TAG_typedef" and ref(d) is not None:
            typedef_names[ref(d)].append(types.name_of(d))
    out = {}
    for d in types.dies.values():
        if d["tag"] not in ("DW_TAG_structure_type", "DW_TAG_union_type"):
            continue
        if "DW_AT_declaration" in d["attrs"]:
            continue
        name = types.name_of(d) or (typedef_names.get(d["off"]) or [None])[0]
        if not name:
            continue
        members = []
        for m in d["children"]:
            if m["tag"] != "DW_TAG_member":
                continue
            loc = num(m["attrs"].get("DW_AT_data_member_location"))
            bit = num(m["attrs"].get("DW_AT_data_bit_offset"))
            members.append((types.name_of(m) or "<anon>", loc if loc is not None else ("bit", bit),
                            types.size(ref(m)), types.is_enum(ref(m))))
        out.setdefault(name, (num(d["attrs"].get("DW_AT_byte_size")), members))
    return out


def enum_sizes(types):
    out = {}
    typedef_of = {}
    for d in types.dies.values():
        if d["tag"] == "DW_TAG_typedef":
            t = types.strip(ref(d))
            if t is not None and t["tag"] == "DW_TAG_enumeration_type":
                typedef_of[types.name_of(d)] = t
    for d in types.dies.values():
        if d["tag"] == "DW_TAG_enumeration_type" and types.name_of(d):
            out[types.name_of(d)] = num(d["attrs"].get("DW_AT_byte_size"))
    for name, t in typedef_of.items():
        out.setdefault(name, num(t["attrs"].get("DW_AT_byte_size")))
    return out


def prototypes(pre_text):
    """(name, return text, [param texts]) for every extern function declaration."""
    text = re.sub(r"\s+", " ", re.sub(r"^#.*$", "", pre_text, flags=re.M))
    text = re.sub(r"__attribute__ ?\(\((?:[^()]|\([^()]*\))*\)\)", "", text)
    out = []
    for decl in text.split(";"):
        decl = decl.strip()
        if not decl.startswith("extern ") or "(" not in decl:
            continue
        m = re.match(r"extern (.*?)\b(\w+) ?\((.*)\)\s*$", decl)
        if not m:
            continue
        params = [p.strip() for p in m.group(3).split(",")] if m.group(3).strip() not in ("", "void") else []
        out.append((m.group(2), m.group(1).strip(), params))
    return out


def scan_sources(roots, idents):
    """identifier -> list of files (relative) mentioning it."""
    hits = collections.defaultdict(list)
    if not idents:
        return hits
    pattern = re.compile(r"\b(" + "|".join(sorted(map(re.escape, idents), key=len, reverse=True)) + r")\b")
    for root in roots:
        for base, _, files in os.walk(root):
            for f in files:
                if not f.endswith((".c", ".cpp", ".cc", ".h", ".hpp", ".mm", ".inl")):
                    continue
                path = os.path.join(base, f)
                try:
                    with open(path, encoding="utf-8", errors="replace") as fh:
                        found = set(pattern.findall(fh.read()))
                except OSError:
                    continue
                for ident in found:
                    hits[ident].append(os.path.relpath(path, os.path.dirname(root.rstrip("/"))))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", required=True)
    ap.add_argument("--readelf", required=True)
    ap.add_argument("--header", required=True, help="header(s) to include, comma-separated, e.g. SDL3/SDL.h")
    ap.add_argument("--cxx", action="store_true", help="compile as C++ (--cc is then the C++ driver)")
    ap.add_argument("--prefix", default="SDL_", help="only report types/functions with this prefix")
    ap.add_argument("--cflag", action="append", default=[])
    ap.add_argument("--sources", action="append", default=[], help="source tree to search for uses")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "audit.cpp" if args.cxx else "audit.c")
        with open(src, "w") as fh:
            for header in args.header.split(","):
                fh.write(f"#include <{header}>\n")
        layouts, lib_types = {}, None
        for mode in ("-fshort-enums", "-fno-short-enums"):
            obj = os.path.join(tmp, mode.strip("-") + ".o")
            run([args.cc, "-c", "-g", "-O0", "-fno-eliminate-unused-debug-types", mode, *args.cflag, src, "-o", obj])
            t = Types(parse_dwarf(args.readelf, obj))
            layouts[mode] = (aggregates(t), enum_sizes(t))
            lib_types = lib_types or t
        pre = run([args.cc, "-E", "-fshort-enums", *args.cflag, src])

    (lib_aggr, lib_enums), (our_aggr, our_enums) = layouts["-fshort-enums"], layouts["-fno-short-enums"]
    narrow_enums = {n: s for n, s in lib_enums.items() if n.startswith(args.prefix) and s is not None and s < 4}

    layout, width = {}, {}
    for name in sorted(lib_aggr):
        if not name.startswith(args.prefix) or name not in our_aggr:
            continue
        (ls, lm), (os_, om) = lib_aggr[name], our_aggr[name]
        lmap, omap = {m[0]: m for m in lm}, {m[0]: m for m in om}
        moved = [n for n in lmap if n in omap and lmap[n][1] != omap[n][1]]
        if ls != os_ or moved:
            layout[name] = (ls, os_, moved)
            continue
        narrowed = [(n, lmap[n][2], omap[n][2]) for n in lmap if n in omap and lmap[n][2] != omap[n][2]]
        if narrowed:
            width[name] = narrowed

    affected_types = set(layout) | set(width) | set(narrow_enums)
    enum_ptr, struct_fn = {}, {}
    for fname, ret, params in prototypes(pre):
        if not fname.startswith(args.prefix):
            continue
        texts = [ret] + params
        eptr = [t for t in texts for e in narrow_enums if re.search(rf"\b{re.escape(e)}\b\s*\*", t)]
        if eptr:
            enum_ptr[fname] = eptr
        touched = sorted({ty for t in texts for ty in (set(layout) | set(width))
                          if re.search(rf"\b{re.escape(ty)}\b", t)})
        if touched:
            struct_fn[fname] = touched

    hits = scan_sources(args.sources, set(layout) | set(width) | set(enum_ptr) | set(struct_fn))
    # Union/struct members read without naming the type (event.key.scancode): search
    # "<embedding member>.<narrow field>" too.
    paths = member_paths(lib_types, set(width) | set(layout))
    access = {}
    for name in width:
        for via in sorted(paths.get(name, ())):
            for field, _, _ in width[name]:
                access[f"{via}.{field}"] = name
    access_hits = collections.defaultdict(list)
    if access:
        pattern = re.compile(r"\b(" + "|".join(re.escape(a).replace(r"\.", r"\s*\.\s*") for a in access) + r")\b")
        for root in args.sources:
            for base, _, files in os.walk(root):
                for f in files:
                    if f.endswith((".c", ".cpp", ".cc", ".h", ".hpp", ".mm")):
                        path = os.path.join(base, f)
                        with open(path, encoding="utf-8", errors="replace") as fh:
                            for found in set(pattern.findall(fh.read())):
                                key = re.sub(r"\s+", "", found)
                                access_hits[access.get(key, key)].append(
                                    f"{key} in {os.path.relpath(path, os.path.dirname(root.rstrip('/')))}")

    def used(ident):
        files = sorted(set(hits.get(ident, [])) | set(access_hits.get(ident, [])))
        return ("used in " + ", ".join(files[:6]) + (f" (+{len(files) - 6})" if len(files) > 6 else "")) if files else "unused"

    lines = [f"Enum-ABI audit of <{args.header}> (library: -fshort-enums, ours: -fno-short-enums)", ""]
    lines.append(f"narrow enums in the library: {len(narrow_enums)}; "
                 f"LAYOUT structs: {len(layout)}; WIDTH structs: {len(width)}; "
                 f"ENUMPTR functions: {len(enum_ptr)}; functions touching affected structs: {len(struct_fn)}")
    lines.append(f"searched: {', '.join(args.sources) or '(nothing)'}")
    lines.append("")
    lines.append("## LAYOUT (size or offsets differ; never share)")
    for name, (ls, os_, moved) in layout.items():
        lines.append(f"{name}: library {ls} B, ours {os_} B, moved members: {', '.join(moved) or '-'}; {used(name)}")
    lines.append("")
    lines.append("## WIDTH (same offsets; enum member narrower in the library)")
    for name, narrowed in width.items():
        desc = ", ".join(f"{m} {a}->{b} B" for m, a, b in narrowed)
        lines.append(f"{name}: {desc}; {used(name)}")
    lines.append("")
    lines.append("## ENUMPTR (enum exchanged through a pointer)")
    for fname, texts in sorted(enum_ptr.items()):
        lines.append(f"{fname}: {' | '.join(texts)}; {used(fname)}")
    lines.append("")
    lines.append("## Functions taking or returning LAYOUT/WIDTH structs")
    for fname, tys in sorted(struct_fn.items()):
        lines.append(f"{fname}: {', '.join(tys)}; {used(fname)}")
    lines.append("")
    lines.append("## Narrow enums (bytes in the library; by-value use is safe)")
    lines.append(", ".join(f"{n}={s}" for n, s in sorted(narrow_enums.items())))
    with open(args.out, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"enum_abi_audit: {lines[2]} -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
