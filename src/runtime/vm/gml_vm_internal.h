/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_GML_VM_INTERNAL_H
#define ANYGM_GML_VM_INTERNAL_H

#include "gml_vm.h"
#include <stdint.h>
#include <string.h>

/*
 * VM-family-private operations shared by execution, lifecycle, room, and
 * canonical-state owners. Keep storage in gml_vm.h until its owning split and
 * do not expose this interface outside src/runtime/vm implementation files.
 */
static inline uint32_t gml_vm_read_u32_le(const uint8_t *data,
                                          uint32_t offset){
  return (uint32_t)data[offset] |
         (uint32_t)data[offset+1]<<8 |
         (uint32_t)data[offset+2]<<16 |
         (uint32_t)data[offset+3]<<24;
}
static inline float gml_vm_read_f32_le(const uint8_t *data, uint32_t offset){
  uint32_t value=gml_vm_read_u32_le(data,offset);
  float result;
  memcpy(&result,&value,4);
  return result;
}
double gml_vm_classic_round_even(double value);
double gml_vm_value_as_number(GmlVal value);
int gml_vm_value_is_true(GmlVal value);
void gml_vm_motion_from_components(GmlVM *vm, GmlInstance *instance);
void gml_vm_motion_from_speed_direction(GmlVM *vm, GmlInstance *instance);
int gml_vm_instance_builtin_set(GmlVM *vm, GmlInstance *instance,
                                const char *name, GmlVal value);
int gml_vm_variable_name_maybe_special(GmlVM *vm, const char *name,
                                       uint32_t name_hash);
GmlVal gml_vm_variable_get_h(GmlVM *vm, int instance,
                             const char *name, uint32_t name_hash);
int gml_vm_code_cache_ensure(GmlWin *win, int code_index);
GmlInstance *gml_vm_instance_by_id(GmlVM *vm, double id);
int gml_vm_instance_bbox(GmlVM *vm, GmlInstance *instance,
                         double *left, double *top, double *right, double *bottom);
int gml_vm_instances_bbox(GmlVM *vm, GmlInstance *instance,
                          double *left, double *top, double *right,
                          double *bottom);
void gml_vm_global_array_set(GmlVM *vm, const char *name, int index,
                             double value);
double gml_vm_global_array_number(GmlVM *vm, const char *name, int index);
int gml_vm_instances_event_lookup(GmlVM *vm, const char *suffix, int object,
                                  int *handler_object, int *code);
int gml_vm_instances_collect_object_slots(GmlVM *vm, int object);
const int *gml_vm_instances_event_objects(GmlVM *vm, const char *suffix,
                                          int *count);
void gml_vm_instances_run_classic_event(GmlVM *vm, const char *suffix);
void gml_vm_instances_sort_slots(GmlVM *vm, int *slots, int count);
void gml_vm_instances_prepare_step(GmlVM *vm, int extent);
GmlInstance *gml_vm_instances_alloc(GmlVM *vm);
void gml_vm_instances_initialize(GmlVM *vm, GmlInstance *instance,
                                 double x, double y, int object);
void gml_vm_instances_apply_room_transform(GmlVM *vm,
                                           GmlInstance *instance,
                                           uint32_t record_offset);
void gml_vm_instances_link(GmlVM *vm, GmlInstance *instance);
void gml_vm_instances_unlink(GmlVM *vm, GmlInstance *instance, int object);
void gml_vm_instances_reap(GmlVM *vm);
void gml_vm_instances_trim_pool_tail(GmlVM *vm);
void gml_vm_instances_rebase_order(GmlVM *vm);
int gml_vm_instances_native_event_declared(
    GmlVM *vm, int event_type, int subtype, int object,
    int *handler_object, int *code);
void gml_vm_instances_run_classic_triggers(GmlVM *vm, int moment);
int gml_vm_instances_step_snapshot_member(GmlVM *vm,
                                          const GmlInstance *instance);
void gml_vm_instances_run_collisions(GmlVM *vm);
void gml_vm_instances_run_boundary_events(GmlVM *vm);
void gml_vm_instances_parse_objects(GmlVM *vm);
void gml_vm_instances_parse_boundary_events(GmlVM *vm);
void gml_vm_instances_parse_dispatch_events(GmlVM *vm);
void gml_vm_instances_reset_caches(GmlVM *vm);
void gml_vm_rooms_init(GmlVM *vm);
void gml_vm_rooms_step_paths(GmlVM *vm);
void gml_vm_rooms_step_timelines(GmlVM *vm, int snapshot_count);
void gml_vm_frame_advance_layers(GmlVM *vm);
uint32_t gml_vm_rooms_layer_list(GmlVM *vm, int room_index,
                                 uint32_t *count);
int gml_vm_frame_apply_tile_mutation(GmlVM *vm, int depth,
                                     int *effective_depth,
                                     double *offset_x, double *offset_y);
void gml_vm_rooms_clear_tilemaps(GmlVM *vm);
void gml_vm_frame_cleanup(GmlVM *vm);
int gml_vm_struct_ensure_capacity(GmlVM *vm, int need);
void gml_vm_struct_free_slot_push(GmlVM *vm, int slot);
int gml_vm_tilemap_ensure_owned(GmlTileMap *tilemap);
void gml_vm_room_reload_layers_mode(GmlVM *vm, int room_index,
                                    int rebuild_runtime_layers);
void gml_vm_prefetch_room_assets(GmlVM *vm);
void gml_vm_warm_audio_for_room_window(GmlVM *vm);
#if defined(ANYGM_DIAGNOSTICS) && ANYGM_DIAGNOSTICS
#include "gml_vm_diagnostics.h"
#define GML_VM_DIAGNOSTIC_OPCODE(...) \
  gml_vm_diagnostics_opcode(__VA_ARGS__)
#define GML_VM_DIAGNOSTIC_OPCODE_ENABLED(...) \
  gml_vm_diagnostics_opcode_enabled(__VA_ARGS__)
#define GML_VM_DIAGNOSTIC_EVENT(...) \
  gml_vm_diagnostics_event(__VA_ARGS__)
#define GML_VM_DIAGNOSTIC_COLLISION(...) \
  gml_vm_diagnostics_collision(__VA_ARGS__)
#define GML_VM_DIAGNOSTIC_VARIABLE_SCOPE(...) \
  gml_vm_diagnostics_variable_scope(__VA_ARGS__)
#define GML_VM_DIAGNOSTIC_VARIABLE_INSTANCE(...) \
  gml_vm_diagnostics_variable_instance(__VA_ARGS__)
#define GML_VM_DIAGNOSTIC_DESTROY(...) \
  gml_vm_diagnostics_destroy(__VA_ARGS__)
#else
#define GML_VM_DIAGNOSTIC_OPCODE(...) ((void)0)
#define GML_VM_DIAGNOSTIC_OPCODE_ENABLED(...) 0
#define GML_VM_DIAGNOSTIC_EVENT(...) ((void)0)
#define GML_VM_DIAGNOSTIC_COLLISION(...) ((void)0)
#define GML_VM_DIAGNOSTIC_VARIABLE_SCOPE(...) ((void)0)
#define GML_VM_DIAGNOSTIC_VARIABLE_INSTANCE(...) ((void)0)
#define GML_VM_DIAGNOSTIC_DESTROY(...) ((void)0)
#endif
#define GML_VM_TILE_MUT_DELETED 1
#define GML_VM_TILE_MUT_HIDDEN  2
#define GML_STRUCT_SLOT_BITS 20
#define GML_STRUCT_SLOT_MAX  (1<<GML_STRUCT_SLOT_BITS)
#define GML_STRUCT_SLOT_MASK (GML_STRUCT_SLOT_MAX-1)
#endif
