/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Instance-owned state and dispatch for built-in runtime functions. */
#ifndef GML_BUILTIN_H
#define GML_BUILTIN_H

#include "gml_vm.h"

typedef struct GmlBuiltinState GmlBuiltinState;

GmlBuiltinState *gml_builtin_state_create(GmlVM *vm);
void gml_builtin_state_reset(GmlBuiltinState *state);
void gml_builtin_state_destroy(GmlBuiltinState *state);
void gml_builtin_files_close(GmlVM *vm);

int gml_builtin_fast_id(GmlVM *vm,const char *name);
GmlVal gml_builtin_call(GmlVM *vm, const char *name, GmlVal *args, int count);
GmlVal gml_builtin_call_fast_id(GmlVM *vm, int id, const char *name,
                                GmlVal *args, int count);

#endif
