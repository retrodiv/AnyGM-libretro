/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Non-CRT room-layer effects and their complete software pixel kernels. */
#include "gml_render.h"
#include "gml_render_internal.h"

#include "anygm_host.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int effect_wrap_coord(int value, int size){
  if(size<=0) return 0;
  value%=size;
  return value<0?value+size:value;
}
static uint8_t effect_unorm8(float value){
  if(value<=0.0f) return 0;
  if(value>=255.0f) return 255;
  return (uint8_t)floorf(value+0.5f);
}
void gml_render_layer_tint(GmlRender *r, uint32_t rgba){
  if(!r || !r->fb || r->fbw<=0 || r->fbh<=0 || rgba==0xFFFFFFFFu) return;
  gml_render_prepare_draw(r);
  uint32_t ta=(rgba>>24)&255, tr=(rgba>>16)&255, tg=(rgba>>8)&255, tb=rgba&255;
  size_t count=(size_t)r->fbw*(size_t)r->fbh;
  for(size_t i=0;i<count;i++){
    uint32_t p=r->fb[i];
    uint32_t a=((p>>24)&255)*ta/255u;
    uint32_t rr=((p>>16)&255)*tr/255u;
    uint32_t rg=((p>>8)&255)*tg/255u;
    uint32_t rb=(p&255)*tb/255u;
    r->fb[i]=(a<<24)|(rr<<16)|(rg<<8)|rb;
  }
  if(ta<255u){ r->fb_opaque_known=0; r->fb_all_opaque=0; }
  r->fb_all_transparent=0;
}

static inline float layer_clamp01(float v){ return v<0.0f?0.0f:(v>1.0f?1.0f:v); }
static inline float layer_fract(float v){ return v-floorf(v); }
static inline float layer_mix(float a,float b,float t){ return a+(b-a)*t; }
static inline float layer_smoothstep(float a,float b,float x){
  if(a==b) return x<a?0.0f:1.0f;
  float t=layer_clamp01((x-a)/(b-a)); return t*t*(3.0f-2.0f*t);
}
static inline uint32_t layer_pack(const float c[4]){
  uint32_t a=effect_unorm8(layer_clamp01(c[3])*255.0f);
  uint32_t rr=effect_unorm8(layer_clamp01(c[0])*255.0f);
  uint32_t gg=effect_unorm8(layer_clamp01(c[1])*255.0f);
  uint32_t bb=effect_unorm8(layer_clamp01(c[2])*255.0f);
  return (a<<24)|(rr<<16)|(gg<<8)|bb;
}
static inline void layer_unpack(uint32_t p,float c[4]){
  c[0]=((p>>16)&255)*(1.0f/255.0f); c[1]=((p>>8)&255)*(1.0f/255.0f);
  c[2]=(p&255)*(1.0f/255.0f); c[3]=(p>>24)*(1.0f/255.0f);
}
static inline void layer_unpack_colour(uint32_t p,float c[4]){ layer_unpack(p,c); }

static int layer_filter_reserve(GmlRender *r,size_t count){
  if(!r || count==0 || count>67108864u) return 0;
  if(count<=r->layer_filter_capacity && r->layer_filter_src &&
     r->layer_filter_work && r->layer_filter_aux) return 1;
  uint32_t *src=malloc(count*sizeof(*src));
  uint32_t *work=malloc(count*sizeof(*work));
  uint32_t *aux=malloc(count*sizeof(*aux));
  if(!src || !work || !aux){ free(src); free(work); free(aux); return 0; }
  free(r->layer_filter_src); free(r->layer_filter_work); free(r->layer_filter_aux);
  r->layer_filter_src=src; r->layer_filter_work=work; r->layer_filter_aux=aux;
  r->layer_filter_capacity=count;
  return 1;
}

static void layer_sprite_texel(GmlRender *r,int sprite,int x,int y,int repeat,float out[4]){
  out[0]=out[1]=out[2]=out[3]=0.0f;
  if(!r || sprite<0 || sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite]; int w=s->w,h=s->h;
  if(w<=0 || h<=0 || s->n_frames<=0) return;
  if(repeat){ x=effect_wrap_coord(x,w); y=effect_wrap_coord(y,h); }
  else { if(x<0)x=0; else if(x>=w)x=w-1; if(y<0)y=0; else if(y>=h)y=h-1; }
  const uint8_t *p=NULL;
  if(s->runtime_rgba) p=s->runtime_rgba+((size_t)y*w+x)*4u;
  else if(s->frame){
    int ti=s->frame[0];
    if(ti>=0 && ti<r->n_tpag){
      GmlTpag *t=&r->tpag[ti];
      int lx=x-t->tx,ly=y-t->ty;
      if(lx>=0 && ly>=0 && lx<t->sw && ly<t->sh && t->atlas>=0 && t->atlas<r->n_atlas){
        uint8_t *ap=atlas_pixels(r,t->atlas); GmlAtlas *a=&r->atlas[t->atlas];
        int ax=t->sx+lx,ay=t->sy+ly;
        if(ap && ax>=0 && ay>=0 && ax<a->w && ay<a->h) p=ap+((size_t)ay*a->w+ax)*4u;
      }
    }
  }
  if(p){ out[0]=p[0]*(1.0f/255.0f); out[1]=p[1]*(1.0f/255.0f);
         out[2]=p[2]*(1.0f/255.0f); out[3]=p[3]*(1.0f/255.0f); }
}
static void layer_sprite_sample(GmlRender *r,int sprite,float u,float v,int repeat,int linear,float out[4]){
  if(!r || sprite<0 || sprite>=r->n_spr || r->spr[sprite].w<=0 || r->spr[sprite].h<=0){
    out[0]=out[1]=out[2]=out[3]=0.0f; return;
  }
  int w=r->spr[sprite].w,h=r->spr[sprite].h;
  if(repeat){ u=layer_fract(u); v=layer_fract(v); }
  else { u=layer_clamp01(u); v=layer_clamp01(v); }
  if(!linear){ layer_sprite_texel(r,sprite,(int)floorf(u*w),(int)floorf(v*h),repeat,out); return; }
  float fx=u*w-0.5f,fy=v*h-0.5f; int x0=(int)floorf(fx),y0=(int)floorf(fy);
  float tx=fx-x0,ty=fy-y0,c[4][4];
  layer_sprite_texel(r,sprite,x0,y0,repeat,c[0]);
  layer_sprite_texel(r,sprite,x0+1,y0,repeat,c[1]);
  layer_sprite_texel(r,sprite,x0,y0+1,repeat,c[2]);
  layer_sprite_texel(r,sprite,x0+1,y0+1,repeat,c[3]);
  for(int k=0;k<4;k++) out[k]=layer_mix(layer_mix(c[0][k],c[1][k],tx),
                                        layer_mix(c[2][k],c[3][k],tx),ty);
}
static void layer_surface_sample(const uint32_t *src,int w,int h,float u,float v,int linear,float out[4]){
  if(!src || w<=0 || h<=0){ out[0]=out[1]=out[2]=out[3]=0; return; }
  u=layer_clamp01(u); v=layer_clamp01(v);
  if(!linear){
    int x=(int)floorf(u*w),y=(int)floorf(v*h);
    if(x>=w) x=w-1;
    if(y>=h) y=h-1;
    layer_unpack(src[(size_t)y*w+x],out); return;
  }
  float fx=u*w-0.5f,fy=v*h-0.5f; int x0=(int)floorf(fx),y0=(int)floorf(fy);
  float tx=fx-x0,ty=fy-y0; int x1=x0+1,y1=y0+1;
  if(x0<0)x0=0; else if(x0>=w)x0=w-1; if(x1<0)x1=0; else if(x1>=w)x1=w-1;
  if(y0<0)y0=0; else if(y0>=h)y0=h-1; if(y1<0)y1=0; else if(y1>=h)y1=h-1;
  float c[4][4]; layer_unpack(src[(size_t)y0*w+x0],c[0]); layer_unpack(src[(size_t)y0*w+x1],c[1]);
  layer_unpack(src[(size_t)y1*w+x0],c[2]); layer_unpack(src[(size_t)y1*w+x1],c[3]);
  for(int k=0;k<4;k++) out[k]=layer_mix(layer_mix(c[0][k],c[1][k],tx),
                                        layer_mix(c[2][k],c[3][k],tx),ty);
}

static void layer_composite_normal(GmlRender *r,const uint32_t *src,int w,int h,double alpha){
  if(!r || !r->fb || !src || w!=r->fbw || h!=r->fbh || alpha<=0.0) return;
  if(alpha>1.0) alpha=1.0;
  size_t count=(size_t)w*h;
  for(size_t i=0;i<count;i++){
    uint32_t sv=src[i]; float sa=(float)((sv>>24)&255)*(1.0f/255.0f)*(float)alpha;
    if(sa<=0.0f) continue;
    uint32_t dv=r->fb[i]; float ia=1.0f-sa;
    int sr=(sv>>16)&255,sg=(sv>>8)&255,sb=sv&255;
    int dr=(dv>>16)&255,dg=(dv>>8)&255,db=dv&255;
    int rr=(int)floorf(sr*sa+dr*ia+0.5f),gg=(int)floorf(sg*sa+dg*ia+0.5f);
    int bb=(int)floorf(sb*sa+db*ia+0.5f),aa=255;
    if(r->target_sp>0){ int da=(dv>>24)&255; aa=(int)floorf(sa*255.0f+da*ia+0.5f); if(aa>255)aa=255; }
    r->fb[i]=((uint32_t)aa<<24)|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
  }
  r->fb_opaque_known=0; r->fb_all_transparent=0;
}
/* Room-layer capture targets contain the result of source-over drawing onto transparent black, so
 * their RGB channels are already alpha-weighted. Filter shaders preserve that representation.
 * Composite those results with the premultiplied source-over equation to avoid applying alpha a
 * second time around blurred/antialiased edges. */
static void layer_composite_premultiplied(GmlRender *r,const uint32_t *src,int w,int h,double alpha){
  if(!r || !r->fb || !src || w!=r->fbw || h!=r->fbh || alpha<=0.0) return;
  if(alpha>1.0) alpha=1.0;
  size_t count=(size_t)w*h;
  for(size_t i=0;i<count;i++){
    uint32_t sv=src[i],dv=r->fb[i]; float scale=(float)alpha;
    float sa=((sv>>24)&255)*(1.0f/255.0f)*scale,ia=1.0f-sa;
    if(sa<=0.0f) continue;
    int sr=(int)floorf(((sv>>16)&255)*scale+0.5f),sg=(int)floorf(((sv>>8)&255)*scale+0.5f);
    int sb=(int)floorf((sv&255)*scale+0.5f);
    int rr=sr+(int)floorf(((dv>>16)&255)*ia+0.5f);
    int gg=sg+(int)floorf(((dv>>8)&255)*ia+0.5f),bb=sb+(int)floorf((dv&255)*ia+0.5f);
    if(rr>255) rr=255;
    if(gg>255) gg=255;
    if(bb>255) bb=255;
    r->fb[i]=0xFF000000u|((uint32_t)rr<<16)|((uint32_t)gg<<8)|(uint32_t)bb;
  }
  r->fb_opaque_known=0; r->fb_all_transparent=0;
}
static void layer_composite_max(GmlRender *r,const uint32_t *src,int w,int h,double tint){
  if(!r || !r->fb || !src || w!=r->fbw || h!=r->fbh) return;
  float t=(float)tint; if(t<0)t=0; if(t>1)t=1;
  size_t count=(size_t)w*h;
  for(size_t i=0;i<count;i++){
    uint32_t s=src[i],d=r->fb[i];
    unsigned sr=(unsigned)floorf(((s>>16)&255)*t+0.5f),sg=(unsigned)floorf(((s>>8)&255)*t+0.5f);
    unsigned sb=(unsigned)floorf((s&255)*t+0.5f),sa=s>>24;
    unsigned dr=(d>>16)&255,dg=(d>>8)&255,db=d&255,da=d>>24;
    if(sr<dr) sr=dr;
    if(sg<dg) sg=dg;
    if(sb<db) sb=db;
    if(sa<da) sa=da;
    r->fb[i]=(sa<<24)|(sr<<16)|(sg<<8)|sb;
  }
  r->fb_opaque_known=0; r->fb_all_transparent=0;
}

int gml_render_layer_filter_begin(GmlRender *r,const GmlLayerFilter *filter){
  if(!r || !filter || filter->kind==GML_LAYER_FILTER_NONE || !r->fb ||
     r->fbw<=0 || r->fbh<=0 || r->layer_filter_active || r->target_sp>=GML_SURFACE_STACK) return 0;
  size_t count=(size_t)r->fbw*(size_t)r->fbh;
  if(!layer_filter_reserve(r,count)) return 0;
  gml_render_prepare_draw(r);
  int captured_known=r->fb_opaque_known,captured_opaque=r->fb_all_opaque;
  int captured_transparent=r->fb_all_transparent;
  if(filter->affects_below) memcpy(r->layer_filter_src,r->fb,count*sizeof(uint32_t));
  else memset(r->layer_filter_src,0,count*sizeof(uint32_t));
  r->target_stack[r->target_sp++]=(typeof(r->target_stack[0])){
    r->fb,r->fbw,r->fbh,r->cam_x,r->cam_y,r->projection_cam_x,r->projection_cam_y,
    r->target_id,r->fb_opaque_known,r->fb_all_opaque,r->fb_all_transparent,
    r->pending_underlay,r->underlay_x,r->underlay_y,r->underlay_w,r->underlay_h,
    r->pending_fill,r->pending_fill_color
  };
  r->fb=r->layer_filter_src; r->target_id=-2;
  r->fb_opaque_known=filter->affects_below?captured_known:1;
  r->fb_all_opaque=filter->affects_below?captured_opaque:0;
  r->fb_all_transparent=filter->affects_below?captured_transparent:1;
  r->pending_underlay=0; r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
  r->pending_fill=0; r->pending_fill_color=0; r->layer_filter_active=1;
  return 1;
}
static void layer_filter_restore_target(GmlRender *r){
  gml_render_flush_pending_underlay(r); gml_render_flush_pending_fill(r);
  if(r->target_sp>0){
    typeof(r->target_stack[0]) t=r->target_stack[--r->target_sp];
    r->fb=t.fb; r->fbw=t.w; r->fbh=t.h;
    r->projection_cam_x=t.projection_cx; r->projection_cam_y=t.projection_cy;
    r->cam_x=t.cx; r->cam_y=t.cy; r->target_id=t.target_id;
    r->fb_opaque_known=t.opaque_known; r->fb_all_opaque=t.all_opaque;
    r->fb_all_transparent=t.all_transparent; r->pending_underlay=t.pending_underlay;
    r->underlay_x=t.underlay_x; r->underlay_y=t.underlay_y;
    r->underlay_w=t.underlay_w; r->underlay_h=t.underlay_h;
    r->pending_fill=t.pending_fill; r->pending_fill_color=t.fill_color;
    gml_d3_sync_render_camera(r);
  }
}

static void layer_filter_tint_pixels(const uint32_t *src,uint32_t *dst,size_t count,uint32_t colour){
  float tint[4]; layer_unpack_colour(colour,tint);
  for(size_t i=0;i<count;i++){
    float c[4]; layer_unpack(src[i],c); for(int k=0;k<4;k++) c[k]*=tint[k]; dst[i]=layer_pack(c);
  }
}

/* Forward declaration for the persistent compositor worker pool defined below. Filter shaders can
 * be much heavier per row than a normal blit even at a small authored resolution. */
typedef void (*GmlRowBandFn)(void *ctx, int py0, int py1, int slot);
void gml_run_row_bands_n(GmlRender *r,int H,int nt,GmlRowBandFn fn,void *ctx);

typedef struct { GmlRender *r; const uint32_t *src; uint32_t *dst; int w,h;
  const GmlLayerFilter *f; float time,camx,camy; } LayerCloudCtx;


typedef struct { int16_t ix,iy; uint16_t w00,w10,w01,w11; } LayerBlurTap;
typedef struct { GmlRender *r; const uint32_t *src; uint32_t *dst; int w,h,nw,nh;
  const GmlLayerFilter *f; const LayerBlurTap *tap; } LayerLargeBlurCtx;

typedef struct {
  const uint32_t *src; uint32_t *dst; int w,h;
  int ix[36],iy[36]; float weight00[36],weight10[36],weight01[36],weight11[36];
  float inv_radius[36],exp_lut[1025];
  float nearest_numerator[36][256],nearest_denominator[36][256];
} LayerGlowCtx;


/* ---- recognized display post-processes: none retained in this revision ---- */

