#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Check the neutral renderer catalogue against both private executor ledgers."""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path

import check_builtin_registry as builtin_registry


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
    "R03-P7-CONTENT-GLOBAL-INIT",
    "R03-SURFACE-THRESHOLD-PALETTE",
    "R03-P7-LAYER-TINT-COLOURISE", "R03-P7-LAYER-ZOOM-BLUR",
    "R03-P7-LAYER-INLINE-CPU", "R03-P7-LAYER-RGB-NOISE",
    "R03-P7-LAYER-CONTINUATION",
    "R03-P7-LAYER-SAMPLER-FALLBACK", "R03-P7-LAYER-BLUR-GLOW-FALLBACK",
    "R03-P7-LAYER-NUMBERED-TARGET", "R03-P7-VIEWLESS-BASE", "R03-P7-INDIRECT-GUI",
    "R03-P9-CLASSIC-VIEWLESS", "R03-P9-GUI-FILTERED-ATLAS",
    "R03-CLASSIC-BORROWED-VIEW", "R03-SPR-CLASSIC-NEAREST", "R03-SPR-ZERO-ALPHA-PART",
    "R03-CLASSIC-STEP-COMPOSITION",
    "R03-CLASSIC-IDENTITY-PRESENT",
    "R03-SPR-PART-OFFSCREEN-NOOP",
    "R03-SPR-PART-INTEGER-ENDPOINT",
    "R03-SPR-PART-ENDPOINT-SOURCE-TINT",
    "R03-LAYER-TRIMMED-UNIT",
    "R03-LAYER-CELLS-FUNCTION",
    "R03-SPR-MODERN-UNIT-PART",
    "R03-SPR-PART-FUNCTION",
    "R03-SPR-GUI-FILTERED-OFFSCREEN-NOOP",
    "R03-SPR-OUTSIDE-FUNCTION",
    "R03-SPR-ZERO-SCALE-NOOP",
    "R03-SPR-AXIS-TURN",
    "R03-SPR-FILTERED-EDGE-NOOP",
    "R03-SPR-PHYSICAL-FILTERED",
    "R03-SPR-CPU-PASSTHROUGH",
    "R03-P9-GUI-MAPPED-SPRITE", "R03-P9-STRETCHED-COVERED-ALPHA",
    "R03-SPR-GUI-MAPPED-STRETCH",
    "R03-SPR-REPLACEMENT-FUNCTION",
    "R03-SPR-NEAREST-CAPACITY",
    "R03-SPR-BLACK-ADDITIVE-NOOP",
    "R03-SPR-MODERN-SCREEN-FILTERED",
    "R03-SPR-MODERN-SCREEN-FILTERED-STORED",
    "R03-TEXT-MODERN-MAXIMISED-FILTERED",
    "R03-SPR-MODERN-SCREEN-FILTERED-TRIM", "R03-SCREEN-FILTERED-ZERO-ALPHA",
    "R03-P9-GUI-ROTATED-SPRITE", "R03-P9-SPRITE-ALPHA-BOUNDARY",
    "R03-P9-DEVICE-CHECKPOINT-SPAN",
    "R03-P9-NUMBERED-TARGET",
    "R03-P9-FIXED-ADDITIVE",
    "R03-P9-FIXED-BACKGROUND",
    "R03-P7-MULTIVIEW-SEED", "R03-P7-MULTIVIEW-RETAINED",
    "R03-P7-MULTIVIEW-SURFACE", "R03-P7-PRESENT-GEOMETRY",
    "R03-P7-PRESENT-FILTER-FALLBACK", "R03-P7-TEMPORAL-PRESENT",
    "R03-P9-RESIDENT-HOST-FIT", "R03-P7-INDIRECT-GUI-HOST-NEAREST",
    "R03-P7-APPLICATION-HOST-NEAREST",
    "R03-P7-COMPLETED-CURSOR",
    "R03-CPU-CURRENT-HOST-NEAREST",
    "R03-CPU-CURRENT-HOST-IDENTITY-CROP",
    "R03-RESIDENT-SCREEN-IDENTITY-CROP",
    "R03-PRESENT-INTEGER-SHIFT",
    "R03-P7-SCREEN-CPU-BORROW", "R03-P7-UNDERLAY-ALPHA-BORROW",
    "R03-RAW-LINEAR-BASE",
    "R03-EXTERNAL-ROOT-MASK",
    "R03-P3-ATLAS-CACHE", "R03-P3-CONTEXT-RESET",
    "R03-P5-MUTABLE-IDENTITY", "R03-P5-RUNTIME-FONT-ATLAS", "R03-P5-SURFACE-OBSERVER",
    "R03-P5-SURFACE-COPY", "R03-P5-SURFACE-CPU-SOURCE",
    "R03-P5-TARGET-PIXEL", "R03-P5-ENGINE-OWNED-READ",
    "R03-P5-SURFACE-RESIZE", "R03-P5-SURFACE-FREE",
    "R03-P5-SURFACE-PART", "R03-P5-SURFACE-STRETCHED",
    "R03-SURFACE-NEAREST-FUNCTION",
    "R03-SURFACE-CANVAS-FUNCTION",
    "R03-SURFACE-CLASSIC-INTEGER-SCREEN",
    "R03-P5-SURFACE-ROTATED",
    "R03-P5-GUI-FULLWIDTH", "R03-P5-GUI-OPAQUE-SUFFIX",
    "R03-P5-SURFACE-DESTINATION-UNIT",
    "R03-TEXT-SPRITE-PACKAGED", "R03-TEXT-REAL-PACKAGED",
    "R03-TEXT-PACKAGED-INTEGER-SCALE", "R03-TEXT-PACKAGED-TINT",
    "R03-TEXT-PACKAGED-PARTIAL-ALPHA",
    "R03-P9-FILTERED-REAL-TEXT", "R03-P9-IMMUTABLE-CHECKPOINT-SPAN",
    "R03-TEXT-FILTERED-CLIPPED",
    "R03-P9-INCREMENTAL-CHECKPOINT",
    "R03-P9-AUTOMATIC-UNDERLAY", "R03-P9-REPLAY-SOURCE",
    "R03-P9-DIRECT-AUTOMATIC-GUI",
    "R03-P9-RECTANGLE-NOOP",
    "R03-TEXT-PACKAGED-ROTATION-FALLBACK",
    "R03-TEXT-RUNTIME-PAGE",
    "R03-TEXT-RUNTIME-SPRITE-FALLBACK", "R03-TEXT-SDF-FALLBACK",
    "R03-PRIM-FLAT", "R03-PRIM-LAYER-FILL", "R03-PRIM-GRADIENT-FALLBACK", "R03-PRIM-SPANS",
    "R03-PRIM-LINES", "R03-PRIM-OUTLINES",
    "R03-PRIM-LINE-FUNCTION", "R03-PRIM-ADDITIVE-BYTES", "R03-PRIM-CIRCLE-FUNCTION",
    "R03-PRIM-PARTICLE-BRIDGE", "R03-PRIM-TRI-OUTLINE",
    "R03-PRIM-PARTICLE-LINE",
    "R03-PRIM-CLASSIC-INVERSE-RECT",
    "R03-PRIM-STUDIO-GUI-OPAQUE",
    "R03-PRIM-STUDIO-ALPHA-RECT",
    "R03-PRIM-FIRSTGEN-GUI-ALPHA-RECT",
    "R03-PRIM-CLASSIC-OWNED",
    "R03-PRIM-RECT-COALESCED",
    "R03-PRIM-RECTANGLE-MATRIX",
    "R03-P5-FAULT-LIMIT",
    "R03-P5-QUEUE-RETRY",
    "R03-P8-RUNTIME-SPRITE-BASE", "R03-P8-STATE-RETIRE",
    "R03-P3-OPAQUE-ELISION",
    "R03-SPR-NEAREST", "R03-SPR-NEAREST-SOURCEOVER", "R03-SPR-INTEGER-SOURCEOVER",
    "R03-SPR-GUI-REPLACE",
    "R03-SPR-INTEGER-FUNCTION",
    "R03-SPR-NEAREST-FUNCTION",
    "R03-SPR-FLAT-FOG-FUNCTION",
    "R03-EMPTY-BASE-RESET",
    "R03-GUI-UNIT-SOURCEOVER",
    "R03-TEXT-FIRSTGEN-GUI-TINTED-POINT",
    "R03-SPR-FIRSTGEN-GUI-HALF-POINT",
    "R03-SPR-CLASSIC-SIGNED-NOOP",
    "R03-SPR-CLASSIC-SCALED-NEAREST",
    "R03-SPR-CLASSIC-ROTATED-NOOP",
    "R03-SPR-RUNTIME-BINARY-UNIT",
    "R03-SPR-CLASSIC-OWNED-BINARY",
    "R03-SPR-FIRSTGEN-NEAREST-REDUCE", "R03-SPR-FIRSTGEN-NEAREST-MAG",
    "R03-SPR-MODERN-NEAREST",
    "R03-SPR-MODERN-SCREEN-POINT",
    "R03-SPR-MODERN-SCREEN-LARGE-AXIS",
    "R03-SPR-MODERN-SCREEN-BINARY-ROTATION",
    "R03-SPR-FIRSTGEN-SIGNED-POINT",
    "R03-SPR-NUMBERED-NEAREST",
    "R03-SPR-FILTER-TRIM", "R03-SPR-ROTATE-GENERAL",
    "R03-SPR-CORNER-COLOUR", "R03-SPR-POSITIONED-AXIS",
    "R03-SPR-POSITIONED-QUAD", "R03-SPR-STRETCH-MAG",
    "R03-SPR-STRETCH-BOX", "R03-SPR-COARSE-MIP", "R03-SPR-TILED-PHASE", "R03-SPR-TILED-FUNCTION", "R03-SPR-TILED-UNDERLAY", "R03-SPR-LAYER-UNTILED",
    "R03-SPR-STRETCH-FUNCTION",
    "R03-SPR-LAYER-REPEAT",
    "R03-BG-DIRECT", "R03-BG-NEAREST-PHASED", "R03-BG-DIRECT-NEAREST-COPY",
    "R03-BG-ADDITIVE", "R03-BG-CLIPPED-PART-NOOP",
    "R03-BG-UNIT-CLIPPED-NOOP",
    "R03-BG-CLASSIC-SCALED-NEAREST",
    "R03-BG-INTEGER-FUNCTION",
    "R03-BG-NEAREST-FUNCTION",
    "R03-BG-SELECTED-FUNCTION",
    "R03-TILE-NEAREST-FUNCTION",
    "R03-TEXT-NEAREST-FUNCTION",
    "R03-TEXT-OUTSIDE-FUNCTION",
    "R03-TEXT-ROTATED-NEAREST-FUNCTION",
    "R03-SPR-CLASSIC-ORTHO",
    "R03-TEXT-CLASSIC-ORTHO",
    "R03-BG-REPEAT", "R03-TILE-CELL", "R03-TILE-UNIT-SOURCE-CLIP", "R03-TILE-CLASSIC-UNIT",
    "R03-TILE-INTEGER-CLIP", "R03-TEXT-MAPPED-INTEGER", "R03-TEXT-MAPPED-FRACTIONAL", "R03-SPR-INTEGER-ROUNDED-NOOP", "R03-SCENE-ORDER",
    "R03-ALPHA-NOOP", "R03-SPR-EMPTY-NOOP", "R03-RESIDENT-PRESENT",
}
MAP_ANCHOR = re.compile(r"`([a-zA-Z0-9_./-]*)::([a-zA-Z_][a-zA-Z0-9_]*)`")
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

# These are actual typed work admission ports, not a successful execution claim.
# Stop traversal here: following resource replay would make CPU kernels appear
# GPU-capable merely because the resource owner also implements device submission.
DISPATCH_PORTS = {
    "gml_render_completed_screen_mask",
    "gml_render_application_scene_enqueue",
    "gml_render_application_scene_enqueue_quads",
    "gml_render_application_scene_enqueue_primitive",
    "gml_render_application_scene_enqueue_primitives",
    "gml_render_application_scene_submit",
    "gml_render_surface_scene_submit",
    "gml_render_resource_submit",
}
DISPATCH_PREFIXES = (
    "gml_draw_", "gml_render_primitive_", "gml_software3d_draw_",
    "gml_d3_draw_", "gml_surface_copy", "gml_render_try_", "gml_render_resident_", "resident_",
)
DISPATCH_CPU_BOUNDARIES = {
    "gml_render_maybe_prepare_draw", "gml_render_prepare_draw",
    "gml_render_flush_pending_fill", "gml_render_application_scene_flush",
    "gml_render_resident_application_cpu_barrier",
    "surface_pixels", "surface_pixels_observe", "surface_pixels_access",
}
DISPATCH_ADDITIONAL = {
    "gml_render_completed_screen_mask",
    "gml_render_layer_rgb_noise", "gml_render_layer_filter_end",
    "gml_render_sprite_ext_unmasked",
    "gml_render_compose_resident_underlay", "gml_render_recover_empty_screen",
    "render_present_first_generation", "render_present_content_shader",
    "gml_software3d_vertex_submit_buffer", "gml_software3d_model_draw",
}
DISPATCH_HELPERS = {
    "resident_sprite_program_blocks",
    "resident_zero_scale_sprite_noop",
    "resident_outside_sprite_noop", "gml_render_resident_sprite_outside",
    "resident_flat_eligible_rule", "resident_flat_eligible", "resident_line_bounds",
    "resident_rectangle_state_eligible", "resident_primitive_state_eligible", "resident_rectangle_payload",
    "resident_primitive_alpha_noop",
    "resident_classify_page", "resident_identity_screen_gui",
    "resident_tile_source_metadata",
    "resident_page_endpoint_rgb_mask",
    "resident_mapped_screen_gui_font", "resident_nearest_text_scale",
    "resident_text_glyph_outside",
    "resident_identity_gui_replacement",
    "resident_classic_rotated_sprite_noop",
    "resident_first_generation_rotated_point",
    "gml_render_resident_application_cpu_barrier",
}
DISPATCH_DEFERRED = {"render_present_first_generation", "render_present_content_shader"}
DISPATCH_DEVICE_EXTRA = {
    "gml_gpu_opengl_scene_submit", "gml_gpu_opengl_execute_shader_draw",
    "gml_gpu_opengl_scene_shader_draw", "gml_gpu_opengl_execute",
}
API_DRAW_PREFIXES = (
    "draw_", "action_draw_", "d3d_draw_", "surface_copy", "vertex_submit",
    "part_system_drawit",
)
API_NONPAINT = {
    "draw_enable_alphablend", "draw_enable_drawevent", "draw_getpixel", "draw_texture_flush",
}
API_CONTEXT_ADAPTERS = {
    "src/runtime/builtins/gml_builtin.c": ("d3_try_draw_2d_builtin",),
}
API_DIRECT_ADAPTER = "gml_builtin_call_fast_id_impl"
C_FUNCTION = re.compile(
    r"(?m)^[ \t]*(?:static\s+)?(?:[A-Za-z_]\w*\s+)+\**"
    r"([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{"
)
C_NONCODE = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
C_TOKENS = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|\w+|'
    r'>>=|<<=|\.\.\.|\+\+|--|->|<<|>>|<=|>=|==|!=|&&|\|\||'
    r'\+=|-=|\*=|/=|%=|&=|\|=|\^=|##|[^\s]'
)


def c_function_definitions(raw: str, label: str) -> dict[str, str]:
    source = C_NONCODE.sub(lambda match: re.sub(r"[^\n]", " ", match[0]), raw)
    result = {}
    for match in C_FUNCTION.finditer(source):
        if match[1] in {"if", "for", "while", "switch"}:
            continue
        depth = 1
        end = match.end()
        while end < len(source) and depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        if depth:
            raise ValueError(f"{label}: unclosed function {match[1]}")
        if match[1] in result:
            raise ValueError(f"{label}::{match[1]}: ambiguous dispatch definition")
        result[match[1]] = raw[match.start():end].strip()
    return result


def dispatch_functions(root: Path) -> dict[str, str]:
    """Read C definitions, ignoring braces/calls hidden in strings or comments."""
    result: dict[str, str] = {}
    for subtree in (root / "src/video/renderer", root / "src/video/software3d"):
        for path in sorted(subtree.rglob("*.c")):
            raw = path.read_text(encoding="utf-8")
            for symbol, body in c_function_definitions(raw, str(path)).items():
                result[f"{path.relative_to(root).as_posix()}::{symbol}"] = body
    return result


def dispatch_inventory(functions: dict[str, str]) -> dict[str, tuple[str, str]]:
    """Discover drawing facades and resident preparators, independently of rows.

    The call graph is deliberately conservative for duplicate private names.
    It proves only a possible typed submission, never state/variant admission.
    API adapters and packet/shader variant coverage are separate parity gates.
    """
    calls: dict[str, set[str]] = {}
    for key, body in functions.items():
        symbol = key.split("::")[1]
        code = C_NONCODE.sub(" ", body)
        destinations = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", code))
        destinations = {name for name in destinations if name not in DISPATCH_CPU_BOUNDARIES
                        and "_cpu_" not in name and not name.startswith("gml_render_completed_screen_")}
        calls.setdefault(symbol, set()).update(destinations)
    routes = {symbol: symbol for symbol in DISPATCH_PORTS}
    while True:
        added = {}
        for symbol, destinations in calls.items():
            if symbol not in routes:
                found = sorted(destinations & routes.keys())
                if found:
                    added[symbol] = routes[found[0]]
        if not added:
            break
        routes.update(added)
    inventory = {}
    for key, body in functions.items():
        symbol = key.split("::")[1]
        device = "/gpu/opengl/" in key and (symbol.startswith("gml_render_gpu_opengl_")
                                            or symbol in DISPATCH_DEVICE_EXTRA)
        if symbol.startswith(DISPATCH_PREFIXES) or symbol in DISPATCH_ADDITIONAL or device:
            uncommented = C_NONCODE.sub(
                lambda match: " " if match[0].startswith(("/*", "//")) else match[0], body)
            tokens = C_TOKENS.findall(uncommented)
            revision = hashlib.sha256(" ".join(tokens).encode()).hexdigest()
            inventory[key] = (f"DEVICE:{symbol}" if device else routes.get(symbol, "CPU"), revision)
    return inventory


def check_dispatch_inventory(document: str, inventory: dict[str, tuple[str, str]]) -> None:
    start, end = "<!-- D8-DISPATCH:START -->", "<!-- D8-DISPATCH:END -->"
    if document.count(start) != 1 or document.count(end) != 1:
        raise ValueError("D8 dispatch inventory markers are missing or repeated")
    section = document.split(start)[1].split(end)[0]
    entries = {}
    for line in section.splitlines():
        if not line.startswith("| `src/"):
            continue
        fields = [field.strip() for field in line.strip("|").split("|")]
        if len(fields) != 5 or not all(fields):
            raise ValueError("D8 dispatch inventory row requires five fields")
        point, route, status, revision, reason = fields
        point, route, revision = (value.strip("`") for value in (point, route, revision))
        if point in entries:
            raise ValueError(f"{point}: duplicate D8 dispatch row")
        symbol = point.split("::")[-1]
        if status not in {"PARTIAL", "DEBT", "HELPER", "DEFERRED", "DEVICE"}:
            raise ValueError(f"{point}: invalid D8 dispatch status")
        if status == "HELPER" and symbol not in DISPATCH_HELPERS:
            raise ValueError(f"{point}: a drawing dispatch cannot be classified as a helper")
        if status == "DEFERRED" and symbol not in DISPATCH_DEFERRED:
            raise ValueError(f"{point}: unknown deferred presentation adapter")
        if (status == "DEVICE") != route.startswith("DEVICE:"):
            raise ValueError(f"{point}: device executor status disagreement")
        if (route == "CPU") != (status in {"DEBT", "HELPER", "DEFERRED"}):
            raise ValueError(f"{point}: CPU debt/GPU admission status disagreement")
        if not reason.strip() or reason == "Pending":
            raise ValueError(f"{point}: D8 debt needs a reason")
        entries[point] = (route, revision)
    if entries.keys() != inventory.keys():
        raise ValueError(f"D8 dispatch coverage: missing={sorted(inventory.keys() - entries.keys())}, "
                         f"extra={sorted(entries.keys() - inventory.keys())}")
    for point, actual in inventory.items():
        if entries[point][0] != actual[0]:
            raise ValueError(f"{point}: D8 GPU route changed; update its debt row")
        if entries[point][1] != actual[1]:
            raise ValueError(f"{point}: D8 dispatch body changed; recheck variants and update its row")


def statement_end(tokens: list[builtin_registry.Token], start: int) -> int:
    """Read one C statement, including nested control statements and else arms."""
    if start >= len(tokens):
        raise ValueError("D8 API adapter has no statement")
    if tokens[start].value == "{":
        return builtin_registry.matching(tokens, start, "{", "}") + 1
    if tokens[start].value in {"if", "for", "while", "switch"}:
        if tokens[start + 1].value != "(":
            raise ValueError("D8 API control statement has no condition")
        condition_end = builtin_registry.matching(tokens, start + 1, "(", ")")
        end = statement_end(tokens, condition_end + 1)
        if tokens[start].value == "if" and end < len(tokens) and tokens[end].value == "else":
            end = statement_end(tokens, end + 1)
        return end
    cursor = start
    while cursor < len(tokens):
        value = tokens[cursor].value
        if value == ";":
            return cursor + 1
        if value in {"(", "[", "{"}:
            cursor = builtin_registry.matching(tokens, cursor, value,
                                                {"(": ")", "[": "]", "{": "}"}[value])
        cursor += 1
    raise ValueError("D8 API adapter has an unterminated statement")


def named_api_branches(data: bytes, symbol: str) -> dict[str, list[str]]:
    """Reuse the canonical builtin lexer for exact-name adapter branches."""
    tokens = builtin_registry.function_tokens(data, symbol)
    result: dict[str, list[str]] = {}
    for index, token in enumerate(tokens):
        if token.value != "if" or tokens[index + 1].value != "(":
            continue
        closing = builtin_registry.matching(tokens, index + 1, "(", ")")
        names = builtin_registry.compared_names(tokens[index + 2:closing], {"nm", "name"})
        if not names:
            continue
        end = statement_end(tokens, index)
        branch = data[token.start:tokens[end - 1].end].decode("utf-8")
        for name in names:
            result.setdefault(name, []).append(branch)
    return result


def direct_api_branches(data: bytes, symbol: str) -> dict[str, list[str]]:
    """Retain each direct-ID arm, including consecutive shared case labels."""
    tokens = builtin_registry.function_tokens(data, symbol)
    depths = []
    depth = 0
    for token in tokens:
        depths.append(depth)
        depth += (token.value == "{") - (token.value == "}")
    result: dict[str, list[str]] = {}
    for index, token in enumerate(tokens[:-2]):
        if token.value != "case" or not tokens[index + 1].value.startswith("BID_"):
            continue
        if tokens[index + 2].value != ":":
            raise ValueError("D8 direct API arm has an unexpected label")
        cursor = index + 3
        while cursor + 2 < len(tokens) and tokens[cursor].value == "case" \
                and tokens[cursor + 2].value == ":":
            cursor += 3
        end = cursor
        while end < len(tokens):
            if depths[end] < depths[index] or (depths[end] == depths[index]
                                              and tokens[end].value in {"case", "default"}):
                break
            end += 1
        if end == cursor:
            raise ValueError("D8 direct API arm has no implementation")
        branch = data[token.start:tokens[end - 1].end].decode("utf-8")
        result.setdefault(tokens[index + 1].value.removeprefix("BID_"), []).append(branch)
    return result


def drawing_calls(body: str) -> set[str]:
    names = re.findall(r"\b([A-Za-z_]\w*)\s*\(", C_NONCODE.sub(" ", body))
    return {name for name in names if name not in DISPATCH_CPU_BOUNDARIES
            and "_cpu_" not in name and not name.startswith((
                "gml_builtin_try_", "builtin_call", "gml_builtin_call", "gml_vm_",
                "gml_render_completed_screen_"))}


def literal_api_delegates(body: str) -> set[str]:
    """Resolve only explicit spellings in the builtin call's name argument."""
    tokens = builtin_registry.c_tokens(body.encode())
    names = set()
    for index, token in enumerate(tokens[:-1]):
        if token.value != "gml_builtin_call" or tokens[index + 1].value != "(":
            continue
        closing = builtin_registry.matching(tokens, index + 1, "(", ")")
        cursor = index + 2
        argument = 0
        while cursor < closing:
            value = tokens[cursor].value
            if value == ",":
                argument += 1
                if argument > 1:
                    break
            elif argument == 1 and tokens[cursor].kind == "string":
                names.add(builtin_registry.decode_c_string(value))
            elif value in {"(", "[", "{"}:
                cursor = builtin_registry.matching(tokens, cursor, value,
                                                    {"(": ")", "[": "]", "{": "}"}[value])
            cursor += 1
    return names


def api_dispatch_inventory(root: Path, renderer_functions: dict[str, str],
                           dispatch: dict[str, tuple[str, str]]) -> dict[str, tuple[str, ...]]:
    """Discover exact drawing spellings and their fast, slow and contextual arms.

    This supplements, rather than replaces, the canonical builtin registry check.
    A possible facade is not evidence that an API's entire variant space is GPU.
    """
    directory = root / "src/runtime/builtins"
    source = {path.relative_to(root).as_posix(): path.read_bytes()
              for path in sorted(directory.glob("*.c"))}
    functions = dict(renderer_functions)
    for path, data in source.items():
        for symbol, body in c_function_definitions(data.decode("utf-8"), path).items():
            functions[f"{path}::{symbol}"] = body
    for path in sorted((root / "src/runtime/particles").glob("*.c")):
        for symbol, body in c_function_definitions(path.read_text(), str(path)).items():
            functions[f"{path.relative_to(root).as_posix()}::{symbol}"] = body
    facade_routes = {key.split("::")[1]: {key.split("::")[1]} for key in dispatch
                     if key.split("::")[1] not in DISPATCH_HELPERS
                     and "/gpu/opengl/" not in key}
    facade_symbols = {key.split("::")[1] for key in dispatch}
    calls: dict[str, set[str]] = {}
    for key, body in functions.items():
        # A generic builtin/event router does not mean every callee belongs to
        # this particular name. Its unresolved dynamic arm remains explicit debt.
        calls.setdefault(key.split("::")[1], set()).update(drawing_calls(body))
    while True:
        changed = False
        for symbol, destinations in calls.items():
            if symbol in facade_symbols:
                continue
            found = set().union(*(facade_routes.get(name, set()) for name in destinations))
            if found != facade_routes.get(symbol, set()):
                facade_routes[symbol] = found
                changed = True
        if not changed:
            break
    branches: dict[str, list[tuple[str, str]]] = {}
    for stage in builtin_registry.STAGES:
        for symbol in stage.functions:
            # These adapters have established source ownership distinct from the
            # generic stage file; the canonical registry checks the same owners.
            path = stage.source
            if symbol == "builtin_input_kbgp":
                path = "src/runtime/builtins/gml_builtin.c"
            elif symbol == "builtin_skeleton":
                path = "src/runtime/builtins/gml_builtin_animation.c"
            elif symbol == "builtin_layer_exact":
                path = "src/runtime/builtins/gml_builtin_layers.c"
            for name, bodies in named_api_branches(source[path], symbol).items():
                branches.setdefault(name, []).extend((f"{path}::{symbol}", body) for body in bodies)
    for path, symbols in API_CONTEXT_ADAPTERS.items():
        for symbol in symbols:
            for name, bodies in named_api_branches(source[path], symbol).items():
                branches.setdefault(name, []).extend((f"{path}::{symbol}", body) for body in bodies)
    direct = direct_api_branches(source[builtin_registry.LEGACY_SOURCE_PATH.as_posix()],
                                 API_DIRECT_ADAPTER)
    rows = builtin_registry.parse_rows((root / builtin_registry.REGISTRY_PATH).read_text())
    api_arms = {}
    api_facades = {}
    api_delegates = {}
    for row in rows:
        arms = list(branches.get(row.name, []))
        arms.extend((f"{builtin_registry.LEGACY_SOURCE_PATH.as_posix()}::{API_DIRECT_ADAPTER}", body)
                    for body in direct.get(row.symbol, []))
        api_arms[row.name] = arms
        api_facades[row.name] = set().union(*(
            facade_routes.get(name, set()) for _, body in arms for name in drawing_calls(body)))
        api_delegates[row.name] = set().union(*(literal_api_delegates(body) for _, body in arms))
    while True:
        changed = False
        for name, targets in api_delegates.items():
            found = set().union(*(api_facades.get(target, set()) for target in targets))
            if found - api_facades[name]:
                api_facades[name].update(found)
                changed = True
        if not changed:
            break
    inventory = {}
    gpu_facades = {key.split("::")[1] for key, (route, _) in dispatch.items() if route != "CPU"}
    for row in rows:
        arms = api_arms[row.name]
        reached = api_facades[row.name]
        named_draw = row.name.startswith(API_DRAW_PREFIXES) \
            and not row.name.startswith(("draw_set_", "draw_get_")) and row.name not in API_NONPAINT
        if not reached and not named_draw and row.name not in {"d3d_model_draw", "screen_redraw"}:
            continue
        if not arms:
            raise ValueError(f"{row.name}: drawing API has no discovered implementation arm")
        identity = f"BID_{row.symbol}/{row.value}/{row.owner}/{row.stage}/{row.cache}/" \
            + ("alias" if row.alias else "entry")
        adapters = ",".join(sorted({adapter for adapter, _ in arms}))
        revision_parts = []
        for adapter, body in sorted(set(arms)):
            uncommented = C_NONCODE.sub(
                lambda match: " " if match[0].startswith(("/*", "//")) else match[0], body)
            revision_parts.append(adapter + ":" + " ".join(C_TOKENS.findall(uncommented)))
        revision = hashlib.sha256("\n".join(revision_parts).encode()).hexdigest()
        status = "PARTIAL" if reached & gpu_facades else "DEBT"
        inventory[row.name] = (identity, adapters, ",".join(sorted(reached)) or "indirect",
                               status, revision)
    return inventory


def check_api_dispatch_inventory(document: str, inventory: dict[str, tuple[str, ...]]) -> None:
    start, end = "<!-- D8-API:START -->", "<!-- D8-API:END -->"
    if document.count(start) != 1 or document.count(end) != 1:
        raise ValueError("D8 drawing API inventory markers are missing or repeated")
    entries = {}
    for line in document.split(start)[1].split(end)[0].splitlines():
        if not line.startswith("| `"):
            continue
        fields = [field.strip().strip("`") for field in line.strip("|").split("|")]
        if len(fields) != 7 or not all(fields):
            raise ValueError("D8 API row requires seven nonempty fields")
        name, identity, adapters, facades, status, revision, reason = fields
        if name in entries:
            raise ValueError(f"{name}: duplicate D8 API row")
        if status not in {"PARTIAL", "DEBT"} or reason == "Pending":
            raise ValueError(f"{name}: D8 API needs an explicit status and reason")
        entries[name] = (identity, adapters, facades, status, revision)
    if entries.keys() != inventory.keys():
        raise ValueError(f"D8 API coverage: missing={sorted(inventory.keys() - entries.keys())}, "
                         f"extra={sorted(entries.keys() - inventory.keys())}")
    for name, actual in inventory.items():
        if entries[name] != actual:
            raise ValueError(f"{name}: D8 API dispatch changed; recheck its adapters and debt row")


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
            previous_relative = ""
            for relative, symbol in anchors:
                # A grouped ::helper inherits only the preceding file in this field.
                if not relative:
                    if not previous_relative:
                        raise ValueError(f"{rule}: unqualified shorthand anchor ::{symbol}")
                    relative = previous_relative
                previous_relative = relative
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
    self_test_dispatch_inventory()
    self_test_api_dispatch_inventory()
    document = BACKEND_MAP.read_text(encoding="utf-8")
    ledger = rows(GPU_TABLE, EXECUTOR_ROW)
    check_repair_map(document, ledger, ROOT)
    for broken in (
        document.replace("::resident_filtered_sprite", "::obsolete_filtered_sprite", 1),
        document.replace("::R03_SHADER_RECTANGLE", "::OBSOLETE_RECTANGLE", 1),
        document.replace("::rotated_nearest_packet", "::obsolete_rotated_packet", 1),
        document.replace("::R03_SHADER_GUI_ROTATED_NEAREST_COVERAGE",
                         "::OBSOLETE_GUI_ROTATED_NEAREST_COVERAGE", 1),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-P9-GUI-ROTATED-SPRITE |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-SURFACE-CANVAS-FUNCTION |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-P9-SPRITE-ALPHA-BOUNDARY |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-CLASSIC-BORROWED-VIEW |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-SPR-CLASSIC-NEAREST |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-SPR-ZERO-ALPHA-PART |")),
        document.replace("::resident_opaque_rect", "::obsolete_opaque_rect", 1),
        document.replace("::R03_SHADER_EXACT_OVERLAP", "::OBSOLETE_EXACT_OVERLAP", 1),
        document.replace("::R03_SHADER_SURFACE_DESTINATION", "::OBSOLETE_SURFACE_DESTINATION", 1),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-P5-SURFACE-DESTINATION-UNIT |")),
        document.replace("::R03_SHADER_NEAREST_PHASED", "::OBSOLETE_NEAREST_PHASED", 1),
        document.replace("::R03_SHADER_MODERN_NUMBERED_NEAREST",
                         "::OBSOLETE_MODERN_NUMBERED_NEAREST", 1),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-SPR-NUMBERED-NEAREST |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-SPR-FIRSTGEN-NEAREST-MAG |")),
        document.replace("device comparison R03-P3-JOURNAL", "comparison missing", 1),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-SPR-CLASSIC-SCALED-NEAREST |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-PRIM-CLASSIC-INVERSE-RECT |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-PRIM-STUDIO-GUI-OPAQUE |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-PRIM-FIRSTGEN-GUI-ALPHA-RECT |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-PRIM-CLASSIC-OWNED |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-PRIM-RECT-COALESCED |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-SPR-CLASSIC-OWNED-BINARY |")),
        document.replace("`supported` | Only the bounded", "`planned` | Only the bounded", 1),
        document.replace(
            "`common/gml_render_sprites.c::gml_render_sprite_pos_geometry`; "
            "`common/gml_render_sprites.c::resident_positioned_quad_sprite`",
            "", 1),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-SPR-STRETCH-BOX |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-P9-GUI-FILTERED-ATLAS |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-P9-GUI-MAPPED-SPRITE |")),
        "\n".join(line for line in document.splitlines()
                  if not line.startswith("| R03-P9-STRETCHED-COVERED-ALPHA |")),
        document.replace("::R03_SHADER_STRETCHED_COVERED_ALPHA",
                         "::OBSOLETE_STRETCHED_COVERED_ALPHA", 1),
        document.replace("::scene_snapshot_region", "::obsolete_snapshot_region", 1),
        document.replace("::R03_SHADER_GUI_FILTERED_ALPHA_STAMP",
                         "::OBSOLETE_GUI_FILTERED_ALPHA_STAMP", 1),
        document.replace("::gml_render_rotated_alpha16_factor",
                         "::obsolete_rotated_alpha16_factor", 1),
        document.replace("::draw_rotated_regions", "::obsolete_rotated_regions", 1),
        document.replace("::gml_render_defer_resident_screen_upscale",
                         "::obsolete_resident_screen_upscale", 1),
    ):
        try:
            check_repair_map(broken, ledger, ROOT)
        except ValueError:
            continue
        raise ValueError("R03 repair-map negative probe unexpectedly passed")


def self_test_dispatch_inventory() -> None:
    fixture = '''
void declared_only(void);
/* void gml_draw_hidden(void){} */
static int
gml_draw_multiline(void){
  if(ready) { note("} fake_gpu_call(); {"); }
  else if(other) { note("plain"); }
  return 1;
}
'''
    parsed = c_function_definitions(fixture, "authored fixture")
    if set(parsed) != {"gml_draw_multiline"} or "return 1;" not in parsed["gml_draw_multiline"]:
        raise ValueError("D8 definition reader lost a multiline function or followed non-code")
    prefix = "src/video/renderer/common/probe.c::"
    functions = {
        prefix + "gml_draw_probe": "void gml_draw_probe(void){ cpu_paint(); }",
        prefix + "gml_draw_gpu_probe":
            "void gml_draw_gpu_probe(void){ gml_render_application_scene_enqueue(); }",
        prefix + "cpu_paint": "void cpu_paint(void){ gml_render_maybe_prepare_draw(); }",
        prefix + "gml_render_maybe_prepare_draw":
            "void gml_render_maybe_prepare_draw(void){ gml_render_application_scene_enqueue(); }",
    }
    inventory = dispatch_inventory(functions)
    if inventory[prefix + "gml_draw_probe"][0] != "CPU":
        raise ValueError("D8 CPU preparation falsely credited an earlier GPU prefix")
    def document_for(entries: dict[str, tuple[str, str]]) -> str:
        result = ["<!-- D8-DISPATCH:START -->"]
        for key, (route, revision) in entries.items():
            status = "DEBT" if route == "CPU" else "PARTIAL"
            result.append(f"| `{key}` | `{route}` | {status} | `{revision}` | Authored test reason. |")
        return "\n".join(result + ["<!-- D8-DISPATCH:END -->"])
    document = document_for(inventory)
    check_dispatch_inventory(document, inventory)
    changed = dict(functions)
    changed[prefix + "gml_draw_probe"] = \
        "void gml_draw_probe(void){ gml_render_application_scene_enqueue(); }"
    changed_inventory = dispatch_inventory(changed)
    # Even refreshing a source digest must not hide CPU debt gaining a GPU route.
    digest_only = document.replace(inventory[prefix + "gml_draw_probe"][1],
                                   changed_inventory[prefix + "gml_draw_probe"][1])
    new = dict(functions, **{prefix + "gml_draw_unregistered": "void gml_draw_unregistered(void){}"})
    gpu_new = dict(functions, **{
        "src/video/renderer/gpu/opengl/probe.c::gml_render_gpu_opengl_unregistered":
        "int gml_render_gpu_opengl_unregistered(void){ return 1; }",
    })
    body_changed = dict(functions)
    body_changed[prefix + "gml_draw_probe"] = "void gml_draw_probe(void){ cpu_other_paint(); }"
    for broken, actual in (
        (document, dispatch_inventory(new)),
        (document, dispatch_inventory(gpu_new)),
        (document, changed_inventory),
        (digest_only, changed_inventory),
        (document, dispatch_inventory(body_changed)),
        (document.replace("<!-- D8-DISPATCH:END -->", ""), inventory),
        (document.replace("Authored test reason.", "Pending"), inventory),
        (document.replace("| DEBT |", "| HELPER |"), inventory),
        (document.replace("<!-- D8-DISPATCH:END -->", document), inventory),
    ):
        try:
            check_dispatch_inventory(broken, actual)
        except ValueError:
            continue
        raise ValueError("D8 dispatch negative probe unexpectedly passed")
    with_comment = dict(functions)
    with_comment[prefix + "gml_draw_probe"] = \
        'void gml_draw_probe(void){ /* gml_render_application_scene_enqueue(); { */ cpu_paint(); }'
    if dispatch_inventory(with_comment) != inventory:
        raise ValueError("D8 comments changed a dispatch route or source revision")
    with_string = dict(functions)
    with_string[prefix + "gml_draw_probe"] = \
        'void gml_draw_probe(void){ note("gml_render_application_scene_enqueue(); }"); cpu_paint(); }'
    if dispatch_inventory(with_string)[prefix + "gml_draw_probe"][0] != "CPU":
        raise ValueError("D8 string falsely credited GPU admission")
    for first, second in (("a ++ b", "a + + b"), ('note("one")', 'note("two")')):
        one = {prefix + "gml_draw_probe": f"void gml_draw_probe(void){{ {first}; }}"}
        two = {prefix + "gml_draw_probe": f"void gml_draw_probe(void){{ {second}; }}"}
        if dispatch_inventory(one) == dispatch_inventory(two):
            raise ValueError("D8 revision lost an operator or string change")
    check_dispatch_inventory(document_for(changed_inventory), changed_inventory)


def self_test_api_dispatch_inventory() -> None:
    source = b'''
void adapter(const char *nm){
  /* if(!strcmp(nm,"draw_hidden")) paint(); */
  note("if(strcmp(nm,draw_fake)) {}");
  if(!strcmp(nm,"draw_probe") || !strcmp(nm,"draw_probe_alias")){
    if(active) paint(); else other();
  }
  if(!strcmp(nm,"draw_single")) if(active) paint(); else other();
  if(!strcmp(nm,"draw_else")) paint(); else if(active) other();
}
void direct(int id){
  switch(id){
    case BID_PROBE:
    case BID_ALIAS:
      if(active){ switch(mode){ case 0: nested(); break; default: break; } }
      paint(); return;
    case BID_SINGLE: other(); return;
    default: return;
  }
}
'''
    branches = named_api_branches(source, "adapter")
    if set(branches) != {"draw_probe", "draw_probe_alias", "draw_single", "draw_else"} \
            or branches["draw_probe"] != branches["draw_probe_alias"] \
            or "else other();" not in branches["draw_single"][0] \
            or "else if(active) other();" not in branches["draw_else"][0]:
        raise ValueError("D8 named API parser lost an alias, nested statement or else arm")
    direct = direct_api_branches(source, "direct")
    if set(direct) != {"PROBE", "ALIAS", "SINGLE"} \
            or any("paint(); return;" not in direct[name][0] for name in ("PROBE", "ALIAS")) \
            or "other();" in direct["PROBE"][0] or "paint();" in direct["SINGLE"][0]:
        raise ValueError("D8 direct API parser lost shared labels or crossed a switch arm")
    if literal_api_delegates(
        'gml_builtin_call(vm, choose(nm)?"draw_one":"draw_two", args, count);'
    ) != {"draw_one", "draw_two"} or literal_api_delegates(
        'note("gml_builtin_call(vm, draw_fake, args, count)"); '
        'gml_builtin_call(vm, dynamic_name, args, count);'
    ):
        raise ValueError("D8 API delegation credited a dynamic name or lost literal targets")
    if drawing_calls('gml_builtin_try_draw(); gml_render_maybe_prepare_draw(); '
                     'note("gml_draw_fake()"); gml_draw_probe();') != {"note", "gml_draw_probe"}:
        raise ValueError("D8 API facade graph credited a generic router or CPU preparation")
    inventory = {"draw_probe": ("BID_PROBE/1/DRAW/DRAW/ALWAYS/entry",
                                "src/runtime/builtins/probe.c::adapter", "gml_draw_probe",
                                "PARTIAL", "a" * 64)}
    def document_for(entries: dict[str, tuple[str, ...]]) -> str:
        result = ["<!-- D8-API:START -->"]
        for name, fields in entries.items():
            result.append("| `" + "` | `".join((name, *fields)) + "` | Authored scope reason. |")
        return "\n".join(result + ["<!-- D8-API:END -->"])
    document = document_for(inventory)
    check_api_dispatch_inventory(document, inventory)
    changed = dict(inventory)
    changed["draw_probe"] = (*inventory["draw_probe"][:3], "DEBT", "b" * 64)
    new_alias = dict(inventory, draw_new_alias=inventory["draw_probe"])
    for broken, actual in (
        (document, new_alias),
        (document, changed),
        (document.replace("ALWAYS/entry", "NEVER/alias"), inventory),
        (document.replace("src/runtime/builtins/probe.c::adapter", "missing_adapter"), inventory),
        (document.replace("gml_draw_probe", "gml_draw_other"), inventory),
        (document.replace("a" * 64, "b" * 64), inventory),
        (document.replace("<!-- D8-API:END -->", ""), inventory),
        (document.replace("<!-- D8-API:END -->", document), inventory),
        (document.replace("Authored scope reason.", "Pending"), inventory),
        (document.replace("`PARTIAL`", "`HELPER`"), inventory),
    ):
        try:
            check_api_dispatch_inventory(broken, actual)
        except ValueError:
            continue
        raise ValueError("D8 drawing API negative probe unexpectedly passed")
    check_api_dispatch_inventory(document_for(changed), changed)


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
        functions = dispatch_functions(ROOT)
        inventory = dispatch_inventory(functions)
        check_dispatch_inventory(BACKEND_MAP.read_text(encoding="utf-8"), inventory)
        api_inventory = api_dispatch_inventory(ROOT, functions, inventory)
        check_api_dispatch_inventory(BACKEND_MAP.read_text(encoding="utf-8"), api_inventory)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"render operations: {error}\n")
    print(f"render operations: ok ({len(catalogue)} IDs, "
          f"{sum(value[2] == 'planned' for value in gpu.values())} GPU planned, "
          f"{len(inventory)} function dispatch points, {len(api_inventory)} drawing API spellings)")


if __name__ == "__main__":
    main()
