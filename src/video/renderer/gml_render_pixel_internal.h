/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer-private final-pixel operations shared by complete software kernels. */
#ifndef GML_RENDER_PIXEL_INTERNAL_H
#define GML_RENDER_PIXEL_INTERNAL_H

#include "gml_render_internal.h"


static inline uint32_t color_write_merge(const GmlRender *r, uint32_t old, uint32_t value){
  unsigned m=r?r->color_write_mask:0x0F;
  uint32_t bits=0;
  if(m&1u) bits|=0x00FF0000u; /* red */
  if(m&2u) bits|=0x0000FF00u; /* green */
  if(m&4u) bits|=0x000000FFu; /* blue */
  if(m&8u) bits|=0xFF000000u; /* alpha */
  return (old&~bits)|(value&bits);
}

/* Fixed-function blend factors (bm_dest_colour, bm_zero): source * destination. The source RGB
 * has already been texture/tint modulated by the caller. Preserve framebuffer coverage here;
 * GameMaker does not expose the main framebuffer alpha, while surface alpha remains conservative. */
static inline uint32_t blend_multiply_pixel(const GmlRender *r,uint32_t dst,int sr,int sg,int sb){
  int dr=(dst>>16)&255,dg=(dst>>8)&255,db=dst&255;
  int rr=(sr*dr+127)/255,gg=(sg*dg+127)/255,bb=(sb*db+127)/255;
  uint32_t alpha=(r&&r->target_sp==0)?0xFF000000u:(dst&0xFF000000u);
  return alpha|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
}


#endif
