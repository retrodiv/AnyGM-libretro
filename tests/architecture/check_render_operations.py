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
BACKEND_MAP = ROOT / "docs/RENDERER_BACKENDS.md"
FAMILIES = {"TEXTURE", "PRIMITIVE", "SURFACE", "EFFECT", "PRESENTATION", "SHADER", "SOFTWARE3D"}
REQUIRED_RULES = {
    "R03-P3-JOURNAL", "R03-P3-CLEAR", "R03-P3-EXACT-OVERLAP",
    "R03-P3-RESIDENT-COPY", "R03-P3-SHADER-CHECKPOINT", "R03-P7-CONTENT-SURFACE",
    "R03-P7-LAYER-TINT-COLOURISE", "R03-P7-LAYER-ZOOM-BLUR",
    "R03-P7-LAYER-INLINE-CPU", "R03-P7-LAYER-RGB-NOISE",
    "R03-P7-LAYER-SAMPLER-FALLBACK", "R03-P7-LAYER-BLUR-GLOW-FALLBACK",
    "R03-P7-LAYER-NUMBERED-TARGET",
    "R03-P3-ATLAS-CACHE", "R03-P3-CONTEXT-RESET",
    "R03-P5-MUTABLE-IDENTITY", "R03-P5-RUNTIME-FONT-ATLAS", "R03-P5-SURFACE-OBSERVER",
    "R03-P5-SURFACE-COPY", "R03-P5-SURFACE-CPU-SOURCE",
    "R03-P5-TARGET-PIXEL", "R03-P5-ENGINE-OWNED-READ",
    "R03-P5-SURFACE-RESIZE", "R03-P5-SURFACE-FREE",
    "R03-P5-SURFACE-PART", "R03-P5-SURFACE-STRETCHED",
    "R03-P5-SURFACE-ROTATED",
    "R03-TEXT-SPRITE-PACKAGED", "R03-TEXT-REAL-PACKAGED",
    "R03-TEXT-PACKAGED-INTEGER-SCALE", "R03-TEXT-PACKAGED-TINT",
    "R03-TEXT-PACKAGED-PARTIAL-ALPHA",
    "R03-TEXT-PACKAGED-ROTATION-FALLBACK",
    "R03-TEXT-RUNTIME-PAGE",
    "R03-TEXT-RUNTIME-SPRITE-FALLBACK", "R03-TEXT-SDF-FALLBACK",
    "R03-PRIM-FLAT", "R03-PRIM-LAYER-FILL", "R03-PRIM-GRADIENT-FALLBACK", "R03-PRIM-SPANS",
    "R03-PRIM-LINES", "R03-PRIM-OUTLINES",
    "R03-PRIM-PARTICLE-BRIDGE", "R03-PRIM-TRI-OUTLINE",
    "R03-P5-FAULT-LIMIT",
    "R03-P5-QUEUE-RETRY",
    "R03-P3-OPAQUE-ELISION",
    "R03-SPR-NEAREST", "R03-SPR-NEAREST-SOURCEOVER",
    "R03-SPR-FIRSTGEN-NEAREST-REDUCE",
    "R03-SPR-MODERN-NEAREST",
    "R03-SPR-FILTER-TRIM", "R03-SPR-ROTATE-GENERAL",
    "R03-SPR-CORNER-COLOUR", "R03-SPR-POSITIONED-AXIS",
    "R03-SPR-POSITIONED-QUAD", "R03-SPR-STRETCH-MAG",
    "R03-SPR-STRETCH-BOX", "R03-SPR-COARSE-MIP", "R03-SPR-TILED-PHASE", "R03-SPR-LAYER-UNTILED",
    "R03-SPR-LAYER-REPEAT",
    "R03-BG-DIRECT", "R03-BG-NEAREST-PHASED", "R03-BG-DIRECT-NEAREST-COPY",
    "R03-BG-ADDITIVE",
    "R03-BG-REPEAT", "R03-TILE-CELL", "R03-SCENE-ORDER",
    "R03-ALPHA-NOOP", "R03-RESIDENT-PRESENT",
}
MAP_ANCHOR = re.compile(r"`([a-zA-Z0-9_./-]+)::([a-zA-Z_][a-zA-Z0-9_]*)`")
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


def check_repair_map(document: str, gpu_table: dict[str, tuple[str, ...]],
                     root: Path) -> None:
    """Check only the map's structure and live anchors, not pixel equivalence."""
    support = dict(re.findall(r"(?m)^\| `([A-Z][A-Z0-9_]*)` \| `(supported|fallback|planned)` \|",
                              document))
    if support.keys() != gpu_table.keys():
        raise ValueError("current renderer support table does not cover the GPU ledger")
    for operation, status in support.items():
        if status != gpu_table[operation][2]:
            raise ValueError(f"{operation}: current support table disagrees with GPU ledger")
    start = "<!-- R03-RULES:START -->"
    end = "<!-- R03-RULES:END -->"
    if document.count(start) != 1 or document.count(end) != 1:
        raise ValueError("R03 repair-level map markers are missing or repeated")
    section = document.split(start, 1)[1].split(end, 1)[0]
    rules: dict[str, list[str]] = {}
    for line in section.splitlines():
        if not line.startswith("| R03-"):
            continue
        fields = [field.strip() for field in line.strip().strip("|").split("|")]
        if len(fields) != 8 or not all(fields):
            raise ValueError("R03 map row has missing required fields")
        rule = fields[0]
        if rule in rules:
            raise ValueError(f"{rule}: duplicate R03 map row")
        rules[rule] = fields
        for index in (2, 3, 4, 5, 7):
            anchors = MAP_ANCHOR.findall(fields[index])
            if not anchors and not (index == 5 and fields[index].startswith("No shader")) \
                    and not (index == 7 and "Core comparison pending" in fields[index]):
                raise ValueError(f"{rule}: missing path::symbol anchor in field {index}")
            for relative, symbol in anchors:
                if ".." in Path(relative).parts or relative.startswith("/"):
                    raise ValueError(f"{rule}: invalid anchor path {relative}")
                path = root / relative if relative.startswith(("tests/", "src/")) else \
                    root / "src/video/renderer" / relative
                if not path.is_file():
                    raise ValueError(f"{rule}: stale anchor file {relative}")
                source = path.read_text(encoding="utf-8")
                if path.suffix == ".c":
                    found = re.search(
                        rf"(?m)^\s*(?:static\s+)?(?:[A-Za-z_][A-Za-z_0-9]*\s+)+"
                        rf"\**{re.escape(symbol)}\s*\(", source)
                else:
                    found = re.search(rf"\b{re.escape(symbol)}\b", source)
                if not found:
                    raise ValueError(f"{rule}: stale anchor symbol {relative}::{symbol}")
        if f"device comparison {rule}" not in fields[7] and \
           f"core-only comparison {rule}" not in fields[7]:
            raise ValueError(f"{rule}: missing comparative case ID")
    if rules.keys() != REQUIRED_RULES:
        raise ValueError(f"R03 rule coverage: missing={sorted(REQUIRED_RULES - rules.keys())}, "
                         f"extra={sorted(rules.keys() - REQUIRED_RULES)}")


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
    document = BACKEND_MAP.read_text(encoding="utf-8")
    ledger = rows(GPU_TABLE, EXECUTOR_ROW)
    check_repair_map(document, ledger, ROOT)
    for broken in (
        document.replace("::resident_filtered_sprite", "::obsolete_filtered_sprite", 1),
        document.replace("::R03_SHADER_EXACT_OVERLAP", "::OBSOLETE_EXACT_OVERLAP", 1),
        document.replace("::R03_SHADER_NEAREST_PHASED", "::OBSOLETE_NEAREST_PHASED", 1),
        document.replace("device comparison R03-P3-JOURNAL", "comparison missing", 1),
        document.replace("`supported` | Only the bounded", "`planned` | Only the bounded", 1),
        document.replace(
            "`common/gml_render_sprites.c::gml_render_sprite_pos_geometry`; "
            "`common/gml_render_sprites.c::resident_positioned_quad_sprite`",
            "", 1),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-SPR-STRETCH-BOX |")),
    ):
        try:
            check_repair_map(broken, ledger, ROOT)
        except ValueError:
            continue
        raise ValueError("R03 repair-map negative probe unexpectedly passed")


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
        check_repair_map(BACKEND_MAP.read_text(encoding="utf-8"), gpu, ROOT)
    except (OSError, ValueError) as error:
        parser.exit(1, f"render operations: {error}\n")
    print(f"render operations: ok ({len(catalogue)} IDs, "
          f"{sum(value[2] == 'planned' for value in gpu.values())} GPU planned)")


if __name__ == "__main__":
    main()
