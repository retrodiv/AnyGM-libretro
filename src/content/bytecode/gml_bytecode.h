/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_bytecode.h - CODE/FUNC/VARI layouts and instruction decoding. */
#ifndef GML_BYTECODE_H
#define GML_BYTECODE_H

#include "gml_win.h"

typedef struct {
  uint32_t start, count, stride, occ_off, addr_off;
  /* occurrence-chain walk: from each chain node `a`, the reference-word address (matching
   * GmlInsn.refaddr) is `a + ref_off`, and the next-node link is read from `u32(a + chain_off)`.
   * bc14/15/16 use 4/4 (the entry `addr` points at the instruction, ref word is the 2nd word);
   * bc17 FUNC selects 4/4 or 0/0 by probing occurrence-chain offsets. */
  uint32_t ref_off, chain_off;
} GmlRefLayout;

int gml_bc_code_start(const GmlWin *w, uint32_t entry_ptr, uint32_t *start);
int gml_bc_ref_layout(const GmlWin *w, const char *chunk, GmlRefLayout *out);

int gml_bc14_code_start(const GmlWin *w, uint32_t entry_ptr, uint32_t *start);
int gml_bc14_ref_layout(const GmlWin *w, const char *chunk, GmlRefLayout *out);
int gml_decode_bc14(const uint8_t *d, uint32_t ia, GmlInsn *out);

int gml_bc15_code_start(const GmlWin *w, uint32_t entry_ptr, uint32_t *start);
int gml_bc15_ref_layout(const GmlWin *w, const char *chunk, GmlRefLayout *out);
int gml_decode_bc15(const uint8_t *d, uint32_t ia, GmlInsn *out);

/* bc17 (GMS2) reuses bc15 decode/code_start; only the FUNC occurrence-chain layout differs. */
int gml_bc17_ref_layout(const GmlWin *w, const char *chunk, GmlRefLayout *out);

#endif
