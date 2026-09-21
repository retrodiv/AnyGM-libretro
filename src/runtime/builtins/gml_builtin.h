/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Instance-owned state and dispatch for built-in runtime functions. */
#ifndef GML_BUILTIN_H
#define GML_BUILTIN_H

#include "gml_vm.h"

typedef struct GmlBuiltinState GmlBuiltinState;
typedef struct GmlVmStateWriter GmlVmStateWriter;
typedef struct GmlVmStateReader GmlVmStateReader;
typedef void (*GmlBuiltinValueVisitor)(void *userdata, GmlVal value);

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
void gml_builtin_physics_room_reset(GmlVM *vm);
int gml_physics_body_enabled(GmlVM *vm,const GmlInstance *instance);
void gml_physics_step(GmlVM *vm);
int gml_physics_variable_get(GmlVM *vm,GmlInstance *instance,const char *name,GmlVal *out);
int gml_physics_variable_set(GmlVM *vm,GmlInstance *instance,const char *name,GmlVal value);

void gml_builtin_state_write_ini_ds(const GmlBuiltinState *state,
                                    GmlVmStateWriter *writer);
int gml_builtin_state_read_ini_ds(GmlBuiltinState *state,
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

int gml_builtin_fast_id(GmlVM *vm,const char *name);
/* Code index of the payload's own script for a name it redefines, or -1. */
int gml_builtin_payload_shadow_script(GmlVM *vm,const char *name);
GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);
GmlVal gml_builtin_call_fast_id(GmlVM *vm, int id, const char *name,
                                GmlVal *args, int count);

#endif
