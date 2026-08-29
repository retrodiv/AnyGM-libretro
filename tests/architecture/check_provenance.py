#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Prove that every digest quoted in docs/PROVENANCE.md is the digest of the file it names.

A provenance record nothing enforces goes stale the first time a generator runs, and a stale
digest is worse than none: it reads as evidence and is not. Every ``PROVENANCE-CHECK:`` line in
that document names a path and is followed by the digest the document claims for it.
"""
from __future__ import annotations

import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCUMENT = ROOT / "docs" / "PROVENANCE.md"
DIRECTIVE = re.compile(r"^<!--\s*PROVENANCE-CHECK:\s*(\S+)\s*=\s*([0-9a-f]{64})\s*-->$", re.M)


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    text = DOCUMENT.read_text(encoding="utf-8")
    entries = DIRECTIVE.findall(text)
    if not entries:
        print("provenance: no PROVENANCE-CHECK directives found", file=sys.stderr)
        return 1
    failures = []
    for relative, claimed in entries:
        path = ROOT / relative
        if not path.is_file():
            failures.append(f"{relative}: named by PROVENANCE.md and not present")
            continue
        actual = digest(path)
        if actual != claimed:
            failures.append(f"{relative}: documented {claimed}, actual {actual}")
    if len(sys.argv) > 1 and sys.argv[1] == "generate":
        for relative, claimed in entries:
            path = ROOT / relative
            if path.is_file():
                text = text.replace(
                    f"<!-- PROVENANCE-CHECK: {relative} = {claimed} -->",
                    f"<!-- PROVENANCE-CHECK: {relative} = {digest(path)} -->")
        DOCUMENT.write_text(text, encoding="utf-8")
        print(f"provenance: rewrote {len(entries)} digests")
        return 0
    if failures:
        print("provenance digests are stale:", file=sys.stderr)
        for line in failures:
            print(f"  {line}", file=sys.stderr)
        print("run python3 tests/architecture/check_provenance.py generate after "
              "regenerating the artifact, and say in the commit why the input changed",
              file=sys.stderr)
        return 1
    print(f"provenance: ok ({len(entries)} digests)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
