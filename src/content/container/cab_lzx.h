/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Decoder for the supported LZX-21 Cabinet profile.
 */
#ifndef ANYGM_CAB_LZX_H
#define ANYGM_CAB_LZX_H

#include <stddef.h>
#include <stdint.h>

/* Decompress a concatenated LZX (window 2^21) stream of `in_len` bytes into exactly
 * `out_len` bytes at `out`. The input is the Cabinet folder's CFDATA payloads joined
 * in order; `out_len` is the folder's total expanded size. Returns 0 on success and a
 * negative value on any malformed input, buffer exhaustion, or inconsistency. Never
 * reads past `in`/`in_len` or writes past `out`/`out_len`. */
int cab_lzx21_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);

#endif
