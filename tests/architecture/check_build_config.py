#!/usr/bin/env python3
"""Exercise incremental builds through the real Makefile with tiny owned sources.

SPDX-License-Identifier: MIT
Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class IncrementalBuild(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("Makefile", "Makefile.common", "link.T", "tests/contract/libretro_exports.txt"):
            destination = self.root / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, destination)
        (self.root / "probe.c").write_text(
            'extern int cpp_value(void);\n'
            '__attribute__((visibility("default"))) int retro_api_version(void) '
            '{ return VALUE + cpp_value(); }\n')
        (self.root / "probe_cpp.cpp").write_text('extern "C" int cpp_value(void) { return CPP_VALUE; }\n')
        for name in ("compiler one", "compiler two"):
            path = self.root / name
            path.write_text('#!/bin/sh\nexec cc "$@"\n')
            path.chmod(0o755)
        self.env = {k: v for k, v in os.environ.items() if k not in {
            "CC", "CXX", "AR", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LDLIBS",
            "MAKEFLAGS", "MFLAGS", "GNUMAKEFLAGS", "MAKEFILES", "CROSS_COMPILE",
            "SDKROOT", "MACOSX_DEPLOYMENT_TARGET"}}
        self.platform = "osx" if sys.platform == "darwin" else "unix"
        self.extension = ".dylib" if sys.platform == "darwin" else ".so"
        self.settings = dict(CC='"./compiler one"', CXX="c++", AR="ar",
            CFLAGS="-O0 -DVALUE=1", CXXFLAGS="-O0 -DCPP_VALUE=10", CPPFLAGS="", LDFLAGS="", LDLIBS="")

    def build(self, goal="core", **changes):
        self.settings.update(changes)
        command = ["make", "--no-print-directory", "-j1", goal, "platform=" + self.platform,
            "ANYGM_RUNTIME_SOURCES=probe.c", "ANYGM_RUNTIME_CXX_SOURCES=probe_cpp.cpp",
            "ANYGM_LIBRETRO_SOURCES=", "ANYGM_CORE_SOURCES=", "ANYGM_TEST_HOST_SOURCES="]
        command += [key + "=" + value for key, value in self.settings.items()]
        result = subprocess.run(command, cwd=self.root, env=self.env, text=True,
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def value(self):
        library = self.root / ("anygm_libretro" + self.extension)
        result = subprocess.check_output([sys.executable, "-c",
            'import ctypes,sys; print(ctypes.CDLL(sys.argv[1]).retro_api_version())',
            str(library)], text=True, timeout=10)
        return int(result)

    def test_compile_settings_change_then_stabilize_and_restore(self):
        self.build()
        self.assertEqual(self.value(), 11)
        self.assertNotIn(" -c ", self.build())
        for changes, expected in ((dict(CFLAGS="-O0 -DVALUE=2"), 12),
                                  (dict(CXXFLAGS="-O0 -DCPP_VALUE=20"), 22),
                                  (dict(CC='"./compiler two"'), 22),
                                  (dict(CFLAGS="-O0 -DVALUE=1", CXXFLAGS="-O0 -DCPP_VALUE=10"), 11)):
            with self.subTest(changes=changes):
                self.assertIn(" -c ", self.build(**changes))
                self.assertEqual(self.value(), expected)
                self.assertNotIn(" -c ", self.build())

    def test_link_flags_relink_without_recompiling(self):
        self.build()
        self.assertEqual(self.build(), "")
        output = self.build(LDFLAGS="-Wl,-map,probe.map" if self.platform == "osx" else "-Wl,-Map=probe.map")
        self.assertNotIn(" -c ", output)
        self.assertTrue((self.root / "probe.map").is_file())
        self.assertEqual(self.build(), "")

    def test_quoted_flags_are_recorded_without_shell_interpretation(self):
        flags = '-DNOTE=\'"two words"\''
        self.build(CPPFLAGS=flags)
        config = (self.root / "build" / self.platform / ".compile-config").read_text()
        self.assertIn("CPPFLAGS=" + flags + "\n", config)
        self.assertNotIn(" -c ", self.build())

    def test_static_archives_contain_only_objects(self):
        self.build(STATIC_LINKING="1")
        self.build(goal="runtime")
        for archive in (self.root / "anygm_libretro.a",
                        self.root / "build" / self.platform / "libanygm_runtime.a"):
            members = subprocess.check_output(["ar", "t", str(archive)], text=True, timeout=10)
            self.assertEqual(members.splitlines(), ["probe.o", "probe_cpp.o"])
        self.assertEqual(self.build(), "")
        self.assertEqual(self.build(goal="runtime"), "")


if __name__ == "__main__":
    unittest.main()
