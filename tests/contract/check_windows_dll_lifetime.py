#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Run explicit native-path MinGW DLL controls without content or a frontend."""

import argparse
import json
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--negative", required=True)
    parser.add_argument("--core", required=True)
    parser.add_argument("--runner", action="append", default=[])
    args = parser.parse_args()
    failures = 0
    for name, image, expected in (("negative", args.negative, 86), ("core", args.core, 0)):
        for mode in ("unload", "exit"):
            try:
                result = subprocess.run(
                    [*args.runner, args.probe, mode, image],
                    capture_output=True, text=True, timeout=15, check=False,
                )
                output = result.stdout + result.stderr
                passed = (
                    result.returncode == expected
                    and "positive-tls-reuse" in output
                    and "free-tls-array" in output
                    and ("FAIL-read-after-free" in output) == (expected == 86)
                    and (expected == 86 or "main-return" in output)
                )
                print(json.dumps({"image": name, "mode": mode, "passed": passed,
                                  "exit_code": result.returncode, "expected_exit": expected,
                                  "stdout": result.stdout, "stderr": result.stderr}), flush=True)
            except (OSError, subprocess.TimeoutExpired) as error:
                passed = False
                print(json.dumps({"image": name, "mode": mode, "passed": False,
                                  "error": str(error)}), flush=True)
            failures += not passed
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
