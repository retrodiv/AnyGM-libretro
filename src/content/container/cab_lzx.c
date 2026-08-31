/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Decoder for the supported LZX-21 Cabinet profile. The implementation
 * operates on caller-owned input and output buffers.
 */
#include "cab_lzx.h"

#include <string.h>

/* ---- LZX-21 constants ---- */
#define NUM_CHARS 256
#define NUM_POSITION_SLOTS 50
#define MAINTREE_LEN (NUM_CHARS + NUM_POSITION_SLOTS * 8) /* 656 */
#define NUM_SECONDARY_LENGTHS 249
#define ALIGNED_LEN 8
#define PRETREE_LEN 20
#define MIN_MATCH 2
#define FRAME 32768u
#define MAX_SYMBOLS MAINTREE_LEN

/* ---- bit reader: 16-bit little-endian words, consumed most-significant-bit first ---- */
typedef struct {
  const uint8_t *buf;
  size_t len, pos;
  uint32_t bitbuf;
  int bitcnt;
} Bits;

static void bits_init(Bits *b, const uint8_t *d, size_t n) {
  b->buf = d; b->len = n; b->pos = 0; b->bitbuf = 0; b->bitcnt = 0;
}
static void bits_fill(Bits *b) {
  while (b->bitcnt <= 16) {
    uint16_t w = 0;
    if (b->pos + 1 < b->len) { w = (uint16_t)(b->buf[b->pos] | (b->buf[b->pos + 1] << 8)); b->pos += 2; }
    else if (b->pos < b->len) { w = (uint16_t)b->buf[b->pos]; b->pos += 1; }
    b->bitbuf |= (uint32_t)w << (16 - b->bitcnt);
    b->bitcnt += 16;
  }
}
static uint32_t bits_read(Bits *b, int n) {
  if (n == 0) return 0;
  if (b->bitcnt < n) bits_fill(b);
  uint32_t v = b->bitbuf >> (32 - n);
  b->bitbuf <<= n;
  b->bitcnt -= n;
  return v;
}
/* realign to a 16-bit input boundary at every 32768-output-byte frame edge */
static void frame_align(Bits *b) {
  if (b->bitcnt > 0) {
    if (b->bitcnt < 16) bits_fill(b);
    int r = b->bitcnt & 15;
    if (r) { b->bitbuf <<= r; b->bitcnt -= r; }
  }
}

/* ---- canonical Huffman with fixed storage ---- */
typedef struct {
  int maxlen;
  int count[24];
  int first_code[24];
  int first_sym[24];
  int syms[MAX_SYMBOLS];
} Huff;

static int huff_build(Huff *h, const uint8_t *lens, int n) {
  memset(h->count, 0, sizeof h->count);
  h->maxlen = 0;
  for (int i = 0; i < n; i++) {
    int L = lens[i];
    if (L) { if (L > 23) return -1; h->count[L]++; if (L > h->maxlen) h->maxlen = L; }
  }
  int offs[24], code = 0, sy = 0;
  for (int L = 1; L <= h->maxlen; L++) {
    h->first_code[L] = code; h->first_sym[L] = sy;
    offs[L] = sy; sy += h->count[L];
    code = (code + h->count[L]) << 1;
  }
  for (int i = 0; i < n; i++) { int L = lens[i]; if (L) h->syms[offs[L]++] = i; }
  return 0;
}
static int huff_decode(Huff *h, Bits *b) {
  int code = 0;
  for (int L = 1; L <= h->maxlen; L++) {
    code = (code << 1) | (int)bits_read(b, 1);
    int cnt = h->count[L];
    if (cnt && code - h->first_code[L] < cnt)
      return h->syms[h->first_sym[L] + (code - h->first_code[L])];
  }
  return -1;
}

/* read tree code lengths via the pretree delta scheme; bounds every write to [first,last) */
static int read_lengths(uint8_t *lens, int first, int last, Bits *b) {
  uint8_t pre[PRETREE_LEN];
  for (int i = 0; i < PRETREE_LEN; i++) pre[i] = (uint8_t)bits_read(b, 4);
  Huff ph;
  if (huff_build(&ph, pre, PRETREE_LEN) < 0) return -1;
  int i = first;
  while (i < last) {
    int sym = huff_decode(&ph, b);
    if (sym < 0) return -1;
    if (sym == 17) { int x = (int)bits_read(b, 4) + 4; while (x-- && i < last) lens[i++] = 0; }
    else if (sym == 18) { int x = (int)bits_read(b, 5) + 20; while (x-- && i < last) lens[i++] = 0; }
    else if (sym == 19) {
      int x = (int)bits_read(b, 1) + 4;
      int s2 = huff_decode(&ph, b); if (s2 < 0) return -1;
      int v = lens[i] - s2; if (v < 0) v += 17;
      while (x-- && i < last) lens[i++] = (uint8_t)v;
    } else {
      int v = lens[i] - sym; if (v < 0) v += 17;
      lens[i++] = (uint8_t)v;
    }
  }
  return 0;
}

/* reverse the LZX intel-E8 (x86 CALL) translation over the finished output frames */
static void e8_decode(uint8_t *out, size_t outlen, int32_t filesize) {
  size_t p = 0;
  while (p < outlen) {
    size_t flen = (outlen - p >= FRAME) ? FRAME : (outlen - p);
    if (flen > 10) {
      for (size_t i = 0; i + 10 < flen; ) {
        if (out[p + i] != 0xE8) { i++; continue; }
        int32_t cur = (int32_t)(p + i);
        int32_t abs = (int32_t)(out[p + i + 1] | (out[p + i + 2] << 8) |
                                (out[p + i + 3] << 16) | ((uint32_t)out[p + i + 4] << 24));
        if (abs >= -cur && abs < filesize) {
          int32_t rel = (abs >= 0) ? abs - cur : abs + filesize;
          out[p + i + 1] = (uint8_t)rel;
          out[p + i + 2] = (uint8_t)(rel >> 8);
          out[p + i + 3] = (uint8_t)(rel >> 16);
          out[p + i + 4] = (uint8_t)(rel >> 24);
        }
        i += 5;
      }
    }
    p += flen;
  }
}

int cab_lzx21_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len) {
  if (!out && out_len) return -1;
  /* offset tables for window 21 */
  uint8_t extra_bits[52];
  uint32_t position_base[52];
  for (int i = 0, j = 0; i < 51; i += 2) {
    extra_bits[i] = extra_bits[i + 1] = (uint8_t)j;
    if (i != 0 && j < 17) j++;
  }
  extra_bits[51] = 17;
  position_base[0] = 0;
  for (int i = 1; i < 51; i++) position_base[i] = position_base[i - 1] + (1u << extra_bits[i - 1]);
  position_base[51] = position_base[50];

  Bits b; bits_init(&b, in, in_len);
  uint32_t R0 = 1, R1 = 1, R2 = 1;
  uint8_t main_lens[MAINTREE_LEN], len_lens[NUM_SECONDARY_LENGTHS], al_lens[ALIGNED_LEN];
  memset(main_lens, 0, sizeof main_lens);
  memset(len_lens, 0, sizeof len_lens);
  Huff mainh, lenh, alh;

  int intel_started = (int)bits_read(&b, 1);
  int32_t intel_filesize = 0;
  if (intel_started) {
    uint32_t hi = bits_read(&b, 16), lo = bits_read(&b, 16);
    intel_filesize = (int32_t)((hi << 16) | lo);
  }

  size_t opos = 0, next_frame = FRAME;
  int block_type = 0; size_t block_left = 0;

  while (opos < out_len) {
    if (block_left == 0) {
      block_type = (int)bits_read(&b, 3);
      uint32_t bs = bits_read(&b, 16); bs = (bs << 8) | bits_read(&b, 8);
      block_left = bs;
      if (block_left == 0) return -1;
      if (block_type == 1 || block_type == 2) {
        if (block_type == 2) {
          for (int i = 0; i < ALIGNED_LEN; i++) al_lens[i] = (uint8_t)bits_read(&b, 3);
          if (huff_build(&alh, al_lens, ALIGNED_LEN) < 0) return -1;
        }
        if (read_lengths(main_lens, 0, NUM_CHARS, &b) < 0) return -1;
        if (read_lengths(main_lens, NUM_CHARS, MAINTREE_LEN, &b) < 0) return -1;
        if (huff_build(&mainh, main_lens, MAINTREE_LEN) < 0) return -1;
        if (read_lengths(len_lens, 0, NUM_SECONDARY_LENGTHS, &b) < 0) return -1;
        if (huff_build(&lenh, len_lens, NUM_SECONDARY_LENGTHS) < 0) return -1;
      } else if (block_type == 3) {
        if (b.bitcnt & 15) { b.bitbuf <<= (b.bitcnt & 15); b.bitcnt -= (b.bitcnt & 15); }
        /* consume 12 raw bytes for R0,R1,R2 from the aligned input */
        size_t consumed = (size_t)b.bitcnt / 16 * 2; /* bytes still buffered */
        size_t bp = b.pos - consumed;
        if (bp + 12 > in_len) return -1;
        R0 = in[bp] | (in[bp+1]<<8) | (in[bp+2]<<16) | ((uint32_t)in[bp+3]<<24);
        R1 = in[bp+4] | (in[bp+5]<<8) | (in[bp+6]<<16) | ((uint32_t)in[bp+7]<<24);
        R2 = in[bp+8] | (in[bp+9]<<8) | (in[bp+10]<<16) | ((uint32_t)in[bp+11]<<24);
        b.pos = bp + 12; b.bitbuf = 0; b.bitcnt = 0;
      } else return -1;
    }

    if (block_type == 3) {
      if (block_left > out_len - opos) return -1;
      if (b.pos + block_left > in_len) return -1;
      while (block_left > 0) {
        out[opos++] = in[b.pos++]; block_left--;
        if (opos == next_frame && opos < out_len) { frame_align(&b); next_frame += FRAME; }
      }
      if (b.pos & 1) { if (b.pos >= in_len) { if (opos < out_len) return -1; } else b.pos++; }
      continue;
    }

    while (block_left > 0 && opos < out_len) {
      int me = huff_decode(&mainh, &b);
      if (me < 0) return -1;
      if (me < NUM_CHARS) {
        out[opos++] = (uint8_t)me; block_left--;
        if (opos == next_frame && opos < out_len) { frame_align(&b); next_frame += FRAME; }
        continue;
      }
      me -= NUM_CHARS;
      int match_len = me & 7;
      if (match_len == 7) { int lf = huff_decode(&lenh, &b); if (lf < 0) return -1; match_len += lf; }
      match_len += MIN_MATCH;
      int slot = me >> 3;
      uint32_t match_off;
      if (slot > 2) {
        if (slot != 3) {
          if (slot > 50) return -1;
          int extra = extra_bits[slot];
          if (block_type == 2 && extra >= 3) {
            uint32_t vb = (extra > 3) ? bits_read(&b, extra - 3) : 0;
            int ab = huff_decode(&alh, &b); if (ab < 0) return -1;
            match_off = position_base[slot] - 2 + (vb << 3) + (uint32_t)ab;
          } else {
            uint32_t vb = bits_read(&b, extra);
            match_off = position_base[slot] - 2 + vb;
          }
        } else match_off = 1;
        R2 = R1; R1 = R0; R0 = match_off;
      } else if (slot == 0) { match_off = R0; }
      else if (slot == 1) { match_off = R1; R1 = R0; R0 = match_off; }
      else { match_off = R2; R2 = R0; R0 = match_off; }

      if (match_off == 0 || match_off > opos) return -1;
      if ((size_t)match_len > block_left) return -1;
      if ((size_t)match_len > out_len - opos) return -1;
      for (int k = 0; k < match_len; k++) { out[opos] = out[opos - match_off]; opos++; }
      block_left -= match_len;
      if (opos == next_frame && opos < out_len) { frame_align(&b); next_frame += FRAME; }
    }
  }

  if (intel_started && intel_filesize > 0) e8_decode(out, out_len, intel_filesize);
  return 0;
}
