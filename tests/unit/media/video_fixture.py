#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Author three solid RGB frames and a mathematical stereo tone in an uncompressed RIFF AVI.

Container field facts: https://learn.microsoft.com/en-us/windows/win32/directshow/avi-riff-file-reference
No encoder, corpus, recorded output, or third-party media is an input.
"""

import math
import random
from pathlib import Path
import struct
import sys


def chunk(kind: bytes, data: bytes) -> bytes:
    return kind + struct.pack("<I", len(data)) + data + bytes(len(data) & 1)


def stream(kind: bytes, handler: bytes, scale: int, rate: int, length: int,
           capacity: int, sample_size: int, width: int = 0, height: int = 0) -> bytes:
    return struct.pack("<4s4sIHHIIIIIIIIhhhh", kind, handler, 0, 0, 0,
                       0, scale, rate, 0, length, capacity, 0xFFFFFFFF, sample_size,
                       0, 0, width, height)


def movie(width: int = 16, height: int = 16, noise: bool = False) -> bytes:
    image_size = width * height * 3
    main = struct.pack("<14I", 500000, 192000 + image_size * 2, 0, 0x10, 3, 0, 2,
                       max(96000, image_size), width, height, 0, 0, 0, 0)
    video_format = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, image_size, 0, 0, 0, 0)
    audio_format = struct.pack("<HHIIHHH", 1, 2, 48000, 192000, 4, 16, 0)
    video = chunk(b"LIST", b"strl" +
                  chunk(b"strh", stream(b"vids", b"DIB ", 1, 2, 3, image_size, 0, width, height)) +
                  chunk(b"strf", video_format))
    audio = chunk(b"LIST", b"strl" +
                  chunk(b"strh", stream(b"auds", bytes(4), 4, 192000, 72000, 96000, 4)) +
                  chunk(b"strf", audio_format))
    header = chunk(b"LIST", b"hdrl" + chunk(b"avih", main) + video + audio)
    packets = bytearray()
    index = bytearray()
    pattern = b""
    if noise:
        block = random.Random(0x41474956).randbytes(65536)
        pattern = (block * ((image_size + len(block) - 1) // len(block)))[:image_size]
    for frame, brightness in enumerate((0, 255, 0)):
        samples = bytearray()
        for sample in range(24000):
            value = round(4096 * math.sin(2 * math.pi * 440 * (frame * 24000 + sample) / 48000))
            samples.extend(struct.pack("<hh", value, value))
        pixels = (pattern.translate(bytes(value ^ brightness for value in range(256)))
                  if noise else bytes([brightness]) * image_size)
        for kind, data in ((b"00db", pixels), (b"01wb", samples)):
            index.extend(struct.pack("<4sIII", kind, 0x10, len(packets) + 4, len(data)))
            packets.extend(chunk(kind, data))
    return chunk(b"RIFF", b"AVI " + header + chunk(b"LIST", b"movi" + packets) +
                 chunk(b"idx1", index))


if __name__ == "__main__":
    if len(sys.argv) != 2 and (len(sys.argv) != 3 or sys.argv[2] != "--large"):
        raise SystemExit("usage: video_fixture.py OUTPUT [--large]")
    Path(sys.argv[1]).write_bytes(movie(1920, 1080, True) if len(sys.argv) == 3 else movie())
