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

int gmlc_bytecode_emit_empty(GmlcCodeBlob *out);
int gmlc_bytecode_compile_source(const GmlcProject *project, const char *path, GmlcCodeBlob *out, char *err, size_t errcap);
void gmlc_bytecode_free(GmlcCodeBlob *b);

#endif
