#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Check catalog structure, optionally against trusted external digests."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]


def arrays(text):
    rows = re.findall(r"static const uint8_t\s+(\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if len({name for name, _ in rows}) != len(rows):
        raise ValueError("duplicate audio array")
    return {name: bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})\b", body))
            for name, body in rows}


def check(recipes, setup_header, wwise_header, verification=None):
    profiles = recipes["fmod_profiles"]
    if recipes["schema"] != 1 or len(profiles) != 161 or len(recipes["wwise_symbols"]) != 598:
        raise ValueError("audio recipe counts changed")
    data = arrays(setup_header)
    if len(data) != 161 or set(data) != {f"vcb_{index}" for index in range(161)}:
        raise ValueError("setup array catalog differs from recipes")
    rows = re.findall(r"\{0x([0-9a-f]{8}),(\d+),vcb_(\d+)\}", setup_header)
    if [(size, index) for _, size, index in rows] != [
            (str(profile["bytes"]), str(index))
            for index, profile in enumerate(profiles)]:
        raise ValueError("setup lookup metadata differs from recipes")
    if verification is not None:
        if verification["schema"] != 1 or len(verification["profiles"]) != 161:
            raise ValueError("external verification profile count differs")
    for index, profile in enumerate(profiles):
        packet = data[f"vcb_{index}"]
        if (len(packet) != profile["bytes"] or not packet.startswith(b"\x05vorbis") or
                int(rows[index][0], 16) != zlib.crc32(packet)):
            raise ValueError("setup packet structure differs from recipes")
        if verification is not None:
            expected = verification["profiles"][index]
            if (f"{zlib.crc32(packet):08x}" != expected["id"] or
                    hashlib.sha256(packet).hexdigest() != expected["sha256"]):
                raise ValueError("setup packet differs from external verification")
    wwise_arrays = arrays(wwise_header)
    if set(wwise_arrays) != {"anygm_wwise_codebooks"}:
        raise ValueError("unexpected Wwise array catalog")
    packed = wwise_arrays["anygm_wwise_codebooks"]
    if len(packed) != 74164:
        raise ValueError("packed codebook size changed")
    if (verification is not None and
            hashlib.sha256(packed).hexdigest() != verification["packed_sha256"]):
        raise ValueError("packed codebooks differ from external verification")
    table = struct.unpack_from("<I", packed, len(packed) - 4)[0]
    if len(packed) - table != 599 * 4:
        raise ValueError("invalid packed codebook offsets")
    offsets = struct.unpack("<599I", packed[table:])
    if offsets[0] != 0 or offsets[-1] != table or any(a >= b for a, b in zip(offsets, offsets[1:])):
        raise ValueError("invalid packed codebook ranges")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verification-manifest", type=Path,
                        help="trusted digest manifest outside this repository")
    args = parser.parse_args()
    verification = None
    if args.verification_manifest is not None:
        manifest_path = args.verification_manifest.resolve(strict=True)
        if manifest_path == ROOT or ROOT in manifest_path.parents:
            raise ValueError("verification manifest must be outside this repository")
        verification = json.loads(manifest_path.read_text())
    recipes = json.loads((ROOT / "tools/audiogen/recipes.json").read_text())
    setup = (ROOT / "src/generated/audio_setup_data.h").read_text()
    wwise = (ROOT / "src/generated/wwise_codebooks.h").read_text()
    check(recipes, setup, wwise, verification)
    # Negative controls protect the guard itself from silently becoming inert.
    for bad_setup, bad_wwise in ((setup.replace("0x05,", "0x04,", 1), wwise),
                                 (setup, wwise.replace("0x", "0y", 1))):
        try:
            check(recipes, bad_setup, bad_wwise, verification)
        except ValueError:
            continue
        raise ValueError("audio drift negative control was not rejected")
    if verification is not None:
        changed = wwise.replace("0x92,", "0x93,", 1)
        if changed == wwise:
            raise ValueError("external digest negative control could not be prepared")
        try:
            check(recipes, setup, changed, verification)
        except ValueError:
            pass
        else:
            raise ValueError("external digest negative control was not rejected")
    scope = "matches supplied external expectations" if verification is not None else "structural consistency"
    print(f"audio data: 161 setups, 598 packed codebooks; {scope} and drift controls passed")


if __name__ == "__main__":
    main()
