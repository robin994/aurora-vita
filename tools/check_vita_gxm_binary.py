#!/usr/bin/env python3
"""Fail if a native GXM ELF links a GL renderer or lacks native drawing symbols."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path, help="Unstripped native Vita ELF, not SELF/VPK")
    parser.add_argument("--map", dest="link_map", type=Path, required=True)
    parser.add_argument("--nm", default="arm-vita-eabi-nm")
    args = parser.parse_args()
    if not args.elf.is_file() or not args.link_map.is_file():
        parser.error("ELF and linker map must both exist")
    try:
        result = subprocess.run([args.nm, "--defined-only", str(args.elf)],
                                check=True, text=True, capture_output=True, timeout=30)
        map_text = args.link_map.read_text(encoding="utf-8", errors="replace")
    except (OSError, subprocess.SubprocessError) as error:
        print(f"FAIL: could not inspect native build: {error}", file=sys.stderr)
        return 2
    symbols = {line.split()[-1].lstrip("_") for line in result.stdout.splitlines() if line.split()}
    forbidden = sorted(s for s in symbols if re.match(r"(?:gl[A-Z]|vgl[A-Za-z]|vita2d_)", s))
    forbidden_archives = re.findall(r"[^\s()]*lib(?:vitaGL|vita2d|GL|GLES\w*)\.(?:a|so)[^\s()]*", map_text, re.I)
    required = {"sceGxmInitialize", "sceGxmDraw", "sceGxmSetVertexProgram", "sceGxmSetFragmentProgram",
                "sceGxmSetVertexStream", "sceGxmSetFragmentTexture", "sceGxmDisplayQueueAddEntry"}
    missing = sorted(required - symbols)
    if forbidden or forbidden_archives or missing:
        print("FAIL: native graphics isolation check", file=sys.stderr)
        for label, values in (("GL symbols", forbidden), ("GL libraries", forbidden_archives), ("Missing GXM symbols", missing)):
            if values:
                print(f"  {label}: {', '.join(values)}", file=sys.stderr)
        return 1
    print(f"PASS: {len(symbols)} defined symbols inspected; native GXM draw/present found; no GL/vgl/vita2d symbols or libraries.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
