#!/usr/bin/env python3
"""Check target isolation and verified-source handling without network or compilation.

SPDX-License-Identifier: MIT
Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""

import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/media"))
import prepare_ci as ci
import build_ffmpeg as builder


class ProviderPreparation(unittest.TestCase):
    def test_none_provider_ignores_host_sdk_discovery_and_flags(self):
        for supplied_flags in (False, True):
            with self.subTest(supplied_flags=supplied_flags), tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                marker = root / "called"
                script = root / "pkg-config"
                script.write_text('#!/bin/sh\ntouch "$MARKER"\n'
                                  'case "$1" in\n'
                                  '  --cflags) printf "%s\\n" -I/host/include ;;\n'
                                  '  --libs) printf "%s\\n" -lhost_provider ;;\n'
                                  'esac\n')
                script.chmod(0o755)
                command = ["make", "-s", "-f", "Makefile", "-f", "-",
                           "VIDEO_PROVIDER=none", f"PKG_CONFIG={script}", "provider-flags"]
                if supplied_flags:
                    command += ["FFMPEG_CFLAGS=-I/host/include", "FFMPEG_LIBS=-lhost_provider"]
                result = subprocess.run(command, cwd=ROOT,
                    env=dict(os.environ, MARKER=str(marker)), input="provider-flags:\n"
                    "\t@printf '%s\\n' '$(CPPFLAGS)' '$(LIBRETRO_CPPFLAGS)' '$(VIDEO_PROVIDER_LDLIBS)'\n",
                    capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(marker.exists(), "disabled video must not query the host SDK")
                self.assertNotIn("/host/include", result.stdout)
                self.assertNotIn("host_provider", result.stdout)
                self.assertIn("-DANYGM_VIDEO_PROVIDER_DYNAMIC=0", result.stdout)

    def test_native_auto_provider_keeps_sdk_discovery_and_flags(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            script = root / "pkg-config"
            script.write_text('#!/bin/sh\ncase "$1" in\n'
                              '  --cflags) printf "%s\\n" -I/native-sdk/include ;;\n'
                              'esac\n')
            script.chmod(0o755)
            result = subprocess.run(["make", "-s", "-f", "Makefile", "-f", "-",
                "VIDEO_PROVIDER=auto", "CROSS_COMPILE=", "platform=unix",
                f"PKG_CONFIG={script}", "provider-flags"], cwd=ROOT,
                input="provider-flags:\n\t@printf '%s\\n' '$(CPPFLAGS)'\n",
                capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("-I/native-sdk/include", result.stdout)
            self.assertIn("-DANYGM_VIDEO_PROVIDER_DYNAMIC=1", result.stdout)

    def test_partial_shared_library_is_rejected_before_install(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            library = root / "libavcodec.so.62"
            library.write_bytes(b"partial")
            nm = root / "nm"
            nm.write_text("#!/bin/sh\nprintf '%s\\n' '                 U ff_missing_private'\n")
            nm.chmod(0o755)
            with self.assertRaisesRegex(SystemExit, "unresolved private symbols"):
                builder.verify_elf_private_symbols(root, str(nm))

    def test_cross_make_never_queries_host_pkg_config(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            marker = root / "called"
            script = root / "pkg-config"
            script.write_text("#!/bin/sh\ntouch \"$MARKER\"\nprintf '%s\\n' -I/host/include\n")
            script.chmod(0o755)
            env = dict(os.environ, MARKER=str(marker))
            result = subprocess.run([
                "make", "-s", "platform=win", "CROSS_COMPILE=target-",
                f"PKG_CONFIG={script}", "runtime-link-libs",
            ], cwd=ROOT, env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse(marker.exists())
            self.assertNotIn("/host/include", result.stdout)

    def test_mxe_keeps_the_selected_cross_tool_family(self):
        values = ci.target_options("windows-x64", {
            "CC": "x86_64-w64-mingw32.static-gcc", "CXX": "x86_64-w64-mingw32.static-g++",
            "AR": "x86_64-w64-mingw32.static-ar"}, "Linux")
        self.assertIn("--cross-prefix=x86_64-w64-mingw32.static-", values)
        self.assertIn("--ar=x86_64-w64-mingw32.static-ar", values)
        self.assertNotIn("--cc=gcc", values)
        with self.assertRaises(ValueError):
            ci.target_options("windows-x64", {"CC": "gcc -m64"}, "Linux")

    def test_android_uses_the_ndk_tools_instead_of_inherited_host_tools(self):
        for abi, triple, arch in (("arm64-v8a", "aarch64-linux-android", "aarch64"),
                                 ("armeabi-v7a", "armv7a-linux-androideabi", "arm")):
            values = ci.target_options("android", {"NDK_ROOT": "/owned ndk", "ANDROID_ABI": abi,
                                                  "CC": "host-gcc", "CXX": "host-g++",
                                                  "AR": "host-ar"}, "Linux")
            self.assertIn(f"--cc=/owned ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/{triple}24-clang", values)
            self.assertIn("--arch=" + arch, values)
            self.assertNotIn("--cc=host-gcc", values)
            self.assertNotIn("--ar=host-ar", values)
        for env in ({}, {"NDK_ROOT": "/owned", "ANDROID_ABI": "unknown"}):
            with self.assertRaises(ValueError):
                ci.target_options("android", env, "Linux")

    def test_pkg_config_cannot_fall_back_to_a_previous_host_provider(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            script = root / "pkg-config"
            script.write_text("#!/bin/sh\n"
                              'test -z "$PKG_CONFIG_PATH" || exit 41\n'
                              'test -z "$PKG_CONFIG_SYSROOT_DIR" || exit 42\n'
                              'test "$PKG_CONFIG_LIBDIR" = "$EXPECTED_PROVIDER/lib/pkgconfig" || exit 43\n'
                              'printf "%s\\n" "$PKG_CONFIG_LIBDIR"\n')
            script.chmod(0o755)
            env = dict(os.environ, PKG_CONFIG=str(script), PKG_CONFIG_PATH="/wrong host",
                       PKG_CONFIG_SYSROOT_DIR="/wrong sysroot", EXPECTED_PROVIDER=str(root))
            values = ci.provider_environment(root, env)
            self.assertEqual(values["FFMPEG_LIBS"], str(root / "lib/pkgconfig"))

    def test_existing_unverified_input_never_triggers_download(self):
        with tempfile.TemporaryDirectory() as folder:
            archive = Path(folder) / "source.tar.xz"
            archive.write_bytes(b"unverified")
            with patch.object(ci.urllib.request, "urlopen") as network:
                with self.assertRaisesRegex(ValueError, "existing archive"):
                    ci.fetch_archive(archive, {"source_sha256": "0" * 64}, True)
                network.assert_not_called()
            archive = Path(folder) / "absent.tar.xz"
            with self.assertRaisesRegex(ValueError, "explicitly"):
                ci.fetch_archive(archive, {}, False)

    def test_download_publishes_only_verified_bytes(self):
        body = b"authored source bytes"
        pin = {"source_url": "https://example.invalid/authored", "source_sha256": hashlib.sha256(body).hexdigest()}
        with tempfile.TemporaryDirectory() as folder:
            archive = Path(folder) / "source.tar.xz"
            with patch.object(ci.urllib.request, "urlopen", return_value=io.BytesIO(b"corrupt")):
                with self.assertRaisesRegex(ValueError, "does not match"):
                    ci.fetch_archive(archive, pin, True)
            self.assertFalse(archive.exists())
            with patch.object(ci.urllib.request, "urlopen", return_value=io.BytesIO(body)):
                ci.fetch_archive(archive, pin, True)
            self.assertEqual(archive.read_bytes(), body)
            self.assertEqual(list(Path(folder).iterdir()), [archive])

    def test_plan_does_not_write_or_download(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            result = subprocess.run([sys.executable, str(ROOT / "tools/media/prepare_ci.py"),
                "--target", "windows-i686", "--work-root", str(root / "work"),
                "--env-file", str(root / "environment"), "--plan"],
                capture_output=True, text=True, timeout=10, check=True)
            self.assertIn("--target-os=mingw32", result.stdout)
            self.assertEqual(list(root.iterdir()), [])


if __name__ == "__main__":
    unittest.main()
