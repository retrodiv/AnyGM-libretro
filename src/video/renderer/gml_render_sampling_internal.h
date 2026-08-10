/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SAMPLING_INTERNAL_H
#define GML_RENDER_SAMPLING_INTERNAL_H

#include "gml_render_internal.h"

#include <math.h>
#include <stdint.h>


const uint8_t *runtime_frame_rgba(GmlSprite *s, int frame);   /* fwd (defined below) */
/* palette-LUT shader active and fully configured (row uniform set + palette texture staged) */
static inline const struct GmlShaderPal *lut_active(GmlRender *r){
  if(r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  const struct GmlShaderPal *sp=&r->shader_pal[r->active_shader];
  return (sp->lut && sp->lut_row>=0 && r->lut_pal_sprite>=0) ? sp : NULL;
}
static inline const struct GmlShaderPal *grid_active(GmlRender *r){
  if(r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  const struct GmlShaderPal *sp=&r->shader_pal[r->active_shader];
  return (sp->grid && sp->grid_id>=0.0f && r->lut_pal_sprite>=0) ? sp : NULL;
}
/* CRT-geom post-process active and configured (recognized fragment + its size uniform has been set). */


static inline const struct GmlShaderPal *dual_active(GmlRender *r){
  if(r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  const struct GmlShaderPal *sp=&r->shader_pal[r->active_shader];
  return sp->dual_sample ? sp : NULL;
}


static inline const struct GmlShaderPal *radial_wave_active(GmlRender *r){
  if(r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  const struct GmlShaderPal *sp=&r->shader_pal[r->active_shader];
  return sp->radial_wave && sp->radial_wave_value[2][0]>0.0f &&
         sp->radial_wave_value[2][1]>0.0f && fabsf(sp->radial_wave_value[4][0])>1e-8f
       ? sp : NULL;
}
static inline int radial_wave_sample_index(const struct GmlShaderPal *sp,int width,int height,
                                           int source_x,int source_y){
  float resolution_x=sp->radial_wave_value[2][0];
  float resolution_y=sp->radial_wave_value[2][1];
  float u=((float)source_x+0.5f)/(float)width;
  float v=((float)source_y+0.5f)/(float)height;
  float aspect=resolution_x/resolution_y;
  float centre_x=(sp->radial_wave_value[1][0]/resolution_x)*aspect;
  float centre_y=sp->radial_wave_value[1][1]/resolution_y;
  float dx=u-0.5f,dy=v-0.5f;
  float distance=hypotf(u*aspect-centre_x,v-centre_y);
  float phase=sinf(distance*sp->radial_wave_value[3][0]-
                   sp->radial_wave_value[0][0]*sp->radial_wave_value[5][0]) /
              sp->radial_wave_value[4][0];
  int x=(int)floorf((u+dx*phase)*width);
  int y=(int)floorf((v+dy*phase)*height);
  if(x<0)x=0;else if(x>=width)x=width-1;
  if(y<0)y=0;else if(y>=height)y=height-1;
  return y*width+x;
}
static inline const struct GmlShaderPal *uv_wave_active(GmlRender *r){
  if(r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  const struct GmlShaderPal *sp=&r->shader_pal[r->active_shader];
  return sp->uv_wave_mode ? sp : NULL;
}
static inline int uv_wave_sample_index(const struct GmlShaderPal *sp,int width,int height,
                                       int source_x,int source_y,double position_y){
  float u=((float)source_x+0.5f)/(float)width;
  float v=((float)source_y+0.5f)/(float)height;
  float offset=0.0f;
  if(sp->uv_wave_mode==1){
    offset=cosf(v*sp->uv_wave_uv_factor+sp->uv_wave_time*sp->uv_wave_time_factor) /
           sp->uv_wave_divisor;
  } else if(sp->uv_wave_mode==2){
    offset=sinf(((float)position_y/sp->uv_wave_spatial+sp->uv_wave_time)*
                sp->uv_wave_time_factor) *
           (sp->uv_wave_amplitude*v) / sp->uv_wave_size_x * (sp->uv_wave_taper-u);
  }
  int x=(int)floorf((u+offset)*width);
  if(x<0)x=0;else if(x>=width)x=width-1;
  return source_y*width+x;
}
static inline const struct GmlShaderPal *paint_active(GmlRender *r){
  if(r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  const struct GmlShaderPal *sp=&r->shader_pal[r->active_shader];
  return (sp->paint && sp->paint_resolution[0]>0.0f && sp->paint_resolution[1]>0.0f) ? sp : NULL;
}
static inline const struct GmlShaderPal *grayscale_active(GmlRender *r){
  if(r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  const struct GmlShaderPal *sp=&r->shader_pal[r->active_shader];
  return sp->grayscale ? sp : NULL;
}
static inline const struct GmlShaderPal *solid_alpha_mask_active(GmlRender *r){
  if(r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  const struct GmlShaderPal *sp=&r->shader_pal[r->active_shader];
  return sp->solid_alpha_mask ? sp : NULL;
}
static inline const struct GmlShaderPal *solid_blur_alpha_active(GmlRender *r){
  if(r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  const struct GmlShaderPal *sp=&r->shader_pal[r->active_shader];
  return sp->solid_blur_alpha ? sp : NULL;
}
static inline const struct GmlShaderPal *shader_active(GmlRender *r){
  if(!r || r->active_shader<0 || r->active_shader>=r->n_shader_pal || !r->shader_pal) return NULL;
  return &r->shader_pal[r->active_shader];
}
static inline int shader_discards_alpha_value(GmlRender *r, double alpha){
  const struct GmlShaderPal *sp=shader_active(r);
  if(sp && sp->alpha_discard){
    double normalized=alpha*(1.0/255.0);
    if(sp->alpha_discard_inclusive ? normalized<=sp->alpha_discard_cutoff
                                   : normalized< sp->alpha_discard_cutoff) return 1;
  }
  return r && r->alpha_test_enable && alpha<=r->alpha_test_ref;
}
static inline int shader_discards_alpha(GmlRender *r, unsigned alpha){
  return shader_discards_alpha_value(r,(double)alpha);
}
/* Evaluate the ordered-dither cutout for one destination pixel. The cell order is the fragment's
 * own arithmetic rather than a copied table: for each of the sixteen steps the column is
 * floor(i/2)*2 + floor((i-1)/4) - floor((i-1)/8) and the row is floor((i-1)/4), plus two when i is
 * even, both wrapped to four. The position is wrapped by the same floor identity the fragment uses,
 * so a negative world coordinate lands on the cell the shader would pick. */
static inline int shader_ordered_dither_active(GmlRender *r){
  const struct GmlShaderPal *sp=shader_active(r);
  return sp && sp->ordered_dither;
}
static inline int shader_ordered_dither_drops(GmlRender *r, double position_x, double position_y){
  const struct GmlShaderPal *sp=shader_active(r);
  if(!sp || !sp->ordered_dither) return 0;
  int level=(int)(sp->ordered_dither_alpha*16.0f);   /* GLSL int() truncates toward zero */
  if(level<=0) return 1;
  if(level>16) level=16;
  int cell_x=(int)(position_x-floor(position_x/4.0)*4.0);
  int cell_y=(int)(position_y-floor(position_y/4.0)*4.0);
  for(int step=1;step<=level;step++){
    int column=((step/2)*2 + (step-1)/4 - (step-1)/8) & 3;
    int row=((step&1) ? (step-1)/4 : 2+(step-1)/4) & 3;
    if(column==cell_x && row==cell_y) return 0;
  }
  return 1;
}
static inline int shader_alpha_test_active(GmlRender *r){
  const struct GmlShaderPal *sp=shader_active(r);
  return (sp && (sp->alpha_discard || sp->ordered_dither)) || (r && r->alpha_test_enable);
}
static inline int shader_alpha_test_requires_filter(GmlRender *r){
  const struct GmlShaderPal *sp=shader_active(r);
  /* A dithered pass covers at most every pixel and usually far fewer, so the caches that assume a
   * textured draw is opaque must not be taken. */
  if(sp && (sp->alpha_discard || sp->ordered_dither)) return 1;
  /* Ordinary textured kernels already skip zero-coverage texels. Fixed-function alpha testing at
   * reference zero therefore changes no output and must not disable sparse and opaque caches. */
  return r && r->alpha_test_enable && r->alpha_test_ref>0;
}
static inline uint32_t sprite_pixel_argb(GmlRender *r, int sprite, int frame, int lx, int ly, uint32_t fallback){
  if(sprite<0||sprite>=r->n_spr) return fallback;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return fallback;
  if(lx<0||ly<0||lx>=s->w||ly>=s->h) return fallback;
  const uint8_t *px=NULL;
  if(s->runtime_rgba){
    const uint8_t *fr=runtime_frame_rgba(s,frame); if(!fr) return fallback;
    px=fr+((size_t)ly*s->w+lx)*4;
  } else {
    int sub=((frame%s->n_frames)+s->n_frames)%s->n_frames;
    int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return fallback;
    GmlTpag *t=&r->tpag[ti];
    int ix=lx-t->tx, iy=ly-t->ty;
    if(ix<0||iy<0||ix>=t->sw||iy>=t->sh) return fallback;
    if(t->atlas<0||t->atlas>=r->n_atlas) return fallback;
    GmlAtlas *a=&r->atlas[t->atlas]; if(!atlas_pixels(r,t->atlas)) return fallback;
    int ax=t->sx+ix, ay=t->sy+iy;
    if(ax<0||ay<0||ax>=a->w||ay>=a->h) return fallback;
    px=a->px+((size_t)ay*a->w+ax)*4;
  }
  return ((uint32_t)px[3]<<24)|((uint32_t)px[0]<<16)|((uint32_t)px[1]<<8)|px[2];
}
static inline uint32_t sprite_pixel_rgb(GmlRender *r, int sprite, int frame, int lx, int ly, uint32_t fallback){
  uint32_t v=sprite_pixel_argb(r,sprite,frame,lx,ly,fallback);
  return 0xFF000000u|(v&0x00FFFFFFu);
}
static inline uint32_t lut_map_px(GmlRender *r, const struct GmlShaderPal *sp, uint32_t v){
  GmlSprite *s=&r->spr[r->lut_pal_sprite];
  int w=s->w>0?s->w:1, h=s->h>0?s->h:1;
  if(sp->lut_indexed){
    int sr=(v>>16)&255, sg=(v>>8)&255, sb=v&255;
    uint32_t mapped=v;
    if(sr==sg && sg==sb && sp->lut_offset>0.0f && sp->lut_colors>0.0f){
      float pu=sp->lut_row;
      float pv=((255.0f*((float)sr/255.0f))/sp->lut_offset+0.5f)/sp->lut_colors;
      int x=(int)floorf(pu*w), y=(int)floorf(pv*h);
      if(x<0) x=0; else if(x>=w) x=w-1;
      if(y<0) y=0; else if(y>=h) y=h-1;
      mapped=sprite_pixel_argb(r,r->lut_pal_sprite,r->lut_pal_frame,x,y,v);
    }
    int mr=(mapped>>16)&255, mg=(mapped>>8)&255, mb=mapped&255;
    /* Tint variants colorize the fragment after either branch: non-indexed source colours must
     * therefore be affected too, rather than escaping through the palette-test fallback. */
    if(sp->lut_has_colorise){
      float amount=sp->lut_colorise[3]; if(amount<0.0f) amount=0.0f; else if(amount>1.0f) amount=1.0f;
      mr=(int)floorf(mr+(sp->lut_colorise[0]*255.0f-mr)*amount+0.5f);
      mg=(int)floorf(mg+(sp->lut_colorise[1]*255.0f-mg)*amount+0.5f);
      mb=(int)floorf(mb+(sp->lut_colorise[2]*255.0f-mb)*amount+0.5f);
      if(mr<0)mr=0;else if(mr>255)mr=255; if(mg<0)mg=0;else if(mg>255)mg=255;
      if(mb<0)mb=0;else if(mb>255)mb=255;
    }
    return (v&0xFF000000u)|((uint32_t)mr<<16)|((uint32_t)mg<<8)|(uint32_t)mb;
  }
  int u=(int)((v>>16)&0xFF);                       /* source red channel selects the column */
  int x=u*w/256; if(x>w-1) x=w-1;
  int y=(int)(sp->lut_row*h); if(y>h-1) y=h-1; if(y<0) y=0;
  return sprite_pixel_rgb(r,r->lut_pal_sprite,r->lut_pal_frame,x,y,v);
}
static inline uint32_t grid_map_px(GmlRender *r, const struct GmlShaderPal *sp, uint32_t v){
  if(!r || !sp || r->lut_pal_sprite<0 || r->lut_pal_sprite>=r->n_spr) return v;
  GmlSprite *s=&r->spr[r->lut_pal_sprite];
  int w=s->w, h=s->h; if(w<=0 || h<=0) return v;
  int va=(v>>24)&255, vr=(v>>16)&255, vg=(v>>8)&255, vb=v&255;
  for(int y=0;y<h;y++){
    uint32_t base=sprite_pixel_argb(r,r->lut_pal_sprite,r->lut_pal_frame,0,y,0);
    int da=((int)(base>>24)&255)-va, dr=((int)(base>>16)&255)-vr;
    int dg=((int)(base>>8)&255)-vg, db=((int)base&255)-vb;
    if(da*da+dr*dr+dg*dg+db*db>1) continue; /* distance < 0.004 in normalized RGBA */
    float id=sp->grid_id; if(id<0) return v;
    int x0=(int)floorf(id), x1=x0+1; float f=id-floorf(id);
    if(x0<0)x0=0; else if(x0>=w)x0=w-1;
    if(x1<0)x1=0; else if(x1>=w)x1=w-1;
    uint32_t c0=sprite_pixel_argb(r,r->lut_pal_sprite,r->lut_pal_frame,x0,y,v);
    uint32_t c1=sprite_pixel_argb(r,r->lut_pal_sprite,r->lut_pal_frame,x1,y,c0);
    unsigned oa=(unsigned)(((c0>>24)&255)*(1.0f-f)+((c1>>24)&255)*f+0.5f);
    unsigned orr=(unsigned)(((c0>>16)&255)*(1.0f-f)+((c1>>16)&255)*f+0.5f);
    unsigned og=(unsigned)(((c0>>8)&255)*(1.0f-f)+((c1>>8)&255)*f+0.5f);
    unsigned ob=(unsigned)((c0&255)*(1.0f-f)+(c1&255)*f+0.5f);
    return (oa<<24)|(orr<<16)|(og<<8)|ob;
  }
  return v;
}
/* Palette-grid shaders target indexed art, so a frame normally repeats a small set of colours.
 * Their literal fragment program scans the palette texture rows for every pixel. Memoizing exact
 * inputs for the duration of one draw preserves that program byte-for-byte while avoiding millions
 * of redundant atlas samples; a collision merely recomputes the same value. */
#define GML_GRID_PIXEL_CACHE_SIZE 256
typedef struct {
  uint32_t key[GML_GRID_PIXEL_CACHE_SIZE];
  uint32_t value[GML_GRID_PIXEL_CACHE_SIZE];
  uint8_t used[GML_GRID_PIXEL_CACHE_SIZE];
} GmlGridPixelCache;
static inline uint32_t grid_map_px_cached(GmlRender *r, const struct GmlShaderPal *sp,
                                          uint32_t v, GmlGridPixelCache *cache){
  uint32_t mixed=v*2654435761u;
  unsigned slot=(unsigned)(mixed>>24);
  if(cache->used[slot] && cache->key[slot]==v) return cache->value[slot];
  uint32_t mapped=grid_map_px(r,sp,v);
  cache->used[slot]=1;
  cache->key[slot]=v;
  cache->value[slot]=mapped;
  return mapped;
}
static inline int mapped_texture_active(GmlRender *r){
  return pal_active(r)!=NULL || lut_active(r)!=NULL || grid_active(r)!=NULL ||
         grayscale_active(r)!=NULL || solid_alpha_mask_active(r)!=NULL;
}
static inline uint32_t mapped_texture_pixel(GmlRender *r, uint32_t value){
  const struct GmlShaderPal *shader;
  if((shader=solid_alpha_mask_active(r))){
    int alpha=(value>>24)&255;
    if(shader->solid_alpha_mask_inclusive
         ? alpha<=shader->solid_alpha_mask_cutoff_step
         : alpha< shader->solid_alpha_mask_cutoff_step) alpha=0;
    return ((uint32_t)alpha<<24)|shader->solid_alpha_mask_rgb;
  }
  if((shader=lut_active(r))) return lut_map_px(r,shader,value);
  if((shader=grid_active(r))) return grid_map_px(r,shader,value);
  if((shader=grayscale_active(r))){
    float luminance=((value>>16)&255)*shader->grayscale_weight[0]+
                    ((value>>8)&255)*shader->grayscale_weight[1]+(value&255)*shader->grayscale_weight[2];
    int gray=(int)floorf(luminance+0.5f); if(gray<0) gray=0; else if(gray>255) gray=255;
    int alpha=(int)floorf(((value>>24)&255)*shader->grayscale_alpha+0.5f);
    if(alpha<0) alpha=0; else if(alpha>255) alpha=255;
    return ((uint32_t)alpha<<24)|((uint32_t)gray<<16)|((uint32_t)gray<<8)|(uint32_t)gray;
  }
  return value;
}


#endif
