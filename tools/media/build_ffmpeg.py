#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Build the pinned, replaceable shared video dependency from a verified local source archive."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile


ROOT = Path(__file__).resolve().parents[2]
PIN_PATH = Path(__file__).with_name("ffmpeg.json")


def digest(path: Path) -> str:
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def run(argv: list[str], cwd: Path, log: Path) -> None:
    print(f"ffmpeg: {log.name}", flush=True)
    with log.open("a") as output:
        result = subprocess.run(argv, cwd=cwd, stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f"ffmpeg: command failed ({result.returncode}); inspect {log}")


def verify_elf_private_symbols(build: Path, nm: str) -> None:
    """Reject a partial ELF shared library left by an interrupted object build."""
    libraries = sorted({path.resolve() for path in build.rglob("*.so*") if path.is_file()})
    for library in libraries:
        result = subprocess.run([nm, "-D", "--undefined-only", str(library)],
                                capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(f"ffmpeg: could not inspect shared-library symbols: {library}")
        private = sorted({line.split()[-1] for line in result.stdout.splitlines()
                          if line.split() and line.split()[-1].startswith("ff_")})
        if private:
            raise SystemExit(
                f"ffmpeg: shared library retains unresolved private symbols: {library}: "
                + ", ".join(private[:8]))


def source_seal(archive: Path, work: Path, source: Path) -> str:
    rows = []
    expected = set()
    with tarfile.open(archive, "r|xz") as packed:
        for member in packed:
            if member.isdir():
                continue
            path = work / member.name
            expected.add(path.relative_to(source).as_posix())
            if member.isfile():
                stream = packed.extractfile(member)
                if stream is None:
                    raise SystemExit("ffmpeg: missing archive file body")
                checksum = hashlib.file_digest(stream, "sha256").hexdigest()
                if path.is_symlink() or not path.is_file() or digest(path) != checksum:
                    raise SystemExit(f"ffmpeg: source differs from pinned archive: {member.name}")
                rows.append((member.name, checksum))
            elif member.issym():
                if not path.is_symlink() or os.readlink(path) != member.linkname:
                    raise SystemExit(f"ffmpeg: source link differs: {member.name}")
                rows.append((member.name, "link", member.linkname))
            else:
                raise SystemExit("ffmpeg: unexpected source archive member type")
    actual = {path.relative_to(source).as_posix() for path in source.rglob("*")
              if path.is_file() or path.is_symlink()}
    if actual != expected:
        raise SystemExit("ffmpeg: source tree has missing or additional files")
    return hashlib.sha256(json.dumps(sorted(rows)).encode()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--jobs", type=int, choices=range(1, 5), default=1)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--cross-prefix")
    parser.add_argument("--target-os")
    parser.add_argument("--arch")
    parser.add_argument("--cc")
    parser.add_argument("--cxx")
    for name in ("ar", "ranlib", "nm", "strip", "install-name-dir", "extra-cflags", "extra-ldflags"):
        parser.add_argument("--" + name)
    args = parser.parse_args()
    pin = json.loads(PIN_PATH.read_text())
    archive = args.source_archive.resolve(strict=True)
    work = args.work_root.resolve()
    if work.is_relative_to(ROOT):
        parser.error("the external dependency work root must be outside this checkout")
    if work == work.parent or work in archive.parents:
        parser.error("the work root must be a private directory separate from the source archive")
    if digest(archive) != pin["source_sha256"]:
        parser.error("source archive SHA-256 does not match the reviewed pin")
    prefix = work / "install"
    source = work / f"ffmpeg-{pin['version']}"
    build = work / "build"
    configure = [str(source / "configure"), f"--prefix={prefix}", *pin["configure"]]
    if args.cross_prefix:
        if not args.target_os or not args.arch:
            parser.error("a cross prefix requires explicit target OS and architecture")
        configure += ["--enable-cross-compile", f"--cross-prefix={args.cross_prefix}"]
    for name in ("target_os", "arch", "cc", "cxx", "ar", "ranlib", "nm", "strip",
                 "install_name_dir", "extra_cflags", "extra_ldflags"):
        value = getattr(args, name)
        if value:
            configure.append(f"--{name.replace('_', '-')}={value}")
    inputs = {
        "schema": 1,
        "source": pin,
        "source_archive": str(archive),
        "configure": configure,
        "environment": {name: os.environ.get(name, "") for name in (
            "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS", "PATH", "CPATH",
            "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "PKG_CONFIG_LIBDIR")},
    }
    for name in ("SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "LIBRETRO_APPLE_ISYSROOT"):
        if os.environ.get(name):
            inputs["environment"][name] = os.environ[name]
    input_record = work / "build-inputs.json"
    if args.resume:
        if not input_record.is_file() or json.loads(input_record.read_text()) != inputs:
            parser.error("resume requires the exact recorded source, compiler and configuration")
    else:
        if work.exists() and any(work.iterdir()):
            parser.error("work root is nonempty; choose a fresh directory or use --resume")
        if not hasattr(tarfile, "data_filter"):
            parser.error("Python with tarfile.data_filter support is required")
        work.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive, "r:xz") as packed:
            for member in packed.getmembers():
                parts = Path(member.name).parts
                if not parts or parts[0] != source.name or ".." in parts:
                    parser.error("source archive contains an unexpected path")
            packed.extractall(work, filter="data")
        input_record.write_text(json.dumps(inputs, indent=2) + "\n")
    build.mkdir(exist_ok=True)
    source_digest = source_seal(archive, work, source)
    if not (build / "config.h").is_file():
        run(configure, build, work / "configure.log")
    config = (build / "config.h").read_text()
    for option in ("GPL", "NONFREE", "VERSION3"):
        if not re.search(rf"^#define CONFIG_{option} 0$", config, re.MULTILINE):
            raise SystemExit(f"ffmpeg: CONFIG_{option} is not disabled")
    if '#define FFMPEG_LICENSE "LGPL version 2.1 or later"' not in config:
        raise SystemExit("ffmpeg: unexpected configured license")
    run(["make", f"-j{args.jobs}"], build, work / "build.log")
    verify_elf_private_symbols(build, args.nm or
                               ((args.cross_prefix or "") + "nm" if args.cross_prefix else "nm"))
    run(["make", "install"], build, work / "install.log")
    if source_seal(archive, work, source) != source_digest:
        raise SystemExit("ffmpeg: source tree changed during the build")
    notices = prefix / "share" / "licenses" / "FFmpeg"
    notices.mkdir(parents=True, exist_ok=True)
    for name in ("COPYING.LGPLv2.1", "LICENSE.md"):
        shutil.copyfile(source / name, notices / name)
    files = {}
    for path in sorted(prefix.rglob("*")):
        relative = path.relative_to(prefix).as_posix()
        if path.is_symlink():
            files[relative] = {"link": os.readlink(path)}
        elif path.is_file():
            files[relative] = {"sha256": digest(path), "size": path.stat().st_size}
    receipt = {**inputs, "installed_files": files, "config_sha256": digest(build / "config.h"),
               "license": "LGPL-2.1-or-later", "patches": [], "jobs": args.jobs,
               "source_tree_sha256": source_digest, "builder_sha256": digest(Path(__file__))}
    (work / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(f"ffmpeg: installed shared libraries in {prefix}", flush=True)
    print(f"ffmpeg: receipt {work / 'build-receipt.json'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
