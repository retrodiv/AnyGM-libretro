/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  SOURCE_WIDTH = 6,
  SOURCE_HEIGHT = 5,
  TARGET_WIDTH = 12,
  TARGET_HEIGHT = 10
};

static uint64_t pixel_hash(const uint32_t *pixels,size_t count){
  uint64_t hash=UINT64_C(1469598103934665603);
  for(size_t index=0;index<count;index++){
    for(int shift=24;shift>=0;shift-=8){
      hash^=(pixels[index]>>shift)&0xffu;
      hash*=UINT64_C(1099511628211);
    }
  }
  return hash;
}

static void fill_source(uint32_t *pixels){
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++){
    unsigned red=(unsigned)(x*37+y*19+11)&255u;
    unsigned green=(unsigned)(x*13+y*47+29)&255u;
    unsigned blue=(unsigned)(x*61+y*7+43)&255u;
    pixels[(size_t)y*SOURCE_WIDTH+x]=
      0xff000000u|(red<<16)|(green<<8)|blue;
  }
}


static uint64_t render_recognized_case(GmlRender *render,uint32_t *target,
                                       int interpolation,int blend_mode,double alpha){
  memset(target,0x19,(size_t)TARGET_WIDTH*TARGET_HEIGHT*sizeof(*target));
  render->interp=interpolation;
  render->blendmode=blend_mode;
  render->fb_opaque_known=0;
  gml_render_begin(render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  render->active_shader=0;
  gml_draw_surface_stretched(render,0,0.0,0.0,
                             TARGET_WIDTH,TARGET_HEIGHT,0xffffffu,alpha);
  render->active_shader=-1;
  return pixel_hash(target,(size_t)TARGET_WIDTH*TARGET_HEIGHT);
}

static uint64_t render_dual_case(GmlRender *render,uint32_t *target){
  struct GmlShaderPal *shader=&render->shader_pal[0];
  memset(shader,0,sizeof(*shader));
  shader->dual_sample=1;
  shader->dual_axis=0;
  shader->dual_sign=1;
  shader->dual_base_gain[0]=0.85f;
  shader->dual_base_gain[1]=0.60f;
  shader->dual_base_gain[2]=0.40f;
  shader->dual_base_gain[3]=1.00f;
  shader->dual_shift_gain[0]=0.25f;
  shader->dual_shift_gain[1]=0.35f;
  shader->dual_shift_gain[2]=0.50f;
  shader->dual_value[0]=0.30f;
  shader->dual_value[1]=0.70f;
  return render_recognized_case(render,target,1,3,0.75);
}

int main(void){
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  GmlRender render;
  memset(&render,0,sizeof(render));
  fill_source(source);

  render.app_surface=source;
  render.app_w=SOURCE_WIDTH;
  render.app_h=SOURCE_HEIGHT;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.shader_pal=calloc(1,sizeof(*render.shader_pal));
  if(!render.shader_pal){
    fprintf(stderr,"renderer post-process: shader allocation failed\n");
    return 1;
  }
  render.n_shader_pal=1;

  uint64_t dual=render_dual_case(&render,target);
  int failed=0;
  if(dual!=UINT64_C(0xec8ea81439a0ebe9)){
    fprintf(stderr,"renderer post-process: dual hash=%016llx\n",
            (unsigned long long)dual);
    failed=1;
  }
  gml_render_free(&render);
  if(failed) return 1;
  puts("renderer post-process: ok");
  return 0;
}
