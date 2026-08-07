#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>
"""Permanent ownership checks for content compiler and importer modules."""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "src/content/project"
MAKEFILE = ROOT / "Makefile.common"

COMPILER_SOURCES = (
    "src/content/project/gmlc_bytecode.c",
    "src/content/project/gmlc_lexer.c",
    "src/content/project/gmlc_parser.c",
    "src/content/project/gmlc_emitter.c",
    "src/content/project/gmlc_function_registry.c",
)
PACKAGE_SOURCES = (
    "src/content/project/gmlc_package.c",
    "src/content/project/gmlc_package_chunks.c",
    "src/content/project/gmlc_package_code.c",
    "src/content/project/gmlc_package_textures.c",
)
PACKAGE_PRIVATE_TYPES = (
    "Buf",
    "StrTab",
    "StrPatch",
    "FramePatch",
    "FontPatch",
    "CodeRef",
    "CodeBlobPatch",
    "CodeNameRef",
    "TexturePlacement",
    "RefTextureFrame",
    "RefTextureLayout",
    "Pkg",
)
PACKAGE_TEXTURE_OPERATIONS = (
    "ref_texture_find_frame",
    "total_sprite_frames",
    "total_texture_pages",
    "texture_request_cmp",
    "load_project_rgba",
    "texture_alpha_bounds",
    "pack_page_add",
    "pack_rect_contains",
    "pack_page_prune",
    "pack_page_split",
    "pack_page_place",
    "build_texture_layout",
    "global_frame_index",
    "write_sprite_masks",
    "write_sprt",
    "write_bgnd",
    "write_path",
    "write_tpag",
    "ref_rd16",
    "ref_rd32",
    "ref_rd32be",
    "ref_has",
    "ref_find_chunk",
    "ref_read_string",
    "free_ref_texture_layout",
    "ref_add_texture_frame",
    "ref_png_len",
    "validate_reference_texture_layout",
    "load_reference_texture_layout",
    "atlas_copy_png",
    "write_atlas_blob",
    "write_reference_atlas_blob",
    "write_txtr",
)
PACKAGE_TEXTURE_SHARED_OPERATIONS = (
    "free_ref_texture_layout",
    "load_reference_texture_layout",
    "write_bgnd",
    "write_path",
    "write_sprt",
    "write_tpag",
    "write_txtr",
)
PACKAGE_TEXTURE_PRIVATE_OPERATIONS = tuple(
    symbol
    for symbol in PACKAGE_TEXTURE_OPERATIONS
    if symbol not in PACKAGE_TEXTURE_SHARED_OPERATIONS
)
PACKAGE_TEXTURE_TYPES = (
    "TextureRequest",
    "PackRect",
    "PackPage",
    "RefChunk",
)
PACKAGE_TEXTURE_MACROS = (
    "GMLC_ATLAS_BASE_DIM",
    "GMLC_ATLAS_MAX_DIM",
    "GMLC_ATLAS_BORDER",
)
PACKAGE_CODE_OPERATIONS = (
    "add_code_ref",
    "add_code_blob_patch",
    "add_code_name_ref",
    "emit_code_data_prefix",
    "finalize_code_blobs",
    "same_ref_name",
    "cmp_ref_order",
    "patch_ref_chains",
    "ref_group_count",
    "ref_group_stats",
    "total_object_events",
    "total_timeline_moments",
    "event_type_rank",
    "room_has_creation_code",
    "room_code_count",
    "startup_code_count",
    "room_creation_code_index",
    "timeline_moment_code_index",
    "room_instance_creation_code_count",
    "trigger_code_base",
    "room_instance_creation_code_base",
    "room_instance_creation_code_index",
    "room_instance_index_by_id",
    "room_layer_contains_instance",
    "room_instance_creation_ordinal",
    "id_token",
    "event_suffix",
    "object_event_code_name",
    "script_code_name",
    "cmp_string_seed_event",
    "seed_code_blob_strings",
    "seed_compiled_path_strings",
    "seed_function_strings",
    "seed_code_string_order",
    "prepare_code_blob",
    "write_code_entry_header",
    "keep_or_emit_placeholder_blob",
    "compile_code_blob",
    "write_compiled_code_entry",
    "function_code_name",
    "write_function_code_entry",
    "write_code",
    "write_trig",
    "write_vari",
    "write_func",
)
PACKAGE_CODE_SHARED_OPERATIONS = (
    "event_type_rank",
    "room_code_count",
    "room_creation_code_index",
    "room_instance_creation_code_index",
    "seed_code_string_order",
    "timeline_moment_code_index",
    "total_timeline_moments",
    "write_code",
    "write_func",
    "write_trig",
    "write_vari",
)
PACKAGE_CODE_PRIVATE_OPERATIONS = tuple(
    symbol
    for symbol in PACKAGE_CODE_OPERATIONS
    if symbol not in PACKAGE_CODE_SHARED_OPERATIONS
)
PACKAGE_CODE_TYPES = (
    "RefOrder",
    "CodeEntryPlan",
    "StringSeedEvent",
)
PACKAGE_CHUNK_OPERATIONS = (
    "reserve",
    "wbytes",
    "wu8",
    "wu16",
    "wu32",
    "wu64",
    "wi32",
    "wf32",
    "zfill",
    "patch32",
    "patch64",
    "string_hash",
    "strtab_rehash",
    "intern",
    "add_str_patch",
    "add_frame_patch",
    "add_font_patch",
    "wstrptr",
    "chunk_begin",
    "chunk_end",
    "empty_list_chunk",
    "read_blob",
    "write_sond",
    "write_scpt",
    "join2",
    "join3",
    "with_crlf_first_newlines",
    "hlsl_zero_swizzle",
    "shader_looks_grayscale",
    "write_shdr",
    "write_tmln",
    "write_font",
    "write_audo",
    "fixed_zero_chunk",
    "write_classic_marker",
    "write_agrp",
    "write_optn",
    "write_embi",
    "write_gen8_uid_block",
    "identifier_from_name",
    "write_gen8",
    "object_event_code_index",
    "write_objt_event_action",
    "write_objt_event",
    "write_objt_events",
    "write_objt",
    "write_room_views",
    "write_room_instances",
    "write_room_backgrounds",
    "write_room_tiles",
    "write_room_layer_list",
    "write_room",
    "write_room_chunk",
    "write_strg",
)
PACKAGE_CHUNK_SHARED_OPERATIONS = (
    "add_frame_patch",
    "chunk_begin",
    "chunk_end",
    "empty_list_chunk",
    "fixed_zero_chunk",
    "intern",
    "patch32",
    "read_blob",
    "wbytes",
    "wf32",
    "wi32",
    "write_agrp",
    "write_audo",
    "write_classic_marker",
    "write_embi",
    "write_font",
    "write_gen8",
    "write_objt",
    "write_optn",
    "write_room_chunk",
    "write_scpt",
    "write_shdr",
    "write_sond",
    "write_strg",
    "write_tmln",
    "wstrptr",
    "wu16",
    "wu32",
    "wu8",
    "zfill",
)
PACKAGE_CHUNK_PRIVATE_OPERATIONS = tuple(
    symbol
    for symbol in PACKAGE_CHUNK_OPERATIONS
    if symbol not in PACKAGE_CHUNK_SHARED_OPERATIONS
)
PACKAGE_CHUNK_TABLES = (
    "gms2_glsles_vertex_prefix",
    "gms2_glsl_vertex_prefix",
    "gms2_glsles_fragment_prefix",
    "gms2_glsl_fragment_prefix",
    "gms2_hlsl_vertex_shared",
    "gms2_hlsl_vertex_color_tail",
    "gms2_hlsl_vertex_texcoord_tail",
    "gms2_hlsl_fragment_color_prefix",
    "gms2_hlsl_fragment_color_suffix",
    "gms2_hlsl_fragment_gray",
)
PACKAGE_COORDINATOR_OPERATIONS = {
    "development_flag_enabled",
    "write_file",
    "safe_relative_folder",
    "package_leaf_name",
    "materialize_included_files",
    "free_pkg",
    "gmlc_package_write_structural",
}
CLASSIC_IMPORT_SOURCES = (
    "src/content/classic/gmlc_classic_import.c",
    "src/content/classic/gmlc_classic_import_code.c",
    "src/content/classic/gmlc_classic_import_extensions.c",
    "src/content/classic/gmlc_classic_import_images.c",
    "src/content/classic/gmlc_classic_import_fonts.c",
    "src/content/classic/gmlc_classic_import_audio.c",
    "src/content/classic/gmlc_classic_import_paths.c",
    "src/content/classic/gmlc_classic_import_objects.c",
    "src/content/classic/gmlc_classic_import_timelines.c",
    "src/content/classic/gmlc_classic_import_rooms.c",
)
CLASSIC_SHARED_OPERATIONS = (
    "import_inflate_owned",
    "import_u32_at",
    "import_u32",
    "import_skip_words",
    "import_blob",
    "import_skip_string",
    "import_copy_string",
    "import_double",
    "copy_string",
    "cache_path",
    "import_source_path",
    "text_append",
    "import_actions",
)
CLASSIC_COMMON_SHARED_OPERATIONS = CLASSIC_SHARED_OPERATIONS[:-2]
CLASSIC_CODE_SHARED_OPERATIONS = CLASSIC_SHARED_OPERATIONS[-2:]
CLASSIC_COMMON_OPERATIONS = (
    *CLASSIC_COMMON_SHARED_OPERATIONS,
    "write_source",
)
CLASSIC_CODE_OPERATIONS = (
    "free_imported_scripts",
    "gmlc_classic_import_scripts",
    "text_reserve",
    "text_append_n",
    "text_append",
    "text_append_int",
    "text_append_quoted",
    "emit_action_call",
    "action_code_end_state",
    "emit_action_code_call",
    # Reading an action's argument as one expression needs to know where that expression ends, and
    # both helpers exist only for that: they are used by import_actions in this file and nowhere
    # else, so this is where they are owned.
    "action_word_operator",
    "action_expression_length",
    "import_actions",
)
CLASSIC_EXTENSION_OPERATIONS = (
    "classic_extension_u32",
    "classic_extension_skip",
    "classic_extension_string",
    "classic_extension_skip_string",
    "classic_extension_skip_blob",
    "classic_extension_blob",
    "classic_extension_identifier_equal",
    "classic_extension_normalize",
    "classic_extension_named_by_project",
    "classic_extension_script_target",
    "classic_extension_span_identifier_equal",
    "classic_extension_identifier_char",
    "classic_extension_define",
    "classic_extension_definition",
    "classic_extension_add_project_script",
    "classic_extension_rollback_scripts",
    "classic_extension_decode_script",
    "classic_extension_add_project_alias",
    "classic_extension_add_project_constant",
    "classic_extension_free_aliases",
    "classic_extension_parse",
    "classic_extension_suffix",
    "classic_extension_name_compare",
    "classic_extension_names_free",
    "classic_extension_names",
    "classic_extension_read_prefix",
    "classic_extension_hash_bytes",
    "classic_extension_hash_u64",
    "gmlc_classic_extension_dependency_hash",
    "gmlc_classic_import_extension_aliases",
)
CLASSIC_EXTENSION_LIMITS = (
    "CLASSIC_EXTENSION_FILE_LIMIT",
    "CLASSIC_EXTENSION_COMPRESSED_SCRIPT_LIMIT",
    "CLASSIC_EXTENSION_SCRIPT_LIMIT",
)
CLASSIC_IMAGE_OPERATIONS = (
    "free_imported_sprites",
    "write_rgba_png",
    "import_rgba_path",
    "import_bgra_to_rgba",
    "apply_legacy_executable_image_alpha",
    "decode_legacy_image",
    "gmlc_classic_import_sprites",
    "free_sprite_range",
    "free_imported_backgrounds",
    "gmlc_classic_import_backgrounds",
)
CLASSIC_FONT_OPERATIONS = (
    "free_imported_fonts",
    "classic_font_spec_free",
    "classic_font_raster_free",
    "classic_font_parse",
    "classic_font_build_compiled",
    "classic_font_normalize_face",
    "classic_font_prefix",
    "classic_font_files_for_face",
    "classic_font_find_leaf",
    "classic_font_style_leaf",
    "classic_font_resolve_file",
    "classic_font_codepoint",
    "classic_font_build_truetype",
    "classic_font_sample",
    "classic_font_build_fallback",
    "classic_font_store_raster",
    "gmlc_classic_import_fonts",
)
CLASSIC_AUDIO_OPERATIONS = (
    "write_binary",
    "import_binary_path",
    "free_imported_sounds",
    "gmlc_classic_import_sounds",
)
CLASSIC_PATH_OPERATIONS = (
    "free_imported_paths",
    "gmlc_classic_import_paths",
)
CLASSIC_OBJECT_OPERATIONS = (
    "free_imported_objects",
    "append_object_event",
    "classic_sprite_project_index",
    "gmlc_classic_import_objects",
)
CLASSIC_TIMELINE_OPERATIONS = (
    "free_imported_timelines",
    "gmlc_classic_import_timelines",
)
CLASSIC_ROOM_OPERATIONS = (
    "free_imported_rooms",
    "import_room_code",
    "gmlc_classic_import_rooms",
    "gmlc_classic_import_room_order",
)
CLASSIC_FONT_CATEGORIES = (
    "CLASSIC_FONT_SANS",
    "CLASSIC_FONT_MONO",
    "CLASSIC_FONT_SERIF",
)
CLASSIC_RESOURCE_FAMILY_OPERATIONS = {
    "src/content/classic/gmlc_classic_import_images.c":
        CLASSIC_IMAGE_OPERATIONS,
    "src/content/classic/gmlc_classic_import_fonts.c":
        CLASSIC_FONT_OPERATIONS,
    "src/content/classic/gmlc_classic_import_audio.c":
        CLASSIC_AUDIO_OPERATIONS,
    "src/content/classic/gmlc_classic_import_paths.c":
        CLASSIC_PATH_OPERATIONS,
    "src/content/classic/gmlc_classic_import_objects.c":
        CLASSIC_OBJECT_OPERATIONS,
    "src/content/classic/gmlc_classic_import_timelines.c":
        CLASSIC_TIMELINE_OPERATIONS,
    "src/content/classic/gmlc_classic_import_rooms.c":
        CLASSIC_ROOM_OPERATIONS,
}
LEXER_OPERATIONS = (
    "dup_range",
    "compiler_source_text",
    "compiler_read_source",
    "lx_skip",
    "lx_next",
    "word_match_at",
    "trim_span",
    "span_empty",
    "skip_ws_comments_at",
    "scan_matching_delim",
    "function_shape_at",
)
LEXER_PRIVATE_OPERATIONS = ("compiler_source_text", "lx_skip")
REGISTRY_OPERATIONS = (
    "source_room_code_count",
    "registry_extra_so_far",
    "gmlc_function_registry_extra_count",
    "registry_add_global",
    "registry_add_macro",
    "registry_add_constant",
    "registry_collect_macros_from_text",
    "registry_add_asset",
    "asset_binding_cmp",
    "registry_collect_assets",
    "collect_globalvars_from_text",
    "registry_add_function",
    "scan_function_at",
    "name_list_free",
    "name_list_has",
    "name_list_add",
    "prev_nonspace_is_dot",
    "skip_string_or_comment",
    "collect_param_names",
    "collect_var_decls",
    "function_body_uses_outer_local",
    "detect_unsupported_function_capture",
    "collect_functions_from_text",
    "gmlc_bytecode_collect_functions",
    "gmlc_function_registry_free",
)
EMITTER_OPERATIONS = (
    "reserve",
    "emit_u32",
    "emit_i32",
    "emit_double",
    "patch_u32",
    "fw",
    "add_ref",
    "add_string_site",
    "emit_push_real",
    "emit_push_i16_full",
    "emit_push_i32_full",
    "emit_const_number",
    "expr_not_const",
    "emit_push_string_literal",
    "emit_push_var",
    "emit_pop_var",
    "classic_identifier_equal",
    "classic_alias_identifier_equal",
    "canonical_call_name",
    "emit_call",
    "emit_callv",
    "emit_branch",
    "patch_branch",
    "emit_binary",
    "emit_binary_typed",
    "emit_cmp",
    "emit_cmp_typed",
    "emit_conv",
    "emit_condition_bool",
)
EMITTER_PRIVATE_OPERATIONS = (
    "reserve",
    "emit_i32",
    "emit_double",
    "patch_u32",
    "add_ref",
    "add_string_site",
    "classic_identifier_equal",
    "classic_alias_identifier_equal",
    "canonical_call_name",
)
EMITTER_SHARED_OPERATIONS = tuple(
    symbol
    for symbol in EMITTER_OPERATIONS
    if symbol not in EMITTER_PRIVATE_OPERATIONS
)
PARSER_OPERATIONS = (
    "tok_is",
    "compile_expr_slice",
    "scan_comma_spans_until",
    "scan_switch_cases",
    "emit_lvalue_address",
    "parse_primary",
    "parse_expr",
    "parse_lvalue_from_name",
    "parse_statement",
    "collect_macros",
    "emit_param_prologue",
    "compile_text_internal",
)
PARSER_PRIVATE_OPERATIONS = tuple(
    symbol for symbol in PARSER_OPERATIONS if symbol != "compile_text_internal"
)
BYTECODE_FACADE_OPERATIONS = {
    "find_script_wrapper",
    "gmlc_bytecode_compile_function_body",
    "gmlc_bytecode_compile_source",
    "gmlc_bytecode_compile_source_ex",
    "gmlc_bytecode_emit_empty",
    "gmlc_bytecode_free",
}


def fail(message: str) -> None:
    raise SystemExit(f"content boundary error: {message}")


def make_list(variable: str, seen: tuple[str, ...] = ()) -> list[str]:
    if variable in seen:
        fail("cyclic Make source group: " + " -> ".join((*seen, variable)))
    lines = MAKEFILE.read_text(encoding="utf-8").splitlines()
    values: list[str] = []
    collecting = False
    for line in lines:
        if not collecting:
            match = re.match(rf"^{re.escape(variable)}\s*:?=\s*(.*)$", line)
            if not match:
                continue
            remainder = match.group(1).rstrip()
        else:
            remainder = line.strip()
        collecting = remainder.endswith("\\")
        remainder = remainder.removesuffix("\\").strip()
        if remainder:
            values.extend(remainder.split())
        if values and not collecting:
            break
    if not values:
        fail(f"missing Make source group {variable}")
    expanded: list[str] = []
    for value in values:
        reference = re.fullmatch(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)", value)
        if reference:
            expanded.extend(make_list(reference.group(1), (*seen, variable)))
        elif value.startswith("$("):
            fail(f"unsupported Make expression {value} in {variable}")
        else:
            expanded.append(value)
    return expanded


def definition_locations(symbol: str) -> list[str]:
    pattern = re.compile(
        rf"(?m)^(?:static\s+)?[A-Za-z_][^;{{}}]*\b{re.escape(symbol)}"
        rf"\s*\([^;{{}}]*\)\s*\{{"
    )
    return sorted(
        path.relative_to(ROOT).as_posix()
        for path in PROJECT.glob("*.c")
        if pattern.search(path.read_text(encoding="utf-8"))
    )


def compiler_definition_locations(symbol: str) -> list[str]:
    pattern = re.compile(
        rf"(?m)^(?:static\s+)?[A-Za-z_][^;{{}}]*\b{re.escape(symbol)}"
        rf"\s*\([^;{{}}]*\)\s*\{{"
    )
    return sorted(
        relative
        for relative in COMPILER_SOURCES
        if pattern.search((ROOT / relative).read_text(encoding="utf-8"))
    )


def package_definition_locations(symbol: str) -> list[str]:
    pattern = re.compile(
        rf"(?m)^(?:static\s+)?[A-Za-z_][^;{{}}]*\b{re.escape(symbol)}"
        rf"\s*\([^;{{}}]*\)\s*\{{"
    )
    return sorted(
        relative
        for relative in PACKAGE_SOURCES
        if pattern.search((ROOT / relative).read_text(encoding="utf-8"))
    )


def classic_definition_locations(symbol: str) -> list[str]:
    pattern = re.compile(
        rf"(?m)^(?:static\s+)?[A-Za-z_][^;{{}}]*\b{re.escape(symbol)}"
        rf"\s*\([^;{{}}]*\)\s*\{{"
    )
    return sorted(
        relative
        for relative in CLASSIC_IMPORT_SOURCES
        if pattern.search((ROOT / relative).read_text(encoding="utf-8"))
    )


def definition_symbols(path: Path) -> set[str]:
    pattern = re.compile(
        r"(?m)^(?:static\s+)?"
        r"(?:[A-Za-z_][A-Za-z0-9_]*\s+|"
        r"[A-Za-z_][A-Za-z0-9_]*\s*\*\s*)+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{"
    )
    return set(pattern.findall(path.read_text(encoding="utf-8")))


def main() -> int:
    compiler_sources = make_list("ANYGM_PROJECT_COMPILER_SOURCES")
    if len(compiler_sources) != len(set(compiler_sources)):
        fail("ANYGM_PROJECT_COMPILER_SOURCES contains a duplicate")
    if tuple(compiler_sources) != COMPILER_SOURCES:
        fail(
            "ANYGM_PROJECT_COMPILER_SOURCES differs from the declared owner order"
        )
    package_sources = make_list("ANYGM_PROJECT_PACKAGE_SOURCES")
    if len(package_sources) != len(set(package_sources)):
        fail("ANYGM_PROJECT_PACKAGE_SOURCES contains a duplicate")
    if tuple(package_sources) != PACKAGE_SOURCES:
        fail(
            "ANYGM_PROJECT_PACKAGE_SOURCES differs from the declared owner order"
        )
    classic_import_sources = make_list("ANYGM_CLASSIC_IMPORT_SOURCES")
    if len(classic_import_sources) != len(set(classic_import_sources)):
        fail("ANYGM_CLASSIC_IMPORT_SOURCES contains a duplicate")
    if tuple(classic_import_sources) != CLASSIC_IMPORT_SOURCES:
        fail(
            "ANYGM_CLASSIC_IMPORT_SOURCES differs from the declared owner order"
        )

    for symbol in LEXER_OPERATIONS:
        locations = definition_locations(symbol)
        if locations != ["src/content/project/gmlc_lexer.c"]:
            fail(f"{symbol} definitions are {locations}, expected only gmlc_lexer.c")

    for symbol in REGISTRY_OPERATIONS:
        locations = definition_locations(symbol)
        if locations != ["src/content/project/gmlc_function_registry.c"]:
            fail(
                f"{symbol} definitions are {locations}, expected only "
                "gmlc_function_registry.c"
            )

    for symbol in EMITTER_OPERATIONS:
        locations = compiler_definition_locations(symbol)
        if locations != ["src/content/project/gmlc_emitter.c"]:
            fail(
                f"{symbol} definitions are {locations}, expected only "
                "gmlc_emitter.c"
            )

    for symbol in PARSER_OPERATIONS:
        locations = compiler_definition_locations(symbol)
        if locations != ["src/content/project/gmlc_parser.c"]:
            fail(
                f"{symbol} definitions are {locations}, expected only "
                "gmlc_parser.c"
            )

    facade_symbols = definition_symbols(PROJECT / "gmlc_bytecode.c")
    if facade_symbols != BYTECODE_FACADE_OPERATIONS:
        fail(
            "gmlc_bytecode.c facade definitions are "
            f"{sorted(facade_symbols)}, expected "
            f"{sorted(BYTECODE_FACADE_OPERATIONS)}"
        )

    name_list = re.compile(r"}\s*NameList\s*;")
    name_list_locations = sorted(
        path.relative_to(ROOT).as_posix()
        for path in PROJECT.glob("*.c")
        if name_list.search(path.read_text(encoding="utf-8"))
    )
    if name_list_locations != [
        "src/content/project/gmlc_function_registry.c"
    ]:
        fail(f"NameList definitions are {name_list_locations}")

    for type_name in ("CaseRec", "LValue"):
        type_pattern = re.compile(rf"}}\s*{re.escape(type_name)}\s*;")
        type_locations = sorted(
            path.relative_to(ROOT).as_posix()
            for path in PROJECT.glob("*.c")
            if type_pattern.search(path.read_text(encoding="utf-8"))
        )
        if type_locations != ["src/content/project/gmlc_parser.c"]:
            fail(f"{type_name} definitions are {type_locations}")

    internal_include = '#include "gmlc_bytecode_internal.h"'
    consumers = sorted(
        path.relative_to(ROOT).as_posix()
        for path in ROOT.glob("src/**/*.c")
        if internal_include in path.read_text(encoding="utf-8")
    )
    if consumers != sorted(COMPILER_SOURCES):
        fail(f"gmlc_bytecode_internal.h consumers are {consumers}")

    package_internal_include = '#include "gmlc_package_internal.h"'
    package_consumers = sorted(
        path.relative_to(ROOT).as_posix()
        for path in ROOT.glob("src/**/*.c")
        if package_internal_include in path.read_text(encoding="utf-8")
    )
    if package_consumers != sorted(PACKAGE_SOURCES):
        fail(f"gmlc_package_internal.h consumers are {package_consumers}")

    classic_internal_include = '#include "gmlc_classic_import_internal.h"'
    classic_internal_consumers = sorted(
        path.relative_to(ROOT).as_posix()
        for path in ROOT.glob("src/**/*.c")
        if classic_internal_include in path.read_text(encoding="utf-8")
    )
    if classic_internal_consumers != sorted(CLASSIC_IMPORT_SOURCES):
        fail(
            "gmlc_classic_import_internal.h consumers are "
            f"{classic_internal_consumers}"
        )

    for type_name in ("ImportReader", "ImportText"):
        type_pattern = re.compile(rf"}}\s*{re.escape(type_name)}\s*;")
        type_locations = sorted(
            path.relative_to(ROOT).as_posix()
            for path in ROOT.glob("src/content/classic/gmlc_classic_import*.[ch]")
            if type_pattern.search(path.read_text(encoding="utf-8"))
        )
        if type_locations != [
            "src/content/classic/gmlc_classic_import_internal.h"
        ]:
            fail(f"{type_name} definitions are {type_locations}")

    classic_code_path = (
        ROOT / "src/content/classic/gmlc_classic_import_code.c"
    )
    classic_code_symbols = definition_symbols(classic_code_path)
    if classic_code_symbols != set(CLASSIC_CODE_OPERATIONS):
        fail(
            "gmlc_classic_import_code.c definitions are "
            f"{sorted(classic_code_symbols)}, expected "
            f"{sorted(CLASSIC_CODE_OPERATIONS)}"
        )
    classic_extension_path = (
        ROOT / "src/content/classic/gmlc_classic_import_extensions.c"
    )
    classic_extension_symbols = definition_symbols(classic_extension_path)
    if classic_extension_symbols != set(CLASSIC_EXTENSION_OPERATIONS):
        fail(
            "gmlc_classic_import_extensions.c definitions are "
            f"{sorted(classic_extension_symbols)}, expected "
            f"{sorted(CLASSIC_EXTENSION_OPERATIONS)}"
        )

    for symbol in CLASSIC_CODE_OPERATIONS:
        locations = classic_definition_locations(symbol)
        if locations != [
            "src/content/classic/gmlc_classic_import_code.c"
        ]:
            fail(
                f"{symbol} definitions are {locations}, expected only "
                "gmlc_classic_import_code.c"
            )
    for symbol in CLASSIC_EXTENSION_OPERATIONS:
        locations = classic_definition_locations(symbol)
        if locations != [
            "src/content/classic/gmlc_classic_import_extensions.c"
        ]:
            fail(
                f"{symbol} definitions are {locations}, expected only "
                "gmlc_classic_import_extensions.c"
            )
    for symbol in CLASSIC_COMMON_SHARED_OPERATIONS:
        locations = classic_definition_locations(symbol)
        if locations != ["src/content/classic/gmlc_classic_import.c"]:
            fail(
                f"{symbol} definitions are {locations}, expected only "
                "gmlc_classic_import.c"
            )

    classic_common_symbols = definition_symbols(
        ROOT / "src/content/classic/gmlc_classic_import.c"
    )
    if classic_common_symbols != set(CLASSIC_COMMON_OPERATIONS):
        fail(
            "gmlc_classic_import.c shared-service definitions are "
            f"{sorted(classic_common_symbols)}, expected "
            f"{sorted(CLASSIC_COMMON_OPERATIONS)}"
        )

    for relative, operations in CLASSIC_RESOURCE_FAMILY_OPERATIONS.items():
        family_symbols = definition_symbols(ROOT / relative)
        if family_symbols != set(operations):
            fail(
                f"{relative} definitions are {sorted(family_symbols)}, "
                f"expected {sorted(operations)}"
            )
        for symbol in operations:
            locations = classic_definition_locations(symbol)
            if locations != [relative]:
                fail(
                    f"{symbol} definitions are {locations}, expected only "
                    f"{relative}"
                )

    for type_name, owner in (
        ("ActionCodeState", "gmlc_classic_import_code.c"),
        ("ClassicExtensionReader", "gmlc_classic_import_extensions.c"),
        ("ClassicExtensionAlias", "gmlc_classic_import_extensions.c"),
        ("ClassicFontSpec", "gmlc_classic_import_fonts.c"),
        ("ClassicFontRaster", "gmlc_classic_import_fonts.c"),
        ("ClassicFontFiles", "gmlc_classic_import_fonts.c"),
        ("ClassicFontFile", "gmlc_classic_import_fonts.c"),
    ):
        type_pattern = re.compile(rf"}}\s*{re.escape(type_name)}\s*;")
        type_locations = sorted(
            path.relative_to(ROOT).as_posix()
            for relative in CLASSIC_IMPORT_SOURCES
            if type_pattern.search(
                (path := ROOT / relative).read_text(encoding="utf-8")
            )
        )
        expected = [f"src/content/classic/{owner}"]
        if type_locations != expected:
            fail(f"{type_name} definitions are {type_locations}")

    for constant in CLASSIC_EXTENSION_LIMITS:
        constant_pattern = re.compile(
            rf"(?m)^\s*{re.escape(constant)}\s*="
        )
        constant_locations = sorted(
            relative
            for relative in CLASSIC_IMPORT_SOURCES
            if constant_pattern.search(
                (ROOT / relative).read_text(encoding="utf-8")
            )
        )
        if constant_locations != [
            "src/content/classic/gmlc_classic_import_extensions.c"
        ]:
            fail(f"{constant} definitions are {constant_locations}")

    for constant in CLASSIC_FONT_CATEGORIES:
        constant_pattern = re.compile(
            rf"(?m)^\s*enum\s*\{{[^}}]*\b{re.escape(constant)}\b[^}}]*\}}\s*;"
        )
        constant_locations = sorted(
            relative
            for relative in CLASSIC_IMPORT_SOURCES
            if constant_pattern.search(
                (ROOT / relative).read_text(encoding="utf-8")
            )
        )
        if constant_locations != [
            "src/content/classic/gmlc_classic_import_fonts.c"
        ]:
            fail(f"{constant} definitions are {constant_locations}")

    for type_name in PACKAGE_PRIVATE_TYPES:
        type_pattern = re.compile(rf"}}\s*{re.escape(type_name)}\s*;")
        type_locations = sorted(
            path.relative_to(ROOT).as_posix()
            for path in PROJECT.glob("gmlc_package*.[ch]")
            if type_pattern.search(path.read_text(encoding="utf-8"))
        )
        if type_locations != [
            "src/content/project/gmlc_package_internal.h"
        ]:
            fail(f"{type_name} definitions are {type_locations}")

    for symbol in PACKAGE_TEXTURE_OPERATIONS:
        locations = definition_locations(symbol)
        if locations != [
            "src/content/project/gmlc_package_textures.c"
        ]:
            fail(
                f"{symbol} definitions are {locations}, expected only "
                "gmlc_package_textures.c"
            )

    for type_name in PACKAGE_TEXTURE_TYPES:
        type_pattern = re.compile(rf"}}\s*{re.escape(type_name)}\s*;")
        type_locations = sorted(
            path.relative_to(ROOT).as_posix()
            for relative in PACKAGE_SOURCES
            if type_pattern.search(
                (path := ROOT / relative).read_text(encoding="utf-8")
            )
        )
        if type_locations != [
            "src/content/project/gmlc_package_textures.c"
        ]:
            fail(f"{type_name} definitions are {type_locations}")

    for macro in PACKAGE_TEXTURE_MACROS:
        macro_pattern = re.compile(
            rf"(?m)^#define\s+{re.escape(macro)}(?:\s|$)"
        )
        macro_locations = sorted(
            relative
            for relative in PACKAGE_SOURCES
            if macro_pattern.search(
                (ROOT / relative).read_text(encoding="utf-8")
            )
        )
        if macro_locations != [
            "src/content/project/gmlc_package_textures.c"
        ]:
            fail(f"{macro} definitions are {macro_locations}")

    for symbol in PACKAGE_CODE_OPERATIONS:
        locations = definition_locations(symbol)
        if locations != ["src/content/project/gmlc_package_code.c"]:
            fail(
                f"{symbol} definitions are {locations}, expected only "
                "gmlc_package_code.c"
            )

    for type_name in PACKAGE_CODE_TYPES:
        type_pattern = re.compile(rf"}}\s*{re.escape(type_name)}\s*;")
        type_locations = sorted(
            path.relative_to(ROOT).as_posix()
            for relative in PACKAGE_SOURCES
            if type_pattern.search(
                (path := ROOT / relative).read_text(encoding="utf-8")
            )
        )
        if type_locations != ["src/content/project/gmlc_package_code.c"]:
            fail(f"{type_name} definitions are {type_locations}")

    for symbol in PACKAGE_CHUNK_OPERATIONS:
        locations = package_definition_locations(symbol)
        if locations != ["src/content/project/gmlc_package_chunks.c"]:
            fail(
                f"{symbol} definitions are {locations}, expected only "
                "gmlc_package_chunks.c"
            )

    for table in PACKAGE_CHUNK_TABLES:
        table_pattern = re.compile(
            rf"(?m)^static\s+const\s+char\s+{re.escape(table)}\s*\[\]\s*="
        )
        table_locations = sorted(
            relative
            for relative in PACKAGE_SOURCES
            if table_pattern.search(
                (ROOT / relative).read_text(encoding="utf-8")
            )
        )
        if table_locations != ["src/content/project/gmlc_package_chunks.c"]:
            fail(f"{table} definitions are {table_locations}")

    coordinator_symbols = definition_symbols(PROJECT / "gmlc_package.c")
    if coordinator_symbols != PACKAGE_COORDINATOR_OPERATIONS:
        fail(
            "gmlc_package.c coordinator definitions are "
            f"{sorted(coordinator_symbols)}, expected "
            f"{sorted(PACKAGE_COORDINATOR_OPERATIONS)}"
        )

    internal = (PROJECT / "gmlc_bytecode_internal.h").read_text(encoding="utf-8")
    for symbol in LEXER_PRIVATE_OPERATIONS:
        if re.search(rf"\b{re.escape(symbol)}\s*\([^;]*;", internal):
            fail(f"owner-local lexer helper {symbol} is declared in the interface")
    for symbol in EMITTER_PRIVATE_OPERATIONS:
        if re.search(rf"\b{re.escape(symbol)}\s*\([^;]*;", internal):
            fail(f"owner-local emitter helper {symbol} is declared in the interface")
    for symbol in EMITTER_SHARED_OPERATIONS:
        if len(re.findall(rf"\b{re.escape(symbol)}\s*\([^;]*;", internal)) != 1:
            fail(f"{symbol} needs exactly one compiler-private declaration")
    for symbol in PARSER_PRIVATE_OPERATIONS:
        if re.search(rf"\b{re.escape(symbol)}\s*\([^;]*;", internal):
            fail(f"owner-local parser helper {symbol} is declared in the interface")
    if len(re.findall(r"\bcompile_text_internal\s*\([^;]*;", internal)) != 1:
        fail("compile_text_internal needs exactly one compiler-private declaration")
    if len(re.findall(r"\bsource_room_code_count\s*\([^;]*;", internal)) != 1:
        fail("source_room_code_count needs exactly one compiler-private declaration")

    package_internal = (
        PROJECT / "gmlc_package_internal.h"
    ).read_text(encoding="utf-8")
    for symbol in PACKAGE_TEXTURE_PRIVATE_OPERATIONS:
        if re.search(
            rf"\b{re.escape(symbol)}\s*\([^;]*;",
            package_internal,
        ):
            fail(
                f"owner-local texture helper {symbol} is declared in the "
                "package interface"
            )
    for symbol in PACKAGE_TEXTURE_SHARED_OPERATIONS:
        if len(
            re.findall(
                rf"\b{re.escape(symbol)}\s*\([^;]*;",
                package_internal,
            )
        ) != 1:
            fail(f"{symbol} needs exactly one package-private declaration")
    for symbol in PACKAGE_CODE_PRIVATE_OPERATIONS:
        if re.search(
            rf"\b{re.escape(symbol)}\s*\([^;]*;",
            package_internal,
        ):
            fail(
                f"owner-local code helper {symbol} is declared in the "
                "package interface"
            )
    for symbol in PACKAGE_CODE_SHARED_OPERATIONS:
        if len(
            re.findall(
                rf"\b{re.escape(symbol)}\s*\([^;]*;",
                package_internal,
            )
        ) != 1:
            fail(f"{symbol} needs exactly one package-private declaration")
    for symbol in PACKAGE_CHUNK_PRIVATE_OPERATIONS:
        if re.search(
            rf"\b{re.escape(symbol)}\s*\([^;]*;",
            package_internal,
        ):
            fail(
                f"owner-local chunk helper {symbol} is declared in the "
                "package interface"
            )
    for symbol in PACKAGE_CHUNK_SHARED_OPERATIONS:
        if len(
            re.findall(
                rf"\b{re.escape(symbol)}\s*\([^;]*;",
                package_internal,
            )
        ) != 1:
            fail(f"{symbol} needs exactly one package-private declaration")

    classic_internal = (
        ROOT / "src/content/classic/gmlc_classic_import_internal.h"
    ).read_text(encoding="utf-8")
    declaration_pattern = re.compile(
        r"(?m)^(?:[A-Za-z_][A-Za-z0-9_]*\s+|"
        r"[A-Za-z_][A-Za-z0-9_]*\s*\*\s*)+"
        r"([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*;"
    )
    classic_declarations = set(
        declaration_pattern.findall(classic_internal)
    )
    if classic_declarations != set(CLASSIC_SHARED_OPERATIONS):
        fail(
            "classic import private declarations are "
            f"{sorted(classic_declarations)}, expected "
            f"{sorted(CLASSIC_SHARED_OPERATIONS)}"
        )
    for symbol in CLASSIC_SHARED_OPERATIONS:
        if len(
            re.findall(
                rf"\b{re.escape(symbol)}\s*\([^;]*;",
                classic_internal,
            )
        ) != 1:
            fail(f"{symbol} needs exactly one classic-private declaration")

    print("content boundaries: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
