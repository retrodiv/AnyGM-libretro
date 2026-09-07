#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Explicit regeneration with the pinned, unmodified xdelta3 encoder library.

Not part of builds or tests. See README.md for the complete producer contract.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--encoder-library", type=Path, required=True)
    args = parser.parse_args()
    encoder = ctypes.CDLL(str(args.encoder_library.resolve())).xd3_encode_memory
    encoder.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_void_p,
                        ctypes.c_uint64, ctypes.c_void_p,
                        ctypes.POINTER(ctypes.c_uint64), ctypes.c_uint64, ctypes.c_int]
    encoder.restype = ctypes.c_int
    source = bytes((i * 13 + i // 97) % 251 for i in range(8192))

    def target(size: int) -> bytes:
        return bytes(source[i % len(source)] if i % 997 < 512 else
                     65 + (i // 31) % 23 for i in range(size))

    lines = ["/* SPDX-License-Identifier: MIT",
             " * Copyright (c) 2026 retrodiv <retrodiv@proton.me>",
             " * Generated from authored synthetic bytes by generate.py; see README.md.",
             " */", "#ifndef ANYGM_VCDIFF_FIXTURES_H", "#define ANYGM_VCDIFF_FIXTURES_H",
             "", "#include <stdint.h>", ""]
    for name, secondary, size in [("plain", 0, 16384), ("djw", 1 << 5, 16384),
                                   ("fgk", 1 << 6, 16384), ("lzma", 1 << 24, 16384),
                                   ("lzma_windows", 1 << 24, 8 * 1024 * 1024 + 8192)]:
        expected = target(size)
        output = ctypes.create_string_buffer(len(expected) * 2 + 65536)
        written = ctypes.c_uint64()
        status = encoder(expected, len(expected), source, len(source), output,
                         ctypes.byref(written), len(output), secondary | (6 << 20))
        if status:
            raise SystemExit(f"encoder failed for {name}: {status}")
        patch = output.raw[:written.value]
        if secondary and (patch[4] & 1 == 0 or patch[5] != {1 << 5: 1, 1 << 6: 16,
                                                         1 << 24: 2}[secondary]):
            raise SystemExit(f"secondary encoder was not selected for {name}")
        lines.append(f"/* {name}: target size {size}, patch SHA-256 {hashlib.sha256(patch).hexdigest()} */")
        lines.append(f"static const uint8_t vcdiff_{name}[] = {{")
        for at in range(0, len(patch), 16):
            lines.append("  " + ", ".join(f"0x{b:02x}" for b in patch[at:at + 16]) + ",")
        lines.extend(["};", ""])
        print(name, len(patch), "bytes; target SHA-256", hashlib.sha256(expected).hexdigest())
    lines.extend(["#endif", ""])
    Path(__file__).with_name("fixtures.h").write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
