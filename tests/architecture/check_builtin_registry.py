#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Build and verify the canonical exact-builtin registry.

The checked-in registry is authoritative for exact names, stable numeric IDs,
family ownership, dispatch stage, and call-site cacheability.  This tool uses a
comment-aware C token stream to inventory the established implementation
branches and fails closed when that inventory and the declarative registry
diverge.  The generated lookup header contains data only.
"""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
from typing import Iterable


TOOL_VERSION = "1"
REGISTRY_PATH = Path("src/runtime/builtins/gml_builtin_registry.h")
INDEX_PATH = Path("src/generated/gml_builtin_registry_index.h")
LEGACY_SOURCE_PATH = Path("src/runtime/builtins/gml_builtin_registry.c")


@dataclass(frozen=True)
class Token:
    kind: str
    value: str
    start: int
    end: int


@dataclass(frozen=True)
class Stage:
    token: str
    owner: str
    functions: tuple[str, ...]
    source: str
    before_scripts: bool = True


@dataclass(frozen=True)
class ExactRow:
    value: int
    symbol: str
    name: str
    owner: str
    stage: str
    cache: str
    alias: bool


STAGES = (
    Stage("FACADE", "FACADE", ("builtin_call_impl",),
          "src/runtime/builtins/gml_builtin.c"),
    Stage("COLLISION", "COLLISION", ("gml_builtin_try_collision",),
          "src/runtime/builtins/gml_builtin_collision.c"),
    Stage("VALUES_MATH", "VALUES", ("gml_builtin_try_values_math",),
          "src/runtime/builtins/gml_builtin_values.c"),
    Stage("COLLISION_PLANNING", "COLLISION",
          ("gml_builtin_try_collision_planning",),
          "src/runtime/builtins/gml_builtin_collision.c"),
    Stage("VALUES_STRINGS", "VALUES", ("gml_builtin_try_values_strings",),
          "src/runtime/builtins/gml_builtin_values.c"),
    Stage("IO_INI", "IO", ("gml_builtin_try_io_ini",),
          "src/runtime/builtins/gml_builtin_io.c"),
    Stage("INSTANCES_ROOMS", "INSTANCES",
          ("gml_builtin_try_instances_rooms",),
          "src/runtime/builtins/gml_builtin_instances.c"),
    Stage("ACTIONS", "ACTIONS", ("gml_builtin_try_actions",),
          "src/runtime/builtins/gml_builtin_actions.c"),
    Stage("INSTANCES", "INSTANCES", ("gml_builtin_try_instances",),
          "src/runtime/builtins/gml_builtin_instances.c"),
    Stage("VALUES_LANGUAGE", "VALUES",
          ("gml_builtin_try_values_language",),
          "src/runtime/builtins/gml_builtin_values.c"),
    Stage("INSTANCES_DESTROY", "INSTANCES",
          ("gml_builtin_try_instances_destroy",),
          "src/runtime/builtins/gml_builtin_instances.c"),
    Stage("ACTIONS_LEGACY", "ACTIONS",
          ("gml_builtin_try_actions_legacy",),
          "src/runtime/builtins/gml_builtin_actions.c"),
    Stage("INSTANCES_QUERIES", "INSTANCES",
          ("gml_builtin_try_instances_queries",),
          "src/runtime/builtins/gml_builtin_instances.c"),
    Stage("PARTICLES", "PARTICLES", ("gml_builtin_try_particles",),
          "src/runtime/builtins/gml_builtin_particles.c"),
    Stage("DRAW", "DRAW", ("gml_builtin_try_draw",),
          "src/runtime/builtins/gml_builtin_draw.c"),
    Stage("INPUT", "INPUT", ("gml_builtin_try_input", "builtin_input_kbgp"),
          "src/runtime/builtins/gml_builtin_input.c"),
    Stage("IO", "IO", ("gml_builtin_try_io",),
          "src/runtime/builtins/gml_builtin_io.c"),
    Stage("INSTANCES_SCRIPTS", "INSTANCES",
          ("gml_builtin_try_instances_scripts",),
          "src/runtime/builtins/gml_builtin_instances.c"),
    Stage("INSTANCES_EVENTS", "INSTANCES",
          ("gml_builtin_try_instances_events",),
          "src/runtime/builtins/gml_builtin_instances.c"),
    Stage("AUDIO", "AUDIO", ("gml_builtin_try_audio",),
          "src/runtime/builtins/gml_builtin_audio.c"),
    Stage("INSTANCES_PATHS", "INSTANCES",
          ("gml_builtin_try_instances_paths",),
          "src/runtime/builtins/gml_builtin_instances.c"),
    Stage("INSTANCES_TIMELINES", "INSTANCES",
          ("gml_builtin_try_instances_timelines", "time_source_builtin"),
          "src/runtime/builtins/gml_builtin_instances.c"),
    Stage("LAYERS_EARLY", "LAYERS", ("gml_builtin_try_layers_early",),
          "src/runtime/builtins/gml_builtin_layers.c"),
    Stage("PLATFORM", "PLATFORM", ("gml_builtin_try_platform",),
          "src/runtime/builtins/gml_builtin_platform.c"),
    Stage("DS", "DS", ("gml_builtin_try_ds",),
          "src/runtime/builtins/gml_builtin_ds.c"),
    Stage("JSON", "JSON", ("gml_builtin_try_json",),
          "src/runtime/builtins/gml_builtin_json.c"),
    Stage("VALUES_VARIABLES", "VALUES",
          ("gml_builtin_try_values_variables",),
          "src/runtime/builtins/gml_builtin_values.c"),
    Stage("DRAW_3D", "DRAW",
          ("gml_builtin_try_draw_3d", "gm_matrix_builtin"),
          "src/runtime/builtins/gml_builtin_draw.c"),
    Stage("ANIMATION", "ANIMATION", ("gml_builtin_try_animation",),
          "src/runtime/builtins/gml_builtin_animation.c"),
    Stage("PHYSICS", "PHYSICS", ("gml_builtin_try_physics",),
          "src/runtime/builtins/gml_builtin_physics.c"),
    Stage("PLATFORM_EXTENSIONS", "PLATFORM",
          ("gml_builtin_try_platform_extensions", "builtin_skeleton",
           "builtin_layer_exact"),
          "src/runtime/builtins/gml_builtin_platform.c"),
    Stage("PLATFORM_NOOPS", "PLATFORM",
          ("gml_builtin_try_platform_noops",),
          "src/runtime/builtins/gml_builtin_platform.c"),
    Stage("LAYERS_LATE", "LAYERS", ("gml_builtin_try_layers",),
          "src/runtime/builtins/gml_builtin_layers.c", False),
    Stage("PLATFORM_TAIL", "PLATFORM",
          ("gml_builtin_try_platform_tail",),
          "src/runtime/builtins/gml_builtin.c", False),
)

# These two names are established exact direct-ID implementations.  Their
# generic hot duplicates are retired by the canonical-registry batch, so no
# family strcmp branch remains to discover.
DIRECT_ONLY = {
    "draw_shadow": ("DRAW", "DRAW"),
    "draw_shadow_ext": ("DRAW", "DRAW"),
}

DYNAMIC_ROWS = (
    (131, "FMOD_PREFIX", "fmod_", "AUDIO", "DRAW", "ALWAYS"),
    (139, "INPUT_KBGP", "gamepad_", "INPUT", "INPUT", "ALWAYS"),
)


def fail(message: str) -> "NoReturn":
    raise RuntimeError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stable_json(value: object) -> str:
    return json.dumps(
        value, ensure_ascii=True, sort_keys=True, separators=(",", ":")
    )


def repository_root() -> Path:
    root = Path(__file__).resolve().parents[2]
    if not (root / "Makefile.common").is_file():
        fail("cannot resolve repository root")
    return root


def c_tokens(data: bytes) -> list[Token]:
    tokens: list[Token] = []
    index = 0
    size = len(data)
    while index < size:
        byte = data[index]
        following = data[index + 1] if index + 1 < size else -1
        if chr(byte).isspace():
            index += 1
            continue
        if byte == ord("/") and following == ord("/"):
            index += 2
            while index < size and data[index] not in (10, 13):
                index += 1
            continue
        if byte == ord("/") and following == ord("*"):
            end = data.find(b"*/", index + 2)
            if end < 0:
                fail("unterminated C block comment")
            index = end + 2
            continue
        if byte in (ord('"'), ord("'")):
            quote = byte
            start = index
            index += 1
            while index < size:
                if data[index] == ord("\\"):
                    index += 2
                    continue
                if data[index] == quote:
                    index += 1
                    break
                index += 1
            else:
                fail("unterminated C literal")
            raw = data[start:index].decode("utf-8")
            kind = "string" if quote == ord('"') else "character"
            tokens.append(Token(kind, raw, start, index))
            continue
        if (
            ord("A") <= byte <= ord("Z")
            or ord("a") <= byte <= ord("z")
            or byte == ord("_")
        ):
            start = index
            index += 1
            while index < size:
                item = data[index]
                if not (
                    ord("A") <= item <= ord("Z")
                    or ord("a") <= item <= ord("z")
                    or ord("0") <= item <= ord("9")
                    or item == ord("_")
                ):
                    break
                index += 1
            tokens.append(
                Token("identifier", data[start:index].decode("ascii"),
                      start, index)
            )
            continue
        if ord("0") <= byte <= ord("9"):
            start = index
            index += 1
            while index < size and (
                chr(data[index]).isalnum() or data[index] in b"._"
            ):
                index += 1
            tokens.append(
                Token("number", data[start:index].decode("ascii"),
                      start, index)
            )
            continue
        tokens.append(Token("punctuation", chr(byte), index, index + 1))
        index += 1
    return tokens


def matching(tokens: list[Token], opening: int, left: str, right: str) -> int:
    depth = 0
    for index in range(opening, len(tokens)):
        value = tokens[index].value
        if value == left:
            depth += 1
        elif value == right:
            depth -= 1
            if depth == 0:
                return index
    fail(f"unmatched {left} token")


def function_tokens(data: bytes, symbol: str) -> list[Token]:
    tokens = c_tokens(data)
    matches: list[tuple[int, int]] = []
    for index, token in enumerate(tokens):
        if token.kind != "identifier" or token.value != symbol:
            continue
        if index + 1 >= len(tokens) or tokens[index + 1].value != "(":
            continue
        closing = matching(tokens, index + 1, "(", ")")
        cursor = closing + 1
        while cursor < len(tokens) and tokens[cursor].value in (
            "__attribute__", "__declspec",
        ):
            if cursor + 1 >= len(tokens) or tokens[cursor + 1].value != "(":
                break
            cursor = matching(tokens, cursor + 1, "(", ")") + 1
        if cursor < len(tokens) and tokens[cursor].value == "{":
            matches.append((cursor, matching(tokens, cursor, "{", "}")))
    if len(matches) != 1:
        fail(f"{symbol} resolves to {len(matches)} active definitions")
    opening, closing = matches[0]
    return tokens[opening + 1:closing]


def function_byte_range(data: bytes, symbol: str) -> tuple[int, int]:
    tokens = c_tokens(data)
    matches: list[tuple[int, int]] = []
    for index, token in enumerate(tokens):
        if token.kind != "identifier" or token.value != symbol:
            continue
        if index + 1 >= len(tokens) or tokens[index + 1].value != "(":
            continue
        closing = matching(tokens, index + 1, "(", ")")
        cursor = closing + 1
        if cursor < len(tokens) and tokens[cursor].value == "{":
            body_end = matching(tokens, cursor, "{", "}")
            line_start = data.rfind(b"\n", 0, token.start) + 1
            matches.append((line_start, tokens[body_end].end))
    if len(matches) != 1:
        fail(f"{symbol} resolves to {len(matches)} byte ranges")
    return matches[0]


def decode_c_string(raw: str) -> str:
    try:
        value = ast.literal_eval(raw)
    except (SyntaxError, ValueError) as error:
        fail(f"cannot decode C string literal {raw!r}: {error}")
    if not isinstance(value, str):
        fail(f"C literal is not a string: {raw!r}")
    return value


def compared_names(tokens: list[Token], variables: set[str]) -> set[str]:
    names: set[str] = set()
    for index, token in enumerate(tokens):
        if token.value != "strcmp" or index + 5 >= len(tokens):
            continue
        if (
            tokens[index + 1].value == "("
            and tokens[index + 2].value in variables
            and tokens[index + 3].value == ","
            and tokens[index + 4].kind == "string"
            and tokens[index + 5].value == ")"
        ):
            names.add(decode_c_string(tokens[index + 4].value))
    return names


def implementation_inventory(root: Path) -> dict[str, tuple[str, str]]:
    owners: dict[str, tuple[str, str, int, str]] = {}
    stage_by_function = {
        function: (index, stage)
        for index, stage in enumerate(STAGES)
        for function in stage.functions
    }
    for function, (index, stage) in stage_by_function.items():
        source = root / stage.source
        if function == "builtin_input_kbgp":
            source = root / "src/runtime/builtins/gml_builtin.c"
        elif function == "builtin_skeleton":
            source = root / "src/runtime/builtins/gml_builtin_animation.c"
        elif function == "builtin_layer_exact":
            source = root / "src/runtime/builtins/gml_builtin_layers.c"
        names = compared_names(function_tokens(source.read_bytes(), function),
                               {"nm", "name"})
        # builtin_call_impl has exactly one pre-chain compatibility shadow.
        if function == "builtin_call_impl":
            names = {name for name in names if name == "tile_add"}
        for name in names:
            candidate = (stage.owner, stage.token, index, function)
            previous = owners.get(name)
            if previous is None or index < previous[2]:
                owners[name] = candidate
    for name, (owner, stage) in DIRECT_ONLY.items():
        owners.setdefault(name, (owner, stage, -1, "direct-ID switch"))
    return {
        name: (record[0], record[1])
        for name, record in owners.items()
    }


def parse_legacy_ids(source: str) -> list[tuple[str, int]]:
    try:
        enum_text = source[source.index("enum {"):
                           source.index("int gml_builtin_fast_id")]
    except ValueError:
        fail("legacy exact-ID enum is absent")
    value = 0
    result: list[tuple[str, int]] = []
    for match in re.finditer(
        r"\b(BID_[A-Z0-9_]+)\s*(?:=\s*(\d+))?\s*,?", enum_text
    ):
        value = int(match.group(2)) if match.group(2) else value + 1
        result.append((match.group(1).removeprefix("BID_"), value))
    if not result:
        fail("legacy exact-ID enum has no entries")
    return result


def parse_legacy_exact_names(source: str) -> dict[str, str]:
    try:
        resolver = source[source.index("int gml_builtin_fast_id"):
                          source.index("GmlVal gml_builtin_call_fast_id")]
    except ValueError:
        fail("legacy exact resolver is absent")
    result: dict[str, str] = {}
    for line in resolver.splitlines():
        ids = re.findall(r"\bBID_[A-Z0-9_]+\b", line)
        names = re.findall(
            r"strcmp\s*\(\s*nm\s*,\s*(\"(?:\\.|[^\"\\])*\")", line
        )
        if not ids:
            continue
        symbol = ids[-1].removeprefix("BID_")
        for raw in names:
            name = decode_c_string(raw)
            previous = result.setdefault(name, symbol)
            if previous != symbol:
                fail(f"legacy resolver assigns {name!r} multiple IDs")
    return result


def symbol_for_name(name: str, occupied: set[str]) -> str:
    base = re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_").upper()
    if not base:
        base = "EXACT"
    if base[0].isdigit():
        base = "N_" + base
    symbol = base
    if symbol in occupied:
        symbol = f"{base}_{sha256(name.encode('utf-8'))[:8].upper()}"
    while symbol in occupied:
        symbol += "_X"
    occupied.add(symbol)
    return symbol


def pre_script_exact_names(root: Path) -> tuple[set[str], set[str]]:
    source = (root / "src/runtime/builtins/gml_builtin.c").read_bytes()
    hot = compared_names(
        function_tokens(source, "fast_hot_builtin"), {"nm", "name"}
    )
    overlay = compared_names(
        function_tokens(source, "d3_try_draw_2d_builtin"), {"nm", "name"}
    )
    return hot, overlay


def cache_for(
    name: str,
    stage: str,
    legacy_names: dict[str, str],
    pre_script_names: set[str],
    contextual_overlay_names: set[str],
) -> str:
    if name == "ds_map_find_value":
        return "DS_LOG_DISABLED"
    if name in contextual_overlay_names:
        return "NEVER"
    if name in legacy_names or name in pre_script_names:
        return "ALWAYS"
    if stage == "FACADE":
        return "NEVER"
    stage_record = next(item for item in STAGES if item.token == stage)
    return "ALWAYS" if stage_record.before_scripts else "NEVER"


def bootstrap_rows(root: Path) -> list[ExactRow]:
    implementation = implementation_inventory(root)
    source = (root / LEGACY_SOURCE_PATH).read_text(encoding="utf-8")
    legacy_ids = parse_legacy_ids(source)
    legacy_names = parse_legacy_exact_names(source)
    pre_script_names, contextual_overlay_names = pre_script_exact_names(root)
    symbol_by_name = dict(legacy_names)
    if "ds_list_clear" not in symbol_by_name:
        symbol_by_name["ds_list_clear"] = "DS_LIST_CLEAR"

    dynamic_symbols = {row[1] for row in DYNAMIC_ROWS}
    names_by_symbol: dict[str, list[str]] = {}
    for name, symbol in symbol_by_name.items():
        names_by_symbol.setdefault(symbol, []).append(name)

    rows: list[ExactRow] = []
    occupied = {symbol for symbol, _ in legacy_ids}
    for symbol, value in legacy_ids:
        if symbol in dynamic_symbols:
            continue
        names = names_by_symbol.get(symbol, [])
        if not names:
            fail(f"stable ID {symbol} has no canonical exact name")
        names.sort(key=lambda item: (
            list(legacy_names).index(item)
            if item in legacy_names else len(legacy_names),
            item,
        ))
        for index, name in enumerate(names):
            if name not in implementation:
                fail(f"legacy exact name {name!r} has no implementation owner")
            owner, stage = implementation[name]
            rows.append(ExactRow(
                value, symbol, name, owner, stage,
                cache_for(
                    name, stage, legacy_names, pre_script_names,
                    contextual_overlay_names,
                ),
                index != 0,
            ))

    next_value = max(value for _, value in legacy_ids) + 1
    for name in sorted(
        set(implementation) - set(symbol_by_name),
        key=lambda item: (
            next(index for index, stage in enumerate(STAGES)
                 if stage.token == implementation[item][1]),
            item,
        ),
    ):
        owner, stage = implementation[name]
        symbol = symbol_for_name(name, occupied)
        rows.append(ExactRow(
            next_value, symbol, name, owner, stage,
            cache_for(
                name, stage, legacy_names, pre_script_names,
                contextual_overlay_names,
            ), False,
        ))
        next_value += 1
    return rows


ENTRY_RE = re.compile(
    r"^\s*ENTRY\(\s*(\d+)\s*,\s*([A-Z0-9_]+)\s*,\s*"
    r"(\"(?:\\.|[^\"\\])*\")\s*,\s*([A-Z0-9_]+)\s*,\s*"
    r"([A-Z0-9_]+)\s*,\s*([A-Z0-9_]+)\s*\)\s*\\?\s*$"
)
ALIAS_RE = re.compile(
    r"^\s*ALIAS\(\s*([A-Z0-9_]+)\s*,\s*"
    r"(\"(?:\\.|[^\"\\])*\")\s*,\s*([A-Z0-9_]+)\s*,\s*"
    r"([A-Z0-9_]+)\s*,\s*([A-Z0-9_]+)\s*\)\s*\\?\s*$"
)


def parse_rows(text: str) -> list[ExactRow]:
    try:
        text = text[
            text.index("#define GML_BUILTIN_EXACT_REGISTRY"):
            text.index("#define GML_BUILTIN_DYNAMIC_REGISTRY")
        ]
    except ValueError as error:
        fail(f"canonical registry macro boundary is absent: {error}")
    values: dict[str, int] = {}
    rows: list[ExactRow] = []
    pending_aliases: list[tuple[str, str, str, str, str]] = []
    for line in text.splitlines():
        match = ENTRY_RE.match(line)
        if match:
            value = int(match.group(1))
            symbol = match.group(2)
            if symbol in values:
                fail(f"registry repeats primary symbol {symbol}")
            values[symbol] = value
            rows.append(ExactRow(
                value, symbol, decode_c_string(match.group(3)),
                match.group(4), match.group(5), match.group(6), False,
            ))
            continue
        match = ALIAS_RE.match(line)
        if match:
            pending_aliases.append((
                match.group(1), decode_c_string(match.group(2)),
                match.group(3), match.group(4), match.group(5),
            ))
    for symbol, name, owner, stage, cache in pending_aliases:
        if symbol not in values:
            fail(f"alias {name!r} references unknown ID {symbol}")
        rows.append(ExactRow(
            values[symbol], symbol, name, owner, stage, cache, True,
        ))
    if not rows:
        fail("canonical registry contains no exact entries")
    # Preserve source order, including aliases.  The parser above appends aliases
    # after primaries, so reparse once with resolved values to retain the macro
    # order used by the generated lookup table.
    ordered: list[ExactRow] = []
    for line in text.splitlines():
        match = ENTRY_RE.match(line)
        if match:
            ordered.append(ExactRow(
                int(match.group(1)), match.group(2),
                decode_c_string(match.group(3)), match.group(4),
                match.group(5), match.group(6), False,
            ))
            continue
        match = ALIAS_RE.match(line)
        if match:
            ordered.append(ExactRow(
                values[match.group(1)], match.group(1),
                decode_c_string(match.group(2)), match.group(3),
                match.group(4), match.group(5), True,
            ))
    return ordered


def registry_enums(rows: list[ExactRow]) -> tuple[list[str], list[str], list[str]]:
    owners = sorted({row.owner for row in rows} | {row[3] for row in DYNAMIC_ROWS})
    stages = [stage.token for stage in STAGES]
    caches = ["ALWAYS", "DS_LOG_DISABLED", "NEVER"]
    return owners, stages, caches


def render_registry(rows: list[ExactRow]) -> bytes:
    owners, stages, caches = registry_enums(rows)
    primary = [row for row in rows if not row.alias]
    aliases_by_symbol: dict[str, list[ExactRow]] = {}
    for row in rows:
        if row.alias:
            aliases_by_symbol.setdefault(row.symbol, []).append(row)

    lines = [
        "/* SPDX-License-Identifier: MIT",
        " * Copyright (c) 2026 retrodiv <retrodiv@proton.me>",
        " */",
        "/* Canonical exact builtin names, stable IDs, ownership, and cache policy. */",
        "#ifndef GML_BUILTIN_REGISTRY_H",
        "#define GML_BUILTIN_REGISTRY_H",
        "",
        "typedef enum GmlBuiltinOwner {",
    ]
    lines.extend(f"  GML_BUILTIN_OWNER_{owner}," for owner in owners)
    lines.extend([
        "  GML_BUILTIN_OWNER_COUNT",
        "} GmlBuiltinOwner;",
        "",
        "typedef enum GmlBuiltinDispatchStage {",
    ])
    lines.extend(
        f"  GML_BUILTIN_STAGE_{stage.token}," for stage in STAGES
    )
    lines.extend([
        "  GML_BUILTIN_STAGE_COUNT",
        "} GmlBuiltinDispatchStage;",
        "",
        "typedef enum GmlBuiltinCachePolicy {",
    ])
    lines.extend(f"  GML_BUILTIN_CACHE_{cache}," for cache in caches)
    lines.extend([
        "} GmlBuiltinCachePolicy;",
        "",
        "/*",
        " * ENTRY(value, id, exact_name, owner, stage, cache_policy)",
        " * ALIAS(id, exact_name, owner, stage, cache_policy)",
        " *",
        " * Numeric values are explicit and append-only.  Aliases deliberately",
        " * share an execution ID but retain per-spelling ownership and cache data.",
        " */",
        "#define GML_BUILTIN_EXACT_REGISTRY(ENTRY, ALIAS) \\",
    ])
    expanded: list[ExactRow] = []
    for row in primary:
        expanded.append(row)
        expanded.extend(sorted(aliases_by_symbol.get(row.symbol, []),
                               key=lambda item: item.name))
    for index, row in enumerate(expanded):
        suffix = " \\" if index + 1 < len(expanded) else ""
        if row.alias:
            lines.append(
                f'  ALIAS({row.symbol}, "{row.name}", {row.owner}, '
                f"{row.stage}, {row.cache}){suffix}"
            )
        else:
            lines.append(
                f'  ENTRY({row.value}, {row.symbol}, "{row.name}", '
                f"{row.owner}, {row.stage}, {row.cache}){suffix}"
            )
    lines.extend([
        "",
        "/* Dynamic prefix IDs retain the previously characterized slow-path order. */",
        "#define GML_BUILTIN_DYNAMIC_REGISTRY(ENTRY) \\",
    ])
    for index, row in enumerate(DYNAMIC_ROWS):
        suffix = " \\" if index + 1 < len(DYNAMIC_ROWS) else ""
        value, symbol, prefix, owner, stage, cache = row
        lines.append(
            f'  ENTRY({value}, {symbol}, "{prefix}", {owner}, '
            f"{stage}, {cache}){suffix}"
        )
    maximum = max(
        [row.value for row in primary] + [row[0] for row in DYNAMIC_ROWS]
    )
    lines.extend([
        "",
        "typedef enum GmlBuiltinId {",
        "#define GML_BUILTIN_ID_ENTRY(value, id, name, owner, stage, cache) \\",
        "  BID_##id = value,",
        "#define GML_BUILTIN_ID_ALIAS(id, name, owner, stage, cache)",
        "  GML_BUILTIN_EXACT_REGISTRY(GML_BUILTIN_ID_ENTRY, "
        "GML_BUILTIN_ID_ALIAS)",
        "  GML_BUILTIN_DYNAMIC_REGISTRY(GML_BUILTIN_ID_ENTRY)",
        "#undef GML_BUILTIN_ID_ALIAS",
        "#undef GML_BUILTIN_ID_ENTRY",
        f"  GML_BUILTIN_ID_LIMIT = {maximum + 1}",
        "} GmlBuiltinId;",
        "",
        "#endif",
        "",
    ])
    return "\n".join(lines).encode("utf-8")


def fnv1a(name: str) -> int:
    value = 2166136261
    for byte in name.encode("utf-8"):
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def render_index(rows: list[ExactRow]) -> bytes:
    size = 1
    while size < len(rows) * 2:
        size *= 2
    slots = [0] * size
    for row_index, row in enumerate(rows):
        slot = fnv1a(row.name) & (size - 1)
        for _ in range(size):
            if slots[slot] == 0:
                slots[slot] = row_index + 1
                break
            slot = (slot + 1) & (size - 1)
        else:
            fail("generated builtin hash table is full")
    lines = [
        "/* SPDX-License-Identifier: MIT",
        " * Copyright (c) 2026 retrodiv <retrodiv@proton.me>",
        " */",
        "/* Generated data: run make builtin-registry-check after registry edits. */",
        "#ifndef GML_BUILTIN_REGISTRY_INDEX_H",
        "#define GML_BUILTIN_REGISTRY_INDEX_H",
        "",
        f"#define GML_BUILTIN_REGISTRY_ENTRY_COUNT {len(rows)}u",
        f"#define GML_BUILTIN_REGISTRY_SLOT_COUNT {size}u",
        "",
        "static const unsigned short gml_builtin_registry_slots[",
        "  GML_BUILTIN_REGISTRY_SLOT_COUNT",
        "]={",
    ]
    width = 16
    for offset in range(0, len(slots), width):
        chunk = slots[offset:offset + width]
        lines.append("  " + ",".join(str(item) for item in chunk) + ",")
    lines.extend([
        "};",
        "",
        "#endif",
        "",
    ])
    return "\n".join(lines).encode("utf-8")


def validate_rows(
    root: Path, rows: list[ExactRow], registry_bytes: bytes
) -> dict[str, object]:
    implementation = implementation_inventory(root)
    by_name: dict[str, ExactRow] = {}
    primary_values: dict[int, ExactRow] = {}
    primary_symbols: dict[str, ExactRow] = {}
    valid_owners, valid_stages, valid_caches = registry_enums(rows)
    for row in rows:
        if row.name in by_name:
            fail(f"canonical registry repeats exact name {row.name!r}")
        by_name[row.name] = row
        if row.owner not in valid_owners:
            fail(f"{row.name!r} has unknown owner {row.owner}")
        if row.stage not in valid_stages:
            fail(f"{row.name!r} has unknown stage {row.stage}")
        if row.cache not in valid_caches:
            fail(f"{row.name!r} has unknown cache policy {row.cache}")
        if not row.alias:
            if row.value in primary_values:
                fail(
                    f"stable ID value {row.value} is shared by primary "
                    f"{primary_values[row.value].symbol} and {row.symbol}"
                )
            if row.symbol in primary_symbols:
                fail(f"canonical registry repeats ID symbol {row.symbol}")
            primary_values[row.value] = row
            primary_symbols[row.symbol] = row
        elif row.symbol not in primary_symbols:
            fail(f"alias {row.name!r} precedes primary {row.symbol}")
    dynamic_values = {row[0] for row in DYNAMIC_ROWS}
    overlap = dynamic_values & set(primary_values)
    if overlap:
        fail(f"dynamic and exact IDs overlap: {sorted(overlap)}")
    if set(by_name) != set(implementation):
        missing = sorted(set(implementation) - set(by_name))
        stale = sorted(set(by_name) - set(implementation))
        fail(
            "canonical exact-name coverage differs from implementation: "
            f"missing={missing[:20]}, stale={stale[:20]}"
        )
    for name, expected in sorted(implementation.items()):
        row = by_name[name]
        if (row.owner, row.stage) != expected:
            fail(
                f"{name!r} registry owner/stage is "
                f"{row.owner}/{row.stage}, expected {expected[0]}/{expected[1]}"
            )
    facade_bytes = (
        root / "src/runtime/builtins/gml_builtin.c"
    ).read_bytes()
    contextual_overlay_names = compared_names(
        function_tokens(facade_bytes, "d3_try_draw_2d_builtin"),
        {"nm", "name"},
    )
    for name in sorted(contextual_overlay_names):
        row = by_name.get(name)
        if row is None:
            fail(f"contextual software-3D builtin {name!r} is unregistered")
        if row.cache != "NEVER":
            fail(
                f"contextual software-3D builtin {name!r} must use "
                "cache policy NEVER"
            )
    legacy_expected = {
        symbol: value
        for symbol, value in (
            (row.symbol, row.value) for row in rows if not row.alias
        )
        if value <= 146
    }
    for value, symbol, *_ in DYNAMIC_ROWS:
        legacy_expected[symbol] = value
    expected_legacy = {
        symbol: value for symbol, value in (
            ("ARRAY_LENGTH", 1),
            ("FMOD_PREFIX", 131),
            ("DS_LIST_CLEAR", 133),
            ("INPUT_KBGP", 139),
            ("LEGACY_DESTROY_SELF", 146),
        )
    }
    for symbol, value in expected_legacy.items():
        if legacy_expected.get(symbol) != value:
            fail(f"stable legacy ID {symbol} changed from {value}")
    values = sorted(set(primary_values) | dynamic_values)
    if len(values) != len(set(values)):
        fail("canonical registry contains duplicate numeric IDs")
    if values != list(range(1, values[-1] + 1)):
        fail("canonical stable builtin IDs are not contiguous")

    registry_source = (
        root / "src/runtime/builtins/gml_builtin_registry.c"
    ).read_bytes()
    resolver_tokens = function_tokens(registry_source, "gml_builtin_fast_id")
    if compared_names(resolver_tokens, {"nm", "name"}):
        fail("exact names returned to the runtime resolver implementation")
    dispatch_tokens = function_tokens(
        registry_source, "gml_builtin_call_registered_stage"
    )
    dispatch_values = [
        dispatch_tokens[index + 1].value
        for index, token in enumerate(dispatch_tokens[:-1])
        if token.value == "case"
        and dispatch_tokens[index + 1].value.startswith(
            "GML_BUILTIN_STAGE_"
        )
    ]
    for stage in STAGES:
        label = "GML_BUILTIN_STAGE_" + stage.token
        if dispatch_values.count(label) != 1:
            fail(
                f"registered stage dispatcher contains "
                f"{dispatch_values.count(label)} {label} cases"
            )
    facade_source = (
        root / "src/runtime/builtins/gml_builtin.c"
    ).read_text(encoding="utf-8")
    draw_source = (
        root / "src/runtime/builtins/gml_builtin_draw.c"
    ).read_text(encoding="utf-8")
    if "fast_hot_builtin" in facade_source:
        fail("retired duplicate exact hot dispatcher returned")
    if "builtin_fmod_exact_name" in draw_source:
        fail("retired duplicate exact FMOD name list returned")

    index_bytes = render_index(rows)
    index_path = root / INDEX_PATH
    if not index_path.is_file() or index_path.read_bytes() != index_bytes:
        fail(
            f"{INDEX_PATH.as_posix()} is stale; run "
            "python3 tests/architecture/check_builtin_registry.py generate"
        )
    names_digest = sha256(
        "".join(name + "\n" for name in sorted(by_name)).encode("utf-8")
    )
    return {
        "cache_policy_counts": dict(sorted(
            __import__("collections").Counter(
                row.cache for row in rows
            ).items()
        )),
        "dynamic_prefix_count": len(DYNAMIC_ROWS),
        "exact_alias_count": sum(row.alias for row in rows),
        "exact_name_count": len(rows),
        "exact_name_set_sha256": names_digest,
        "generated_index_sha256": sha256(index_bytes),
        "generated_slot_count": int(
            re.search(
                rb"GML_BUILTIN_REGISTRY_SLOT_COUNT (\d+)u", index_bytes
            ).group(1)
        ),
        "maximum_stable_id": max(
            [row.value for row in rows] + [row[0] for row in DYNAMIC_ROWS]
        ),
        "owner_counts": dict(sorted(
            __import__("collections").Counter(
                row.owner for row in rows
            ).items()
        )),
        "primary_id_count": sum(not row.alias for row in rows)
                           + len(DYNAMIC_ROWS),
        "registry_sha256": sha256(registry_bytes),
        "registered_stage_case_count": len(dispatch_values),
        "stage_counts": dict(sorted(
            __import__("collections").Counter(
                row.stage for row in rows
            ).items()
        )),
        "status": "verified",
        "tool_version": TOOL_VERSION,
    }


def write_generated(root: Path, rows: list[ExactRow]) -> None:
    registry_path = root / REGISTRY_PATH
    index_path = root / INDEX_PATH
    registry_path.parent.mkdir(parents=True, exist_ok=True)
    index_path.parent.mkdir(parents=True, exist_ok=True)
    registry_path.write_bytes(render_registry(rows))
    canonical_rows = parse_rows(registry_path.read_text(encoding="utf-8"))
    index_path.write_bytes(render_index(canonical_rows))


def command_bootstrap(root: Path) -> dict[str, object]:
    if (root / REGISTRY_PATH).exists():
        fail(f"{REGISTRY_PATH.as_posix()} already exists; use generate")
    rows = bootstrap_rows(root)
    write_generated(root, rows)
    registry_bytes = (root / REGISTRY_PATH).read_bytes()
    canonical_rows = parse_rows(registry_bytes.decode("utf-8"))
    return validate_rows(root, canonical_rows, registry_bytes)


def command_rebootstrap(root: Path) -> dict[str, object]:
    rows = bootstrap_rows(root)
    write_generated(root, rows)
    registry_bytes = (root / REGISTRY_PATH).read_bytes()
    canonical_rows = parse_rows(registry_bytes.decode("utf-8"))
    return validate_rows(root, canonical_rows, registry_bytes)


def command_generate(root: Path) -> dict[str, object]:
    registry_path = root / REGISTRY_PATH
    if not registry_path.is_file():
        fail(f"{REGISTRY_PATH.as_posix()} is absent; use bootstrap")
    registry_bytes = registry_path.read_bytes()
    rows = parse_rows(registry_bytes.decode("utf-8"))
    (root / INDEX_PATH).write_bytes(render_index(rows))
    return validate_rows(root, rows, registry_bytes)


def command_check(root: Path) -> dict[str, object]:
    registry_path = root / REGISTRY_PATH
    if not registry_path.is_file():
        fail(f"{REGISTRY_PATH.as_posix()} is absent")
    registry_bytes = registry_path.read_bytes()
    rows = parse_rows(registry_bytes.decode("utf-8"))
    rendered = render_registry(rows)
    if registry_bytes != rendered:
        fail(
            f"{REGISTRY_PATH.as_posix()} is not in deterministic canonical "
            "format; run generate after preserving intended rows"
        )
    return validate_rows(root, rows, registry_bytes)


def command_apply_runtime(root: Path) -> dict[str, object]:
    path = root / LEGACY_SOURCE_PATH
    data = path.read_text(encoding="utf-8")
    start = data.find("enum {\n")
    end_marker = "GmlVal gml_builtin_call_fast_id("
    end = data.find(end_marker, start)
    if start < 0 or end < 0:
        fail("legacy enum/resolver boundary is absent or already transformed")
    replacement = r'''typedef struct GmlBuiltinRegistryEntry {
  const char *name;
  unsigned short id;
  unsigned char owner;
  unsigned char stage;
  unsigned char cache;
} GmlBuiltinRegistryEntry;

#define GML_BUILTIN_REGISTRY_ENTRY(value,id,name,owner,stage,cache) \
  {name,BID_##id,GML_BUILTIN_OWNER_##owner,GML_BUILTIN_STAGE_##stage, \
   GML_BUILTIN_CACHE_##cache},
#define GML_BUILTIN_REGISTRY_ALIAS(id,name,owner,stage,cache) \
  {name,BID_##id,GML_BUILTIN_OWNER_##owner,GML_BUILTIN_STAGE_##stage, \
   GML_BUILTIN_CACHE_##cache},
static const GmlBuiltinRegistryEntry gml_builtin_exact_registry[]={
  GML_BUILTIN_EXACT_REGISTRY(
    GML_BUILTIN_REGISTRY_ENTRY,GML_BUILTIN_REGISTRY_ALIAS
  )
};
#undef GML_BUILTIN_REGISTRY_ALIAS
#undef GML_BUILTIN_REGISTRY_ENTRY

_Static_assert(
  sizeof(gml_builtin_exact_registry)/sizeof(gml_builtin_exact_registry[0])==
    GML_BUILTIN_REGISTRY_ENTRY_COUNT,
  "generated exact builtin index differs from the canonical registry"
);
_Static_assert(
  (GML_BUILTIN_REGISTRY_SLOT_COUNT&
   (GML_BUILTIN_REGISTRY_SLOT_COUNT-1u))==0,
  "exact builtin hash table must have a power-of-two slot count"
);

#define GML_BUILTIN_STAGE_ENTRY(value,id,name,owner,stage,cache) \
  [BID_##id]=GML_BUILTIN_STAGE_##stage,
#define GML_BUILTIN_STAGE_ALIAS(id,name,owner,stage,cache)
static const unsigned char gml_builtin_id_stage[GML_BUILTIN_ID_LIMIT]={
  GML_BUILTIN_EXACT_REGISTRY(
    GML_BUILTIN_STAGE_ENTRY,GML_BUILTIN_STAGE_ALIAS
  )
  GML_BUILTIN_DYNAMIC_REGISTRY(GML_BUILTIN_STAGE_ENTRY)
};
#undef GML_BUILTIN_STAGE_ALIAS
#undef GML_BUILTIN_STAGE_ENTRY

static uint32_t gml_builtin_name_hash(const char *name){
  uint32_t value=UINT32_C(2166136261);
  const unsigned char *cursor=(const unsigned char*)name;
  while(cursor[0]){
    value^=cursor[0];
    cursor++;
    value*=UINT32_C(16777619);
  }
  return value;
}

static const GmlBuiltinRegistryEntry *gml_builtin_exact_entry(
  const char *name
){
  unsigned slot=gml_builtin_name_hash(name)&
                (GML_BUILTIN_REGISTRY_SLOT_COUNT-1u);
  for(unsigned probe=0;probe<GML_BUILTIN_REGISTRY_SLOT_COUNT;probe++){
    unsigned entry=gml_builtin_registry_slots[slot];
    if(!entry) return NULL;
    const GmlBuiltinRegistryEntry *candidate=
      &gml_builtin_exact_registry[entry-1u];
    if(!strcmp(candidate->name,name)) return candidate;
    slot=(slot+1u)&(GML_BUILTIN_REGISTRY_SLOT_COUNT-1u);
  }
  return NULL;
}

int gml_builtin_fast_id(GmlVM *vm,const char *nm){
  if(!nm || !*nm) return -1;
  const GmlBuiltinRegistryEntry *entry=gml_builtin_exact_entry(nm);
  if(entry){
    switch((GmlBuiltinCachePolicy)entry->cache){
      case GML_BUILTIN_CACHE_ALWAYS:
        return entry->id;
      case GML_BUILTIN_CACHE_DS_LOG_DISABLED:
        return log_ds_on(vm)?-1:entry->id;
      case GML_BUILTIN_CACHE_NEVER:
        return -1;
    }
    return -1;
  }
  /* Prefix and dynamic fallbacks deliberately remain outside the exact table. */
  if(nm[0]=='f' && !strncmp(nm,"fmod_",5)) return BID_FMOD_PREFIX;
  if(nm[0]=='g' && !strncmp(nm,"gamepad_",8)) return BID_INPUT_KBGP;
  return -1;
}
'''
    path.write_text(data[:start] + replacement + data[end:],
                    encoding="utf-8")
    return {
        "replacement_sha256": sha256(replacement.encode("utf-8")),
        "source_path": LEGACY_SOURCE_PATH.as_posix(),
        "status": "applied",
        "tool_version": TOOL_VERSION,
    }


def command_apply_facade(root: Path) -> dict[str, object]:
    path = root / "src/runtime/builtins/gml_builtin.c"
    data = path.read_bytes()
    function_start, function_end = function_byte_range(
        data, "fast_hot_builtin"
    )
    comment_start = data.rfind(
        b"/* Short-circuit very hot draw/UI builtins", 0, function_start
    )
    if comment_start < 0:
        fail("hot-dispatch retirement comment is absent")
    if data[comment_start:function_start].count(b"/*") != 1:
        fail("hot-dispatch retirement comment boundary is ambiguous")
    while function_end < len(data) and data[function_end] in b"\r\n":
        function_end += 1
    data = data[:comment_start] + data[function_end:]

    old = (
        b"GmlVal builtin_call_impl(GmlVM *vm, const char *nm, "
        b"GmlVal *a, int n){\n"
        b"  GmlRender *R=(GmlRender*)vm->render;\n"
        b"  (void)graphics_state_for_vm(vm);\n"
        b"  { GmlVal v;\n"
        b"    if(fast_hot_builtin(vm,nm,a,n,&v)) return v; }\n"
    )
    new = (
        b"GmlVal builtin_call_impl(GmlVM *vm, const char *nm, "
        b"GmlVal *a, int n){\n"
        b"  int id=gml_builtin_fast_id(vm,nm);\n"
        b"  if(id>0) return gml_builtin_call_fast_id(vm,id,nm,a,n);\n"
        b"  GmlRender *R=(GmlRender*)vm->render;\n"
        b"  (void)graphics_state_for_vm(vm);\n"
        b"  if(d3_try_draw_2d_builtin(vm,nm,a,n)) return vreal(0);\n"
    )
    if data.count(old) != 1:
        fail("generic builtin facade prologue is not the characterized form")
    data = data.replace(old, new, 1)
    path.write_bytes(data)
    return {
        "retired_function": "fast_hot_builtin",
        "source_path": path.relative_to(root).as_posix(),
        "status": "applied",
        "tool_version": TOOL_VERSION,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "command",
        choices=(
            "bootstrap", "rebootstrap", "generate", "check",
            "apply-runtime", "apply-facade",
        ),
    )
    parser.add_argument("--output")
    args = parser.parse_args()
    root = repository_root()
    if args.command == "bootstrap":
        result = command_bootstrap(root)
    elif args.command == "rebootstrap":
        result = command_rebootstrap(root)
    elif args.command == "generate":
        result = command_generate(root)
    elif args.command == "apply-runtime":
        result = command_apply_runtime(root)
    elif args.command == "apply-facade":
        result = command_apply_facade(root)
    else:
        result = command_check(root)
    output = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        path = root / args.output
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(output, encoding="utf-8")
    print(stable_json(result))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        raise SystemExit(f"builtin registry: {error}")
