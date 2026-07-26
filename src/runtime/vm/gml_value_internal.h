/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Private value storage and lifetime operations shared only by VM implementation owners. */
#ifndef GML_VALUE_INTERNAL_H
#define GML_VALUE_INTERNAL_H

#include "gml_value.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  void **set;
  size_t capacity, count;
  int active, skip_escaped;
} GmlValueFreeContext;

uint32_t gml_value_name_hash(const char *name);
GmlVal *gml_varmap_get_hashed(GmlVarMap *map, const char *key, uint32_t hash);
GmlVal *gml_varmap_put_hashed(GmlVarMap *map, const char *key, uint32_t hash);
GmlVal *gml_varmap_put_owned_hashed(GmlVarMap *map, char *key, uint32_t hash);

GmlArr *gml_arr_slot_ensure(GmlVal *slot);
void gml_arr_index_ensure(GmlArr *array, int index);
void gml_arr_note_legacy_2d_set(GmlArr *array, int index);
void gml_arr_rebuild_legacy_2d_meta(GmlArr *array);
int gml_arr_nested_set_flat(GmlVal array, int flat_index, GmlVal value);
int gml_arr_nested_get_flat(GmlVal array, int flat_index, GmlVal *value);

void gml_value_free_context_begin(GmlValueFreeContext *context);
void gml_value_free_context_end(GmlValueFreeContext *context);
void gml_val_free(GmlValueFreeContext *context, GmlVal value);
void gml_varmap_free_with_context(GmlVarMap *map, int skip_escaped,
                                  GmlValueFreeContext *context);
void gml_varmap_free_ex(GmlVarMap *map, int skip_escaped);
void gml_varmap_free(GmlVarMap *map);

void gml_arr_row_ensure(GmlArr *array, int row);

/* ---- arrays ---- */
#define GML_2D_STRIDE 32000
#endif
