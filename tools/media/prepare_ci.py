#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Explicit CI preparation of the pinned shared provider, outside the source checkout."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tempfile
import urllib.request

from build_ffmpeg import PIN_PATH, ROOT, digest


TARGETS = ("native", "windows-x64", "windows-i686", "linux-i686", "linux-armhf", "android")


def target_options(target: str, env: dict[str, str], system: str) -> list[str]:
    options = []
    if target == "android":
        abi = env.get("ANDROID_ABI")
        triples = {"arm64-v8a": ("aarch64-linux-android", "aarch64"),
                   "armeabi-v7a": ("armv7a-linux-androideabi", "arm")}
        if abi not in triples or not env.get("NDK_ROOT"):
            raise ValueError("Android preparation requires NDK_ROOT and a supported ANDROID_ABI")
        triple, arch = triples[abi]
        tools = Path(env["NDK_ROOT"]) / "toolchains/llvm/prebuilt/linux-x86_64/bin"
        options += [f"--cross-prefix={tools}/llvm-", "--target-os=android", f"--arch={arch}",
                    f"--cc={tools}/{triple}24-clang", f"--cxx={tools}/{triple}24-clang++"]
        options += [f"--{name}={tools}/llvm-{name}" for name in ("ar", "ranlib", "nm", "strip")]
    elif target.startswith("windows-") or target == "linux-armhf":
        windows = target.startswith("windows-")
        triple = ("x86_64-w64-mingw32" if target == "windows-x64" else
                  "i686-w64-mingw32" if windows else "arm-linux-gnueabihf")
        compiler = env.get("CC") or triple + "-gcc"
        # The build infrastructure also supplies MXE's *.static-gcc names. Keep
        # that exact tool family rather than accidentally selecting host tools.
        if not compiler.endswith("gcc") or len(shlex.split(compiler)) != 1:
            raise ValueError("cross preparation requires one gcc toolchain command")
        prefix = compiler[:-3]
        arch = "x86_64" if target == "windows-x64" else "x86_32" if windows else "arm"
        options += [f"--cross-prefix={prefix}", f"--target-os={'mingw32' if windows else 'linux'}",
                    f"--arch={arch}", f"--cc={compiler}", f"--cxx={env.get('CXX') or prefix + 'g++'}"]
        if not windows:
            options += ["--extra-cflags=-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard"]
    elif target == "linux-i686":
        options += ["--arch=x86_32", "--extra-cflags=-m32", "--extra-ldflags=-m32"]
    elif system == "Darwin":
        options += ["--install-name-dir=@rpath"]
    for name in ("CC", "CXX", "AR", "RANLIB", "NM", "STRIP"):
        value = env.get(name)
        option = "--" + name.lower() + "="
        if value and not any(item.startswith(option) for item in options):
            options.append(option + value)
    return options


def fetch_archive(archive: Path, pin: dict, download: bool) -> None:
    if archive.exists():
        if digest(archive) != pin["source_sha256"]:
            raise ValueError("existing archive does not match the pinned source")
        return
    if not download:
        raise ValueError("source archive is absent; supply it or explicitly request --download")
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=archive.parent) as temporary_directory:
        temporary = Path(temporary_directory) / "source.tar.xz"
        checksum = hashlib.sha256()
        size = 0
        with urllib.request.urlopen(pin["source_url"], timeout=30) as source, temporary.open("wb") as output:
            while data := source.read(1024 * 1024):
                size += len(data)
                if size > 16 * 1024 * 1024:
                    raise ValueError("pinned source archive exceeds the download bound")
                checksum.update(data)
                output.write(data)
        if checksum.hexdigest() != pin["source_sha256"]:
            raise ValueError("downloaded source archive does not match the pin")
        # Publish only verified bytes; the builder separately verifies them again.
        temporary.replace(archive)


def provider_environment(prefix: Path, env: dict[str, str]) -> dict[str, str]:
    packages = ["libavformat", "libavcodec", "libavutil", "libswscale", "libswresample"]
    pkg_env = dict(env, PKG_CONFIG_PATH="", PKG_CONFIG_SYSROOT_DIR="",
                   PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"))
    values = {}
    for name, flag in (("FFMPEG_CFLAGS", "--cflags"), ("FFMPEG_LIBS", "--libs")):
        values[name] = subprocess.check_output(
            [env.get("PKG_CONFIG", "pkg-config"), flag, *packages], env=pkg_env, text=True).strip()
    for name, part in (("LD_LIBRARY_PATH", "lib"), ("DYLD_LIBRARY_PATH", "lib")):
        values[name] = str(prefix / part) + (os.pathsep + env[name] if env.get(name) else "")
    return values


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=TARGETS, default="native")
    parser.add_argument("--work-root", type=Path, required=True)
    parser.add_argument("--source-archive", type=Path)
    parser.add_argument("--env-file", type=Path, required=True)
    parser.add_argument("--format", choices=("shell", "github"), default="shell")
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--plan", action="store_true", help="Print arguments without writing, fetching or building")
    args = parser.parse_args()
    try:
        if sys.version_info < (3, 11):
            raise ValueError("provider preparation requires Python 3.11 or later with tarfile.data_filter")
        work = args.work_root.resolve()
        if work == work.parent or work.is_relative_to(ROOT):
            raise ValueError("CI provider work must be outside the source checkout")
        archive = (args.source_archive or work.with_suffix(".tar.xz")).resolve()
        if work in archive.parents or args.env_file.resolve().is_relative_to(work) or args.env_file.resolve() == archive:
            raise ValueError("source archive, provider work and exported environment must be separate")
        command = [sys.executable, str(Path(__file__).with_name("build_ffmpeg.py")),
                   "--source-archive", str(archive), "--work-root", str(work), "--jobs", "1",
                   *target_options(args.target, dict(os.environ), platform.system())]
        if args.resume:
            command.append("--resume")
        if args.plan:
            print(json.dumps(command))
            return 0
        fetch_archive(archive, json.loads(PIN_PATH.read_text()), args.download)
        subprocess.run(command, check=True)
        values = provider_environment(work / "install", dict(os.environ))
        if args.target == "android":
            values["ANYGM_VIDEO_ABI"] = os.environ["ANDROID_ABI"]
        if any("\n" in value or "\r" in value for value in values.values()):
            raise ValueError("provider environment must contain single-line values")
        args.env_file.parent.mkdir(parents=True, exist_ok=True)
        with args.env_file.open("a") as output:
            for key, value in values.items():
                output.write((f"{key}={value}" if args.format == "github" else
                              f"export {key}={shlex.quote(value)}") + "\n")
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"video preparation: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
