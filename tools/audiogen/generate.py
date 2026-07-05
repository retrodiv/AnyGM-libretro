#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Reproduce audio configuration from externally verified Xiph source releases.

Generation never reads an existing core table. The check phase alone reads the
checked-in headers, after both complete candidate outputs have been produced.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import tarfile
import urllib.request
import zlib

from packed_books import pack_library

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PINS = {"libogg-1.3.5": ("ogg", None), "libvorbis-1.2.0": ("vorbis", None), "libvorbis-1.3.7": ("vorbis", None)}
VORBIS_FILES = "mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0 res0 mapping0 registry codebook sharedbook lookup bitrate".split()
WWISE_SHA256 = None


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def acquire(work, archives):
    records = []
    sources = work / "sources"
    sources.mkdir()
    for name, (family, expected) in PINS.items():
        filename = name + ".tar.gz"
        url = f"https://ftp.osuosl.org/pub/xiph/releases/{family}/{filename}"
        if archives:
            with (archives / filename).open("rb") as stream:
                data = stream.read(4 * 1024 * 1024 + 1)
        else:
            with urllib.request.urlopen(url, timeout=45) as stream:
                data = stream.read(4 * 1024 * 1024 + 1)
        if len(data) > 4 * 1024 * 1024 or sha256(data) != expected:
            raise ValueError(f"unrecognized source archive: {filename}")
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            members = archive.getmembers()
            if len(members) > 5000 or sum(m.size for m in members) > 64 * 1024 * 1024:
                raise ValueError("source archive exceeds extraction bounds")
            for member in members:
                path = PurePosixPath(member.name)
                if (path.is_absolute() or ".." in path.parts or not path.parts or
                        path.parts[0] != name or not (member.isfile() or member.isdir())):
                    raise ValueError("unsafe source archive member")
            archive.extractall(sources, members=members, filter="data")
        records.append(dict(name=name, url=url, sha256=expected, bytes=len(data)))
    return sources, records


def run(arguments, work, *, data=None, capture=False):
    return subprocess.run([str(a) for a in arguments], cwd=work, input=data, check=True,
                          stdout=subprocess.PIPE if capture else None).stdout


def compile_generators(work, sources, symbols, sanitize):
    prefix = work / "prefix"
    run(["cmake", "-S", sources / "libogg-1.3.5", "-B", work / "ogg-build",
         "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DCMAKE_BUILD_TYPE=Release",
         "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF", "-DINSTALL_DOCS=OFF",
         f"-DCMAKE_INSTALL_PREFIX={prefix}"], work)
    run(["cmake", "--build", work / "ogg-build", "--parallel", "1"], work)
    run(["cmake", "--install", work / "ogg-build"], work)
    archives = list(prefix.rglob("libogg.a"))
    if len(archives) != 1:
        raise ValueError("expected one static Ogg library")
    vorbis = sources / "libvorbis-1.3.7"
    flags = ["-std=gnu11", "-O1" if sanitize else "-O2", "-DHAVE_ALLOCA_H",
             "-I" + str(vorbis / "lib"), "-I" + str(vorbis / "include"),
             "-I" + str(prefix / "include")]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    objects = []
    for name in VORBIS_FILES:
        obj = work / (name + ".o")
        run(["cc", *flags, "-c", vorbis / "lib" / (name + ".c"), "-o", obj], work)
        objects.append(obj)
    run(["cc", *flags, "-Wall", "-Wextra", HERE / "emit_setups.c", *objects,
         archives[0], "-lm", "-o", work / "emit-setups"], work)
    selected = "static const static_codebook *const selected_books[]={\n"
    for symbol in symbols:
        if not re.fullmatch(r"_[A-Za-z0-9_]+", symbol):
            raise ValueError("invalid reference codebook symbol")
        selected += f"    &{symbol},\n"
    selected += "};\nstatic const static_codebook *const reference_books[]={\n"
    declared = []
    for header in sorted((sources / "libvorbis-1.2.0/lib/books").rglob("*.h")):
        declared += re.findall(r"static\s+static_codebook\s+(\w+)\s*=", header.read_text())
    if len(declared) != 598 or len(set(declared)) != 598:
        raise ValueError("unexpected reference source declarations")
    selected += "".join(f"    &{symbol},\n" for symbol in declared)
    (work / "selected_books.h").write_text(selected + "};\n", encoding="utf-8")
    flags = ["-std=c11", "-O0", "-Wall", "-Wextra",
             "-I" + str(sources / "libvorbis-1.2.0/lib"),
             "-I" + str(prefix / "include"), "-I" + str(work)]
    if sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    run(["cc", *flags, HERE / "emit_books.c", "-o", work / "emit-books"], work)


def generate_data(work, recipes):
    profiles = recipes["fmod_profiles"]
    request = "".join(f'{p["channels"]} {p["rate"]} {p["quality_bits"]} {p["coupling"]}\n'
                      for p in profiles).encode("ascii")
    stream = io.BytesIO(run([work / "emit-setups"], work, data=request, capture=True))
    setups = []
    for profile in profiles:
        header = stream.read(12)
        if len(header) != 12:
            raise ValueError("truncated setup output")
        short, long, size = struct.unpack("<3I", header)
        packet = stream.read(size)
        if ([short, long] != profile["blocks"] or size != profile["bytes"] or
                len(packet) != size or f"{zlib.crc32(packet):08x}" != profile["id"] or
                sha256(packet) != profile["sha256"]):
            raise ValueError(f'non-reproducing setup: {profile["id"]}')
        setups.append((profile["id"], packet))
    if stream.read():
        raise ValueError("unexpected setup output")
    books = json.loads(run([work / "emit-books"], work, capture=True))
    if len(books) != 598:
        raise ValueError("wrong codebook count")
    packed = pack_library(books)
    if len(packed) != 74164 or sha256(packed) != WWISE_SHA256:
        raise ValueError("non-reproducing packed codebook library")
    return setups, packed


def array(name, data):
    lines = [f"static const uint8_t {name}[]={{"]
    lines += ["  " + ",".join(f"0x{b:02x}" for b in data[at:at + 16]) + ","
              for at in range(0, len(data), 16)]
    return "\n".join([*lines, "};", ""])


def render(setups, packed):
    setup = """/* SPDX-License-Identifier: MIT AND BSD-3-Clause
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 * MIT covers original wrapper text; Xiph data retains its BSD license.
 * Generated from the BSD-licensed Xiph libvorbis reference source.
 * Regenerate: tools/audiogen/generate.py (see tools/audiogen/README.md).
 * See LICENSES/libvorbis-1.3.7.txt.
 */
#ifndef ANYGM_AUDIO_SETUP_DATA_H
#define ANYGM_AUDIO_SETUP_DATA_H
#include <stdint.h>

typedef struct AnygmAudioSetupEntry {
    uint32_t id;
    uint32_t size;
    const uint8_t *data;
} AnygmAudioSetupEntry;

"""
    setup += "\n".join(array("vcb_" + str(i), packet) for i, (_, packet) in enumerate(setups))
    setup += "\nstatic const AnygmAudioSetupEntry anygm_audio_setup_entries[]={\n"
    setup += "".join(f"    {{0x{key},{len(packet)},vcb_{i}}},\n" for i, (key, packet) in enumerate(setups))
    setup += "};\n\nstatic const int anygm_audio_setup_entry_count = sizeof(anygm_audio_setup_entries) / sizeof(anygm_audio_setup_entries[0]);\n\n#endif\n"
    wwise = """/* SPDX-License-Identifier: MIT AND BSD-3-Clause
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 * MIT covers original wrapper text; Xiph data retains its BSD license.
 * Generated from the BSD-licensed Xiph libvorbis reference source.
 * Regenerate: tools/audiogen/generate.py (see tools/audiogen/README.md).
 * See LICENSES/libvorbis-1.2.0.txt.
 */
#ifndef ANYGM_WWISE_CODEBOOKS_H
#define ANYGM_WWISE_CODEBOOKS_H
#include <stddef.h>
#include <stdint.h>

"""
    wwise += array("anygm_wwise_codebooks", packed)
    wwise += "static const size_t anygm_wwise_codebooks_size=sizeof anygm_wwise_codebooks;\n\n#endif\n"
    return {"src/generated/audio_setup_data.h": setup, "src/generated/wwise_codebooks.h": wwise}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True, help="new, private build directory")
    parser.add_argument("--source-archives", type=Path, help="offline directory containing the three pinned tar.gz files")
    parser.add_argument("--verification-manifest", type=Path, required=True, help="trusted integrity expectations outside the source directory")
    parser.add_argument("--sanitize", action="store_true", help="instrument reference Vorbis and both generators")
    parser.add_argument("--write", action="store_true", help="replace generated headers after complete reproduction")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    manifest_path = args.verification_manifest.resolve()
    if work == ROOT or ROOT in work.parents or manifest_path == ROOT or ROOT in manifest_path.parents:
        raise ValueError("work directory and verification manifest must be outside the source directory")
    verification = json.loads(manifest_path.read_bytes())
    if verification["schema"] != 1 or len(verification["profiles"]) != 161:
        raise ValueError("invalid verification manifest")
    global PINS, WWISE_SHA256
    PINS = {name: (family, verification["archives"][name]) for name, (family, _) in PINS.items()}
    WWISE_SHA256 = verification["packed_sha256"]
    work.mkdir(parents=True, exist_ok=False)
    recipe_bytes = (HERE / "recipes.json").read_bytes()
    recipes = json.loads(recipe_bytes)
    if len(recipes["fmod_profiles"]) != len(verification["profiles"]):
        raise ValueError("verification profile count mismatch")
    for profile, expected in zip(recipes["fmod_profiles"], verification["profiles"]):
        profile["id"] = expected["id"]
        profile["sha256"] = expected["sha256"]
    if (recipes["schema"] != 1 or len(recipes["fmod_profiles"]) != 161 or
            len({p["id"] for p in recipes["fmod_profiles"]}) != 161 or
            len(recipes["wwise_symbols"]) != 598):
        raise ValueError("invalid recipe catalog")
    sources, inputs = acquire(work, args.source_archives)
    compile_generators(work, sources, recipes["wwise_symbols"], args.sanitize)
    setups, packed = generate_data(work, recipes)
    outputs = render(setups, packed)
    for name, content in outputs.items():
        (work / Path(name).name).write_text(content, encoding="utf-8", newline="\n")
    if not args.write:
        for name, content in outputs.items():
            if (ROOT / name).read_bytes() != content.encode("utf-8"):
                raise ValueError(f"generated header drift: {name}")
    else:
        for name, content in outputs.items():
            (ROOT / name).write_text(content, encoding="utf-8", newline="\n")
    report = dict(schema=1,inputs=inputs,recipe_sha256=sha256(recipe_bytes),
                  setup_count=len(setups),setup_bytes=sum(len(p) for _,p in setups),
                  wwise_count=598,wwise_bytes=len(packed),wwise_sha256=sha256(packed),
                  outputs={name:sha256(data.encode("utf-8")) for name,data in outputs.items()},
                  sanitized=args.sanitize,mode="write" if args.write else "check")
    (work / "reproduction.json").write_text(json.dumps(report,indent=2)+"\n", encoding="utf-8")
    print("audio data: reproduced 161 setup packets and 598 packed codebooks")


if __name__ == "__main__":
    main()
