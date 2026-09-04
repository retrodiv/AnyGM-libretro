#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Build and verify the embedded notice table from the repository texts."""

from __future__ import annotations

import sys
from pathlib import Path

OUTPUT_PATH = Path("src/generated/anygm_third_party_notices.h")
FIXED_INPUTS = (Path("NOTICE"), Path("THIRD_PARTY_NOTICES.md"))
LICENSES_DIR = Path("LICENSES")
BANNER_RULE = "=" * 72


def collect_inputs() -> list[Path]:
    if not LICENSES_DIR.is_dir() or LICENSES_DIR.is_symlink():
        raise SystemExit("notices: LICENSES/ is missing")
    license_paths = sorted(LICENSES_DIR.rglob("*"))
    if any(path.is_symlink() for path in license_paths):
        raise SystemExit("notices: LICENSES/ contains a symbolic link")
    licenses = [path for path in license_paths if path.is_file()]
    if not licenses:
        raise SystemExit("notices: LICENSES/ holds no license texts")
    inputs = [*FIXED_INPUTS, *licenses]
    for path in inputs:
        if not path.is_file() or path.is_symlink():
            raise SystemExit(f"notices: required input {path} is missing or linked")
    return inputs


def build_payload(inputs: list[Path]) -> bytes:
    sections = []
    for path in inputs:
        data = path.read_bytes()
        if not data.strip():
            raise SystemExit(f"notices: {path} is empty")
        if not data.endswith(b"\n"):
            data += b"\n"
        banner = f"{BANNER_RULE}\n{path.as_posix()}\n{BANNER_RULE}\n".encode()
        sections.append(banner + data)
    return b"\n".join(sections) + b"\0"


def render_header(inputs: list[Path], payload: bytes) -> str:
    lines = [
        "/* SPDX-License-Identifier: MIT",
        " * Copyright (c) 2026 retrodiv <retrodiv@proton.me>",
        " * The wrapper is first-party MIT code; embedded notice text retains",
        " * the terms and attribution shown in its source files.",
        " * Inputs, concatenated in this order with a filename banner:",
    ]
    for path in inputs:
        lines.append(f" *   {path.as_posix()}")
    lines += [
        " * Regenerate: python3 tests/architecture/check_notices.py generate",
        " * See THIRD_PARTY_NOTICES.md and docs/PROVENANCE.md.",
        " */",
        "#ifndef ANYGM_THIRD_PARTY_NOTICES_H",
        "#define ANYGM_THIRD_PARTY_NOTICES_H",
        "",
        "#include <stdint.h>",
        "",
        f"/* {len(payload)} bytes including the terminating NUL. */",
        "static const uint8_t anygm_third_party_notices_data[]={",
    ]
    for at in range(0, len(payload), 16):
        chunk = payload[at:at + 16]
        lines.append("  " + ",".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines += ["};", "", "#endif", ""]
    return "\n".join(lines)


def main() -> int:
    mode = sys.argv[1] if len(sys.argv) > 1 else "check"
    if mode not in {"generate", "check"}:
        print(f"unknown mode: {mode}", file=sys.stderr)
        return 2
    inputs = collect_inputs()
    payload = build_payload(inputs)
    rendered = render_header(inputs, payload)
    if mode == "generate":
        OUTPUT_PATH.write_text(rendered, newline="\n")
        print(f"notices: wrote {OUTPUT_PATH} ({len(inputs)} inputs, {len(payload)} bytes)")
        return 0
    if not OUTPUT_PATH.is_file():
        print(f"notices: {OUTPUT_PATH} is missing; run generate", file=sys.stderr)
        return 1
    if OUTPUT_PATH.read_text() != rendered:
        print(
            f"notices: {OUTPUT_PATH} does not match its inputs; run "
            "python3 tests/architecture/check_notices.py generate",
            file=sys.stderr,
        )
        return 1
    print(f"notices: ok ({len(inputs)} inputs, {len(payload)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
