#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Keep this project's version in one place, and keep publishable metadata free of a number.

Every surface that showed the version used to carry the number by hand, so each copy could promise a
value that no longer existed: the runtime string a frontend displays, the core-info metadata in this
repository, and the metadata staged in the tools repository for the libretro build infrastructure.
The version is now written once, as ``ANYGM_VERSION`` in the public header ``src/api/anygm.h``; the
adapter reports that macro, publishable metadata declares the literal ``Git``, and
``tools/bump-version.sh`` is the one documented step that increments it.

Each rule below refuses a failure with a consequence, not untidiness:

* a value that is not a plain three-component version -- a patch component with a leading zero is a
  different number to whoever parses it next, and a two-component value has nothing to increment;
* a second copy of the series anywhere in the tree -- it goes stale the moment the source moves, and
  it is the copy somebody reads;
* publishable metadata that names a number instead of ``Git``, or that puts the core back into the
  experimental list the downloader hides by default;
* a bump script that fails to increment, or that accepts a malformed value. It runs on a copy of the
  header, so the check never moves the tree it inspects;
* the shipped ``.githooks/pre-commit`` allowing a commit that keeps the previous version, or refusing
  the one that declares it. That hook is exercised in a throwaway repository.

    check_version_policy.py                 validate the source, the surfaces, and the bump step
    check_version_policy.py --bump-since R  require the version to have advanced since git ref R,
                                            which is the evidence integrating a range has to show
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = Path("src/api/anygm.h")
METADATA = Path("anygm_libretro.info")
ADAPTER = Path("src/adapters/libretro/libretro_entry.c")
BUMP = Path("tools/bump-version.sh")
HOOK = Path(".githooks/pre-commit")
EXCLUDED_PREFIXES = ("src/third_party/",)
DEFINE = re.compile(r'^#define ANYGM_VERSION "([^"]*)"', re.MULTILINE)
COMPONENT = re.compile(r"(?:0|[1-9][0-9]*)\Z")


class Failure(Exception):
    """A rule this check exists to enforce."""


def read_define(text: str, source: str) -> str:
    values = DEFINE.findall(text)
    if len(values) != 1:
        raise Failure(f"{source}: expected one ANYGM_VERSION definition, found {len(values)}")
    return values[0]


def parse_version(value: str) -> tuple[int, int, int]:
    parts = value.split(".")
    if len(parts) != 3 or not all(COMPONENT.fullmatch(part) for part in parts):
        raise Failure(
            f"version {value!r} is not three components without leading zeros (9 is followed by 10)"
        )
    return int(parts[0]), int(parts[1]), int(parts[2])


def repository_files() -> list[Path]:
    try:
        # Tracked files, plus files that are new and not ignored: a version written by hand should
        # fail before it is committed rather than after.
        listing = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        # A source drop without history still has to pass, and it walks the same exclusions the
        # staged build products and the vendored trees need.
        return [
            path
            for path in sorted(ROOT.rglob("*"))
            if path.is_file()
            and not path.relative_to(ROOT)
            .as_posix()
            .startswith(("build/", "tmp/", *EXCLUDED_PREFIXES))
        ]
    return [ROOT / name for name in listing.split("\0") if name]


def stray_copies(version: tuple[int, int, int]) -> list[str]:
    """Name every first-party line that writes this series' patch number by hand."""

    series = re.compile(rf"(?<![0-9]){version[0]}\.{version[1]}\.([0-9]+)(?![0-9])")
    found: list[str] = []
    for path in repository_files():
        relative = path.relative_to(ROOT).as_posix()
        if relative == HEADER.as_posix() or relative.startswith(EXCLUDED_PREFIXES):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for number, line in enumerate(text.splitlines(), 1):
            if series.search(line):
                found.append(f"{relative}:{number}: {line.strip()}")
    return found


def metadata_value(text: str, key: str, source: str) -> str:
    match = re.search(rf'^{key}\s*=\s*"([^"]*)"', text, re.MULTILINE)
    if not match:
        raise Failure(f"{source}: no {key} declaration")
    return match.group(1)


def bump(header: Path) -> subprocess.CompletedProcess[str]:
    command = [str(ROOT / BUMP), str(header)]
    try:
        return subprocess.run(command, capture_output=True, text=True)
    except OSError:
        return subprocess.run(["sh", *command], capture_output=True, text=True)


def prove_bump_step(version: tuple[int, int, int]) -> None:
    """Exercise the one documented increment on copies, and leave the tree where it was."""

    serial = f"{version[0]}.{version[1]}"
    header = ROOT / HEADER
    original = header.read_text(encoding="utf-8")

    def replace(value: str) -> str:
        return DEFINE.sub(f'#define ANYGM_VERSION "{value}"', original, count=1)

    def copy_of(text: str) -> Path:
        handle = tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", suffix=".h", delete=False, prefix="anygm-version-"
        )
        handle.write(text)
        handle.close()
        return Path(handle.name)

    created: list[Path] = []
    try:
        incremented = f"{version[0]}.{version[1]}.{version[2] + 1}"
        subject = copy_of(original)
        created.append(subject)
        done = bump(subject)
        if done.returncode != 0 or incremented not in done.stdout:
            raise Failure(
                f"bump step does not report the new value on the current version: "
                f"{done.stdout.strip()} {done.stderr.strip()}".strip()
            )
        if read_define(subject.read_text(encoding="utf-8"), "bumped copy") != incremented:
            raise Failure(f"bump step does not write {incremented}")

        subject = copy_of(replace(f"{serial}.9"))
        created.append(subject)
        done = bump(subject)
        if done.returncode != 0 or read_define(
            subject.read_text(encoding="utf-8"), "bumped copy"
        ) != f"{serial}.10":
            raise Failure("bump step does not carry 9 to 10 without a leading zero")

        for malformed in (f"{serial}.09", serial, f"{serial}.0.1", "01.1.0"):
            subject = copy_of(replace(malformed))
            created.append(subject)
            before = subject.read_text(encoding="utf-8")
            done = bump(subject)
            if done.returncode == 0:
                raise Failure(f"bump step accepted the malformed version {malformed!r}")
            if subject.read_text(encoding="utf-8") != before:
                raise Failure(f"bump step wrote before refusing the malformed version {malformed!r}")

        if header.read_text(encoding="utf-8") != original:
            raise Failure("the check moved the version header it was inspecting")
    finally:
        for path in created:
            path.unlink(missing_ok=True)


def prove_hook(version: tuple[int, int, int]) -> None:
    """Exercise the shipped hook in a throwaway repository.

    The hook is what makes the policy hold at commit time rather than at review time, so it is worth
    more than a promise: a commit that keeps the previous version must be refused, the commit that
    declares the version must pass, and a version that moved must pass.
    """

    hook = ROOT / HOOK
    if not hook.is_file():
        raise Failure(f"{HOOK}: the commit hook that enforces the increment is missing")
    environment = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "HOME": os.environ.get("HOME", "/var/tmp"),
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_SYSTEM": os.devnull,
        "GIT_AUTHOR_NAME": "version policy check",
        "GIT_AUTHOR_EMAIL": "check@anygm.invalid",
        "GIT_COMMITTER_NAME": "version policy check",
        "GIT_COMMITTER_EMAIL": "check@anygm.invalid",
    }

    def git(repository: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(repository), *arguments],
            capture_output=True,
            text=True,
            env=environment,
        )

    with tempfile.TemporaryDirectory(prefix="anygm-hook-") as directory:
        repository = Path(directory)
        (repository / "src/api").mkdir(parents=True)
        (repository / HEADER).write_text(
            f'#define ANYGM_VERSION "{version[0]}.{version[1]}.{version[2]}"\n', encoding="utf-8"
        )
        (repository / "notes.txt").write_text("a change\n", encoding="utf-8")
        if git(repository, "init", "-q", "-b", "main").returncode != 0:
            raise Failure("the hook check could not create its throwaway repository")
        hooks = repository / ".git/hooks"
        hooks.mkdir(parents=True, exist_ok=True)
        shutil.copy2(hook, hooks / "pre-commit")

        def commit(message: str) -> subprocess.CompletedProcess[str]:
            git(repository, "add", "-A")
            return git(repository, "commit", "-q", "-m", message)

        if commit("declare the version").returncode != 0:
            raise Failure("the hook refused the commit that declares the version")

        (repository / "notes.txt").write_text("a change\nand another\n", encoding="utf-8")
        refused = commit("change something and keep the version")
        if refused.returncode == 0:
            raise Failure("the hook allowed a commit that kept the previous version")
        if "bump-version.sh" not in refused.stderr:
            raise Failure("the hook refused without naming the increment step")

        bumped = subprocess.run(
            [str(ROOT / BUMP), str(repository / HEADER)], capture_output=True, text=True
        )
        if bumped.returncode != 0:
            raise Failure(f"the bump step failed inside the hook check: {bumped.stderr.strip()}")
        if commit("change something and move the version").returncode != 0:
            raise Failure("the hook refused a commit that moved the version")


def version_at(reference: str) -> str | None:
    """The version at ``reference``, or None when that revision predates the version header."""

    def git(*arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(ROOT), *arguments], capture_output=True, text=True
        )

    try:
        if git("rev-parse", "--verify", "--quiet", f"{reference}^{{commit}}").returncode != 0:
            raise Failure(f"{reference} is not a git commit in this checkout")
    except OSError as error:
        raise Failure(f"cannot read {reference}: {error}") from error
    shown = git("show", f"{reference}:{HEADER.as_posix()}")
    if shown.returncode != 0:
        # The range under review introduced the header, so there is no earlier value to compare.
        return None
    return read_define(shown.stdout, f"{reference}:{HEADER.as_posix()}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Validate the single-source project version and the publishable metadata."
    )
    parser.add_argument(
        "--bump-since",
        metavar="REF",
        help="git ref the version must have advanced beyond, compared numerically",
    )
    arguments = parser.parse_args(argv)

    failures: list[str] = []
    value = "unreadable"
    try:
        value = read_define((ROOT / HEADER).read_text(encoding="utf-8"), HEADER.as_posix())
        version = parse_version(value)

        info = (ROOT / METADATA).read_text(encoding="utf-8")
        declared = metadata_value(info, "display_version", METADATA.as_posix())
        if declared != "Git":
            failures.append(
                f'{METADATA}: display_version is {declared!r}, and publishable metadata says "Git"'
            )
        experimental = metadata_value(info, "is_experimental", METADATA.as_posix())
        if experimental != "false":
            failures.append(
                f"{METADATA}: is_experimental is {experimental!r}, which hides the core by default"
            )

        adapter = (ROOT / ADAPTER).read_text(encoding="utf-8")
        if "info->library_version=ANYGM_VERSION;" not in adapter:
            failures.append(f"{ADAPTER}: the runtime version is not the single-source macro")

        failures.extend(stray_copies(version))
        prove_bump_step(version)
        prove_hook(version)

        if arguments.bump_since:
            base = version_at(arguments.bump_since)
            if base is not None and parse_version(base) >= version:
                failures.append(
                    f"version {value} has not advanced beyond {base} at {arguments.bump_since}"
                )
    except Failure as failure:
        failures.append(str(failure))

    if failures:
        for failure in failures:
            print(f"version-policy: {failure}", file=sys.stderr)
        return 1
    print(f"version policy: {value} in {HEADER}; metadata says Git; the bump step holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
