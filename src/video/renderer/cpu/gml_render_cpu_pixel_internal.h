/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer-private final-pixel operations shared by complete software kernels. */
#ifndef GML_RENDER_CPU_PIXEL_INTERNAL_H
#define GML_RENDER_CPU_PIXEL_INTERNAL_H

#include "gml_render_internal.h"


static inline uint32_t gml_render_target_source_over_alpha(const GmlRender *r,uint32_t dst,
                                               unsigned source_alpha){
  if(!gml_render_target_preserves_alpha(r)) return 0xFF000000u;
  if(source_alpha>255u) source_alpha=255u;
  unsigned destination_alpha=dst>>24;
  unsigned inverse=255u-source_alpha;
  unsigned output=(source_alpha*source_alpha+destination_alpha*inverse+127u)/255u;
  if(output>255u) output=255u;
  return output<<24;
}
/* Retain each rounded fixed-function contribution before adding the channels,
 * as the existing filtered blit kernels do. The caller supplies sampled RGB. */
static inline uint32_t surface_source_over_pixel(const GmlRender *render,uint32_t dst,
                                                 double sr,double sg,double sb,double alpha){
  int dr=(dst>>16)&255,dg=(dst>>8)&255,db=dst&255;
  double inverse=1.0-alpha;
  int rr=(int)(sr*alpha+0.5)+(int)(dr*inverse+0.5);
  int rg=(int)(sg*alpha+0.5)+(int)(dg*inverse+0.5);
  int rb=(int)(sb*alpha+0.5)+(int)(db*inverse+0.5);
  if(rr<0) rr=0; else if(rr>255) rr=255;
  if(rg<0) rg=0; else if(rg>255) rg=255;
  if(rb<0) rb=0; else if(rb>255) rb=255;
  unsigned source_alpha=(unsigned)(alpha*255.0+0.5);
  return gml_render_target_source_over_alpha(render,dst,source_alpha)|
    ((uint32_t)rr<<16)|((uint32_t)rg<<8)|(uint32_t)rb;
}

static inline uint32_t color_write_bits(unsigned m){
  uint32_t bits=0;
  if(m&1u) bits|=0x00FF0000u;
  if(m&2u) bits|=0x0000FF00u;
  if(m&4u) bits|=0x000000FFu;
  if(m&8u) bits|=0xFF000000u;
  return bits;
}
static inline uint32_t color_write_merge(const GmlRender *r,uint32_t old,uint32_t value){
  uint32_t bits=color_write_bits(r?r->color_write_mask:0x0F);
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
static inline uint32_t blend_multiply_channels(uint32_t dst,int sr,int sg,int sb,int opaque){
  int dr=(dst>>16)&255,dg=(dst>>8)&255,db=dst&255;
  int rr=(sr*dr+127)/255,gg=(sg*dg+127)/255,bb=(sb*db+127)/255;
  uint32_t alpha=opaque?0xFF000000u:(dst&0xFF000000u);
  return alpha|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
}

static inline uint32_t blend_multiply_pixel(const GmlRender *r,uint32_t dst,int sr,int sg,int sb){
  return blend_multiply_channels(dst,sr,sg,sb,r && r->target_sp==0);
}

/* Colour-factor pairs apply factors to RGBA, even when source coverage is zero.
 * Alpha testing is a separate fragment-admission decision made by the sampling kernel. */
static inline unsigned blend_extended_factor(int factor,unsigned channel,
                                             const unsigned source[4],const unsigned dest[4]){
  switch(factor){
    case 1:return 0u;
    case 2:return 255u;
    case 3:return source[channel];
    case 4:return 255u-source[channel];
    case 5:return source[3];
    case 6:return 255u-source[3];
    case 7:return dest[3];
    case 8:return 255u-dest[3];
    case 9:return dest[channel];
    case 10:return 255u-dest[channel];
    case 11:return channel==3u?255u:(source[3]<255u-dest[3]?source[3]:255u-dest[3]);
    default:return 0u;
  }
}

static inline uint32_t blend_destination_colour_pixel(const GmlRender *r,uint32_t destination,
                                                        int sr,int sg,int sb,unsigned sa){
  unsigned source[4]={(unsigned)sr,(unsigned)sg,(unsigned)sb,sa>255u?255u:sa};
  unsigned dest[4]={(destination>>16)&255u,(destination>>8)&255u,
                    destination&255u,destination>>24};
  unsigned out[4];
  int extended=gml_render_blend_is_extended(r->blendmode),factors[4]={0};
  if(extended)gml_render_blend_mode_factors(r->blendmode,factors);
  int source_colour_alpha=r->blendmode==GML_RENDER_BLEND_SOURCE_COLOUR_SOURCE_ALPHA;
  for(int channel=0;channel<4;channel++){
    unsigned sf=r->blendmode==GML_RENDER_BLEND_DEST_ALPHA_DEST_COLOUR?dest[3]:dest[channel];
    unsigned df=r->blendmode==GML_RENDER_BLEND_DEST_SOURCE_COLOUR?source[channel]:dest[channel];
    if(source_colour_alpha){sf=source[channel];df=source[3];}
    if(extended){
      sf=blend_extended_factor(factors[channel==3?2:0],(unsigned)channel,source,dest);
      df=blend_extended_factor(factors[channel==3?3:1],(unsigned)channel,source,dest);
    }
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
