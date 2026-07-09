/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#ifndef GMLC_BYTECODE_H
#define GMLC_BYTECODE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint8_t *data;
  size_t size;
} GmlcCodeBlob;

int gmlc_bytecode_emit_empty(GmlcCodeBlob *out);
void gmlc_bytecode_free(GmlcCodeBlob *b);

#endif
