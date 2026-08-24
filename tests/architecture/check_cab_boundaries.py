#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Enforce the embedded-Cabinet decompressor and native-execution boundaries.

The intended decoder boundary checks for a self-contained LZX-21 owner.
This historical revision does not yet provide that source file.
The content path must not spawn processes or load dynamic libraries.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def fail(message: str) -> None:
    raise SystemExit(f"Cabinet architecture: {message}")


container = ROOT / "src/content/container"
if not (container / "cab_lzx.c").is_file() or not (container / "cab_lzx.h").is_file():
    fail("first-party CAB LZX decoder is missing")

# Reject an unexpected vendored archive decoder; the intended local decoder
# is required separately above.
third_party = ROOT / "src/third_party"
if third_party.is_dir():
    for entry in third_party.iterdir():
        if entry.is_dir() and re.search(r"cab|archive|zip|lzx|lzma", entry.name, re.I):
            fail(f"an archive/compression decoder was vendored: {entry.name}")

make_text = (ROOT / "Makefile").read_text() + (ROOT / "Makefile.common").read_text()
if re.search(r"(?:^|\s)-l(?:archive|z|bz2|lzma|cab)(?:\s|$)", make_text):
    fail("build links an installed archive or compression library")

# The decompressor is self-contained: neither it nor the extraction owner may enter native
# execution or dynamic loading.
first_party = "\n".join(
    path.read_text(errors="ignore")
    for root in (ROOT / "src/content/container", ROOT / "src/core")
    for path in root.glob("*.c")
)
for operation in (
    "system", "popen", "fork", "execv", "execve", "posix_spawn",
    "CreateProcess", "ShellExecute", "LoadLibrary", "dlopen",
):
    if re.search(rf"\b{operation}\s*\(", first_party):
        fail(f"native execution or dynamic loading entered the content path: {operation}")

print("Cabinet architecture boundary: ok")
