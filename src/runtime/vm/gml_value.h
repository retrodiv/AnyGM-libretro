/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_value.h owns language values, arrays, variable maps, and their lifetime rules. */
#ifndef GML_VALUE_H
#define GML_VALUE_H

#include <stddef.h>
#include <stdint.h>

/* ---- value ---- */
typedef enum { V_REAL=0, V_STR=1, V_ARR=2, V_UNDEF=3 } GmlValType;
typedef struct { GmlValType t; double d; const char *s; void *arr; } GmlVal;
typedef struct {
  GmlVal *data; int len, cap;
  int is_2d, height2d, row_cap;
  int nested_2d;
  int *row_len;
  int escaped;   /* referenced beyond its creating scope (stored to a global/instance var,
                    a ds structure, or returned) — locals cleanup must not free it */
  unsigned gc_epoch; /* the collection pass that last scanned this array; see gml_struct_gc */
} GmlArr;
static inline GmlVal vreal(double d){ GmlVal v; v.t=V_REAL; v.d=d; v.s=0; v.arr=0; return v; }
static inline GmlVal vstr(const char *s){ GmlVal v; v.t=V_STR; v.d=0; v.s=s; v.arr=0; return v; }
/* owned = the string is a fresh malloc'd temporary (v.d!=0 marks ownership); the VM frees it when
 * consumed. Literals/references (data.win STRG, rodata, var pointers) use vstr() and are never freed. */
static inline GmlVal vstr_owned(char *s){ GmlVal v; v.t=V_STR; v.d=1; v.s=s; v.arr=0; return v; }
static inline GmlVal vundef(void){ GmlVal v; v.t=V_UNDEF; v.d=0; v.s=0; v.arr=0; return v; }

/* ---- variable map: open-addressing, key = interned name pointer ---- */
typedef struct {
  const char *key;
  uint32_t hash;
  GmlVal val;
  unsigned char key_owned; /* heap key released with the map; STRG/literal keys stay borrowed */
} GmlVarSlot;
typedef struct { GmlVarSlot *slots; int cap, len; } GmlVarMap;
int          gml_val_array_length(GmlVal v);   /* array_length_1d: logical length of a V_ARR value, else 0 */
int          gml_val_array_height_2d(GmlVal v);
int          gml_val_array_length_2d(GmlVal v, int row);
/* GMS2.3 array-function forms (array_create/get/set/push/pop/resize/copy) — operate on V_ARR values */
GmlVal       gml_arr_store_clone(GmlVal v); /* own strings / mark-escape arrays before storing in an array */
GmlVal       gml_arr_new(int size, GmlVal fill);
void         gml_arr_set(GmlVal arr, int idx, GmlVal val);
GmlVal       gml_arr_get(GmlVal arr, int idx);
GmlVal       gml_arr_chain_ensure(GmlVal arr, int idx);
void         gml_arr_set_2d(GmlVal arr, int row, int column, GmlVal val);
GmlVal       gml_arr_get_2d(GmlVal arr, int row, int column);
void         gml_arr_push(GmlVal arr, GmlVal val);
GmlVal       gml_arr_pop(GmlVal arr);
void         gml_arr_resize(GmlVal arr, int size);
void         gml_arr_copy(GmlVal dst, int di, GmlVal src, int si, int count);
void         gml_arr_insert(GmlVal arr, int index, GmlVal *values, int count);

GmlVal *gml_varmap_get(GmlVarMap *m, const char *key);   /* NULL if absent */
GmlVal *gml_varmap_put(GmlVarMap *m, const char *key);   /* get-or-create slot */

void gml_arr_mark_escaped(GmlVal v);   /* array stored beyond its scope: locals cleanup must not free it */
/* Release a bounded group of value roots with one alias-deduplication pass. */
void gml_values_release(GmlVal *values, size_t count);
#endif
