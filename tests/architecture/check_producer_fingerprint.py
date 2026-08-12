#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Keep the cache producer fingerprint tied to the code that produces cached payloads.

A generated payload is only reusable while the code that generated it is unchanged. The producer
fingerprint used to be a hand-written recipe string, so a change to the importer, the packager, or
anything they read left every previously generated payload accepted by its cache marker. Content
then kept running bytes from an earlier revision until the cache directory was deleted by hand.

The fingerprint is now derived from the first-party sources themselves. Vendored third-party code
is excluded because its own provenance is pinned separately, and the generated header is excluded
because it holds the result. Over-invalidation is deliberate: a payload regenerated needlessly costs
one import, while a payload reused wrongly is a correctness failure that is invisible until
behaviour diverges.

  check_producer_fingerprint.py generate   rewrite the generated header
  check_producer_fingerprint.py check      fail when the header does not match the sources
"""
from __future__ import annotations

import hashlib
import sys
from pathlib import Path

HEADER_PATH = Path("src/generated/anygm_producer_fingerprint.h")
SOURCE_ROOT = Path("src")
EXCLUDED_DIRECTORIES = ("src/third_party",)
SUFFIXES = (".c", ".cpp", ".h")


def hashed_sources(root: Path) -> list[tuple[str, str]]:
    rows: list[tuple[str, str]] = []
    for path in sorted((root / SOURCE_ROOT).rglob("*")):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        if relative == HEADER_PATH.as_posix():
            continue
        if any(relative.startswith(prefix) for prefix in EXCLUDED_DIRECTORIES):
            continue
        rows.append((relative, hashlib.sha256(path.read_bytes()).hexdigest()))
    return rows


def fingerprint(rows: list[tuple[str, str]]) -> int:
    digest = hashlib.sha256()
    for relative, source_hash in rows:
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(source_hash.encode("ascii"))
        digest.update(b"\n")
    return int.from_bytes(digest.digest()[:8], "little")


def header_text(value: int, count: int) -> str:
    return (
        "/* SPDX-License-Identifier: MIT\n"
        " * Copyright (c) 2026 retrodiv <retrodiv@proton.me>\n"
        " *\n"
        " * Generated data: run make producer-fingerprint-check after changing any first-party\n"
        " * source. The value identifies the exact code that produces cached payloads, so a cache\n"
        " * written by another revision is regenerated instead of trusted.\n"
        " */\n"
        "#ifndef ANYGM_PRODUCER_FINGERPRINT_H\n"
        "#define ANYGM_PRODUCER_FINGERPRINT_H\n"
        "\n"
        "#include <stdint.h>\n"
        "\n"
        f"/* Derived from {count} first-party sources. */\n"
        f"#define ANYGM_PRODUCER_FINGERPRINT UINT64_C(0x{value:016x})\n"
        "\n"
        "#endif\n"
    )


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "check"
    if mode not in {"generate", "check"}:
        print(f"unknown mode: {mode}", file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parents[2]
    rows = hashed_sources(root)
    if not rows:
        print("no first-party sources were found", file=sys.stderr)
        return 2
    expected = header_text(fingerprint(rows), len(rows))
    target = root / HEADER_PATH
    if mode == "generate":
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(expected, encoding="utf-8")
        print(f"producer fingerprint: wrote {HEADER_PATH.as_posix()} from {len(rows)} sources")
        return 0
    current = target.read_text(encoding="utf-8") if target.is_file() else ""
    if current != expected:
        print(
            "producer fingerprint is stale; run "
            "python3 tests/architecture/check_producer_fingerprint.py generate",
            file=sys.stderr,
        )
        return 1
    print(f"producer fingerprint: ok ({len(rows)} sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
