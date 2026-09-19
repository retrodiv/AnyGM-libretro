#!/usr/bin/env python3
"""Run a command in a bounded Linux cgroup, including every process it starts.

A build or a hostile-input suite can reserve far more memory than the host has, and the kernel
answers that with a machine-wide out-of-memory stall rather than with a failure of the command that
asked for it. This wrapper puts the whole process tree under an enforced memory ceiling, removes
swap from it and stops every descendant when the wall clock runs out, so the worst case is a killed
command with a clear reason.

    python3 tools/run_guarded.py --memory-mib 6144 --seconds 1800 -- make -j4 platform=osx

The limits are established by the systemd user manager and then re-checked from inside the cgroup
before the command starts, so a host where the guard cannot apply them fails instead of running
unbounded. Suites that must never run unguarded import require_limits() as their entry check.

SPDX-License-Identifier: MIT
Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

GUARDED_ENVIRONMENT = "ANYGM_GUARDED"
MAX_TASKS_DEFAULT = 128
MIN_MEMORY_MIB = 64


def require_limits(max_memory_mib: int = 1024) -> None:
    """Fail closed before a guarded workload starts, including under test discovery."""

    if sys.platform != "linux" or os.environ.get(GUARDED_ENVIRONMENT) != "1":
        raise RuntimeError("Run this command through tools/run_guarded.py")
    group = next(
        (
            line[3:]
            for line in Path("/proc/self/cgroup").read_text(encoding="utf-8").splitlines()
            if line.startswith("0::/")
        ),
        None,
    )
    if group is None:
        raise RuntimeError("A cgroup v2 memory limit is required")
    root = Path("/sys/fs/cgroup").resolve()
    folder = (root / group.lstrip("/")).resolve()
    folder.relative_to(root)
    memory = (folder / "memory.max").read_text(encoding="utf-8").strip()
    swap = (folder / "memory.swap.max").read_text(encoding="utf-8").strip()
    tasks = (folder / "pids.max").read_text(encoding="utf-8").strip()
    if (
        memory == "max"
        or int(memory) > max_memory_mib * 1024 * 1024
        or swap != "0"
        or tasks == "max"
        or int(tasks) > MAX_TASKS_DEFAULT
    ):
        raise RuntimeError("Missing or excessive cgroup limits; use tools/run_guarded.py")


def _stop_unit(unit: str) -> None:
    subprocess.run(
        ["systemctl", "--user", "stop", unit],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=10,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run a command in a bounded Linux cgroup, including every process it starts."
    )
    parser.add_argument("--memory-mib", type=int, default=1024)
    parser.add_argument("--seconds", type=int, default=180)
    parser.add_argument("--tasks", type=int, default=MAX_TASKS_DEFAULT)
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command or args.memory_mib < MIN_MEMORY_MIB or args.seconds < 1 or args.tasks < 1:
        parser.error(
            "provide a command, at least %d MiB, a positive timeout and a positive task limit"
            % MIN_MEMORY_MIB
        )
    if args.child:
        os.environ[GUARDED_ENVIRONMENT] = "1"
        os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
        require_limits(args.memory_mib)
        os.execvp(command[0], command)
    if sys.platform != "linux" or not shutil.which("systemd-run"):
        parser.error("Linux cgroup v2 and a working systemd user manager are required; no unlimited fallback")
    unit = "anygm-guard-" + uuid.uuid4().hex + ".scope"
    invocation = [
        "systemd-run",
        "--user",
        "--scope",
        "--quiet",
        "--unit=" + unit,
        "-p",
        "MemoryMax=%dM" % args.memory_mib,
        "-p",
        "MemorySwapMax=0",
        "-p",
        "OOMPolicy=kill",
        "-p",
        "TasksMax=%d" % args.tasks,
        "-p",
        "RuntimeMaxSec=%ds" % args.seconds,
        sys.executable,
        str(Path(__file__).resolve()),
        "--child",
        "--memory-mib",
        str(args.memory_mib),
        "--seconds",
        str(args.seconds),
        "--tasks",
        str(args.tasks),
        "--",
    ] + command
    print(
        "Guard: %d MiB total, no swap, %ds, <=%d tasks" % (args.memory_mib, args.seconds, args.tasks),
        flush=True,
    )
    try:
        result = subprocess.run(invocation, timeout=args.seconds + 5)
        if result.returncode < 0:
            print(
                "Guard: stopped the process tree (signal %d)" % -result.returncode,
                file=sys.stderr,
            )
        return result.returncode if result.returncode >= 0 else 128 - result.returncode
    except subprocess.TimeoutExpired:
        print("Guard timeout; stopping the entire process tree", file=sys.stderr)
        return 124
    except KeyboardInterrupt:
        print("Guard interrupted; stopping the entire process tree", file=sys.stderr)
        return 130
    finally:
        _stop_unit(unit)


if __name__ == "__main__":
    sys.exit(main())
