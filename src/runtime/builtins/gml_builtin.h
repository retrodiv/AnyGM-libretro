/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Instance-owned state and dispatch for built-in runtime functions. */
#ifndef GML_BUILTIN_H
#define GML_BUILTIN_H

#include "gml_vm.h"

/* Typed particle records share these buffer names with ordinary GML accesses. */
#define GML_BUFFER_RECORD_CONSTANTS(ENTRY) \
  ENTRY(0,"buffer_fixed") \
  ENTRY(1,"buffer_grow") \
  ENTRY(1,"buffer_u8") \
  ENTRY(3,"buffer_u16") \
  ENTRY(4,"buffer_s16") \
  ENTRY(5,"buffer_u32") \
  ENTRY(6,"buffer_s32") \
  ENTRY(8,"buffer_f32") \
  ENTRY(0,"buffer_seek_start")

typedef struct GmlBuiltinState GmlBuiltinState;
typedef struct GmlVmStateWriter GmlVmStateWriter;
typedef struct GmlVmStateReader GmlVmStateReader;
typedef void (*GmlBuiltinValueVisitor)(void *userdata, GmlVal value);
void gml_builtin_finish_game_load(GmlVM *vm);
void gml_builtin_video_close(GmlVM *vm);
void gml_builtin_video_tick(GmlVM *vm);
typedef struct GmlBuiltinVideoSource GmlBuiltinVideoSource;
GmlBuiltinVideoSource *gml_builtin_video_checkpoint(GmlVM *vm);
void gml_builtin_video_source_release(GmlBuiltinVideoSource *source);
void gml_builtin_video_rebind(GmlVM *vm);
int gml_builtin_video_finish_restore(GmlVM *vm);
size_t gml_builtin_video_state_capacity_hint(const GmlVM *vm);
void gml_builtin_state_write_video(const GmlBuiltinState *state,GmlVmStateWriter *writer);
int gml_builtin_state_read_video(GmlBuiltinState *state,GmlVmStateReader *reader);

GmlBuiltinState *gml_builtin_state_create(GmlVM *vm);
GmlBuiltinState *gml_builtin_state_ensure(GmlVM *vm);
void gml_builtin_state_rebind(GmlBuiltinState *state,GmlVM *vm);
void gml_builtin_state_reset(GmlBuiltinState *state);
void gml_builtin_state_destroy(GmlBuiltinState *state);
void gml_builtin_state_visit_values(const GmlBuiltinState *state,
                                    GmlBuiltinValueVisitor visitor,
                                    void *userdata);
void gml_builtin_state_take_owned_values(GmlBuiltinState *state,
                                         GmlBuiltinValueVisitor visitor,
                                         void *userdata);
void gml_builtin_state_profile_ds(const GmlBuiltinState *state,
                                  size_t byte_totals[3], int live_counts[3]);
int gml_builtin_ini_entry_count(const GmlVM *vm);
int gml_builtin_account_picker_pending_count(const GmlVM *vm);
void gml_builtin_physics_room_reset(GmlVM *vm);
int gml_physics_body_enabled(GmlVM *vm,const GmlInstance *instance);
/* The same test against a physics world scale the caller already read for this room. */
int gml_physics_body_enabled_in_world(GmlVM *vm,const GmlInstance *instance,double world_scale);
double gml_physics_world_scale(GmlVM *vm);
void gml_physics_step(GmlVM *vm);
int gml_physics_variable_get(GmlVM *vm,GmlInstance *instance,const char *name,GmlVal *out);
int gml_physics_variable_set(GmlVM *vm,GmlInstance *instance,const char *name,GmlVal value);

void gml_builtin_state_write_ini_ds(const GmlBuiltinState *state,
                                    GmlVmStateWriter *writer);
int gml_builtin_state_read_ini_ds(GmlBuiltinState *state,
                                  GmlVmStateReader *reader);
void gml_builtin_state_write_gamepad_thresholds(const GmlBuiltinState *state,
                                               GmlVmStateWriter *writer);
int gml_builtin_state_read_gamepad_thresholds(GmlBuiltinState *state,
                                              GmlVmStateReader *reader);
void gml_builtin_state_write_mp_grids(const GmlBuiltinState *state,
                                      GmlVmStateWriter *writer);
int gml_builtin_state_read_mp_grids(GmlBuiltinState *state,
                                    GmlVmStateReader *reader);
void gml_builtin_state_write_physics(const GmlBuiltinState *state,
                                     GmlVmStateWriter *writer);
int gml_builtin_state_read_physics(GmlBuiltinState *state,
                                   GmlVmStateReader *reader);
void gml_builtin_state_write_audio(const GmlBuiltinState *state,
                                   GmlVmStateWriter *writer);
int gml_builtin_state_read_audio(GmlBuiltinState *state,
                                 GmlVmStateReader *reader);
void gml_builtin_state_write_time_sources(const GmlBuiltinState *state,
                                          GmlVmStateWriter *writer);
int gml_builtin_state_read_time_sources(GmlBuiltinState *state,
                                        GmlVmStateReader *reader);
void gml_builtin_state_write_account_picker(const GmlBuiltinState *state,
                                            GmlVmStateWriter *writer);
int gml_builtin_state_read_account_picker(GmlBuiltinState *state,
                                           GmlVmStateReader *reader);

/* Copies instance user storage through the existing value-clone owner. */
int gml_builtin_copy_instance_variables(GmlVM *vm,const GmlInstance *source,uint32_t copy_id,
                                         GmlVarMap *destination);
int gml_builtin_fork_array(GmlVM *vm,GmlVal *slot,uint64_t owner);
/* Virtual instance_id access uses the existing active-instance finder. */
GmlVal gml_builtin_instance_id_at(GmlVM *vm,int index);

int gml_builtin_fast_id(GmlVM *vm,const char *name);
int gml_builtin_resolve_script(GmlVM *vm,const char *name,int *declared);
/* Code index of the payload's own script for a name it redefines, or -1. */
int gml_builtin_payload_shadow_script(GmlVM *vm,const char *name);
GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);
GmlVal gml_builtin_call_fast_id(GmlVM *vm, int id, const char *name,
                                GmlVal *args, int count);

#endif
