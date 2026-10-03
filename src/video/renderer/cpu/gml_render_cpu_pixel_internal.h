/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer-private final-pixel operations shared by complete software kernels. */
#ifndef GML_RENDER_CPU_PIXEL_INTERNAL_H
#define GML_RENDER_CPU_PIXEL_INTERNAL_H

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

/* Fixed-function destination-dependent pairs: (inverse destination alpha, destination alpha)
 * and (inverse destination colour, zero). The latter uses alpha only for surface coverage. */
static inline uint32_t blend_destination_factor_pixel(
    const GmlRender *render,uint32_t destination,
    int source_red,int source_green,int source_blue,unsigned source_alpha){
  unsigned destination_alpha=destination>>24;
  unsigned inverse=255u-destination_alpha;
  unsigned destination_red=(destination>>16)&255u;
  unsigned destination_green=(destination>>8)&255u;
  unsigned destination_blue=destination&255u;
  if(render->blendmode==8){
    unsigned red=((unsigned)source_red*(255u-destination_red)+127u)/255u;
    unsigned green=((unsigned)source_green*(255u-destination_green)+127u)/255u;
    unsigned blue=((unsigned)source_blue*(255u-destination_blue)+127u)/255u;
    unsigned coverage=gml_render_target_preserves_alpha(render)?
      (source_alpha*inverse+127u)/255u:255u;
    return (coverage<<24)|(red<<16)|(green<<8)|blue;
  }
  unsigned red=((unsigned)source_red*inverse+destination_red*destination_alpha+127u)/255u;
  unsigned green=((unsigned)source_green*inverse+
                  destination_green*destination_alpha+127u)/255u;
  unsigned blue=((unsigned)source_blue*inverse+destination_blue*destination_alpha+127u)/255u;
  unsigned coverage=gml_render_target_preserves_alpha(render)?
    (source_alpha*inverse+destination_alpha*destination_alpha+127u)/255u:255u;
  return (coverage<<24)|(red<<16)|(green<<8)|blue;
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

/* Destination-dependent pairs apply factors to RGBA, even when source coverage is zero.
 * Alpha testing is a separate fragment-admission decision made by the sampling kernel. */
static inline uint32_t blend_destination_colour_pixel(const GmlRender *r,uint32_t destination,
                                                        int sr,int sg,int sb,unsigned sa){
  unsigned source[4]={(unsigned)sr,(unsigned)sg,(unsigned)sb,sa>255u?255u:sa};
  unsigned dest[4]={(destination>>16)&255u,(destination>>8)&255u,
                    destination&255u,destination>>24};
  unsigned out[4];
  for(int channel=0;channel<4;channel++){
    unsigned sf=r->blendmode==GML_RENDER_BLEND_DEST_ALPHA_DEST_COLOUR?dest[3]:dest[channel];
    unsigned df=r->blendmode==GML_RENDER_BLEND_DEST_SOURCE_COLOUR?source[channel]:dest[channel];
    int equation=channel==3?r->blend_equation_alpha:r->blend_equation;
    int value;
    if(equation==2) value=source[channel]>dest[channel]?source[channel]:dest[channel];
    else if(equation==5) value=source[channel]<dest[channel]?source[channel]:dest[channel];
    else {
      int s=(int)(source[channel]*sf),d=(int)(dest[channel]*df);
      int scaled=equation==3?d-s:equation==4?s-d:s+d;
      value=scaled<=0?0:(scaled+127)/255;
    }
    out[channel]=value>255?255u:(unsigned)value;
  }
  if(!gml_render_target_preserves_alpha(r)) out[3]=255u;
  return (out[3]<<24)|(out[0]<<16)|(out[1]<<8)|out[2];
}

static inline int destination_colour_blend_active(const GmlRender *r){
  return r && r->alphablend && gml_render_blend_uses_destination_colour(r->blendmode);
}

/* Basic maximum preset: (source alpha, inverse source colour) with the add equation.  The name
 * describes the preset's visual purpose; unlike the independent maximum equation, both blend
 * factors remain active.  Source RGB has already been texture/tint modulated by the caller. */
static inline uint32_t blend_max_preset_pixel(const GmlRender *r,uint32_t dst,
                                               int sr,int sg,int sb,unsigned source_alpha){
  if(source_alpha>255u) source_alpha=255u;
  unsigned dr=(dst>>16)&255u,dg=(dst>>8)&255u,db=dst&255u;
  unsigned rr=((unsigned)sr*source_alpha+dr*(255u-(unsigned)sr)+127u)/255u;
  unsigned gg=((unsigned)sg*source_alpha+dg*(255u-(unsigned)sg)+127u)/255u;
  unsigned bb=((unsigned)sb*source_alpha+db*(255u-(unsigned)sb)+127u)/255u;
  unsigned coverage=255u;
  if(r && r->target_sp>0){
    unsigned destination_alpha=dst>>24;
    coverage=(source_alpha*source_alpha+
              destination_alpha*(255u-source_alpha)+127u)/255u;
  }
  return (coverage<<24)|(rr<<16)|(gg<<8)|bb;
}


#endif
