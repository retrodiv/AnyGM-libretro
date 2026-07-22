/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_bc17.c — GameMaker bytecode 17 (GMS2) version-specific layout.
 *
 * bc17 shares bc15's instruction decode and CODE-entry layout (dispatched in gml_bytecode.c); the
 * only override is the FUNC occurrence-chain layout. And bc17 is NOT uniform: some builds keep the
 * bc15/16 layout (the FUNC entry `addr` points at the instruction `ia`, with the chain link in the
 * following word — ref/chain offset 4/4), while others point `addr` straight at the reference word
 * `ia+4`, with the chain link in that word itself (offset 0/0). Other bytecode 17 exports retain
 * the 4/4 layout. The parser therefore detects the layout per file by walking the first
 * multi-occurrence FUNC chain both ways and keep whichever stays in bounds. VARI is always 4/4.
 * The bounded probe selects the layout whose occurrence chain remains valid. */
#include <string.h>
#include "gml_bytecode.h"

static uint32_t bc17_u32(const GmlWin *w, uint32_t o){
  if(!w || (size_t)o+4 > w->size) return 0;
  const uint8_t *d=w->data;
  return (uint32_t)d[o]|((uint32_t)d[o+1]<<8)|((uint32_t)d[o+2]<<16)|((uint32_t)d[o+3]<<24);
}

/* how many chain nodes stay in-bounds reading the next-link from u32(node + chain_off). */
static int bc17_walk_steps(const GmlWin *w, uint32_t addr, uint32_t occ, uint32_t chain_off){
  uint32_t a=addr; int steps=0;
  for(uint32_t k=0; k<occ && k<12; k++){
    if(a<4 || chain_off>UINT32_MAX-a ||
       (size_t)a+chain_off>w->size || 4u>w->size-((size_t)a+chain_off)) break;
    steps++;
    uint32_t nxt = bc17_u32(w, a+chain_off) & 0x07FFFFFF;
    if(nxt==0) break;
    if(nxt>UINT32_MAX-a) break;
    a += nxt;
  }
  return steps;
}

int gml_bc17_ref_layout(const GmlWin *w, const char *chunk, GmlRefLayout *out){
  if(!gml_bc15_ref_layout(w,chunk,out)) return 0;   /* defaults ref_off=chain_off=4 (bc15 layout) */
  if(chunk && !strcmp(chunk,"FUNC")){
    for(uint32_t i=0;i<out->count;i++){
      uint32_t e=out->start + i*out->stride;
      uint32_t occ=bc17_u32(w,e+out->occ_off), addr=bc17_u32(w,e+out->addr_off);
      if(occ<4 || addr==0) continue;
      int s4=bc17_walk_steps(w,addr,occ,4);   /* bc15/16-style: chain in the word after the entry addr */
      int s0=bc17_walk_steps(w,addr,occ,0);   /* newer bc17: entry addr IS the reference word */
      if(s0>s4){ out->ref_off=0; out->chain_off=0; }   /* else keep the bc15 default (4/4) */
      break;   /* stop after the first eligible chain */
    }
  }
  return 1;
}
