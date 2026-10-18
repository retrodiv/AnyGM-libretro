/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_PALETTE_INTERNAL_H
#define GML_RENDER_PALETTE_INTERNAL_H

#include "gml_render_internal.h"

/* One existing evaluator serves CPU pixels and finite device-palette preparation.
 * Preparing ten colours does not observe source or destination image bytes. */
static inline uint32_t indexed_brightness_map_px(
    const struct GmlShaderPal *sp,uint32_t value){
  float red=(float)((value>>16)&255)*(1.0f/255.0f);
  float green=(float)((value>>8)&255)*(1.0f/255.0f);
  int branch=red<sp->indexed_brightness_red?0:1;
  int band=4;
  for(int threshold=0;threshold<4;threshold++)
    if(green>sp->indexed_brightness_green[branch][threshold]){ band=threshold; break; }
  float colour_id=(float)(branch?9-band:4-band);
  const uint8_t *colour=NULL;
  if(colour_id<sp->indexed_brightness_family_cut){
    if(sp->indexed_brightness_value<sp->indexed_brightness_negative_cut &&
       colour_id>sp->indexed_brightness_special_min &&
       colour_id<sp->indexed_brightness_special_max){
      colour_id+=sp->indexed_brightness_value;
      int index=colour_id>sp->indexed_brightness_special_threshold[0]?0:
                colour_id>sp->indexed_brightness_special_threshold[1]?1:2;
      colour=sp->indexed_brightness_special_colour[index];
    } else {
      colour_id+=sp->indexed_brightness_value;
      int index=6;
      for(int threshold=0;threshold<6;threshold++)
        if(colour_id<sp->indexed_brightness_low_threshold[threshold]){
          index=threshold; break;
        }
      colour=sp->indexed_brightness_low_colour[index];
    }
  } else {
    colour_id+=sp->indexed_brightness_value;
    int index=7;
    for(int threshold=0;threshold<7;threshold++)
      if(colour_id<sp->indexed_brightness_high_threshold[threshold]){
        index=threshold; break;
      }
    colour=sp->indexed_brightness_high_colour[index];
  }
  return (value&0xff000000u)|((uint32_t)colour[0]<<16)|
         ((uint32_t)colour[1]<<8)|(uint32_t)colour[2];
}

#endif
