/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#ifndef GMLC_BYTECODE_H
#define GMLC_BYTECODE_H

#include <stdint.h>
#include <stddef.h>
#include "gmlc_project.h"

typedef enum {
  GMLC_REF_FUNC,
  GMLC_REF_VARI
} GmlcRefKind;

typedef struct {
  char *name;
  uint32_t instr_off;
  uint32_t ref_off;
  uint32_t high_bits;
  int inst;
  GmlcRefKind kind;
} GmlcRefSite;

typedef struct {
  char *value;
  uint32_t payload_off;
} GmlcStringSite;

typedef struct {
  uint8_t *data;
  size_t size;
  GmlcRefSite *refs;
  int n_refs, cap_refs;
  GmlcStringSite *strings;
  int n_strings, cap_strings;
  int is_placeholder;
  char *diagnostic;
} GmlcCodeBlob;

typedef struct {
  char *name;
  char *params;
  char *body;
  char *source_path;
  size_t start, end;
  int code_index;
  int is_script_wrapper;
} GmlcFunctionDef;

typedef struct {
  char *name;
  double value;
  int script_code_index;
} GmlcAssetBinding;

typedef struct {
  GmlcFunctionDef *defs;
  int n_defs, cap_defs;
  char **globals;
  int n_globals, cap_globals;
  GmlcAssetBinding *assets;
  int n_assets, cap_assets;
  int *asset_hash_slots;
  int asset_hash_cap;
  char **macro_names;
  double *macro_values;
  int n_macros, cap_macros;
  char **constant_names;
  char **constant_exprs;
  int n_constants, cap_constants;
} GmlcFunctionRegistry;

int gmlc_bytecode_emit_empty(GmlcCodeBlob *out);
int gmlc_bytecode_compile_source(const GmlcProject *project, const char *path, GmlcCodeBlob *out, char *err, size_t errcap);
int gmlc_bytecode_compile_source_ex(const GmlcProject *project, const GmlcFunctionRegistry *funcs, int script_index, const char *path, GmlcCodeBlob *out, char *err, size_t errcap);
int gmlc_bytecode_compile_function_body(const GmlcProject *project, const GmlcFunctionRegistry *funcs, const GmlcFunctionDef *def, GmlcCodeBlob *out, char *err, size_t errcap);
int gmlc_bytecode_collect_functions(const GmlcProject *project, int appended_base, GmlcFunctionRegistry *out, char *err, size_t errcap);
int gmlc_function_registry_extra_count(const GmlcFunctionRegistry *r);
void gmlc_function_registry_free(GmlcFunctionRegistry *r);
void gmlc_bytecode_free(GmlcCodeBlob *b);

#endif
