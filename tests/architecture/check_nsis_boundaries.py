#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Enforce the embedded-NSIS dependency and native-execution boundaries."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/content/container/embedded_nsis.c"


def fail(message: str) -> None:
    raise SystemExit(f"NSIS architecture: {message}")


make_common = (ROOT / "Makefile.common").read_text()
if "src/content/container/embedded_nsis.c" not in make_common:
    fail("container source is absent from the runtime source list")

text = SOURCE.read_text()
for operation in (
    "system", "popen", "fork", "execv", "execve", "posix_spawn",
    "CreateProcess", "ShellExecute", "LoadLibrary", "dlopen", "fopen", "open",
):
    if re.search(rf"\b{operation}\s*\(", text):
        fail(f"ambient file or process operation entered the NSIS path: {operation}")

all_paths = [path.as_posix().casefold() for path in (ROOT / "src").rglob("*")]
if any("third_party/nsis" in path or "third_party/7zip" in path for path in all_paths):
    fail("an NSIS or 7-Zip implementation was vendored into the core")

if "gml_deflate_decode_nsis_to_buffer" not in text:
    fail("NSIS extraction bypasses the reviewed shared inflate seam")

print("NSIS architecture boundary: ok")
