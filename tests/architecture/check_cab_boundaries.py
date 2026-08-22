#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Enforce the embedded-Cabinet dependency and native-execution boundaries."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VENDOR = ROOT / "src/third_party/libarchive"

EXPECTED = {
    "archive.h",
    "archive_acl.c",
    "archive_acl_private.h",
    "archive_check_magic.c",
    "archive_endian.h",
    "archive_entry.c",
    "archive_entry.h",
    "archive_entry_locale.h",
    "archive_entry_private.h",
    "archive_entry_sparse.c",
    "archive_entry_xattr.c",
    "archive_integer.h",
    "archive_platform.h",
    "archive_platform_stat.h",
    "archive_private.h",
    "archive_random_private.h",
    "archive_read.c",
    "archive_read_private.h",
    "archive_read_support_filter_none.c",
    "archive_read_support_format_cab.c",
    "archive_string.c",
    "archive_string.h",
    "archive_string_composition.h",
    "archive_string_sprintf.c",
    "archive_util.c",
    "archive_virtual.c",
    "archive_windows.h",
    "archive_config_anygm.h",
    "anygm_archive_windows_shim.c",
}


def fail(message: str) -> None:
    raise SystemExit(f"Cabinet architecture: {message}")


actual = {path.name for path in VENDOR.iterdir() if path.is_file()}
if actual != set(EXPECTED):
    fail(f"unexpected vendored closure: missing={sorted(set(EXPECTED)-actual)} extra={sorted(actual-set(EXPECTED))}")

for name in EXPECTED:
    contents = (VENDOR / name).read_text(errors="replace")
    if name in {"archive_config_anygm.h", "anygm_archive_windows_shim.c"}:
        if "SPDX-License-Identifier: MIT" not in contents or "retrodiv" not in contents:
            fail(f"first-party notice missing from {name}")
    elif "Copyright" not in contents or not (
        "Redistribution and use" in contents or "SPDX-License-Identifier: BSD-2-Clause" in contents
    ):
        fail(f"upstream notice missing from {name}")

sources = re.findall(r"src/third_party/libarchive/[^\s\\]+\.c", (ROOT / "Makefile.common").read_text())
expected_sources = sorted(
    f"src/third_party/libarchive/{name}" for name in EXPECTED if name.endswith(".c")
)
if sorted(set(sources)) != expected_sources:
    fail("compiled libarchive source list differs from the reviewed closure")

make_text = (ROOT / "Makefile").read_text() + (ROOT / "Makefile.common").read_text()
if re.search(r"(?:^|\s)-larchive(?:\s|$)", make_text):
    fail("build links an installed archive library")

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

for forbidden in (
    "archive_read_open_filename.c", "archive_windows.c", "archive_write.c",
    "archive_read_support_filter_gzip.c", "archive_read_support_filter_xz.c",
):
    if (VENDOR / forbidden).exists():
        fail(f"forbidden upstream body retained: {forbidden}")

print("Cabinet architecture boundary: ok")
