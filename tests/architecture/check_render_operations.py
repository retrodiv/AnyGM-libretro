#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Check the neutral renderer catalogue against both private executor ledgers."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "src/video/renderer"
CATALOGUE = RENDERER / "gml_render_operations.def"
CPU_TABLE = RENDERER / "cpu/gml_render_cpu_operations.def"
GPU_TABLE = RENDERER / "gpu/gml_render_gpu_operations.def"
FAMILIES = {"TEXTURE", "PRIMITIVE", "SURFACE", "EFFECT", "PRESENTATION", "SHADER", "SOFTWARE3D"}
PAIR_SUFFIX = {
    "TEXTURED_QUAD": "draw_textured_quad",
    "TEXTURED_TRIANGLES": "draw_textured_triangles",
    "COLOURED_PRIMITIVE": "draw_primitive",
    "CLEAR": "clear",
    "COPY_IMAGE": "copy_image",
    "COMPOSE_SURFACE": "compose_surface",
    "LAYER_EFFECT": "apply_effect",
    "PRESENT_IMAGE": "present_image",
}
CATALOGUE_ROW = re.compile(r"GML_RENDER_OPERATION\(([A-Z][A-Z0-9_]*), ([A-Z][A-Z0-9_]*)\)")
EXECUTOR_ROW = re.compile(
    r'GML_RENDER_(CPU|GPU)_OPERATION\(([A-Z][A-Z0-9_]*), '
    r'(supported|fallback|planned), ([a-z][a-z0-9_]*), "([^"\n]+)"\)'
)


def rows(path: Path, pattern: re.Pattern[str]) -> dict[str, tuple[str, ...]]:
    result: dict[str, tuple[str, ...]] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("/*") or line.startswith("*") or line.startswith("*/"):
            continue
        match = pattern.fullmatch(line)
        if not match:
            raise ValueError(f"{path}:{line_number}: invalid operation row")
        fields = match.groups()
        key = fields[1] if pattern is EXECUTOR_ROW else fields[0]
        if key in result:
            raise ValueError(f"{path}:{line_number}: duplicate {key}")
        result[key] = fields
    return result


def check_ids(catalogue: dict[str, tuple[str, ...]],
              cpu: dict[str, tuple[str, ...]],
              gpu: dict[str, tuple[str, ...]]) -> None:
    if not catalogue:
        raise ValueError("empty neutral operation catalogue")
    for key, (_, family) in catalogue.items():
        if family not in FAMILIES:
            raise ValueError(f"{key}: unknown family {family}")
    for name, table in (("CPU", cpu), ("GPU", gpu)):
        missing = sorted(catalogue.keys() - table.keys())
        extra = sorted(table.keys() - catalogue.keys())
        if missing or extra:
            raise ValueError(f"{name} operation coverage: missing={missing}, extra={extra}")


def definitions(root: Path) -> tuple[dict[str, list[Path]], dict[str, list[Path]]]:
    cpu: dict[str, list[Path]] = {}
    gpu: dict[str, list[Path]] = {}
    for subtree in (root / "src/video/renderer", root / "src/video/software3d"):
        for path in subtree.rglob("*.c"):
            source = path.read_text(encoding="utf-8")
            for match in re.finditer(r"^\s*(?:static\s+)?(?:void|int)\s+([a-z][a-z0-9_]*)\s*\(",
                                     source, re.MULTILINE):
                symbol = match.group(1)
                target = gpu if "/renderer/gpu/opengl/" in path.as_posix() else cpu
                target.setdefault(symbol, []).append(path)
    return cpu, gpu


def check_symbols(cpu_table: dict[str, tuple[str, ...]],
                  gpu_table: dict[str, tuple[str, ...]],
                  cpu_defs: dict[str, list[Path]],
                  gpu_defs: dict[str, list[Path]], require_delivered: bool) -> None:
    for key, (_, _, status, symbol, reason) in cpu_table.items():
        if status == "planned":
            raise ValueError(f"{key}: CPU cannot be planned at the CPU milestone")
        if len(cpu_defs.get(symbol, [])) != 1:
            raise ValueError(f"{key}: CPU {status} anchor {symbol} must have one definition")
        if not reason.strip():
            raise ValueError(f"{key}: missing CPU classification reason")
    for key, (_, _, status, symbol, reason) in gpu_table.items():
        if not reason.strip():
            raise ValueError(f"{key}: missing GPU classification reason")
        if key in PAIR_SUFFIX and status != "fallback":
            expected = "gml_render_gpu_opengl_" + PAIR_SUFFIX[key]
            if symbol != expected:
                raise ValueError(f"{key}: GPU entry must be named {expected}")
        if status == "planned":
            if require_delivered:
                raise ValueError(f"{key}: planned GPU entry cannot satisfy delivery")
            if symbol in cpu_defs or symbol in gpu_defs:
                raise ValueError(f"{key}: planned GPU entry {symbol} has a premature definition")
        elif status == "supported":
            if len(gpu_defs.get(symbol, [])) != 1:
                raise ValueError(f"{key}: supported GPU entry {symbol} must have one OpenGL definition")
        elif status == "fallback":
            if len(cpu_defs.get(symbol, [])) != 1:
                raise ValueError(f"{key}: GPU fallback {symbol} must name one real CPU checkpoint")


def self_test() -> None:
    sample = {"CLEAR": ("CLEAR", "PRIMITIVE")}
    cpu = {"CLEAR": ("CPU", "CLEAR", "supported", "gml_render_clear", "existing clear")}
    gpu = {"CLEAR": ("GPU", "CLEAR", "planned", "gml_render_gpu_opengl_clear", "later")}
    check_ids(sample, cpu, gpu)
    check_symbols(cpu, gpu, {"gml_render_clear": [Path("cpu.c")]}, {}, False)
    for broken in (
        lambda: check_ids(sample, {}, gpu),
        lambda: check_symbols(cpu, gpu, {}, {}, False),
        lambda: check_symbols(cpu, gpu, {"gml_render_clear": [Path("cpu.c")]}, {}, True),
        lambda: check_symbols(cpu, gpu, {"gml_render_clear": [Path("cpu.c")]},
                              {"gml_render_gpu_opengl_clear": [Path("gpu.c")]}, False),
    ):
        try:
            broken()
        except ValueError:
            continue
        raise ValueError("render operation negative probe unexpectedly passed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-delivered", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    try:
        if args.self_test:
            self_test()
        catalogue = rows(CATALOGUE, CATALOGUE_ROW)
        cpu = rows(CPU_TABLE, EXECUTOR_ROW)
        gpu = rows(GPU_TABLE, EXECUTOR_ROW)
        check_ids(catalogue, cpu, gpu)
        cpu_defs, gpu_defs = definitions(ROOT)
        check_symbols(cpu, gpu, cpu_defs, gpu_defs, args.require_delivered)
    except (OSError, ValueError) as error:
        parser.exit(1, f"render operations: {error}\n")
    print(f"render operations: ok ({len(catalogue)} IDs, "
          f"{sum(value[2] == 'planned' for value in gpu.values())} GPU planned)")


if __name__ == "__main__":
    main()
