/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gm_qoi.h — decoder for GameMaker's texture image format.
 *
 * The supported texture blobs use a pre-1.0 QOI variant rather than the released QOI
 * specification. They come in two shells, both handled here:
 *   - "fioq" : a bare GameMaker-QOI stream (magic 'f','i','o','q').
 *   - "2zoq" : a bzip2-compressed container wrapping a "fioq" stream (magic '2','z','o','q').
 * (bc14-16 games instead store plain PNG in the texture blob; the caller detects that separately.)
 *
 * The op set is the pre-1.0 QOI one (2/3/4-bit tags: INDEX / RUN_8 / RUN_16 / DIFF_8 / DIFF_16 /
 * DIFF_24 / COLOR) with an XOR running-pixel hash.
 *
 * The "2zoq" path needs a bzip2 decompressor; the caller passes already-decompressed "fioq" bytes.
 */
#ifndef GM_QOI_H
#define GM_QOI_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Decode a GameMaker "fioq" QOI stream to a freshly allocated RGBA buffer (w*h*4 bytes).
 * Returns the caller-owned buffer and sets both output dimensions, or NULL on malformed input. */
static inline uint8_t *gm_qoi_decode(const uint8_t *d, size_t len, int *out_w, int *out_h) {
  if (len < 12 || d[0] != 'f' || d[1] != 'i' || d[2] != 'o' || d[3] != 'q') return NULL;
  int w = d[4] | (d[5] << 8);
  int h = d[6] | (d[7] << 8);
  if (w <= 0 || h <= 0 || (long)w * h > 64L * 1024 * 1024) return NULL;   /* sanity cap */
  size_t total = (size_t)w * h;
  uint8_t *out = (uint8_t *)malloc(total * 4);
  if (!out) return NULL;

  uint8_t r = 0, g = 0, b = 0, a = 255;
  uint8_t idx[64][4]; memset(idx, 0, sizeof idx);
  size_t p = 12, cnt = 0, oi = 0;

  #define GMQ_EMIT(n) do{ int _run=(n); while(_run-- > 0 && cnt < total){ \
        out[oi]=r; out[oi+1]=g; out[oi+2]=b; out[oi+3]=a; oi+=4; cnt++; } }while(0)
  #define GMQ_STORE() do{ int _h=(r^g^b^a)&63; idx[_h][0]=r; idx[_h][1]=g; idx[_h][2]=b; idx[_h][3]=a; }while(0)

  while (cnt < total && p < len) {
    uint8_t op = d[p++];
    if ((op & 0xc0) == 0x00) {                 /* INDEX  00xxxxxx */
      const uint8_t *ip = idx[op & 0x3f];
      r = ip[0]; g = ip[1]; b = ip[2]; a = ip[3];
      GMQ_EMIT(1);
    } else if ((op & 0xe0) == 0x40) {          /* RUN_8  010xxxxx */
      GMQ_EMIT((op & 0x1f) + 1);
    } else if ((op & 0xe0) == 0x60) {          /* RUN_16 011xxxxx xxxxxxxx */
      if (p >= len) break;
      int run = (((op & 0x1f) << 8) | d[p++]) + 33;
      GMQ_EMIT(run);
    } else if ((op & 0xc0) == 0x80) {          /* DIFF_8 10rrggbb (2-bit two's-complement per ch) */
      r = (uint8_t)(r + ((((op >> 4) & 3) ^ 2) - 2));
      g = (uint8_t)(g + ((((op >> 2) & 3) ^ 2) - 2));
      b = (uint8_t)(b + ((( op       & 3) ^ 2) - 2));
      GMQ_STORE(); GMQ_EMIT(1);
    } else if ((op & 0xe0) == 0xc0) {          /* DIFF_16 110rrrrr ggggbbbb (r5/g4/b4, two's-compl.) */
      if (p >= len) break;
      uint8_t b2 = d[p++];
      r = (uint8_t)(r + (((op & 0x1f) ^ 16) - 16));
      g = (uint8_t)(g + (((b2 >> 4)   ^  8) -  8));
      b = (uint8_t)(b + (((b2 & 0x0f) ^  8) -  8));
      GMQ_STORE(); GMQ_EMIT(1);
    } else if ((op & 0xf0) == 0xe0) {          /* DIFF_24 1110xxxx x2 (r5/g5/b5/a5, two's-compl., incl. alpha) */
      if (p + 1 >= len) break;
      uint8_t b2 = d[p++], b3 = d[p++];
      r = (uint8_t)(r + (((((op & 0x0f) << 1) | (b2 >> 7)) ^ 16) - 16));
      g = (uint8_t)(g + ((((b2 >> 2) & 0x1f)               ^ 16) - 16));
      b = (uint8_t)(b + (((((b2 & 0x03) << 3) | (b3 >> 5)) ^ 16) - 16));
      a = (uint8_t)(a + (((b3 & 0x1f)                      ^ 16) - 16));
      GMQ_STORE(); GMQ_EMIT(1);
    } else {                                    /* COLOR  1111rgba (present channels follow) */
      if (op & 8) { if (p >= len) break; r = d[p++]; }
      if (op & 4) { if (p >= len) break; g = d[p++]; }
      if (op & 2) { if (p >= len) break; b = d[p++]; }
      if (op & 1) { if (p >= len) break; a = d[p++]; }
      GMQ_STORE(); GMQ_EMIT(1);
    }
  }
  /* pad any tail (malformed/truncated stream) with transparent so the atlas stays a valid w*h */
  while (cnt < total) { out[oi]=0; out[oi+1]=0; out[oi+2]=0; out[oi+3]=0; oi+=4; cnt++; }

  #undef GMQ_EMIT
  #undef GMQ_STORE
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  return out;
}

#endif /* GM_QOI_H */
