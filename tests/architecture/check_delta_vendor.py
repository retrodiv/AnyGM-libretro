#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Audit the decoder import inventory and single first-party interface owner."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "src/third_party/delta-import.json"
COMPONENTS = ("xdelta3", "liblzma")
MODIFIED = {"xdelta3/xdelta3.h", "xdelta3/xdelta3-fgk.h",
            "xdelta3/xdelta3-djw.h", "xdelta3/xdelta3-lzma.h"}
OWNED = {"anygm_config.h", "README.anygm.md"}


def imported_files() -> dict[str, tuple[str, str]]:
    found = {}
    for component in COMPONENTS:
        folder = ROOT / "src/third_party" / component
        for path in sorted(folder.rglob("*")):
            if not path.is_file() or path.name in OWNED:
                continue
            relative = path.relative_to(folder).as_posix()
            if component == "xdelta3":
                original = "xdelta3/" + relative
            elif relative.startswith("common_support/"):
                original = "src/common/" + relative.removeprefix("common_support/")
            else:
                original = "src/liblzma/" + relative
            found[path.relative_to(ROOT).as_posix()] = (component, original)
    found["LICENSES/xdelta3.txt"] = ("xdelta3", "xdelta3/LICENSE")
    found["LICENSES/liblzma.txt"] = ("liblzma", "COPYING.0BSD")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream-xdelta", type=Path)
    parser.add_argument("--upstream-xz", type=Path)
    args = parser.parse_args()
    inputs = imported_files()
    if bool(args.upstream_xdelta) != bool(args.upstream_xz):
        parser.error("supply both upstream roots or neither")
    roots = {"xdelta3": args.upstream_xdelta, "liblzma": args.upstream_xz}
    recorded = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if (recorded.get("schema") != 2 or recorded.get("spdx") != "MIT" or
            recorded.get("copyright") != "Copyright (c) 2026 retrodiv <retrodiv@proton.me>"):
        raise SystemExit("decoder import: manifest declaration changed")
    if set(recorded["files"]) != set(inputs):
        raise SystemExit("decoder import: retained file set differs from the manifest")
    for relative, (component, original) in inputs.items():
        row = recorded["files"][relative]
        expected_modified = relative.removeprefix("src/third_party/") in MODIFIED
        if row != {"component": component, "upstream_path": original,
                   "modified": expected_modified}:
            raise SystemExit(f"decoder import: inventory mismatch: {relative}")
        if args.upstream_xdelta:
            upstream = roots[component] / original
            if not upstream.is_file():
                raise SystemExit(f"decoder import: upstream file missing: {original}")
            differs = upstream.read_bytes() != (ROOT / relative).read_bytes()
            if differs != expected_modified:
                raise SystemExit(f"decoder import: unexpected upstream difference: {relative}")
        if expected_modified and b"Modified for AnyGM by retrodiv" not in (ROOT / relative).read_bytes():
            raise SystemExit(f"decoder import: modification notice missing: {relative}")
    raw_api = re.compile(r"\b(?:xd3|lzma)_[a-zA-Z0-9_]+\b")
    for path in (ROOT / "src").rglob("*"):
        if path.suffix not in {".c", ".h", ".cpp"} or "third_party" in path.parts:
            continue
        if path == ROOT / "src/content/container/content_delta.c":
            continue
        if raw_api.search(path.read_text(encoding="utf-8")):
            raise SystemExit(f"decoder import: raw vendor API escaped its owner: {path.relative_to(ROOT)}")
    print(f"decoder import: {len(inputs)} inventoried files; raw API ownership: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
