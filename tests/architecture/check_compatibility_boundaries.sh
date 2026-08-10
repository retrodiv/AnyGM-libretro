#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 retrodiv <retrodiv@proton.me>

set -eu

fail_matches() {
  message=$1
  matches=$2
  if [ -n "$matches" ]; then
    printf '%s\n' "$message" >&2
    printf '%s\n' "$matches" >&2
    exit 1
  fi
}

portable_product_files() {
  find src/api src/core src/compatibility src/content src/runtime src/video src/audio src/host \
    -type f \( -name '*.c' -o -name '*.h' \) \
    -not -path '*/third_party/*' -not -path '*/generated/*'
}

runtime_files() {
  find src/core src/runtime src/video src/audio -type f \
    \( -name '*.c' -o -name '*.h' \)
}

transitional_engine_aliases=$(find src/core -type f \( -name '*.c' -o -name '*.h' \) \
  -exec grep -n -H -E \
  '^#[[:space:]]*define[[:space:]]+g_[[:alnum:]_]+[[:space:]]+[(]engine->[[:alnum:]_]+[)]' \
  {} + 2>/dev/null || true)
fail_matches "Transitional engine field aliases must not return:" \
  "$transitional_engine_aliases"

local_product_externs=$(find src -type f -name '*.c' \
  -not -path 'src/third_party/*' -exec grep -n -H -E \
  '^[[:space:]]*extern[[:space:]]' {} + 2>/dev/null || true)
fail_matches "First-party implementation files must include owning declarations instead of local externs:" \
  "$local_product_externs"

video_builtin_coupling=$(find src/video -type f \( -name '*.c' -o -name '*.h' \) \
  -exec grep -n -H -E 'gml_builtin_|gml_builtin\.h' {} + 2>/dev/null || true)
fail_matches "Video code must not depend on builtin implementation:" "$video_builtin_coupling"

software3d_vm_coupling=$(find src/video/software3d -type f \( -name '*.c' -o -name '*.h' \) \
  -exec grep -n -H -E '(^|[^[:alnum:]_])(GmlVM|gml_vm_[[:alnum:]_]*|graphics_state_for_vm)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Software-3D code must consume typed contexts without depending on VM ownership:" \
  "$software3d_vm_coupling"

legacy_graphics_state=$(portable_product_files | xargs grep -n -H 'GmlGraphicsState' 2>/dev/null || true)
fail_matches "The retired builtin-owned GmlGraphicsState type must not return:" \
  "$legacy_graphics_state"

software3d_type_definitions=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*struct[[:space:]]+GmlSoftware3D[[:space:]]*\{' 2>/dev/null || true)
if [ "$software3d_type_definitions" != "src/video/software3d/gml_software3d_internal.h" ]; then
  printf '%s\n' "Only the software-3D internal header may define GmlSoftware3D:" >&2
  printf '%s\n' "$software3d_type_definitions" >&2
  exit 1
fi

renderer_type_definitions=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*(typedef[[:space:]]+)?struct[[:space:]]+GmlRender[[:space:]]*\{' \
  2>/dev/null || true)
if [ "$renderer_type_definitions" != "src/video/renderer/gml_render_internal.h" ]; then
  printf '%s\n' "Only gml_render_internal.h may define renderer storage:" >&2
  printf '%s\n' "$renderer_type_definitions" >&2
  exit 1
fi

value_type_definitions=$(portable_product_files | xargs grep -l -E \
  '}[[:space:]]+(GmlVal|GmlArr|GmlVarSlot|GmlVarMap);' 2>/dev/null || true)
if [ "$value_type_definitions" != "src/runtime/vm/gml_value.h" ]; then
  printf '%s\n' "Language value, array, and variable-map record definitions belong only to gml_value.h:" >&2
  printf '%s\n' "$value_type_definitions" >&2
  exit 1
fi

value_free_context_definitions=$(portable_product_files | xargs grep -l -E \
  '}[[:space:]]+GmlValueFreeContext;' 2>/dev/null || true)
if [ "$value_free_context_definitions" != "src/runtime/vm/gml_value_internal.h" ]; then
  printf '%s\n' "The value teardown context belongs only to gml_value_internal.h:" >&2
  printf '%s\n' "$value_free_context_definitions" >&2
  exit 1
fi

external_value_internal_includes=$(find src -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '^[[:space:]]*#include[[:space:]]+"gml_value_internal[.]h"' {} + 2>/dev/null | \
  grep -v -E '^src/runtime/vm/[^/]+[.]c$' || true)
fail_matches "Only VM implementation owners may include gml_value_internal.h:" \
  "$external_value_internal_includes"

external_vm_internal_includes=$(find src -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '^[[:space:]]*#include[[:space:]]+"gml_vm_internal[.]h"' {} + 2>/dev/null | \
  grep -v -E '^src/runtime/vm/[^/]+[.]c$' || true)
fail_matches "Only VM implementation owners may include gml_vm_internal.h:" \
  "$external_vm_internal_includes"

value_implementation_owners=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*(unsigned[[:space:]]+gml_value_name_hash|GmlVal[[:space:]]+gml_arr_new|void[[:space:]]+gml_varmap_free)[(].*[{]' \
  2>/dev/null || true)
if [ "$value_implementation_owners" != "src/runtime/vm/gml_value.c" ]; then
  printf '%s\n' "Language value implementation belongs only to gml_value.c:" >&2
  printf '%s\n' "$value_implementation_owners" >&2
  exit 1
fi

vm_state_implementation_owners=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*(size_t[[:space:]]+gml_vm_state_size|int[[:space:]]+gml_vm_state_(save|load))[(].*[{]' \
  2>/dev/null || true)
if [ "$vm_state_implementation_owners" != "src/runtime/vm/gml_vm_state.c" ]; then
  printf '%s\n' "Canonical VM state implementation belongs only to gml_vm_state.c:" >&2
  printf '%s\n' "$vm_state_implementation_owners" >&2
  exit 1
fi

builtin_state_definitions=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*struct[[:space:]]+GmlBuiltinState[[:space:]]*\{' \
  2>/dev/null || true)
if [ "$builtin_state_definitions" != "src/runtime/builtins/gml_builtin_internal.h" ]; then
  printf '%s\n' "Builtin resource storage must have one private GmlBuiltinState definition:" >&2
  printf '%s\n' "$builtin_state_definitions" >&2
  exit 1
fi

builtin_state_implementation_owners=$(find src -type f -name '*.c' \
  -not -path '*/third_party/*' -not -path '*/generated/*' \
  -exec grep -l -E \
  '^[[:space:]]*(GmlBuiltinState[[:space:]]*[*][[:space:]]*gml_builtin_state_(create|ensure)|void[[:space:]]+gml_builtin_state_(reset|destroy|write_ini_ds|write_physics|write_audio|write_time_sources)|int[[:space:]]+gml_builtin_state_(read_ini_ds|read_physics|read_audio|read_time_sources))[(]' \
  {} + 2>/dev/null || true)
if [ "$builtin_state_implementation_owners" != "src/runtime/builtins/gml_builtin_state.c" ]; then
  printf '%s\n' "Builtin resource lifetime and canonical state sections belong only to gml_builtin_state.c:" >&2
  printf '%s\n' "$builtin_state_implementation_owners" >&2
  exit 1
fi

builtin_state_source_count=$(grep -c -E \
  '^[[:space:]]*src/runtime/builtins/gml_builtin_state[.]c[[:space:]\\]*$' \
  Makefile.common 2>/dev/null || true)
if [ "$builtin_state_source_count" -ne 1 ]; then
  printf '%s\n' "ANYGM_BUILTIN_SOURCES must contain gml_builtin_state.c exactly once." >&2
  exit 1
fi

vm_builtin_resource_storage=$(grep -n -E \
  '(^|[^[:alnum:]_])(GmlDS(Map|List|Grid)|GmlTimeSource|GmlPhysics(Fixture|Joint)|GML_(INI_MAX|DS_(MAP|LIST|GRID)_MAX|TIME_SOURCE_(MAX|ID_BASE)|MAX_EMITTERS|PHYS_(FIXTURE|JOINT)_MAX)|ini_(kv|n|open|path)|bin_file|next_buffer_id|async_(sl|http|group)|next_ds_id|ds_(map|list|grid|map_last_slot|list_compat_repair)|next_time_source_id|time_source(_game_state)?|emitter_(live|gain|x|y|z|ref|max|factor)|listener_(x|y|z|forward|up)|audio_falloff_model|phys_(fixture|joint|next_id|gravity|update|paused|debug_draw))([^[:alnum:]_]|$)' \
  src/runtime/vm/gml_vm.h 2>/dev/null || true)
fail_matches "GmlVM must retain only the opaque builtin-state pointer, not builtin resource storage:" \
  "$vm_builtin_resource_storage"

external_builtin_state_access=$(find src -type f \
  \( -name '*.c' -o -name '*.h' \) -not -path 'src/runtime/builtins/*' \
  -exec grep -n -H -E -- '->[[:space:]]*builtins[[:space:]]*->[[:space:]]*[A-Za-z_]' \
  {} + 2>/dev/null || true)
fail_matches "Code outside builtin owners must use coarse GmlBuiltinState operations:" \
  "$external_builtin_state_access"

vm_state_codec_consumers=$(find src -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '^[[:space:]]*#include[[:space:]]+"gml_vm_state_codec[.]h"' {} + \
  2>/dev/null | LC_ALL=C sort || true)
if [ "$vm_state_codec_consumers" != "src/runtime/builtins/gml_builtin_state.c
src/runtime/vm/gml_vm_state.c" ]; then
  printf '%s\n' "Only canonical VM state and builtin resource state may consume the opaque state codec:" >&2
  printf '%s\n' "$vm_state_codec_consumers" >&2
  exit 1
fi

vm_rng_implementation_owners=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*(void[[:space:]]+gml_rng_seed|double[[:space:]]+gml_rng_value)[(].*[{]' \
  2>/dev/null || true)
if [ "$vm_rng_implementation_owners" != "src/runtime/vm/gml_vm_rng.c" ]; then
  printf '%s\n' "The compatibility-aware VM random stream belongs only to gml_vm_rng.c:" >&2
  printf '%s\n' "$vm_rng_implementation_owners" >&2
  exit 1
fi

vm_rooms_implementation_owners=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*(int|void)[[:space:]]+(gml_room_enter|gml_vm_room_reload_layers_mode|gml_vm_rooms_(init|step_paths|step_timelines)|gml_tile_layer_(delete|depth|shift|hide))[(].*[{]' \
  2>/dev/null || true)
if [ "$vm_rooms_implementation_owners" != "src/runtime/vm/gml_vm_rooms.c" ]; then
  printf '%s\n' "Room, layer, tilemap, path, and timeline implementation belongs only to gml_vm_rooms.c:" >&2
  printf '%s\n' "$vm_rooms_implementation_owners" >&2
  exit 1
fi

vm_frame_implementation_owners=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*(double[[:space:]]+gml_legacy_view_follow_axis|void[[:space:]]+gml_vm_(step|draw|post_draw|draw_pass|draw_gui|frame_cleanup))[(].*[{]' \
  2>/dev/null || true)
if [ "$vm_frame_implementation_owners" != "src/runtime/vm/gml_vm_frame.c" ]; then
  printf '%s\n' "Step, draw, and frame-scratch implementation belongs only to gml_vm_frame.c:" >&2
  printf '%s\n' "$vm_frame_implementation_owners" >&2
  exit 1
fi

vm_exec_implementation_owners=$(find src -type f -name '*.c' \
  -not -path '*/third_party/*' -not -path '*/generated/*' \
  -exec grep -l -E \
  '^[[:space:]]*GmlVal[[:space:]]+gml_vm_(run_code|call_callable)[(]' {} + \
  2>/dev/null || true)
if [ "$vm_exec_implementation_owners" != "src/runtime/vm/gml_vm_exec.c" ]; then
  printf '%s\n' "Bytecode execution implementation belongs only to gml_vm_exec.c:" >&2
  printf '%s\n' "$vm_exec_implementation_owners" >&2
  exit 1
fi

vm_instances_implementation_owners=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*(GmlInstance[[:space:]]*\\*[[:space:]]+gml_vm_instances_alloc|void[[:space:]]+gml_vm_instances_(run_collisions|run_boundary_events))[(].*[{]' \
  2>/dev/null || true)
if [ "$vm_instances_implementation_owners" != "src/runtime/vm/gml_vm_instances.c" ]; then
  printf '%s\n' "Object, instance, event, and collision implementation belongs only to gml_vm_instances.c:" >&2
  printf '%s\n' "$vm_instances_implementation_owners" >&2
  exit 1
fi

builtin_draw_definition_files=$(find src/runtime/builtins -type f -name '*.c' \
  -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(GmlVal|double|int|void)[[:space:]]+(gml_builtin_try_draw(_3d)?|gm_matrix_builtin|gml_camera_alloc|d3_model_file_save|sprite_uvs|texture_uvs|builtin_fmod_exact_name|gml_shader_get_sampler)[[:space:]]*[(]' \
  {} + 2>/dev/null || true)
if [ "$builtin_draw_definition_files" != "src/runtime/builtins/gml_builtin_draw.c" ]; then
  printf '%s\n' "Draw, camera, vertex, model, and fixed-function builtin adaptation belongs only to gml_builtin_draw.c:" >&2
  printf '%s\n' "$builtin_draw_definition_files" >&2
  exit 1
fi

builtin_graphics_context_definitions=$(find src/runtime/builtins -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '^[[:space:]]*static[[:space:]]+inline[[:space:]]+GmlSoftware3D[[:space:]]+[*][[:space:]]*graphics_state_for_render[[:space:]]*[(]' \
  {} + 2>/dev/null || true)
if [ "$builtin_graphics_context_definitions" != "src/runtime/builtins/gml_builtin_internal.h" ]; then
  printf '%s\n' "The builtin graphics-context accessor must remain a single private static-inline definition:" >&2
  printf '%s\n' "$builtin_graphics_context_definitions" >&2
  exit 1
fi

builtin_draw_link_predecessor=$(awk '
  $1 == "src/runtime/builtins/gml_builtin_draw.c" { print previous; exit }
  { previous=$1 }
' Makefile.common)
if [ "$builtin_draw_link_predecessor" != "src/runtime/builtins/gml_builtin.c" ]; then
  printf '%s\n' "gml_builtin_draw.c must remain immediately after gml_builtin.c in ANYGM_BUILTIN_SOURCES to preserve the characterized link layout." >&2
  exit 1
fi

builtin_registry_link_predecessor=$(awk '
  $1 == "src/runtime/builtins/gml_builtin_registry.c" { print previous; exit }
  { previous=$1 }
' Makefile.common)
if [ "$builtin_registry_link_predecessor" != "src/runtime/builtins/gml_builtin_draw.c" ]; then
  printf '%s\n' "gml_builtin_registry.c must remain immediately after the facade-adjacent draw owner in ANYGM_BUILTIN_SOURCES." >&2
  exit 1
fi

builtin_registry_source_count=$(grep -c -E \
  '^[[:space:]]*src/runtime/builtins/gml_builtin_registry[.]c[[:space:]\\]*$' \
  Makefile.common 2>/dev/null || true)
if [ "$builtin_registry_source_count" -ne 1 ]; then
  printf '%s\n' "ANYGM_BUILTIN_SOURCES must contain gml_builtin_registry.c exactly once." >&2
  exit 1
fi

builtin_registry_definition_files=$(find src/runtime/builtins -type f -name '*.c' \
  -exec grep -l -E \
  '^[[:space:]]*(int[[:space:]]+gml_builtin_fast_id|GmlVal[[:space:]]+gml_builtin_call_fast_id)[[:space:]]*[(].*[{]' \
  {} + 2>/dev/null || true)
if [ "$builtin_registry_definition_files" != "src/runtime/builtins/gml_builtin_registry.c" ]; then
  printf '%s\n' "Exact name resolution and direct cached-ID execution belong only to gml_builtin_registry.c:" >&2
  printf '%s\n' "$builtin_registry_definition_files" >&2
  exit 1
fi

builtin_id_owners=$(find src/runtime/builtins -type f \
  \( -name '*.c' -o -name '*.h' \) \
  -exec grep -l -E '\bBID_[A-Z0-9_]+' {} + 2>/dev/null || true)
if [ "$builtin_id_owners" != "src/runtime/builtins/gml_builtin_registry.c" ]; then
  printf '%s\n' "Concrete builtin ID uses must remain confined to the registry implementation:" >&2
  printf '%s\n' "$builtin_id_owners" >&2
  exit 1
fi

builtin_registry_header_consumers=$(find src -type f \
  \( -name '*.c' -o -name '*.h' \) \
  -exec grep -l -E \
  '^[[:space:]]*#include[[:space:]]+"gml_builtin_registry[.]h"' {} + \
  2>/dev/null || true)
if [ "$builtin_registry_header_consumers" != "src/runtime/builtins/gml_builtin_registry.c" ]; then
  printf '%s\n' "Only the registry implementation may consume the canonical exact-name data:" >&2
  printf '%s\n' "$builtin_registry_header_consumers" >&2
  exit 1
fi

if ! grep -q -F '#define GML_BUILTIN_EXACT_REGISTRY(ENTRY, ALIAS)' \
  src/runtime/builtins/gml_builtin_registry.h ||
   ! grep -q -F '#define GML_BUILTIN_DYNAMIC_REGISTRY(ENTRY)' \
  src/runtime/builtins/gml_builtin_registry.h; then
  printf '%s\n' "The canonical exact-name and explicit dynamic-prefix registries are absent." >&2
  exit 1
fi

if ! grep -q -E '^[[:space:]]*switch[[:space:]]*[(][[:space:]]*id[[:space:]]*[)]' \
  src/runtime/builtins/gml_builtin_registry.c; then
  printf '%s\n' "Cached builtin execution must remain a direct integer switch." >&2
  exit 1
fi

if ! grep -q -E \
  '^[[:space:]]*#define[[:space:]]+graphics_state_for_vm[[:space:]]+gml_vm_software3d_ensure[[:space:]]*$' \
  src/runtime/builtins/gml_builtin_registry.c; then
  printf '%s\n' "The physical registry must preserve the pre-split direct software-3D ensure call." >&2
  exit 1
fi

if ! grep -q -E '__attribute__[(][(]aligned[(]64[)][)]' \
  src/runtime/builtins/gml_builtin_registry.c ||
   ! grep -q -E '__attribute__[(][(]hot[)][)]' \
  src/runtime/builtins/gml_builtin_registry.c; then
  printf '%s\n' "The physical registry must retain its measured local hot-path code-layout annotations." >&2
  exit 1
fi

builtin_registry_function_pointer=$(grep -n -E \
  '[(][*][[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*[)]' \
  src/runtime/builtins/gml_builtin_registry.c 2>/dev/null || true)
fail_matches "The cached builtin registry must not introduce function-pointer dispatch:" \
  "$builtin_registry_function_pointer"

for builtin_registry_crossing in \
  gp_deadzone_set \
  gp_axis_value_filtered \
  gp_debug_on \
  builtin_call_impl \
  builtin_set_blendmode_ext \
  builtin_set_blendmode \
  draw_legacy_sprite_shadow \
  graphics_state_for_vm
do
  builtin_registry_declaration_count=$(grep -c -E \
    "^[[:space:]]*(void|double|int|GmlVal|GmlSoftware3D[[:space:]]*[*])[[:space:]]*${builtin_registry_crossing}[[:space:]]*[(]" \
    src/runtime/builtins/gml_builtin_internal.h 2>/dev/null || true)
  if [ "$builtin_registry_declaration_count" -ne 1 ]; then
    printf '%s\n' "The private builtin boundary must declare ${builtin_registry_crossing} exactly once." >&2
    exit 1
  fi
  builtin_registry_static_declaration=$(find src/runtime/builtins -type f -name '*.c' \
    -exec grep -n -H -E \
    "^[[:space:]]*static[[:space:]]+(void|double|int|GmlVal|GmlSoftware3D[[:space:]]*[*])[[:space:]]*${builtin_registry_crossing}[[:space:]]*[(]" \
    {} + 2>/dev/null || true)
  fail_matches "The registry crossing ${builtin_registry_crossing} must not regain translation-unit-static linkage:" \
    "$builtin_registry_static_declaration"
done

builtin_registry_crossing_definition_files=$(find src/runtime/builtins -type f -name '*.c' \
  -exec grep -l -E \
  '^[[:space:]]*(void[[:space:]]+(gp_deadzone_set|builtin_set_blendmode_ext|builtin_set_blendmode|draw_legacy_sprite_shadow)|double[[:space:]]+gp_axis_value_filtered|int[[:space:]]+gp_debug_on|GmlVal[[:space:]]+builtin_call_impl|GmlSoftware3D[[:space:]]*[*][[:space:]]*graphics_state_for_vm)[[:space:]]*[(][^;]*[{]' \
  {} + 2>/dev/null || true)
if [ "$builtin_registry_crossing_definition_files" != "src/runtime/builtins/gml_builtin.c" ]; then
  printf '%s\n' "Registry crossing implementations must remain single and facade-owned:" >&2
  printf '%s\n' "$builtin_registry_crossing_definition_files" >&2
  exit 1
fi

for builtin_io_crossing in \
  file_find_reset \
  vm_file_slot \
  vm_file_ungetc \
  builtin_ini_open_file \
  builtin_file_text_open_read \
  builtin_file_text_read_string \
  builtin_file_text_readln
do
  builtin_io_declaration_count=$(grep -c -E \
    "^[[:space:]]*(void|int|GmlVal)[[:space:]]+${builtin_io_crossing}[[:space:]]*[(]" \
    src/runtime/builtins/gml_builtin_internal.h 2>/dev/null || true)
  if [ "$builtin_io_declaration_count" -ne 1 ]; then
    printf '%s\n' "The private builtin boundary must declare ${builtin_io_crossing} exactly once." >&2
    exit 1
  fi
  builtin_io_static_definition=$(find src/runtime/builtins -type f -name '*.c' \
    -exec grep -n -H -E \
    "^[[:space:]]*static[[:space:]]+(void|int|GmlVal)[[:space:]]+${builtin_io_crossing}[[:space:]]*[(]" \
    {} + 2>/dev/null || true)
  fail_matches "The I/O crossing ${builtin_io_crossing} must not regain translation-unit-static linkage:" \
    "$builtin_io_static_definition"
done

for builtin_ds_crossing in \
  log_ds_on \
  ds_map_slot \
  ds_list_slot \
  ds_map_mark_child \
  ds_map_destroy_id \
  ds_list_create_id \
  ds_list_push_kind \
  ds_list_destroy_id \
  gml_ds_list_clear_direct \
  gml_ds_map_find_previous_direct \
  gml_ds_map_exists_direct \
  gml_ds_map_empty_direct \
  gml_ds_map_find_last_direct \
  gml_ds_list_find_value_direct \
  gml_ds_list_size_direct \
  gml_ds_map_view_count \
  gml_ds_map_item_view \
  gml_ds_list_view_count \
  gml_ds_list_item_view \
  gml_ds_list_append_direct
do
  builtin_ds_declaration_count=$(grep -c -E \
    "^[[:space:]]*(GmlVal|GmlDSMap[[:space:]]*[*]|GmlDSList[[:space:]]*[*]|void|int)[[:space:]]*${builtin_ds_crossing}[[:space:]]*[(]" \
    src/runtime/builtins/gml_builtin_internal.h 2>/dev/null || true)
  if [ "$builtin_ds_declaration_count" -ne 1 ]; then
    printf '%s\n' "The private builtin boundary must declare ${builtin_ds_crossing} exactly once." >&2
    exit 1
  fi
done

for builtin_ds_promoted_crossing in \
  log_ds_on \
  ds_map_slot \
  ds_list_slot \
  ds_map_mark_child \
  ds_map_destroy_id \
  ds_list_create_id \
  ds_list_push_kind \
  ds_list_destroy_id
do
  builtin_ds_static_definition=$(find src/runtime/builtins -type f -name '*.c' \
    -exec grep -n -H -E \
    "^[[:space:]]*static[[:space:]]+(GmlDSMap[[:space:]]*[*]|GmlDSList[[:space:]]*[*]|void|int)[[:space:]]*${builtin_ds_promoted_crossing}[[:space:]]*[(]" \
    {} + 2>/dev/null || true)
  fail_matches "The DS crossing ${builtin_ds_promoted_crossing} must not regain translation-unit-static linkage:" \
    "$builtin_ds_static_definition"
done

for builtin_ds_fast_operation in \
  gml_ds_list_clear_direct \
  gml_ds_map_find_previous_direct \
  gml_ds_map_exists_direct \
  gml_ds_map_empty_direct \
  gml_ds_map_find_last_direct \
  gml_ds_list_find_value_direct \
  gml_ds_list_size_direct
do
  if ! grep -q -E \
    "^[[:space:]]*(return[[:space:]]+)?(vreal[(])?${builtin_ds_fast_operation}[[:space:]]*[(]" \
    src/runtime/builtins/gml_builtin_registry.c; then
    printf '%s\n' "The exact-ID registry must cross DS ownership through ${builtin_ds_fast_operation}." >&2
    exit 1
  fi
done

builtin_io_definition_files=$(find src/runtime/builtins -type f -name '*.c' \
  -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(GmlVal|char[[:space:]]*[*]|int|void|uint32_t|unsigned[[:space:]]+char[[:space:]]*[*])[[:space:]]+(gml_builtin_try_io(_ini)?|wild_match|resolve_read_path|vm_file_open|builtin_ini_open_file|buffer_alloc|md5_transform|sha1_transform|base64_decode_alloc|async_saveload_request)[[:space:]]*[(]' \
  {} + 2>/dev/null || true)
if [ "$builtin_io_definition_files" != "src/runtime/builtins/gml_builtin_io.c" ]; then
  printf '%s\n' "VFS, INI, buffer, codec, async-queue, and ordered I/O builtin adaptation belongs only to gml_builtin_io.c:" >&2
  printf '%s\n' "$builtin_io_definition_files" >&2
  exit 1
fi

builtin_io_link_predecessor=$(awk '
  $1 == "src/runtime/builtins/gml_builtin_io.c" { print previous; exit }
  { previous=$1 }
' Makefile.common)
if [ "$builtin_io_link_predecessor" != "src/runtime/builtins/gml_builtin_registry.c" ]; then
  printf '%s\n' "gml_builtin_io.c must remain immediately after the exact builtin registry in ANYGM_BUILTIN_SOURCES." >&2
  exit 1
fi

builtin_ds_definition_files=$(find src/runtime/builtins -type f -name '*.c' \
  -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(GmlVal|GmlDSMap[[:space:]]*[*]|GmlDSList[[:space:]]*[*]|char[[:space:]]*[*]|double|int|void)[[:space:]]+(gml_builtin_try_ds|ds_key_make|ds_grid_make|ds_map_write_text|ds_list_write_text|gml_ds_map_item_view|gml_ds_list_item_view)[[:space:]]*[(]' \
  {} + 2>/dev/null || true)
if [ "$builtin_ds_definition_files" != "src/runtime/builtins/gml_builtin_ds.c" ]; then
  printf '%s\n' "Map, list, grid, priority, and DS text adaptation belongs only to gml_builtin_ds.c:" >&2
  printf '%s\n' "$builtin_ds_definition_files" >&2
  exit 1
fi

builtin_json_definition_files=$(find src/runtime/builtins -type f -name '*.c' \
  -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(GmlVal|char[[:space:]]*[*]|int|void)[[:space:]]+(gml_builtin_try_json|json_writer_reserve|json_encode_(map|list|root)|json_parse_(array|object|value)|gml_builtin_json_writer_put_value|gml_builtin_json_decode_ds)[[:space:]]*[(]' \
  {} + 2>/dev/null || true)
if [ "$builtin_json_definition_files" != "src/runtime/builtins/gml_builtin_json.c" ]; then
  printf '%s\n' "JSON parsing, encoding, and ordered adaptation belongs only to gml_builtin_json.c:" >&2
  printf '%s\n' "$builtin_json_definition_files" >&2
  exit 1
fi

builtin_ds_link_predecessor=$(awk '
  $1 == "src/runtime/builtins/gml_builtin_ds.c" { print previous; exit }
  { previous=$1 }
' Makefile.common)
if [ "$builtin_ds_link_predecessor" != "src/runtime/builtins/gml_builtin_io.c" ]; then
  printf '%s\n' "gml_builtin_ds.c must remain immediately after gml_builtin_io.c in ANYGM_BUILTIN_SOURCES." >&2
  exit 1
fi

builtin_json_link_predecessor=$(awk '
  $1 == "src/runtime/builtins/gml_builtin_json.c" { print previous; exit }
  { previous=$1 }
' Makefile.common)
if [ "$builtin_json_link_predecessor" != "src/runtime/builtins/gml_builtin_ds.c" ]; then
  printf '%s\n' "gml_builtin_json.c must remain immediately after gml_builtin_ds.c in ANYGM_BUILTIN_SOURCES." >&2
  exit 1
fi

if grep -R -n -E \
  '^[[:space:]]*GmlVal[[:space:]]+gml_builtin_try_ds_json[[:space:]]*[(]' \
  src/runtime/builtins >/dev/null 2>&1; then
  printf '%s\n' "The combined DS/JSON stage must not be reintroduced." >&2
  exit 1
fi
if ! grep -q -E \
  '^[[:space:]]*return[[:space:]]+gml_builtin_try_ds[(]' \
  src/runtime/builtins/gml_builtin_platform.c; then
  printf '%s\n' "The platform stage must delegate directly to the DS owner." >&2
  exit 1
fi
if ! grep -q -E \
  '^[[:space:]]*return[[:space:]]+gml_builtin_try_json[(]' \
  src/runtime/builtins/gml_builtin_ds.c; then
  printf '%s\n' "The DS stage must delegate directly to the JSON owner." >&2
  exit 1
fi
if ! grep -q -E \
  '^[[:space:]]*return[[:space:]]+gml_builtin_try_values_variables[(]' \
  src/runtime/builtins/gml_builtin_json.c; then
  printf '%s\n' "The JSON stage must delegate directly to value variables." >&2
  exit 1
fi

json_ds_storage_access=$(grep -n -E \
  'GmlDS(Map|List)|ds_(map|list)_slot[[:space:]]*[(]|->[[:space:]]*(entry|item|child_kind)' \
  src/runtime/builtins/gml_builtin_json.c 2>/dev/null || true)
fail_matches "JSON must use typed DS item views and mutation operations instead of container storage:" \
  "$json_ds_storage_access"

for builtin_json_crossing in \
  gml_builtin_json_writer_putc \
  gml_builtin_json_writer_puts \
  gml_builtin_json_writer_put_value \
  gml_builtin_json_writer_discard \
  gml_builtin_json_writer_take \
  gml_builtin_json_decode_ds
do
  builtin_json_declaration_count=$(grep -c -E \
    "^[[:space:]]*(GmlVal|char[[:space:]]*[*]|int|void)[[:space:]]*${builtin_json_crossing}[[:space:]]*[(]" \
    src/runtime/builtins/gml_builtin_internal.h 2>/dev/null || true)
  if [ "$builtin_json_declaration_count" -ne 1 ]; then
    printf '%s\n' "The private builtin boundary must declare ${builtin_json_crossing} exactly once." >&2
    exit 1
  fi
  builtin_json_crossing_owner=$(find src/runtime/builtins -type f -name '*.c' \
    -exec grep -l -E \
    "^[[:space:]]*(GmlVal|char[[:space:]]*[*]|int|void)[[:space:]]*${builtin_json_crossing}[[:space:]]*[(]" \
    {} + 2>/dev/null || true)
  if [ "$builtin_json_crossing_owner" != "src/runtime/builtins/gml_builtin_json.c" ]; then
    printf '%s\n' "The JSON crossing ${builtin_json_crossing} belongs only to gml_builtin_json.c:" >&2
    printf '%s\n' "$builtin_json_crossing_owner" >&2
    exit 1
  fi
done

renderer_private_record_definitions=$(portable_product_files | xargs grep -l -E \
  '}[[:space:]]+(GmlTpagAlphaRun|GmlTpag|GmlRuntimeAxisKey|GmlRuntimeAxisRun|GmlSprite|GmlAtlas|GmlInterpSubrectCache|GmlBg|GmlGlyph|GmlFont|GmlSurface);' \
  2>/dev/null || true)
if [ "$renderer_private_record_definitions" != "src/video/renderer/gml_render_internal.h" ]; then
  printf '%s\n' "Private renderer record definitions belong only to gml_render_internal.h:" >&2
  printf '%s\n' "$renderer_private_record_definitions" >&2
  exit 1
fi

external_renderer_internal_includes=$(find src -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '^[[:space:]]*#include[[:space:]]+"gml_render_internal[.]h"' {} + 2>/dev/null | \
  grep -v -E '^(src/core/engine_internal[.]h|src/video/renderer/gml_render_(pixel|sampling)_internal[.]h|src/video/renderer/[^/]+[.]c)$' || true)
fail_matches "Only renderer implementation and the engine composition root may include renderer storage:" \
  "$external_renderer_internal_includes"

if ! grep -q -E '^[[:space:]]*#include[[:space:]]+"gml_render_internal[.]h"' \
  src/core/engine_internal.h; then
  printf '%s\n' "engine_internal.h must include renderer storage explicitly for its by-value member" >&2
  exit 1
fi

public_renderer_private_include=$(grep -n -H -E \
  '^[[:space:]]*#include[[:space:]]+"gml_render_internal[.]h"' \
  src/video/renderer/gml_render.h 2>/dev/null || true)
fail_matches "The subsystem-facing renderer header must keep GmlRender opaque:" \
  "$public_renderer_private_include"

renderer_state_definition_files=$(find src -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(int|size_t)[[:space:]]+gml_render_state_(size|save|load|profile_metrics)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_state_definition_files" != "src/video/renderer/gml_render_state.c" ]; then
  printf '%s\n' "Canonical renderer payload operations belong only to gml_render_state.c:" >&2
  printf '%s\n' "$renderer_state_definition_files" >&2
  exit 1
fi

core_state_private_renderer_access=$(grep -n -H -E \
  '(^|[^[:alnum:]_])(GmlSprite|GmlTpag|GmlAtlas|GmlBg|GmlSurface|GmlFont)([^[:alnum:]_]|$)|g_render[[:space:]]*\\.[[:space:]]*[A-Za-z_][A-Za-z0-9_]*' \
  src/core/engine_state.c 2>/dev/null || true)
fail_matches "Root state framing must use renderer payload operations, not renderer storage:" \
  "$core_state_private_renderer_access"

stateful_renderer_inlines=$(grep -n -E \
  'static[[:space:]]+inline.*gml_render_(gui_|maybe_prepare_)' \
  src/video/renderer/gml_render.h 2>/dev/null || true)
fail_matches "State-sensitive renderer operations must not dereference storage from gml_render.h:" \
  "$stateful_renderer_inlines"

software3d_context_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(GmlSoftware3D[[:space:]]*\*|void)[[:space:]]+gml_software3d_(create|destroy|reset)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d\.c:' || true)
fail_matches "Software-3D context lifecycle definitions belong only to gml_software3d.c:" \
  "$software3d_context_definitions"

software3d_state_mapping_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*void[[:space:]]+gml_software3d_state_(get|set)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_state\.c:' || true)
fail_matches "Canonical software-3D state mapping belongs only to gml_software3d_state.c:" \
  "$software3d_state_mapping_definitions"

software3d_matrix_kernel_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*static[[:space:]]+(double|int|void)[[:space:]]+d3_(dot|normalize|cross|matrix_multiply|matrix_prepend|matrix_translation|matrix_scaling|matrix_rotation_axis)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_raster\.c:' || true)
fail_matches "Software-3D matrix kernels belong only to gml_software3d_raster.c:" \
  "$software3d_matrix_kernel_definitions"

software3d_matrix_operation_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(double|int|void)[[:space:]]+gml_software3d_(dot|normalize|cross|matrix_multiply|matrix_prepend|matrix_translation|matrix_scaling|matrix_rotation_axis)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_raster\.c:' || true)
fail_matches "Software-3D matrix operations belong only to gml_software3d_raster.c:" \
  "$software3d_matrix_operation_definitions"

software3d_camera_sync_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*void[[:space:]]+gml_d3_sync_render_camera[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_raster\.c:' || true)
fail_matches "Software-3D camera synchronization belongs only to gml_software3d_raster.c:" \
  "$software3d_camera_sync_definitions"

software3d_raster_kernel_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*static[[:space:]]+(double|int|void|GmlD3Vertex)[[:space:]]+(d3_(transform_point|depth_prepare|texture|texture_texel|sample|edge|raster_triangle|camera_vertex|vertex_lerp|clip_z|emit_triangle|draw_quad|set_camera|set_default_projection|vertex_camera|transform_normal|vertex_light_factor|apply_light_factor|draw_ellipsoid|draw_texture_part_2d|project_vertex|raster_sample|clip_segment_plane|emit_point|emit_line|primitive_flush)|vertex_(texture|submit_buffer))[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_raster\.c:' || true)
fail_matches "Software-3D raster kernels belong only to gml_software3d_raster.c:" \
  "$software3d_raster_kernel_definitions"

software3d_raster_operation_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(int|void)[[:space:]]+(gml_d3_(set_draw_depth|is_active|draw_sprite_2d|draw_sprite_pos_2d|draw_background_2d|draw_sprite_part_2d|draw_background_part_2d|draw_atlas_part_2d|draw_surface_part_2d)|gml_software3d_(transform_point|depth_prepare|texture|emit_triangle|emit_point|emit_line|draw_quad|set_camera|set_default_projection|draw_ellipsoid|primitive_flush|vertex_submit_buffer))[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_raster\.c:' || true)
fail_matches "Software-3D raster operations belong only to gml_software3d_raster.c:" \
  "$software3d_raster_operation_definitions"

software3d_2d_kernel_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*static[[:space:]]+(int|void|GmlD3Vertex)[[:space:]]+(d3_2d_(vertex|line|triangle|rectangle|ellipse)|prim_try_fast_surface_quad|d3_flush_2d_primitive)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_raster\.c:' || true)
fail_matches "Ordinary 2D software-3D kernels belong only to gml_software3d_raster.c:" \
  "$software3d_2d_kernel_definitions"

software3d_2d_operation_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(int|void|GmlD3Vertex)[[:space:]]+(gml_d3_draw_rectangle_2d|gml_software3d_(2d_(vertex|line|triangle|rectangle|ellipse)|flush_2d_primitive))[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_raster\.c:' || true)
fail_matches "Ordinary 2D software-3D operations belong only to gml_software3d_raster.c:" \
  "$software3d_2d_operation_definitions"

software3d_model_algorithm_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*static[[:space:]]+(int|void)[[:space:]]+d3_model_(grow_vertices|grow_batches|begin_batch|append_vertex|append_quad|add_shape|draw)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_models\.c:' || true)
fail_matches "Software-3D model algorithms belong only to gml_software3d_models.c:" \
  "$software3d_model_algorithm_definitions"

software3d_model_operation_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(int|void)[[:space:]]+gml_software3d_model_(grow_vertices|grow_batches|begin_batch|append_vertex|append_quad|add_shape|draw)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_models\.c:' || true)
fail_matches "Software-3D model operations belong only to gml_software3d_models.c:" \
  "$software3d_model_operation_definitions"

software3d_model_state_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(static[[:space:]]+)?(double|int|size_t|uint32_t|void)[[:space:]]+(d3_blob_(write|read|u32|read_u32|double|read_double|write_model|read_model)|gml_d3_models_state_(size|save|load))[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/software3d/gml_software3d_models\.c:' || true)
fail_matches "Software-3D canonical model-state definitions belong only to gml_software3d_models.c:" \
  "$software3d_model_state_definitions"

software3d_model_state_schema=$(portable_product_files | xargs grep -l -E \
  'GML_MODEL_STATE_(MAGIC|SCHEMA)' 2>/dev/null || true)
if [ "$software3d_model_state_schema" != "src/video/software3d/gml_software3d_models.c" ]; then
  printf '%s\n' "Only gml_software3d_models.c may own the canonical model-state schema:" >&2
  printf '%s\n' "$software3d_model_state_schema" >&2
  exit 1
fi

local_camera_sync_declarations=$(portable_product_files | xargs grep -n -H -E \
  'extern[[:space:]]+void[[:space:]]+gml_d3_sync_render_camera' 2>/dev/null || true)
fail_matches "Software-3D camera synchronization must use its declared video interface:" \
  "$local_camera_sync_declarations"

renderer_pixel_backend_definitions=$(find src -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(uint32_t|void|int)[[:space:]]+gml_render_backend_(color_to_xrgb|lerp_xrgb|fill_xrgb|flat_blend_init|flat_blend_run|flat_blend_uses_float_alpha|draw_xrgb_alpha|draw_pixel_alpha|draw_pixel)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/video/renderer/gml_render_blit\.c:' || true)
fail_matches "Renderer pixel backend definitions belong only to gml_render_blit.c:" \
  "$renderer_pixel_backend_definitions"

renderer_primitive_definition_files=$(find src -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(int|void|uint32_t)[[:space:]]+(draw_rect_prim_alpha|draw_rect_prim|draw_rect_colour_prim|draw_line_prim|draw_circle_prim|gm_color_lerp_fan|draw_px_fan|draw_circle_colour_prim|prim_tri_fill_ex|gml_render_primitive_point|gml_render_primitive_rectangle|gml_render_primitive_rectangle_color|gml_render_primitive_line|gml_render_primitive_circle|gml_render_primitive_circle_color|gml_render_primitive_triangle_alpha|gml_draw_layer_color_fill)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_primitive_definition_files" != "src/video/renderer/gml_render_primitives.c" ]; then
  printf '%s\n' "Renderer fixed-function primitive definitions belong only to gml_render_primitives.c:" >&2
  printf '%s\n' "$renderer_primitive_definition_files" >&2
  exit 1
fi

renderer_asset_view_definitions=$(find src -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*int[[:space:]]+gml_render_backend_(texture|atlas)_view[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_asset_view_definitions" != "src/video/renderer/gml_render_assets.c" ]; then
  printf '%s\n' "Renderer asset-view definitions belong only to gml_render_assets.c:" >&2
  printf '%s\n' "$renderer_asset_view_definitions" >&2
  exit 1
fi

renderer_raster_definition_files=$(find src/video/renderer -type f -name '*.c' \
  -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(int|void|uint32_t)[[:space:]]+(gml_draw_(sprite|background|room_backgrounds|room_tiles|tile)|draw_sprite_nineslice|gml_render_shader_fill_rect|gml_render_backend_(color_to_xrgb|fill_xrgb|draw_pixel))([[:alnum:]_]*)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_raster_definition_files" != "src/video/renderer/gml_render_blit.c" ]; then
  printf '%s\n' "Complete sprite, background, tile, paint, and pixel raster definitions belong only to gml_render_blit.c:" >&2
  printf '%s\n' "$renderer_raster_definition_files" >&2
  exit 1
fi

renderer_runtime_sprite_definition_files=$(find src/video/renderer -type f -name '*.c' \
  -exec grep -l -E \
  '^[[:space:]]*(int|void)[[:space:]]+gml_(sprite_(append_from_rgba_frames|duplicate|set_alpha_from_sprite|set_offset|add_file|replace_from_rgba_frames|delete|collision_mask|create_from_surface|replace_from_file)|render_clear_runtime_sprites)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_runtime_sprite_definition_files" != "src/video/renderer/gml_render_assets.c" ]; then
  printf '%s\n' "The complete runtime-sprite record lifecycle belongs only to gml_render_assets.c:" >&2
  printf '%s\n' "$renderer_runtime_sprite_definition_files" >&2
  exit 1
fi

renderer_atlas_definition_files=$(find src -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(uint8_t[[:space:]]*\*|void|int)[[:space:]]*(decode_texture_blob|atlas_(decode_publish|worker|pool_get|pool_free|pixels)|gml_render_(prefetch_atlas|prefetch_sprite|prefetch_bg|warm_atlas|warm_sprite|warm_bg)|parse_txtr)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_atlas_definition_files" != "src/video/renderer/gml_render_atlas.c" ]; then
  printf '%s\n' "Renderer atlas decode and prefetch definitions belong only to gml_render_atlas.c:" >&2
  printf '%s\n' "$renderer_atlas_definition_files" >&2
  exit 1
fi

renderer_surface_definition_files=$(find src/video/renderer -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(int|void|uint32_t[[:space:]]*\*|const[[:space:]]+uint32_t[[:space:]]*\*)[[:space:]]*(surface_(slot|known_opaque|known_transparent|alpha_all_zero|store_target_coverage|pixels)|draw_(scaled_full_surface_normal|surface_interp_phase|surface_region|surface_stretched_impl)|gml_(surface_(exists|width|height|pixels_read|copy|create|free|resize|set_target|reset_target|get_target)|draw_surface_(stretched|ext|part_ext)|render_backend_surface_stretched))[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_surface_definition_files" != "src/video/renderer/gml_render_surfaces.c" ]; then
  printf '%s\n' "Renderer surface storage, targets, and complete composition definitions belong only to gml_render_surfaces.c:" >&2
  printf '%s\n' "$renderer_surface_definition_files" >&2
  exit 1
fi

renderer_crt_geometry_definition_files=$(find src/video/renderer -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(inline[[:space:]]+)?((double|void|CrtWeightRow|float|uint8_t)[[:space:]]+|(GmlCrtTables|void)[[:space:]]*\*[[:space:]]*|const[[:space:]]+(float|uint8_t)[[:space:]]*\*[[:space:]]*)(crt_(tex|scanline_weights|tables|tables_free|gamma_lut|weight_table|weight_row|weight_values|weight_value|powout_lut|powout|u8_table|u8|scratch_grow|conv_row|band_run|band_rows)|draw_surface_crt)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_crt_geometry_definition_files" != "src/video/renderer/gml_render_crt.c" ]; then
  printf '%s\n' "Geometric CRT definitions belong only to gml_render_crt.c:" >&2
  printf '%s\n' "$renderer_crt_geometry_definition_files" >&2
  exit 1
fi

renderer_final_pixel_definition_files=$(find src/video/renderer -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '^[[:space:]]*static[[:space:]]+inline[[:space:]]+uint32_t[[:space:]]+(color_write_merge|blend_multiply_pixel)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_final_pixel_definition_files" != "src/video/renderer/gml_render_pixel_internal.h" ]; then
  printf '%s\n' "Shared final-pixel definitions belong only to gml_render_pixel_internal.h:" >&2
  printf '%s\n' "$renderer_final_pixel_definition_files" >&2
  exit 1
fi

renderer_sampling_definition_files=$(find src/video/renderer -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '^[[:space:]]*static[[:space:]]+inline[[:space:]]+([^[:space:]]+[[:space:]]+|const[[:space:]]+struct[[:space:]]+GmlShaderPal[[:space:]]*\*[[:space:]]*)(pal_map_px|pal_active|lut_active|grid_active|crt_active|sampled_crt_active|dual_active|hsv_scan_active|radial_wave_active|radial_wave_sample_index|uv_wave_active|uv_wave_sample_index|paint_active|grayscale_active|solid_alpha_mask_active|solid_blur_alpha_active|shader_active|shader_discards_alpha_value|shader_discards_alpha|shader_alpha_test_active|sprite_pixel_argb|sprite_pixel_rgb|lut_map_px|grid_map_px|grid_map_px_cached|mapped_texture_active|mapped_texture_pixel)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_sampling_definition_files" != "src/video/renderer/gml_render_sampling_internal.h" ]; then
  printf '%s\n' "Shared recognized-shader sampling definitions belong only to gml_render_sampling_internal.h:" >&2
  printf '%s\n' "$renderer_sampling_definition_files" >&2
  exit 1
fi

renderer_blit_definition_files=$(find src/video/renderer -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '^[[:space:]]*static[[:space:]]+(inline[[:space:]]+)?(int|void|uint32_t)[[:space:]]+(render_modern_cardinal_anchor|blend_fast8_cached|row_all_opaque32|blend_fast8_src_run|surf_bi_band)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_blit_definition_files" != "src/video/renderer/gml_render_blit_internal.h" ]; then
  printf '%s\n' "Shared sprite/surface hot blit definitions belong only to gml_render_blit_internal.h:" >&2
  printf '%s\n' "$renderer_blit_definition_files" >&2
  exit 1
fi

renderer_blit_record_files=$(find src/video/renderer -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '}[[:space:]]+SurfBiCtx;' {} + 2>/dev/null || true)
if [ "$renderer_blit_record_files" != "src/video/renderer/gml_render_blit_internal.h" ]; then
  printf '%s\n' "The shared bilinear surface-band record belongs only to gml_render_blit_internal.h:" >&2
  printf '%s\n' "$renderer_blit_record_files" >&2
  exit 1
fi

renderer_postprocess_definition_files=$(find src/video/renderer -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(inline[[:space:]]+)?(int|void|uint32_t)[[:space:]]+(dual_(u8|texel|fast_band)|draw_surface_dual_sample|hsv_scan_(rgb|fast_band|blend_pixel)|draw_surface_hsv_scan|sampled_crt_(texture_init|texture_pixel|texture_sample|surface|quintic|axis_index|geometry|prepare_axes|surface_pixel|band)|draw_surface_sampled_crt)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_postprocess_definition_files" != "src/video/renderer/gml_render_crt.c" ]; then
  printf '%s\n' "Recognized display post-process definitions belong only to gml_render_crt.c:" >&2
  printf '%s\n' "$renderer_postprocess_definition_files" >&2
  exit 1
fi

renderer_postprocess_record_files=$(find src/video/renderer -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -l -E \
  '}[[:space:]]+(DualFastCtx|HsvScanFastCtx|SampledCrtTexture|SampledCrtCtx);' \
  {} + 2>/dev/null || true)
if [ "$renderer_postprocess_record_files" != "src/video/renderer/gml_render_crt.c" ]; then
  printf '%s\n' "Recognized display post-process records belong only to gml_render_crt.c:" >&2
  printf '%s\n' "$renderer_postprocess_record_files" >&2
  exit 1
fi

renderer_effect_definition_files=$(find src/video/renderer -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(inline[[:space:]]+)?(void|int|float|uint8_t|uint32_t)[[:space:]]+(effect_(wrap_coord|unorm8)|layer_[[:alnum:]_]+|gml_render_layer_(rgb_noise|tint|filter_begin|filter_end))[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_effect_definition_files" != "src/video/renderer/gml_render_effects.c" ]; then
  printf '%s\n' "Non-CRT renderer effect definitions belong only to gml_render_effects.c:" >&2
  printf '%s\n' "$renderer_effect_definition_files" >&2
  exit 1
fi

renderer_shader_recognition_definition_files=$(find src/video/renderer -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(const[[:space:]]+void[[:space:]]*\*|char[[:space:]]*\*|void|int)[[:space:]]+(pal_parse_vec3|mem_find|glsl_[[:alnum:]_]+|parse_shader_palettes)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_shader_recognition_definition_files" != "src/video/renderer/gml_render_effects.c" ]; then
  printf '%s\n' "Renderer shader-recognition definitions belong only to gml_render_effects.c:" >&2
  printf '%s\n' "$renderer_shader_recognition_definition_files" >&2
  exit 1
fi

renderer_text_definition_files=$(find src/video/renderer -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(static[[:space:]]+)?(void|int|double)[[:space:]]+(parse_font|build_default_font|gml_font_(add_sprite|add_sprite_ext|add_file|delete)|gml_render_rebuild_font_maps|gml_text_(width|height|width_ext|height_ext)|gml_draw_(classic_game_information|text|text_transformed|text_ext|text_ext_transformed))[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_text_definition_files" != "src/video/renderer/gml_render_text.c" ]; then
  printf '%s\n' "Renderer font and text definitions belong only to gml_render_text.c:" >&2
  printf '%s\n' "$renderer_text_definition_files" >&2
  exit 1
fi

stb_image_implementation_files=$(find src -type f -name '*.c' \
  -not -path 'src/third_party/*' -exec grep -l -E \
  '^[[:space:]]*#[[:space:]]*define[[:space:]]+STB_IMAGE_IMPLEMENTATION([[:space:]]|$)' \
  {} + 2>/dev/null || true)
if [ "$stb_image_implementation_files" != "src/media/gml_image_codec.c" ]; then
  printf '%s\n' "The sole stb_image implementation owner must be gml_image_codec.c:" >&2
  printf '%s\n' "$stb_image_implementation_files" >&2
  exit 1
fi

stb_image_write_implementation_files=$(find src -type f -name '*.c' \
  -not -path 'src/third_party/*' -exec grep -l -E \
  '^[[:space:]]*#[[:space:]]*define[[:space:]]+STB_IMAGE_WRITE_IMPLEMENTATION([[:space:]]|$)' \
  {} + 2>/dev/null || true)
if [ "$stb_image_write_implementation_files" != "src/media/gml_image_codec.c" ]; then
  printf '%s\n' "The sole stb_image_write implementation owner must be gml_image_codec.c:" >&2
  printf '%s\n' "$stb_image_write_implementation_files" >&2
  exit 1
fi

raw_stb_image_consumers=$(find src -type f \( -name '*.c' -o -name '*.h' \) \
  -not -path 'src/third_party/*' -not -path 'src/media/gml_image_codec.c' \
  -exec grep -n -H -E \
  '(^|[^[:alnum:]_])(stbi_[[:alnum:]_]+|STBI_FREE)([^[:alnum:]_]|$)|#[[:space:]]*include[[:space:]]*"stb_image(_write)?\.h"|#[[:space:]]*define[[:space:]]+(STB_IMAGE|STBI_)' \
  {} + 2>/dev/null || true)
fail_matches "Raw stb image APIs and configuration belong only to gml_image_codec.c:" \
  "$raw_stb_image_consumers"

media_path_dependencies=$(find src/media -type f \( -name '*.c' -o -name '*.h' \) \
  -exec grep -n -H -E \
  'anygm_(host|vfs)|[[:space:]]FILE[[:space:]*]|f(open|read|write|close)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
fail_matches "The media codec leaf must not perform host or path I/O:" \
  "$media_path_dependencies"

stb_truetype_implementation_files=$(find src -type f -name '*.c' \
  -not -path 'src/third_party/*' -exec grep -l -E \
  '^[[:space:]]*#[[:space:]]*define[[:space:]]+STB_TRUETYPE_IMPLEMENTATION([[:space:]]|$)' \
  {} + 2>/dev/null || true)
if [ "$stb_truetype_implementation_files" != "src/media/gml_font_raster.c" ]; then
  printf '%s\n' "The sole stb_truetype implementation owner must be gml_font_raster.c:" >&2
  printf '%s\n' "$stb_truetype_implementation_files" >&2
  exit 1
fi

raw_stb_truetype_consumers=$(find src -type f \( -name '*.c' -o -name '*.h' \) \
  -not -path 'src/third_party/*' -not -path 'src/media/gml_font_raster.c' \
  -exec grep -n -H -E \
  '(^|[^[:alnum:]_])(stbtt_[[:alnum:]_]+|stbtt_fontinfo)([^[:alnum:]_]|$)|#[[:space:]]*include[[:space:]]*"stb_truetype\.h"|#[[:space:]]*define[[:space:]]+(STB_TRUETYPE|STBTT_)' \
  {} + 2>/dev/null || true)
fail_matches "Raw stb truetype APIs and configuration belong only to gml_font_raster.c:" \
  "$raw_stb_truetype_consumers"

legacy_renderer_asset_lookup=$(portable_product_files | xargs grep -n -H \
  'gml_render_backend_sprite_tpag_info' 2>/dev/null || true)
fail_matches "The private-record renderer asset lookup must not return:" \
  "$legacy_renderer_asset_lookup"

software3d_private_renderer_assets=$(find src/video/software3d -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  '(^|[^[:alnum:]_])(GmlSprite|GmlTpag|GmlAtlas|GmlBg|GmlSurface|GmlFont)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Software-3D code must use neutral backend views, not renderer-private asset records:" \
  "$software3d_private_renderer_assets"

software3d_asset_facade_calls=$(find src/video/software3d -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  'gml_render_(warm_sprite|warm_bg|warm_atlas|surface_pixels_read)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
fail_matches "Software-3D asset access must stay behind the non-reentrant backend:" \
  "$software3d_asset_facade_calls"

renderer_target_backend_definitions=$(find src -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(int|void|GmlSoftware3D[[:space:]]*\*)[[:space:]]+gml_render_backend_(software3d|draw_view|prepare_draw|prepare_draw_view|sync_camera|gui_map_point)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_target_backend_definitions" != "src/video/renderer/gml_render.c" ]; then
  printf '%s\n' "Renderer target-view backend definitions belong only to gml_render.c:" >&2
  printf '%s\n' "$renderer_target_backend_definitions" >&2
  exit 1
fi

software3d_renderer_field_access=$(find src/video/software3d -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E -- \
  '(^|[^[:alnum:]_])(R|render|renderer)[[:space:]]*->[[:space:]]*(fb|fbw|fbh|win|interp|cam_x|cam_y|projection_cam_x|projection_cam_y|frame|alpha|alphablend|blendmode|circle_precision|pending_underlay|pending_fill|fb_all_transparent)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Software-3D code must use the borrowed draw view, not GmlRender fields:" \
  "$software3d_renderer_field_access"

software3d_retired_renderer_leaf_calls=$(find src/video/software3d -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  'gml_render_(software3d|maybe_prepare_draw|prepare_draw|gui_map_point)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
fail_matches "Software-3D target and draw policy must stay behind backend leaves:" \
  "$software3d_retired_renderer_leaf_calls"

renderer_surface_backend_definitions=$(find src -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*int[[:space:]]+gml_render_backend_surface_stretched[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_surface_backend_definitions" != "src/video/renderer/gml_render_surfaces.c" ]; then
  printf '%s\n' "The non-reentrant stretched-surface backend belongs only to gml_render_surfaces.c:" >&2
  printf '%s\n' "$renderer_surface_backend_definitions" >&2
  exit 1
fi

software3d_renderer_facade_calls=$(find src/video/software3d -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  'gml_(render|surface|draw)_[[:alnum:]_]+[[:space:]]*\(' {} + 2>/dev/null | \
  grep -v 'gml_render_backend_' || true)
fail_matches "Software-3D may call renderer code only through non-reentrant backend leaves:" \
  "$software3d_renderer_facade_calls"

particle_private_renderer_assets=$(find src/runtime/particles -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  '(^|[^[:alnum:]_])(GmlSprite|GmlTpag|GmlAtlas|GmlBg|GmlSurface|GmlFont)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Particle code must use renderer operations and neutral views, not private asset records:" \
  "$particle_private_renderer_assets"

particle_renderer_field_access=$(find src/runtime/particles -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E -- \
  '(^|[^[:alnum:]_])(r|render|renderer)[[:space:]]*->[[:space:]]*(fb|fbw|fbh|base_fb|target_sp|classic_interp_phase|interp|alphablend|cam_x|cam_y|n_spr|spr)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Particle code must use the borrowed draw view, not GmlRender fields:" \
  "$particle_renderer_field_access"

vm_private_renderer_assets=$(find src/runtime/vm -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  '(^|[^[:alnum:]_])(GmlSprite|GmlTpag|GmlAtlas|GmlBg|GmlSurface|GmlFont)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "VM code must use renderer operations and metadata snapshots, not private asset records:" \
  "$vm_private_renderer_assets"

vm_renderer_field_access=$(find src/runtime/vm -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E -- \
  '(^|[^[:alnum:]_])(R|render|renderer)[[:space:]]*->[[:space:]]*[A-Za-z_][A-Za-z0-9_]*|GmlRender[[:space:]]*\*[[:space:]]*\)[^;]*\)[[:space:]]*->[[:space:]]*[A-Za-z_][A-Za-z0-9_]*' \
  {} + 2>/dev/null || true)
fail_matches "VM code must not dereference renderer storage:" \
  "$vm_renderer_field_access"

builtin_renderer_state_access=$(find src/runtime/builtins -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E -- \
  '(^|[^[:alnum:]_])(R|R2|r|render|renderer)[[:space:]]*->[[:space:]]*(active_shader|alpha|alphablend|app_draw_enable|app_h|app_surface|app_w|blend_equation|blend_equation_alpha|blendmode|cam_x|cam_y|circle_precision|color|fb|fbh|fbw|font|gui_base_logical_h|gui_base_logical_w|gui_pass_active|halign|interp|presentation_h|presentation_w|resolution_h|resolution_w|aspect_fullwidth|aspect_wide_h|aspect_wide_w|valign|alpha_test_enable|alpha_test_ref|color_write_mask|gpu_state_sp|gpu_state_stack|win)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Builtins must use typed renderer draw, target, presentation, shader, and GPU operations:" \
  "$builtin_renderer_state_access"

builtin_private_video_headers=$(find src/runtime/builtins -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  '^[[:space:]]*#[[:space:]]*include[[:space:]]+"(gml_render_internal|gml_render_backend|gml_software3d_internal)[.]h"' \
  {} + 2>/dev/null || true)
fail_matches "Builtins must not include renderer or software-3D internal boundaries:" \
  "$builtin_private_video_headers"

builtin_private_software3d_storage=$(find src/runtime/builtins -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E -- \
  '(^|[^[:alnum:]_])(GmlD3(State|Texture|Model|Batch|Vertex)|GML_D3_[[:alnum:]_]+|GML_TEX_(SPR_TAG|SURF_TAG|BG_TAG|KIND_MASK))([^[:alnum:]_]|$)|(^|[^[:alnum:]_])(graphics|software3d|d3|g_d3)[[:space:]]*->[[:space:]]*[A-Za-z_][A-Za-z0-9_]*' \
  {} + 2>/dev/null || true)
fail_matches "Builtins must use opaque software-3D operations and value copies, not private storage:" \
  "$builtin_private_software3d_storage"

builtin_private_renderer_assets=$(find src/runtime/builtins -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  '(^|[^[:alnum:]_])(GmlSprite|GmlTpag|GmlAtlas|GmlBg|GmlSurface|GmlFont)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Builtins must use renderer asset operations and value snapshots, not private records:" \
  "$builtin_private_renderer_assets"

builtin_renderer_asset_field_access=$(find src/runtime/builtins -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E -- \
  '(^|[^[:alnum:]_])(R|R2|r|render|renderer)[[:space:]]*->[[:space:]]*(n_spr|spr|n_bg|bg|n_tpag|tpag|n_fonts|fonts|default_font)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Builtins must not dereference renderer sprite, background, texture-page, or font storage:" \
  "$builtin_renderer_asset_field_access"

builtin_private_renderer_shaders=$(find src/runtime/builtins -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E \
  '(^|[^[:alnum:]_])(GmlShaderPal|GML_SHADER_HANDLE_STRIDE|GML_SHADER_HANDLE)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Builtins must treat renderer shader records and handles as private:" \
  "$builtin_private_renderer_shaders"

builtin_renderer_shader_field_access=$(find src/runtime/builtins -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E -- \
  '(^|[^[:alnum:]_])(R|R2|r|render|renderer)[[:space:]]*->[[:space:]]*(n_shader_pal|shader_pal|crt_shader_enable|lut_pal_sprite|lut_pal_frame)([^[:alnum:]_]|$)' \
  {} + 2>/dev/null || true)
fail_matches "Builtins must use opaque renderer shader control operations:" \
  "$builtin_renderer_shader_field_access"

renderer_control_definition_files=$(find src -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*(int|void)[[:space:]]+gml_render_(set_frame|control_update|resource_metrics|diagnostic_metrics|target_metrics|target_metrics_update|target_coverage|target_coverage_update|presentation_metrics|presentation_effective_set|sample_planes_update|application_surface_bind|application_surface_owned_clear|application_surface_select_owned|surface_mirror_pixels|draw_state_get|draw_state_update|gpu_state_push|gpu_state_pop|application_surface_set_draw_enabled|target_pixel|shader_set_current|shader_current|shader_is_compiled|shader_uniform_handle|shader_sampler_handle|shader_uniform_set|shader_texture_stage_set)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_control_definition_files" != "src/video/renderer/gml_render.c" ]; then
  printf '%s\n' "Renderer frame, target, and shader control operations belong only to gml_render.c:" >&2
  printf '%s\n' "$renderer_control_definition_files" >&2
  exit 1
fi

core_renderer_field_access=$(find src/core -type f \
  \( -name '*.c' -o -name '*.h' \) -exec grep -n -H -E -- \
  '(g_render[.]|->[[:space:]]*render[.])[A-Za-z_][A-Za-z0-9_]*' \
  {} + 2>/dev/null || true)
fail_matches "Core must use renderer snapshots, borrowed composition views, and coarse operations rather than renderer storage:" \
  "$core_renderer_field_access"

renderer_metadata_definition_files=$(find src -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*int[[:space:]]+gml_render_(sprite_metrics|sprite_set_playback|sprite_texture_handle|surface_texture_handle|texture_metrics|background_metrics|background_texture_handle|background_tile_source_index|font_metrics)[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_metadata_definition_files" != "src/video/renderer/gml_render_assets.c" ]; then
  printf '%s\n' "Renderer asset metadata operations belong only to gml_render_assets.c:" >&2
  printf '%s\n' "$renderer_metadata_definition_files" >&2
  exit 1
fi

renderer_clear_definition_files=$(find src -type f -name '*.c' -exec grep -l -E \
  '^[[:space:]]*void[[:space:]]+gml_render_clear[[:space:]]*\(' \
  {} + 2>/dev/null || true)
if [ "$renderer_clear_definition_files" != "src/video/renderer/gml_render_primitives.c" ]; then
  printf '%s\n' "Renderer clear semantics belong only to gml_render_primitives.c:" >&2
  printf '%s\n' "$renderer_clear_definition_files" >&2
  exit 1
fi

texture_handle_tag_definitions=$(portable_product_files | xargs grep -n -H -E \
  '^#[[:space:]]*define[[:space:]]+GML_TEX_(SPR_TAG|SURF_TAG|BG_TAG|KIND_MASK)[[:space:]]' \
  2>/dev/null | grep -v '^src/video/renderer/gml_render_backend\.h:' || true)
fail_matches "Renderer texture-handle tags belong only to gml_render_backend.h:" \
  "$texture_handle_tag_definitions"

software3d_texture_type_definitions=$(portable_product_files | xargs grep -l -E \
  '}[[:space:]]+GmlD3Texture;' 2>/dev/null || true)
if [ "$software3d_texture_type_definitions" != "src/video/software3d/gml_software3d_internal.h" ]; then
  printf '%s\n' "Only the software-3D internal header may define GmlD3Texture:" >&2
  printf '%s\n' "$software3d_texture_type_definitions" >&2
  exit 1
fi

legacy_builtin_asset_lookup=$(find src -type f \( -name '*.c' -o -name '*.h' \) \
  -exec grep -n -H -E '(^|[^[:alnum:]_])sprite_tpag_info[[:space:]]*\(' {} + 2>/dev/null || true)
fail_matches "The builtin-local sprite_tpag_info lookup must not return:" \
  "$legacy_builtin_asset_lookup"

engine_definitions=$(portable_product_files | xargs grep -l -E \
  '^[[:space:]]*struct[[:space:]]+AnygmEngine[[:space:]]*\{' 2>/dev/null || true)
if [ "$engine_definitions" != "src/core/engine_internal.h" ]; then
  printf '%s\n' "Only src/core/engine_internal.h may define struct AnygmEngine:" >&2
  printf '%s\n' "$engine_definitions" >&2
  exit 1
fi

root_state_definitions=$(find src/core -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(static[[:space:]]+)?(bool|int|size_t|void|uint64_t)[[:space:]]+(state_(header_(read|write)|read_render|write_render|write|profile_maybe_log|unserialize_impl)|engine_state_(size|save|load))[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/core/engine_state\.c:' || true)
fail_matches "Root state framing definitions belong only to src/core/engine_state.c:" \
  "$root_state_definitions"

root_state_header=$(find src/core -type f \( -name '*.c' -o -name '*.h' \) \
  -not -path 'src/core/engine_state.c' -exec grep -n -H 'ANYGM_STATE_MAGIC' {} + 2>/dev/null || true)
fail_matches "The root state header belongs only to src/core/engine_state.c:" \
  "$root_state_header"

input_owner_definitions=$(find src/core -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(void|int)[[:space:]]+engine_input_(bind|poll_keyboard|poll_mouse)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/core/engine_input\.c:' || true)
fail_matches "Normalized input service definitions belong only to src/core/engine_input.c:" \
  "$input_owner_definitions"

presentation_owner_definitions=$(find src/core -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(static[[:space:]]+)?(int|void|uint32_t)[[:space:]]+(ensure_classic_phase|ensure_primary_buffers|ensure_scratch_buffer|log_present_pass|classic_transition_(reset|release|start|apply)|draw_game_cursor|core_opt_(resolution|embedded_shaders|crt_mask|onoff|crt_tristate|fast_alpha_cull)|clamp_camera_to_current_room|aspect_forced_camera|present_view_(get|count)|stale_full_view_port|application_surface_scales_full_view_port|aspect_hud_rect|compute_present|aspect_view_overlay_(begin|end)|aspect_draw_event_hook|sync_room_fps|cur_room_bg|draw_runtime_backgrounds|aspect_mask_outside_room|setup_display|compose_view_rect|render_multiview_application|content_router_log)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/core/engine_presentation\.c:' || true)
fail_matches "Presentation service definitions belong only to src/core/engine_presentation.c:" \
  "$presentation_owner_definitions"

override_owner_definitions=$(find src/core -type f -name '*.c' -exec grep -n -H -E \
  '^[[:space:]]*(static[[:space:]]+)?(int|void)[[:space:]]+(core_opt_(god|start_room|redirect_room_order)|engine_override_(reset|set)|apply_sticky_cheats|aspect_apply_program|aspect_(compositor_fullwidth|center_view_target|wide_gameplay_view|draw_full_view)_gen|menu_run|room_skip_hook|introskip_hook)[[:space:]]*\(' \
  {} + 2>/dev/null | grep -v '^src/core/engine_overrides\.c:' || true)
fail_matches "Runtime override and menu service definitions belong only to src/core/engine_overrides.c:" \
  "$override_owner_definitions"

raw_revision_pattern='classic_version|bytecode[[:space:]]*(==|!=|<=|>=|<|>)'
matches=$(runtime_files | xargs grep -n -E "$raw_revision_pattern" 2>/dev/null || true)
fail_matches "Runtime code must consume named compatibility policies:" "$matches"

frontend_coupling=$(portable_product_files | xargs grep -n -E \
  'libretro|(^|[^[:alnum:]_])retro_[[:alnum:]_]+|RETRO_[[:alnum:]_]+' 2>/dev/null || true)
fail_matches "Portable product code must not depend on the libretro adapter:" "$frontend_coupling"

public_includes=$(grep -R -n -E '^[[:space:]]*#include' src/api --include='*.h' | \
  grep -v -E '#include[[:space:]]+[<"](stddef|stdint)\.h[>"]' || true)
fail_matches "Public headers may include only standard fixed-width and size headers:" "$public_includes"

public_internals=$(grep -R -n -E 'Gml|gml_|libretro|retro_|RETRO_|engine_internal|content_router' \
  src/api --include='*.h' || true)
fail_matches "Public headers must not expose internal or adapter types:" "$public_internals"

adapter_private_includes=$(grep -R -n -E '^[[:space:]]*#include[[:space:]]+"' \
  src/adapters/libretro --include='*.c' --include='*.h' | \
  grep -v -E '"(libretro_internal|anygm|libretro)\.h"' || true)
fail_matches "The libretro adapter may include only its boundary header and public API:" \
  "$adapter_private_includes"

direct_diagnostics=$(portable_product_files | xargs grep -n -E \
  '(^|[^[:alnum:]_])(fprintf|printf|puts|putchar|perror|fputc|fputs)[[:space:]]*\(' \
  2>/dev/null | grep -v 'src/host/stdio_vfs.c:' || true)
fail_matches "Portable diagnostics must use the host log service:" "$direct_diagnostics"

direct_system=$(portable_product_files | xargs grep -n -E \
  '(^|[^[:alnum:]_])(getenv|fopen|freopen|fread|fwrite|fclose|fflush|fseek|ftell|remove|rename|opendir|readdir|closedir|mkdir|rmdir|stat|lstat|clock_gettime|gettimeofday|localtime|localtime_r|gmtime|gmtime_r|CreateWindowExA|LoadLibraryA|GetModuleHandleA|GetProcAddress|PrintWindow)[[:space:]]*\(' \
  2>/dev/null | grep -v -E 'src/host/(stdio_vfs\.c|gml_thread\.h):' || true)
fail_matches "Portable modules must use host services instead of operating-system APIs:" \
  "$direct_system"

active_content=$(portable_product_files | xargs grep -n -E \
  '(^|[^[:alnum:]_])(system|popen|fork|execl|execv|posix_spawn|dlopen|LoadLibrary|socket|connect|curl_easy_)[[:space:]]*\(' \
  2>/dev/null | grep -v -E ':[[:space:]]*(/\*|\*|//)' || true)
fail_matches "Content and runtime paths must not execute processes, load plugins, or open network sockets:" \
  "$active_content"

mutable_local_static=$(portable_product_files | xargs grep -n -E \
  '^[[:space:]]+static[[:space:]]+' 2>/dev/null | \
  grep -v -E 'static[[:space:]]+const[[:space:]]' || true)
fail_matches "Function-local static state must be immutable or owned by an engine context:" \
  "$mutable_local_static"

unbounded_decode=$(find src -type f -name '*.c' \
  -not -path 'src/content/bytecode/gml_bytecode.c' \
  -exec grep -n -H -E 'gml_decode_bc[[:space:]]*\(' {} + 2>/dev/null || true)
fail_matches "Content-derived bytecode must use the bounded decoder entry point:" \
  "$unbounded_decode"

printf '%s\n' "architecture boundaries: ok"
