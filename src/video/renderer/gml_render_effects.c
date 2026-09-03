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
/* Room-layer effects. Each kernel is a standard image operation configured by the properties the
 * room record carries; docs/EFFECT_LAYERS.md says what every effect does and which properties it
 * reads. */

/* A 32-bit integer hash over a position and a seed: three rounds of multiply-and-fold with large
 * odd constants, which is enough to decorrelate neighbouring pixels and successive seeds. */
static inline uint32_t effect_hash(uint32_t x,uint32_t y,uint32_t seed){
  uint32_t h=x*0x9E3779B1u ^ (y+0x7F4A7C15u)*0x85EBCA77u ^ seed*0xC2B2AE3Du;
  h^=h>>15; h*=0x2C1B3C6Du; h^=h>>12; h*=0x297A2D39u; h^=h>>15;
  return h;
}
static inline float effect_hash_unit(uint32_t h){ return (float)(h&0xFFFFFFu)*(1.0f/16777216.0f); }

/* RGB noise: every pixel takes an independent random colour, scaled by the effect colour and
 * mixed in by the intensity and the pixel's own coverage. The pattern is a function of position
 * and animation phase only, so the plane is cached until either changes. The sampler the room
 * names is not read. */
void gml_render_layer_rgb_noise(GmlRender *r, uint32_t sampler_tpag_ptr,
                                double intensity_d, double animation_d, uint32_t rgb){
  (void)sampler_tpag_ptr;
  if(!r || !r->fb || r->fbw<=0 || r->fbh<=0 || intensity_d<=0.0) return;
  float animation=(float)animation_d;
  uint32_t seed=(uint32_t)(int32_t)llround((double)animation*4096.0);
  int rebuild=!r->layer_noise_rgb || r->layer_noise_w!=r->fbw || r->layer_noise_h!=r->fbh ||
              r->layer_noise_animation!=animation || r->layer_noise_colour!=(rgb&0xFFFFFFu);
  size_t count=(size_t)r->fbw*(size_t)r->fbh;
  if(count==0 || count>67108864u) return;
  if(rebuild){
    uint32_t *cache=realloc(r->layer_noise_rgb,count*sizeof(uint32_t));
    if(!cache) return;
    r->layer_noise_rgb=cache;
    const unsigned tint[3]={(rgb>>16)&255u,(rgb>>8)&255u,rgb&255u};
    for(int y=0;y<r->fbh;y++) for(int x=0;x<r->fbw;x++){
      uint32_t h=effect_hash((uint32_t)x,(uint32_t)y,seed);
      uint32_t packed=0;
      for(int c=0;c<3;c++){
        unsigned value=(h>>(c*8))&255u;
        packed|=(uint32_t)((value*tint[c]+127u)/255u)<<(16-c*8);
      }
      cache[(size_t)y*r->fbw+x]=packed;
    }
    r->layer_noise_w=r->fbw; r->layer_noise_h=r->fbh;
    r->layer_noise_tpag_ptr=0;
    r->layer_noise_animation=animation;
    r->layer_noise_colour=rgb&0xFFFFFFu;
  }
  gml_render_prepare_draw(r);
  float intensity=(float)intensity_d;
  if(intensity>1.0f) intensity=1.0f;
  for(size_t i=0;i<count;i++){
    uint32_t base=r->fb[i], noise=r->layer_noise_rgb[i];
    float mix=((base>>24)&255)/255.0f*intensity;
    int br=(base>>16)&255, bg=(base>>8)&255, bb=base&255;
    int nr=(noise>>16)&255, ng=(noise>>8)&255, nb=noise&255;
    uint32_t rr=effect_unorm8(br+(nr-br)*mix);
    uint32_t rg=effect_unorm8(bg+(ng-bg)*mix);
    uint32_t rb=effect_unorm8(bb+(nb-bb)*mix);
    r->fb[i]=(base&0xFF000000u)|(rr<<16)|(rg<<8)|rb;
  }
  r->fb_all_transparent=0;
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
/* Additive merge: the halo's premultiplied colour, scaled, is added to the scene and clipped. */
static void layer_composite_add(GmlRender *r,const uint32_t *src,int w,int h,double scale){
  if(!r || !r->fb || !src || w!=r->fbw || h!=r->fbh || scale<=0.0) return;
  float t=(float)scale; size_t count=(size_t)w*h;
  for(size_t i=0;i<count;i++){
    uint32_t s=src[i],d=r->fb[i];
    unsigned rr=((d>>16)&255)+(unsigned)floorf(((s>>16)&255)*t+0.5f);
    unsigned gg=((d>>8)&255)+(unsigned)floorf(((s>>8)&255)*t+0.5f);
    unsigned bb=(d&255)+(unsigned)floorf((s&255)*t+0.5f);
    unsigned aa=(d>>24); unsigned sa=(unsigned)floorf((s>>24)*t+0.5f); if(sa>aa) aa=sa;
    if(rr>255) rr=255;
    if(gg>255) gg=255;
    if(bb>255) bb=255;
    r->fb[i]=(aa<<24)|(rr<<16)|(gg<<8)|bb;
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
/* Colourise: a duotone. Each pixel's luminance picks a point on the black -> tint -> white ramp,
 * with the tint sitting at its own luminance, and the intensity mixes that with the original. The
 * capture is premultiplied, so the ramp is applied to the straight colour and re-weighted. */
static const float LAYER_LUMA[3]={0.299f,0.587f,0.114f};
static inline float layer_luma(const float c[3]){ return c[0]*LAYER_LUMA[0]+c[1]*LAYER_LUMA[1]+c[2]*LAYER_LUMA[2]; }
static void layer_filter_colourise_pixels(const uint32_t *src,uint32_t *dst,size_t count,
                                          const GmlLayerFilter *f){
  float tint[4]; layer_unpack_colour(f->u.colourise.tint_colour,tint);
  float tint_luma=layer_luma(tint);
  float intensity=layer_clamp01((float)f->u.colourise.intensity);
  for(size_t i=0;i<count;i++){
    float c[4]; layer_unpack(src[i],c); float a=c[3];
    if(a<=0.0f){ dst[i]=src[i]; continue; }
    float straight[3]={c[0]/a,c[1]/a,c[2]/a};
    float luma=layer_luma(straight),ramp[3];
    if(luma<=tint_luma){
      float t=tint_luma>0.0f?luma/tint_luma:0.0f;
      for(int k=0;k<3;k++) ramp[k]=tint[k]*t;
    } else {
      float t=tint_luma<1.0f?(luma-tint_luma)/(1.0f-tint_luma):1.0f;
      for(int k=0;k<3;k++) ramp[k]=layer_mix(tint[k],1.0f,t);
    }
    for(int k=0;k<3;k++) c[k]=layer_mix(straight[k],ramp[k],intensity)*a;
    dst[i]=layer_pack(c);
  }
}

/* Forward declaration for the persistent compositor worker pool defined below. Filter shaders can
 * be much heavier per row than a normal blit even at a small authored resolution. */
typedef void (*GmlRowBandFn)(void *ctx, int py0, int py1, int slot);
void gml_run_row_bands_n(GmlRender *r,int H,int nt,GmlRowBandFn fn,void *ctx);

/* Value noise in 0..1 from the sampler: the sampler's red channel read bilinearly with repeat at
 * a point in noise units, two octaves, the second at twice the frequency and a fixed offset. */
static float layer_value_noise(GmlRender *r,int sprite,float px,float py){
  int tw=(sprite>=0&&sprite<r->n_spr)?r->spr[sprite].w:1;
  int th=(sprite>=0&&sprite<r->n_spr)?r->spr[sprite].h:1;
  if(tw<=0) tw=1;
  if(th<=0) th=1;
  float a[4],b[4];
  layer_sprite_sample(r,sprite,px/(float)tw,py/(float)th,1,1,a);
  layer_sprite_sample(r,sprite,(px*2.0f+0.37f*tw)/(float)tw,(py*2.0f+0.61f*th)/(float)th,1,1,b);
  return layer_clamp01(0.65f*a[0]+0.35f*b[0]);
}

/* Clouds: a drifting, churning noise field thresholded into coverage, lit from one colour and
 * shaded from another where a second read at the shade offset is denser. */
typedef struct { GmlRender *r; const uint32_t *src; uint32_t *dst; int w,h;
  const GmlLayerFilter *f; float time,camx,camy; } LayerCloudCtx;
static void layer_filter_clouds_band(void *opaque,int y0,int y1,int slot){
  (void)slot;
  LayerCloudCtx *ctx=(LayerCloudCtx*)opaque; GmlRender *r=ctx->r;
  const uint32_t *src=ctx->src; uint32_t *dst=ctx->dst; int w=ctx->w;
  const GmlLayerFilter *f=ctx->f; float time=ctx->time;
  const typeof(f->u.clouds) *c=&f->u.clouds;
  float lit[4],shade[4]; layer_unpack_colour(c->light_colour,lit); layer_unpack_colour(c->shade_colour,shade);
  float scale=(float)c->scale; if(fabsf(scale)<1e-6f) scale=1.0f;
  float density=(float)c->density; if(density<1e-4f) density=1e-4f;
  float fade=(float)c->fade; if(fade<1e-4f) fade=1e-4f;
  float shade_fade=(float)c->shade_fade; if(shade_fade<1e-4f) shade_fade=1e-4f;
  float level=(float)c->level,waves=(float)c->waves,turbulence=(float)c->turbulence;
  float edge=fade/density,shade_edge=shade_fade/density;
  float churn_x=turbulence*0.25f*sinf(time*0.7f),churn_y=turbulence*0.25f*cosf(time*0.9f);
  for(int y=y0;y<y1;y++) for(int x=0;x<w;x++){
    float px=((float)x+0.5f+ctx->camx-(float)c->velocity[0]*time)/scale*(float)c->shape[0];
    float py=((float)y+0.5f+ctx->camy-(float)c->velocity[1]*time)/scale*(float)c->shape[1];
    px+=churn_x*sinf(py*0.5f+time); py+=churn_y*cosf(px*0.5f+time*1.3f);
    py+=waves*sinf(px*3.0f);
    float field=layer_value_noise(r,f->sampler_sprite,px,py);
    float coverage=layer_smoothstep(level,level+edge,field);
    float shaded_field=layer_value_noise(r,f->sampler_sprite,
      px+(float)c->shade_offset[0],py+(float)c->shade_offset[1]);
    float shaded=layer_smoothstep(level,level+shade_edge,shaded_field);
    float base[4]; layer_unpack(src[(size_t)y*w+x],base);
    float paint=coverage*base[3];
    for(int k=0;k<3;k++) base[k]=layer_mix(base[k],layer_mix(lit[k],shade[k],shaded)*base[3],paint);
    dst[(size_t)y*w+x]=layer_pack(base);
  }
}
static void layer_filter_clouds(GmlRender *r,const uint32_t *src,uint32_t *dst,int w,int h,
                                const GmlLayerFilter *f,float time,float camx,float camy){
  LayerCloudCtx ctx={r,src,dst,w,h,f,time,camx,camy};
  gml_run_row_bands_n(r,h,h>=32?4:1,layer_filter_clouds_band,&ctx);
}

/* Boxes: one rounded box per cell, whose size, spin, wander phase and palette entry come from a
 * hash of the cell, orbiting its centre and spinning around the base angle. */
typedef struct { GmlRender *r; const uint32_t *src; uint32_t *dst; int w,h;
  const GmlLayerFilter *f; float time,camx,camy; } LayerBoxCtx;
static void layer_filter_boxes_band(void *opaque,int y0,int y1,int slot){
  (void)slot;
  LayerBoxCtx *ctx=(LayerBoxCtx*)opaque; GmlRender *r=ctx->r;
  const uint32_t *src=ctx->src; uint32_t *dst=ctx->dst; int w=ctx->w;
  const GmlLayerFilter *f=ctx->f; float time=ctx->time;
  const typeof(f->u.boxes) *b=&f->u.boxes;
  float scale=(float)b->scale,sharp=(float)b->sharpness;
  float palette_count=(float)b->colours; if(palette_count<1.0f) palette_count=1.0f;
  float edge=sharp>1e-4f?scale/sharp:scale; if(edge<0.5f) edge=0.5f;
  float roundness=layer_clamp01((float)b->roundness);
  for(int y=y0;y<y1;y++) for(int x=0;x<w;x++){
    float px=(float)x+0.5f-ctx->camx,py=(float)y+0.5f-ctx->camy;
    int cell_x=(int)floorf(px/scale),cell_y=(int)floorf(py/scale);
    float colour[4]; layer_unpack(src[(size_t)y*w+x],colour);
    if(colour[3]<=0.0f){ dst[(size_t)y*w+x]=src[(size_t)y*w+x]; continue; }
    for(int oy=-1;oy<=1;oy++) for(int ox=-1;ox<=1;ox++){
      int cx=cell_x+ox,cy=cell_y+oy;
      uint32_t h0=effect_hash((uint32_t)cx,(uint32_t)cy,0x1B0C5Du);
      uint32_t h1=effect_hash((uint32_t)cx,(uint32_t)cy,0x2A7F31u);
      float r0=effect_hash_unit(h0),r1=effect_hash_unit(h0>>8),r2=effect_hash_unit(h1),r3=effect_hash_unit(h1>>8);
      float size=layer_mix((float)b->size[0],(float)b->size[1],r0)*scale;
      float phase=r2*6.2831853f,wander=(float)b->displacement*scale*0.5f;
      float centre_x=((float)cx+0.5f)*scale+wander*sinf((float)b->speed*time+phase);
      float centre_y=((float)cy+0.5f)*scale+wander*cosf((float)b->speed*time*0.8f+phase*1.7f);
      float spin=layer_mix((float)b->rotation[0],(float)b->rotation[1],r1)*time;
      float theta=-((float)b->angle+spin)*0.01745329252f,ct=cosf(theta),st=sinf(theta);
      float dx=px-centre_x,dy=py-centre_y;
      float lx=fabsf(dx*ct-dy*st),ly=fabsf(dx*st+dy*ct);
      float radius=roundness*size*0.5f,half=size*0.5f-radius;
      float qx=fmaxf(lx-half,0.0f),qy=fmaxf(ly-half,0.0f);
      float distance=hypotf(qx,qy)-radius;
      float coverage=layer_clamp01(0.5f-distance/edge);
      if(coverage<=0.0f) continue;
      float entry=(floorf(r3*palette_count)+0.5f)/palette_count;
      float pal[4]; layer_sprite_sample(r,f->sampler_sprite,entry,layer_fract(time*(float)b->colour_speed+r0),1,r->interp,pal);
      float alpha=coverage*pal[3]*colour[3];
      for(int k=0;k<3;k++) colour[k]=layer_mix(colour[k],pal[k]*colour[3],alpha);
    }
    dst[(size_t)y*w+x]=layer_pack(colour);
  }
}
static void layer_filter_boxes(GmlRender *r,const uint32_t *src,uint32_t *dst,int w,int h,
                               const GmlLayerFilter *f,float time,float camx,float camy){
  float scale=(float)f->u.boxes.scale;
  if(scale<=0.0f){ memcpy(dst,src,(size_t)w*h*sizeof(uint32_t)); return; }
  LayerBoxCtx ctx={r,src,dst,w,h,f,time,camx,camy};
  gml_run_row_bands_n(r,h,h>=32?4:1,layer_filter_boxes_band,&ctx);
}

/* A separable box blur of the given radius over premultiplied pixels, three passes, which is a
 * close approximation of a Gaussian. `scratch` holds one full-size plane between the passes. */
/* One axis of a box blur over premultiplied pixels, as a running window. */
static void layer_box_blur_axis(const uint32_t *src,uint32_t *dst,int w,int h,int radius,int vertical){
  int length=vertical?h:w,lines=vertical?w:h;
  size_t step=vertical?(size_t)w:1u,line_step=vertical?1u:(size_t)w;
  float window=(float)(2*radius+1);
  for(int line=0;line<lines;line++){
    const uint32_t *s=src+(size_t)line*line_step; uint32_t *d=dst+(size_t)line*line_step;
    float sum[4]={0,0,0,0};
    for(int i=-radius;i<=radius;i++){
      int j=i<0?0:(i>=length?length-1:i);
      float c[4]; layer_unpack(s[(size_t)j*step],c); for(int k=0;k<4;k++) sum[k]+=c[k];
    }
    for(int i=0;i<length;i++){
      float c[4]; for(int k=0;k<4;k++) c[k]=sum[k]/window;
      d[(size_t)i*step]=layer_pack(c);
      int leave=i-radius,enter=i+radius+1;
      if(leave<0) leave=0; else if(leave>=length) leave=length-1;
      if(enter>=length) enter=length-1;
      float out[4],in[4]; layer_unpack(s[(size_t)leave*step],out); layer_unpack(s[(size_t)enter*step],in);
      for(int k=0;k<4;k++) sum[k]+=in[k]-out[k];
    }
  }
}
static void layer_blur(const uint32_t *src,uint32_t *dst,uint32_t *scratch,int w,int h,float radius,int passes){
  int r=(int)floorf(fabsf(radius)+0.5f);
  if(r<=0 || passes<=0){ if(dst!=src) memcpy(dst,src,(size_t)w*h*sizeof(uint32_t)); return; }
  const uint32_t *from=src;
  for(int pass=0;pass<passes;pass++){
    layer_box_blur_axis(from,scratch,w,h,r,0);
    layer_box_blur_axis(scratch,dst,w,h,r,1);
    from=dst;
  }
}
/* A one-pass blur whose destination doubles as its scratch: the horizontal pass lands in dst and
 * the vertical pass reads dst line by line into a row buffer, so no third plane is needed. */
static void layer_blur_in_place(const uint32_t *src,uint32_t *dst,int w,int h,float radius){
  int r=(int)floorf(fabsf(radius)+0.5f);
  if(r<=0){ if(dst!=src) memcpy(dst,src,(size_t)w*h*sizeof(uint32_t)); return; }
  layer_box_blur_axis(src,dst,w,h,r,0);
  uint32_t *column=malloc((size_t)h*sizeof(uint32_t)*2u);
  if(!column) return;
  uint32_t *blurred=column+h;
  for(int x=0;x<w;x++){
    for(int y=0;y<h;y++) column[y]=dst[(size_t)y*w+x];
    layer_box_blur_axis(column,blurred,1,h,r,1);
    for(int y=0;y<h;y++) dst[(size_t)y*w+x]=blurred[y];
  }
  free(column);
}
static void layer_filter_large_blur(const uint32_t *src,uint32_t *dst,uint32_t *scratch,int w,int h,
                                    const GmlLayerFilter *f){
  layer_blur(src,dst,scratch,w,h,(float)f->u.large_blur.radius,3);
}

/* Zoom blur: samples along the segment from the pixel toward the centre, whose length grows with
 * the intensity and the distance from the centre and is zero inside the focus radius. */
typedef struct { const uint32_t *src; uint32_t *dst; int w,h; const GmlLayerFilter *f; int linear; } LayerZoomCtx;
static void layer_filter_zoom_blur_band(void *opaque,int y0,int y1,int slot){
  (void)slot;
  LayerZoomCtx *ctx=(LayerZoomCtx*)opaque; const uint32_t *src=ctx->src; uint32_t *dst=ctx->dst;
  int w=ctx->w,h=ctx->h; const GmlLayerFilter *f=ctx->f;
  const int samples=16;
  float cx=(float)f->u.zoom_blur.centre[0]*w,cy=(float)f->u.zoom_blur.centre[1]*h;
  float intensity=(float)f->u.zoom_blur.intensity,focus=(float)f->u.zoom_blur.focus_radius;
  for(int y=y0;y<y1;y++) for(int x=0;x<w;x++){
    float px=(float)x+0.5f,py=(float)y+0.5f;
    float dist=hypotf(px-cx,py-cy);
    float reach=intensity*layer_smoothstep(focus,focus+fmaxf(dist,1.0f),dist);
    float sum[4]={0,0,0,0};
    for(int i=0;i<samples;i++){
      float t=reach*(float)i/(float)samples;
      float sample[4]; layer_surface_sample(src,w,h,layer_mix(px,cx,t)/w,layer_mix(py,cy,t)/h,ctx->linear,sample);
      for(int k=0;k<4;k++) sum[k]+=sample[k];
    }
    for(int k=0;k<4;k++) sum[k]/=(float)samples;
    dst[(size_t)y*w+x]=layer_pack(sum);
  }
}
static void layer_filter_zoom_blur(GmlRender *r,const uint32_t *src,uint32_t *dst,int w,int h,
                                   const GmlLayerFilter *f){
  LayerZoomCtx ctx={src,dst,w,h,f,r->interp};
  gml_run_row_bands_n(r,h,h>=32?4:1,layer_filter_zoom_blur_band,&ctx);
}

/* Underwater: two scrolled reads of the sampler give a displacement in pixels; each channel
 * samples the capture at its own spread of that displacement; a glint is added where the
 * displacement is strongest, then the tint multiplies and the add colour is added. */
typedef struct { GmlRender *r; const uint32_t *src; uint32_t *dst; int w,h;
  const GmlLayerFilter *f; float time,camx,camy; } LayerWaterCtx;
static void layer_filter_underwater_band(void *opaque,int y0,int y1,int slot){
  (void)slot;
  LayerWaterCtx *ctx=(LayerWaterCtx*)opaque; GmlRender *r=ctx->r;
  const uint32_t *src=ctx->src; uint32_t *dst=ctx->dst; int w=ctx->w,h=ctx->h;
  const GmlLayerFilter *f=ctx->f; float time=ctx->time;
  const typeof(f->u.underwater) *u=&f->u.underwater;
  int nw=(f->sampler_sprite>=0&&f->sampler_sprite<r->n_spr)?r->spr[f->sampler_sprite].w:1;
  int nh=(f->sampler_sprite>=0&&f->sampler_sprite<r->n_spr)?r->spr[f->sampler_sprite].h:1;
  if(nw<=0) nw=1;
  if(nh<=0) nh=1;
  float glint[4],tint[4],add[4]; layer_unpack_colour(u->glint_colour,glint);
  layer_unpack_colour(u->tint_colour,tint); layer_unpack_colour(u->add_colour,add);
  float amount_total=fabsf((float)u->amount[0])+fabsf((float)u->amount[1]);
  float chroma=(float)u->chroma;
  for(int y=y0;y<y1;y++) for(int x=0;x<w;x++){
    float px=(float)x+0.5f,py=(float)y+0.5f,disp_x=0.0f,disp_y=0.0f;
    for(int q=0;q<2;q++){
      float sx=(float)u->scale[q][0],sy=(float)u->scale[q][1];
      if(fabsf(sx)<1e-6f) sx=1.0f;
      if(fabsf(sy)<1e-6f) sy=1.0f;
      float nu=(px+ctx->camx*(float)u->camera_scale)/(sx*(float)nw);
      float nv=(py+ctx->camy*(float)u->camera_scale)/(sy*(float)nh)+(float)u->speed[q]*time;
      float n[4]; layer_sprite_sample(r,f->sampler_sprite,nu,nv,1,1,n);
      disp_x+=(n[0]-0.5f)*2.0f*(float)u->amount[q];
      disp_y+=(n[1]-0.5f)*2.0f*(float)u->amount[q];
    }
    float red[4],green[4],blue[4];
    layer_surface_sample(src,w,h,(px+disp_x*(1.0f+chroma))/w,(py+disp_y*(1.0f+chroma))/h,r->interp,red);
    layer_surface_sample(src,w,h,(px+disp_x*(1.0f+chroma*0.5f))/w,(py+disp_y*(1.0f+chroma*0.5f))/h,r->interp,green);
    layer_surface_sample(src,w,h,(px+disp_x)/w,(py+disp_y)/h,r->interp,blue);
    float out[4]={red[0],green[1],blue[2],blue[3]};
    float strength=amount_total>1e-6f?hypotf(disp_x,disp_y)/amount_total:0.0f;
    float shine=layer_smoothstep(0.55f,1.0f,strength)*out[3];
    for(int k=0;k<3;k++) out[k]=(out[k]+glint[k]*shine)*tint[k]+add[k]*out[3];
    dst[(size_t)y*w+x]=layer_pack(out);
  }
}
static void layer_filter_underwater(GmlRender *r,const uint32_t *src,uint32_t *dst,int w,int h,
                                    const GmlLayerFilter *f,float time,double camx,double camy){
  LayerWaterCtx ctx={r,src,dst,w,h,f,time,(float)camx,(float)camy};
  gml_run_row_bands_n(r,h,h>=32?4:1,layer_filter_underwater_band,&ctx);
}


void gml_render_layer_filter_end(GmlRender *r,const GmlLayerFilter *filter,double time_seconds){
  if(!r || !filter || !r->layer_filter_active) return;
  int w=r->fbw,h=r->fbh; size_t count=(size_t)w*h;
  uint32_t *src=r->layer_filter_src,*work=r->layer_filter_work,*aux=r->layer_filter_aux;
  double camx=r->cam_x,camy=r->cam_y;
  int source_empty=r->fb_all_transparent;
  if(render_setting(r,"GML_LOG_LAYER_EFFECT") && (r->frame<4 || (r->frame%60)==0))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[layer-filter] f%ld kind=%d empty=%d size=%dx%d cam=%.3f,%.3f time=%.6f linear=%d\n",
            r->frame,filter->kind,source_empty,w,h,camx,camy,time_seconds,r->interp);
  layer_filter_restore_target(r); r->layer_filter_active=0;
  if(source_empty || !r->fb || r->fbw!=w || r->fbh!=h) return;
  if(filter->kind==GML_LAYER_FILTER_GLOW){
    /* The halo is built from the inside out: one blur per quality step, each reaching further
     * toward the radius than the last, added to the scene as it goes, each step carrying its share of the intensity. A blur averages
     * a small source away, so each step is lifted by a gain that keeps a source a few pixels wide
     * at full brightness at its centre while its skirt fades with distance; the gamma raises the
     * result so the skirt falls off harder or softer. */
    layer_composite_normal(r,src,w,h,filter->u.glow.alpha);
    int quality=(int)floor(filter->u.glow.quality+0.5); if(quality<1)quality=1;if(quality>16)quality=16;
    float radius=fabsf((float)filter->u.glow.radius);
    float gamma=(float)filter->u.glow.gamma; if(gamma<0.1f) gamma=0.1f;
    if(render_setting(r,"GML_LOG_LAYER_EFFECT") && r->frame<4)
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
        "[layer-filter] glow radius=%.3f quality=%d intensity=%.3f gamma=%.3f alpha=%.3f\n",
        radius,quality,filter->u.glow.intensity,gamma,filter->u.glow.alpha);
    for(int step=1;step<=quality;step++){
      float reach=radius*(float)step/(float)quality*0.5f;
      int rounded=(int)floorf(reach+0.5f); if(rounded<1) rounded=1;
      layer_blur_in_place(src,work,w,h,(float)rounded);
      layer_blur_in_place(work,aux,w,h,(float)rounded);
      /* The gain restores part of what the blur averaged away - the square root of the ratio
       * between the source's peak and the blurred peak - so a narrow reach keeps the core bright
       * and a wide reach leaves a dimmer skirt, which is the falloff of a halo. */
      float source_peak=0.0f,blurred_peak=0.0f;
      for(size_t i=0;i<count;i++){
        float c[4]; layer_unpack(src[i],c); source_peak=fmaxf(source_peak,fmaxf(c[0],fmaxf(c[1],c[2])));
        layer_unpack(aux[i],c); blurred_peak=fmaxf(blurred_peak,fmaxf(c[0],fmaxf(c[1],c[2])));
      }
      float gain=blurred_peak>1e-6f?sqrtf(source_peak/blurred_peak):1.0f; if(gain<1.0f) gain=1.0f;
      for(size_t i=0;i<count;i++){
        float c[4]; layer_unpack(aux[i],c);
        for(int k=0;k<4;k++) c[k]=powf(layer_clamp01(c[k]*gain),gamma);
        aux[i]=layer_pack(c);
      }
      layer_composite_add(r,aux,w,h,filter->u.glow.intensity/sqrt((double)quality));
    }
    return;
  }
  switch(filter->kind){
    case GML_LAYER_FILTER_TINT: layer_filter_tint_pixels(src,work,count,filter->u.tint.colour); break;
    case GML_LAYER_FILTER_COLOURISE: layer_filter_colourise_pixels(src,work,count,filter); break;
    case GML_LAYER_FILTER_CLOUDS: layer_filter_clouds(r,src,work,w,h,filter,(float)time_seconds,(float)camx,(float)camy); break;
    case GML_LAYER_FILTER_BOXES: layer_filter_boxes(r,src,work,w,h,filter,(float)time_seconds,(float)camx,(float)camy); break;
    case GML_LAYER_FILTER_LARGE_BLUR: layer_filter_large_blur(src,work,aux,w,h,filter); break;
    case GML_LAYER_FILTER_ZOOM_BLUR: layer_filter_zoom_blur(r,src,work,w,h,filter); break;
    case GML_LAYER_FILTER_UNDERWATER: layer_filter_underwater(r,src,work,w,h,filter,(float)time_seconds,camx,camy); break;
    default: memcpy(work,src,count*sizeof(uint32_t)); break;
  }
  layer_composite_premultiplied(r,work,w,h,1.0);
}

/* portable memmem (mingw lacks it) */
static const void *mem_find(const void *hay, size_t hn, const void *nee, size_t nn){
  if(nn==0 || hn<nn) return NULL;
  const char *h=(const char*)hay, *ne=(const char*)nee;
  for(size_t i=0;i+nn<=hn;i++) if(h[i]==ne[0] && !memcmp(h+i,ne,nn)) return h+i;
  return NULL;
}

/* Parse `<name> = vec2(a, b)` (or vecN); fills a,b. */
static int glsl_parse_vec2(const char *src, const char *name, float *a, float *b){
  size_t nl=strlen(name); const char *p=src;
  while((p=strstr(p,name))){
    const char *q=p+nl; p=q;
    while(*q==' '||*q=='\t') q++;
    if(*q!='=') continue;
    q++;
    while(*q==' '||*q=='\t') q++;
    if(sscanf(q,"vec2(%f , %f)",a,b)==2 || sscanf(q,"vec2(%f,%f)",a,b)==2) return 1;
  }
  return 0;
}
static int glsl_parse_const_vec3(const char *src, float out[3]){
  const char *p=src;
  while((p=strstr(p,"const vec3 "))){
    const char *eq=strchr(p,'='); p+=10;
    if(!eq) continue;
    const char *q=eq+1; while(*q==' '||*q=='\t') q++;
    if(sscanf(q,"vec3(%f , %f , %f)",out,out+1,out+2)==3 ||
       sscanf(q,"vec3(%f,%f,%f)",out,out+1,out+2)==3) return 1;
  }
  return 0;
}
/* Copy the identifier following `uniform <type> ` for the `which`-th (0-based) such declaration
 * whose name does not start with "gm_". Returns 1 on success. */
static int glsl_uniform_name(const char *src, const char *type, int which, char *out, int outsz){
  char pat[48]; snprintf(pat,sizeof pat,"uniform %s ",type);
  const char *p=src; int seen=0;
  while((p=strstr(p,pat))){
    const char *q=p+strlen(pat); p=q;
    while(*q==' '||*q=='\t') q++;
    if(!strncmp(q,"gm_",3)) continue;              /* skip engine uniforms */
    if(seen++ != which) continue;
    int k=0; for(;k<outsz-1 && (isalnum((unsigned char)q[k])||q[k]=='_');k++) out[k]=q[k];
    out[k]=0; return k>0;
  }
  return 0;
}
/* Helpers for a compact, structural parser of the common two-texture-sample channel-offset
 * fragment family. They deliberately derive local and uniform identifiers from the source. */
static int glsl_texture_lhs(const char *src, const char *sample, char *out, int outsz){
  const char *line=sample;
  while(line>src && line[-1]!='\n' && line[-1]!='\r') line--;
  const char *v=strstr(line,"vec4 ");
  if(!v || v>=sample) return 0;
  v+=5; while(*v==' '||*v=='\t') v++;
  int k=0;
  while(k<outsz-1 && (isalnum((unsigned char)v[k])||v[k]=='_')){ out[k]=v[k]; k++; }
  out[k]=0;
  const char *eq=strchr(v,'=');
  return k>0 && eq && eq<sample;
}
static int glsl_uniform_float_named(const char *src, const char *name){
  char pat[96];
  snprintf(pat,sizeof pat,"uniform float %s",name);
  const char *p=strstr(src,pat);
  if(!p) return 0;
  p+=strlen(pat);
  return !isalnum((unsigned char)*p) && *p!='_';
}
static int glsl_channel_scale(const char *src, const char *var, char chan, float *out){
  char pat[80]; snprintf(pat,sizeof pat,"%s.%c",var,chan);
  const char *p=strstr(src,pat); size_t pl=strlen(pat);
  while(p){
    const char *q=p+pl;
    while(*q==' '||*q=='\t') q++;
    if(q[0]=='*' && q[1]=='='){
      q+=2; while(*q==' '||*q=='\t') q++;
      if(sscanf(q,"%f",out)==1) return 1;
    }
    p=strstr(p+pl,pat);
  }
  return 0;
}
static int glsl_channel_assign(const char *src, const char *var, char chan, float *out){
  char pat[80]; snprintf(pat,sizeof pat,"%s.%c",var,chan);
  const char *p=strstr(src,pat); size_t pl=strlen(pat);
  while(p){
    const char *q=p+pl;
    while(*q==' '||*q=='\t') q++;
    if(q[0]=='=' && q[1]!='='){
      q++; while(*q==' '||*q=='\t') q++;
      if(sscanf(q,"%f",out)==1) return 1;
    }
    p=strstr(p+pl,pat);
  }
  return 0;
}

static char *glsl_compact_source(const char *src);
static int glsl_take_text(const char **cursor,const char *text);
static int glsl_take_float(const char **cursor,float *out);
static int glsl_token_count(const char *source,const char *token);

static int glsl_take_symbol(const char **cursor,char *out,size_t capacity){
  size_t length=0;
  if(!cursor || !*cursor || !out || capacity<2) return 0;
  while(length+1<capacity &&
        (isalnum((unsigned char)(*cursor)[length])||(*cursor)[length]=='_')){
    out[length]=(*cursor)[length];
    length++;
  }
  if(!length || isdigit((unsigned char)out[0])) return 0;
  out[length]=0;
  *cursor+=length;
  return 1;
}

static int glsl_take_literal_ratio(const char **cursor,float *out){
  float numerator=0.0f;
  if(!glsl_take_float(cursor,&numerator)) return 0;
  if(**cursor=='/'){
    float denominator=0.0f;
    (*cursor)++;
    if(!glsl_take_float(cursor,&denominator) || fabsf(denominator)<1.0e-12f) return 0;
    numerator/=denominator;
  }
  *out=numerator;
  return 1;
}

static int glsl_take_literal_vec3(const char **cursor,float out[3]){
  if(!glsl_take_text(cursor,"vec3(")) return 0;
  for(int component=0;component<3;component++){
    if(!glsl_take_literal_ratio(cursor,&out[component]) ||
       out[component]<0.0f || out[component]>1.0f ||
       (component<2 && !glsl_take_text(cursor,","))) return 0;
  }
  return glsl_take_text(cursor,")");
}

static int glsl_parse_dual_sample(const char *src, struct GmlShaderPal *sp){
  const char *shift=NULL; int axis=-1,sign=0;
  if((shift=strstr(src,".x -="))){ axis=0; sign=-1; }
  else if((shift=strstr(src,".x +="))){ axis=0; sign=1; }
  else if((shift=strstr(src,".y -="))){ axis=1; sign=-1; }
  else if((shift=strstr(src,".y +="))){ axis=1; sign=1; }
  if(!shift) return 0;

  /* The last lookup before the coordinate shift is the base sample; the first after it is the
   * shifted sample. This also avoids depending on either local variable's spelling. */
  const char *base_tex=NULL;
  for(const char *p=strstr(src,"texture2D("); p && p<shift; p=strstr(p+1,"texture2D(")) base_tex=p;
  const char *shift_tex=strstr(shift,"texture2D(");
  if(!base_tex || !shift_tex) return 0;
  char base[48]="", shifted[48]="";
  if(!glsl_texture_lhs(src,base_tex,base,sizeof base) ||
     !glsl_texture_lhs(src,shift_tex,shifted,sizeof shifted) || !strcmp(base,shifted)) return 0;

  const char *q=strstr(shift,(sign<0)?"-=":"+=");
  if(!q || q>shift_tex) return 0;
  q+=2; while(*q==' '||*q=='\t') q++;
  char u0[32]="",u1[32]=""; int k=0;
  while(k<31 && (isalnum((unsigned char)q[k])||q[k]=='_')){ u0[k]=q[k]; k++; } u0[k]=0; q+=k;
  while(*q==' '||*q=='\t') q++;
  if(*q!='*') return 0;
  q++; while(*q==' '||*q=='\t') q++; k=0;
  while(k<31 && (isalnum((unsigned char)q[k])||q[k]=='_')){ u1[k]=q[k]; k++; } u1[k]=0;
  if(!u0[0]||!u1[0]||!glsl_uniform_float_named(src,u0)||!glsl_uniform_float_named(src,u1)) return 0;

  float bg[4]={1,1,1,1}, sg[4]={1,1,1,1};
  for(int c=0;c<3;c++){
    char ch="rgb"[c];
    if(!glsl_channel_scale(src,base,ch,&bg[c])) return 0;
    if(!glsl_channel_scale(src,shifted,ch,&sg[c])){
      float assigned=1.0f;
      if(!glsl_channel_assign(src,shifted,ch,&assigned)) return 0;
      sg[c]=assigned;
    }
  }
  const char *out=strstr(shift_tex,"gl_FragColor");
  if(!out || !strstr(out,base) || !strstr(out,shifted) || !strchr(out,'+')) return 0;

  sp->dual_sample=1; sp->dual_axis=axis; sp->dual_sign=sign;
  memcpy(sp->dual_base_gain,bg,sizeof bg);
  memcpy(sp->dual_shift_gain,sg,sizeof sg);
  snprintf(sp->dual_uniform[0],sizeof sp->dual_uniform[0],"%s",u0);
  snprintf(sp->dual_uniform[1],sizeof sp->dual_uniform[1],"%s",u1);
  return 1;
}
/* Remove comments and insignificant whitespace while retaining the one kind of spacing GLSL
 * needs: a separator between two identifier characters. This gives the structural parsers below
 * a stable expression form without assuming the source author's indentation. */
static char *glsl_compact_source(const char *src){
  size_t n=strlen(src),used=0; char *out=malloc(n+1); int pending_space=0;
  if(!out) return NULL;
  for(size_t i=0;i<n;){
    if(src[i]=='/' && i+1<n && src[i+1]=='/'){
      i+=2; while(i<n && src[i]!='\n' && src[i]!='\r') i++;
      pending_space=1; continue;
    }
    if(src[i]=='/' && i+1<n && src[i+1]=='*'){
      i+=2; while(i+1<n && !(src[i]=='*'&&src[i+1]=='/')) i++;
      if(i+1<n)i+=2;
      pending_space=1; continue;
    }
    unsigned char ch=(unsigned char)src[i++];
    if(isspace(ch)){ pending_space=1; continue; }
    if(pending_space && used && (isalnum((unsigned char)out[used-1])||out[used-1]=='_') &&
       (isalnum(ch)||ch=='_')) out[used++]=' ';
    pending_space=0; out[used++]=(char)ch;
  }
  out[used]=0; return out;
}
/* Recognize the common single-sample fragment whose only post-sample operation clears selected
 * RGB channels. Keeping the accepted graph deliberately complete prevents a more complicated
 * colour shader that happens to contain the same swizzle assignment from being approximated as a
 * channel mask. */
static int glsl_parse_channel_mask(const char *src,struct GmlShaderPal *sp){
  char *compact=glsl_compact_source(src); if(!compact) return 0;
  int ok=0,keep=7;
  const char *mainfn=strstr(compact,"void main()");
  if(!mainfn || glsl_token_count(mainfn,"texture2D(gm_BaseTexture,")!=1 ||
     glsl_token_count(mainfn,"gl_FragColor")!=2) goto done;
  static const char *const sample_forms[]={
    "gl_FragColor=v_vColour*texture2D(gm_BaseTexture,v_vTexcoord);",
    "gl_FragColor=texture2D(gm_BaseTexture,v_vTexcoord)*v_vColour;"
  };
  const char *body=mainfn+strlen("void main()");
  if(*body!='{') goto done;
  body++;
  const char *sample=NULL,*sample_end=NULL;
  for(unsigned form=0;form<sizeof sample_forms/sizeof sample_forms[0];form++){
    size_t length=strlen(sample_forms[form]);
    if(!strncmp(body,sample_forms[form],length)){
      sample=body;
      sample_end=body+length;
      break;
    }
  }
  if(!sample) goto done;
  const char *mask=sample_end;
  if(strncmp(mask,"gl_FragColor.",strlen("gl_FragColor."))) goto done;
  const char *p=mask+strlen("gl_FragColor.");
  char swizzle[4]={0}; int count=0;
  while(count<3 && (*p=='r'||*p=='g'||*p=='b')){
    if(strchr(swizzle,*p)) goto done;
    swizzle[count++]=*p++;
  }
  if(count<1 || *p!='=') goto done;
  p++;
  if(count==1){
    char *end=NULL; float value=strtof(p,&end);
    if(end==p || value!=0.0f){ goto done; }
    p=end;
  } else {
    char vector[16]; snprintf(vector,sizeof vector,"vec%d(",count);
    size_t length=strlen(vector);
    if(strncmp(p,vector,length)) goto done;
    p+=length;
    for(int component=0;component<count;component++){
      char *end=NULL; float value=strtof(p,&end);
      if(end==p || value!=0.0f) goto done;
      p=end;
      if(component+1<count){ if(*p!=',') goto done; p++; }
    }
    if(*p!=')') goto done;
    p++;
  }
  if(*p!=';') goto done;
  p++;
  while(*p=='}') p++;
  if(*p) goto done;
  for(int channel=0;channel<count;channel++)
    keep&=~(swizzle[channel]=='r'?4:swizzle[channel]=='g'?2:1);
  sp->channel_mask=1;
  sp->channel_mask_keep=keep;
  ok=1;
done:
  free(compact);
  return ok;
}
static int glsl_take_text(const char **cursor,const char *text){
  size_t n=strlen(text); if(strncmp(*cursor,text,n)) return 0; *cursor+=n; return 1;
}
static int glsl_take_float(const char **cursor,float *out){
  char *end=NULL; float value=strtof(*cursor,&end);
  if(end==*cursor || !isfinite(value)) return 0;
  *cursor=end; *out=value; return 1;
}
static int glsl_vec3_uniform_named(const char *source,const char *name){
  char declaration[80];
  /* A name too long to spell here would be searched for as a truncated prefix, which is a
   * different question and can only answer yes by accident. */
  int written=snprintf(declaration,sizeof declaration,"uniform vec3 %s;",name);
  if(written<0 || (size_t)written>=sizeof declaration) return 0;
  return strstr(source,declaration)!=NULL;
}
static int glsl_take_vec3_symbol(const char **cursor,char *out,size_t capacity){
  char first[32];
  if(!glsl_take_symbol(cursor,first,sizeof first)) return 0;
  if(**cursor!='('){
    if(strlen(first)>=capacity) return 0;
    snprintf(out,capacity,"%s",first);
    return 1;
  }
  if(strcmp(first,"vec3")) return 0;
  (*cursor)++;
  if(!glsl_take_symbol(cursor,out,capacity) || **cursor!=')') return 0;
  (*cursor)++;
  return 1;
}
/* Recognize a sampled ten-colour threshold palette from its complete main-body graph. The
 * source colour is divided into two red families, and four descending green thresholds choose
 * five uniform RGB values inside each family. Deriving every identifier and literal keeps this
 * data-driven and rejects shaders that merely contain a similar comparison. */
static int glsl_parse_threshold_palette(const char *src,struct GmlShaderPal *sp){
  char *compact=glsl_compact_source(src); if(!compact) return 0;
  int ok=0;
  const char *cursor=strstr(compact,"void main()");
  char sampled[32]="",uv[32]="",vertex_colour[32]="",expected[160];
  char names[10][32]={{0}};
  float red=0.0f,green[2][4]={{0}};
  int braced=0;
  if(!cursor || glsl_token_count(cursor,"texture2D(gm_BaseTexture,")!=2 ||
     glsl_token_count(cursor,"gl_FragColor")!=11) goto done;
  if(!glsl_take_text(&cursor,"void main(){vec4 ") ||
     !glsl_take_symbol(&cursor,sampled,sizeof sampled)) goto done;
  if(!glsl_take_text(&cursor,"=texture2D(gm_BaseTexture,") ||
     !glsl_take_symbol(&cursor,uv,sizeof uv) || !glsl_take_text(&cursor,");gl_FragColor=") ||
     !glsl_take_symbol(&cursor,vertex_colour,sizeof vertex_colour)) goto done;
  snprintf(expected,sizeof expected,"*texture2D(gm_BaseTexture,%s);if(%s.r<",uv,sampled);
  if(!glsl_take_text(&cursor,expected) || !glsl_take_float(&cursor,&red) ||
     !glsl_take_text(&cursor,")")) goto done;
  if(*cursor=='{'){ braced=1; cursor++; }
  for(int branch=0;branch<2;branch++){
    for(int band=0;band<4;band++){
      snprintf(expected,sizeof expected,"%sif(%s.g>",band?"else ":"",sampled);
      if(!glsl_take_text(&cursor,expected)) goto done;
      if(!glsl_take_float(&cursor,&green[branch][band])) goto done;
      if(!glsl_take_text(&cursor,")gl_FragColor.rgb=")) goto done;
      if(!glsl_take_vec3_symbol(&cursor,names[branch*5+band],sizeof names[0])) goto done;
      if(!glsl_take_text(&cursor,";")) goto done;
    }
    if(!glsl_take_text(&cursor,"else gl_FragColor.rgb=") ||
       !glsl_take_vec3_symbol(&cursor,names[branch*5+4],sizeof names[0]) ||
       !glsl_take_text(&cursor,";")) goto done;
    if(branch==0){
      if(braced && !glsl_take_text(&cursor,"}")) goto done;
      if(!glsl_take_text(&cursor,"else")) goto done;
      if(braced && !glsl_take_text(&cursor,"{")) goto done;
      if(!braced && *cursor==' ') cursor++;
    } else if(braced && !glsl_take_text(&cursor,"}")) goto done;
  }
  if(!glsl_take_text(&cursor,"}") || *cursor) goto done;
  if(red<0.0f || red>1.0f) goto done;
  for(int branch=0;branch<2;branch++) for(int band=0;band<4;band++){
    if(green[branch][band]<0.0f || green[branch][band]>1.0f ||
       (band && green[branch][band]>=green[branch][band-1])) goto done;
  }
  for(int index=0;index<10;index++){
    if(!glsl_vec3_uniform_named(compact,names[index])) goto done;
    for(int other=0;other<index;other++) if(!strcmp(names[index],names[other])) goto done;
  }
  if(!glsl_vec3_uniform_named(compact,names[0])) goto done;
  snprintf(expected,sizeof expected,"varying vec2 %s;",uv);
  if(!strstr(compact,expected)) goto done;
  snprintf(expected,sizeof expected,"varying vec4 %s;",vertex_colour);
  if(!strstr(compact,expected)) goto done;
  sp->threshold_palette=1;
  sp->threshold_palette_red=red;
  memcpy(sp->threshold_palette_green,green,sizeof green);
  for(int index=0;index<10;index++){
    /* A truncated uniform name would be looked up as a different uniform and silently miss. */
    int written=snprintf(sp->threshold_palette_uniform[index],
                         sizeof sp->threshold_palette_uniform[index],"%s",names[index]);
    if(written<0 || (size_t)written>=sizeof sp->threshold_palette_uniform[index]){
      sp->threshold_palette=0;
      goto done;
    }
  }
  ok=1;
done:
  free(compact);
  return ok;
}
static void glsl_rgb8(const float source[3],uint8_t destination[3]){
  for(int component=0;component<3;component++){
    int value=(int)floorf(source[component]*255.0f+0.5f);
    if(value<0)value=0; else if(value>255)value=255;
    destination[component]=(uint8_t)value;
  }
}
static int glsl_take_id_assignment(const char **cursor,const char *name,float expected_value){
  char expected[64]; float value=0.0f;
  snprintf(expected,sizeof expected,"%s=",name);
  return glsl_take_text(cursor,expected) && glsl_take_float(cursor,&value) &&
         fabsf(value-expected_value)<1.0e-6f && glsl_take_text(cursor,";");
}
static int glsl_take_fragment_rgb(const char **cursor,uint8_t colour[3]){
  float parsed[3];
  if(!glsl_take_text(cursor,"gl_FragColor.rgb=") ||
     !glsl_take_literal_vec3(cursor,parsed) || !glsl_take_text(cursor,";")) return 0;
  glsl_rgb8(parsed,colour);
  return 1;
}
static int glsl_take_indexed_ramp(
    const char **cursor,const char *id,const char *comparison,int threshold_count,
    float *thresholds,uint8_t (*colours)[3]){
  char expected[96];
  for(int index=0;index<threshold_count;index++){
    snprintf(expected,sizeof expected,"%sif(%s%s",index?"else ":"",id,comparison);
    if(!glsl_take_text(cursor,expected) || !glsl_take_float(cursor,&thresholds[index]) ||
       !glsl_take_text(cursor,")") || !glsl_take_fragment_rgb(cursor,colours[index])) return 0;
  }
  return glsl_take_text(cursor,"else ") &&
         glsl_take_fragment_rgb(cursor,colours[threshold_count]);
}
/* Recognize a scalar brightness shift over a sampled ten-colour index. The shader derives an id
 * from two threshold families, adds one uniform, and maps it through literal low/high ramps. A
 * narrow negative branch can retain extra resolution around the two darkest non-black ids. The
 * operation graph, identifiers, comparisons, thresholds and colours all come from the fragment. */
static int glsl_parse_indexed_brightness(const char *src,struct GmlShaderPal *sp){
  char *compact=glsl_compact_source(src); if(!compact) return 0;
  int ok=0;
  const char *cursor=strstr(compact,"void main()");
  char sampled[32]="",uv[32]="",tint[32]="",id[32]="",uniform[32]="",expected[192];
  float red=0.0f,green[2][4]={{0}};
  float family_cut=0.0f,negative_cut=0.0f,special_min=0.0f,special_max=0.0f;
  float special_threshold[2]={0},low_threshold[6]={0},high_threshold[7]={0};
  uint8_t special_colour[3][3]={{0}},low_colour[7][3]={{0}},high_colour[8][3]={{0}};
  if(!cursor || glsl_token_count(cursor,"texture2D(gm_BaseTexture,")!=2) goto done;
  if(!glsl_take_text(&cursor,"void main(){vec4 ") ||
     !glsl_take_symbol(&cursor,sampled,sizeof sampled) ||
     !glsl_take_text(&cursor,"=texture2D(gm_BaseTexture,") ||
     !glsl_take_symbol(&cursor,uv,sizeof uv) || !glsl_take_text(&cursor,");gl_FragColor=") ||
     !glsl_take_symbol(&cursor,tint,sizeof tint)) goto done;
  snprintf(expected,sizeof expected,"*texture2D(gm_BaseTexture,%s);float ",uv);
  if(!glsl_take_text(&cursor,expected) || !glsl_take_symbol(&cursor,id,sizeof id)) goto done;
  snprintf(expected,sizeof expected,";if(%s.r<",sampled);
  if(!glsl_take_text(&cursor,expected) || !glsl_take_float(&cursor,&red) ||
     !glsl_take_text(&cursor,"){")) goto done;
  for(int branch=0;branch<2;branch++){
    for(int band=0;band<4;band++){
      snprintf(expected,sizeof expected,"%sif(%s.g>",band?"else ":"",sampled);
      if(!glsl_take_text(&cursor,expected) || !glsl_take_float(&cursor,&green[branch][band]) ||
         !glsl_take_text(&cursor,")") ||
         !glsl_take_id_assignment(&cursor,id,(float)(branch?9-band:4-band))) goto done;
    }
    if(!glsl_take_text(&cursor,"else ") ||
       !glsl_take_id_assignment(&cursor,id,(float)(branch?5:0)) ||
       !glsl_take_text(&cursor,"}")) goto done;
    if(branch==0 && !glsl_take_text(&cursor,"else{")) goto done;
  }
  snprintf(expected,sizeof expected,"if(%s<",id);
  if(!glsl_take_text(&cursor,expected) || !glsl_take_float(&cursor,&family_cut) ||
     !glsl_take_text(&cursor,"){if((") ||
     !glsl_take_symbol(&cursor,uniform,sizeof uniform) ||
     !glsl_take_text(&cursor,"<") || !glsl_take_float(&cursor,&negative_cut)) goto done;
  snprintf(expected,sizeof expected,")&&(%s>",id);
  if(!glsl_take_text(&cursor,expected) || !glsl_take_float(&cursor,&special_min)) goto done;
  snprintf(expected,sizeof expected,"&&%s<",id);
  if(!glsl_take_text(&cursor,expected) || !glsl_take_float(&cursor,&special_max) ||
     !glsl_take_text(&cursor,")){")) goto done;
  snprintf(expected,sizeof expected,"%s+=%s;",id,uniform);
  if(!glsl_take_text(&cursor,expected) ||
     !glsl_take_indexed_ramp(&cursor,id,">",2,special_threshold,special_colour) ||
     !glsl_take_text(&cursor,"}else{") || !glsl_take_text(&cursor,expected) ||
     !glsl_take_indexed_ramp(&cursor,id,"<",6,low_threshold,low_colour) ||
     !glsl_take_text(&cursor,"}}else{") || !glsl_take_text(&cursor,expected) ||
     !glsl_take_indexed_ramp(&cursor,id,"<",7,high_threshold,high_colour) ||
     !glsl_take_text(&cursor,"}}") || *cursor) goto done;
  snprintf(expected,sizeof expected,"uniform float %s;",uniform);
  if(!strstr(compact,expected) || red<0.0f || red>1.0f || family_cut<0.0f ||
     special_min>=special_max || special_threshold[0]<=special_threshold[1]) goto done;
  for(int branch=0;branch<2;branch++) for(int band=0;band<4;band++)
    if(green[branch][band]<0.0f || green[branch][band]>1.0f ||
       (band && green[branch][band]>=green[branch][band-1])) goto done;
  for(int index=1;index<6;index++)
    if(low_threshold[index]<=low_threshold[index-1]) goto done;
  for(int index=1;index<7;index++)
    if(high_threshold[index]<=high_threshold[index-1]) goto done;
  sp->indexed_brightness=1;
  snprintf(sp->indexed_brightness_uniform,sizeof sp->indexed_brightness_uniform,"%s",uniform);
  sp->indexed_brightness_red=red;
  memcpy(sp->indexed_brightness_green,green,sizeof green);
  sp->indexed_brightness_family_cut=family_cut;
  sp->indexed_brightness_negative_cut=negative_cut;
  sp->indexed_brightness_special_min=special_min;
  sp->indexed_brightness_special_max=special_max;
  memcpy(sp->indexed_brightness_special_threshold,special_threshold,sizeof special_threshold);
  memcpy(sp->indexed_brightness_special_colour,special_colour,sizeof special_colour);
  memcpy(sp->indexed_brightness_low_threshold,low_threshold,sizeof low_threshold);
  memcpy(sp->indexed_brightness_low_colour,low_colour,sizeof low_colour);
  memcpy(sp->indexed_brightness_high_threshold,high_threshold,sizeof high_threshold);
  memcpy(sp->indexed_brightness_high_colour,high_colour,sizeof high_colour);
  ok=1;
done:
  free(compact);
  return ok;
}
/* Recognize a three-channel sampled HSV post-process by its complete operation graph. Local names
 * are derived from declarations, every artistic control is parsed from the fragment, and partial
 * matches are rejected. This intentionally evaluates a shader family rather than an asset name. */
static int glsl_parse_grayscale(const char *src, struct GmlShaderPal *sp){
  if(!strstr(src,"luminance") || !strstr(src,"dot(") ||
     !strstr(src,"vec4(luminance,luminance,luminance") ||
     !glsl_parse_const_vec3(src,sp->grayscale_weight)) return 0;
  sp->grayscale=1;
  sp->grayscale_alpha=1.0f;
  if(glsl_uniform_name(src,"float",0,sp->grayscale_alpha_uniform,sizeof sp->grayscale_alpha_uniform)){
    sp->grayscale_has_alpha_uniform=1;
    sp->grayscale_alpha=0.0f; /* GLSL user uniforms initialize to zero until the game supplies one. */
  }
  return 1;
}
/* Recognize a pass-through texture fragment whose only post-sample operation clears alpha below a
 * literal RGB-channel threshold. This is the fixed-function colour-key shape used to remove a
 * dark backing rectangle while preserving the sampled RGB. Accept the complete main-body graph so
 * a shader that performs additional colour work cannot be approximated as a key. */
static int glsl_parse_channel_alpha_key(const char *src,struct GmlShaderPal *sp){
  char *compact=glsl_compact_source(src); if(!compact) return 0;
  int ok=0;
  const char *mainfn=strstr(compact,"void main()");
  if(!mainfn || glsl_token_count(mainfn,"gl_FragColor")!=3) goto done;
  const char *body=mainfn+strlen("void main()");
  if(*body!='{') goto done;
  body++;
  /* Some exporters leave one unused local copy of the base sample before assigning the fragment.
   * It has no output semantics, but require its exact single-sample declaration when present. */
  if(!strncmp(body,"vec4 ",5)){
    const char *name=body+5;
    static const char sample_copy[]="=texture2D(gm_BaseTexture,v_vTexcoord);";
    if(!(isalpha((unsigned char)*name)||*name=='_')) goto done;
    while(isalnum((unsigned char)*name)||*name=='_') name++;
    if(strncmp(name,sample_copy,sizeof sample_copy-1)) goto done;
    body=name+sizeof sample_copy-1;
  }
  static const char *const sample_forms[]={
    "gl_FragColor=v_vColour*texture2D(gm_BaseTexture,v_vTexcoord);",
    "gl_FragColor=texture2D(gm_BaseTexture,v_vTexcoord)*v_vColour;"
  };
  const char *after_sample=NULL;
  for(unsigned form=0;form<sizeof sample_forms/sizeof sample_forms[0];form++){
    size_t length=strlen(sample_forms[form]);
    if(!strncmp(body,sample_forms[form],length)){ after_sample=body+length; break; }
  }
  if(!after_sample || strncmp(after_sample,"if(gl_FragColor.",16)) goto done;
  const char *p=after_sample+16;
  int channel=*p=='r'?0:*p=='g'?1:*p=='b'?2:-1;
  if(channel<0) goto done;
  p++;
  int inclusive=0;
  if(p[0]=='<' && p[1]=='='){ inclusive=1; p+=2; }
  else if(*p=='<') p++;
  else goto done;
  char *end=NULL; float cutoff=strtof(p,&end);
  if(end==p || cutoff<0.0f || cutoff>1.0f || *end!=')') goto done;
  end++;
  if(*end=='{') end++;
  static const char clear[]="gl_FragColor.a=0.0;";
  if(strncmp(end,clear,sizeof clear-1)) goto done;
  end+=sizeof clear-1;
  if(*end=='}') end++;
  while(*end==';'||*end=='}') end++;
  if(*end) goto done;
  sp->channel_alpha_key=1;
  sp->channel_alpha_key_channel=channel;
  sp->channel_alpha_key_inclusive=inclusive;
  sp->channel_alpha_key_cutoff=cutoff;
  ok=1;
done:
  free(compact);
  return ok;
}
/* Recognize the deliberately small pass-through family that samples gm_BaseTexture once, rejects
 * texels below a literal alpha threshold, and returns that sample unchanged. Restricting the
 * accepted operation graph keeps shaders with unrelated colour/effect math on the unknown path. */
static int glsl_parse_alpha_discard_passthrough(const char *src, struct GmlShaderPal *sp){
  const char *mainfn=strstr(src,"void main");
  if(!mainfn) return 0;
  const char *sample=strstr(mainfn,"texture2D(");
  if(!sample || !strstr(sample,"gm_BaseTexture") || strstr(sample+10,"texture2D(")) return 0;
  char value[48]="";
  if(!glsl_texture_lhs(mainfn,sample,value,sizeof value)) return 0;
  const char *frag=strstr(sample,"gl_FragColor");
  if(!frag) return 0;
  const char *eq=strchr(frag,'=');
  if(!eq) return 0;
  eq++; while(*eq==' '||*eq=='\t') eq++;
  size_t vl=strlen(value);
  if(strncmp(eq,value,vl) || (isalnum((unsigned char)eq[vl])||eq[vl]=='_')) return 0;
  eq+=vl; while(*eq==' '||*eq=='\t') eq++;
  if(*eq!=';') return 0;
  char alpha_pat[56]; snprintf(alpha_pat,sizeof alpha_pat,"%s.a",value);
  const char *cond=strstr(sample,alpha_pat);
  if(!cond || cond>frag) return 0;
  const char *q=cond+strlen(alpha_pat);
  while(*q==' '||*q=='\t') q++;
  int inclusive=0;
  if(q[0]=='<' && q[1]=='='){ inclusive=1; q+=2; }
  else if(q[0]=='<') q++;
  else return 0;
  while(*q==' '||*q=='\t') q++;
  char *end=NULL; float cutoff=strtof(q,&end);
  if(end==q || cutoff<0.0f || cutoff>1.0f) return 0;
  const char *discard=strstr(end,"discard");
  if(!discard || discard>frag || (size_t)(discard-end)>96) return 0;
  sp->alpha_discard=1;
  sp->alpha_discard_inclusive=inclusive;
  sp->alpha_discard_cutoff=cutoff;
  return 1;
}
/* Recognize the four-colour intensity quantiser by its operation graph. The accepted shape is an
 * average of the three sampled channels, three threshold comparisons scaled by two, three and four,
 * exactly four vec3 uniforms taken in declaration order as darkest to lightest, and an output that
 * replaces alpha with one. The thresholds are read from the source rather than assumed, so a sheet
 * that moves them still resolves. */
static int glsl_parse_quantise4(const char *src, struct GmlShaderPal *sp){
  char *compact=glsl_compact_source(src);
  if(!compact) return 0;
  int ok=0;
  do {
    if(!strstr(compact,")/3.0")) break;
    if(!strstr(compact,",1.0);")) break;
    /* three comparisons, each scaled by its band number */
    float threshold[3]; int found=0;
    for(const char *p=compact;(p=strstr(p,">"))!=NULL;p++){
      char *end=NULL; float value=strtof(p+1,&end);
      if(end==p+1) continue;
      if(strncmp(end,")*",2)) continue;
      int band=(int)strtol(end+2,NULL,10);
      if(band<2 || band>4) continue;
      if(band-2!=found) continue;
      threshold[found++]=value;
      if(found==3) break;
    }
    if(found!=3) break;
    if(!(threshold[0]<threshold[1] && threshold[1]<threshold[2])) break;
    /* four vec3 uniforms, in declaration order */
    int n=0;
    const char *decl=compact;
    while(n<4 && (decl=strstr(decl,"uniform vec3 "))!=NULL){
      decl+=13;
      size_t k=0;
      while(k+1<sizeof sp->quantise4_uniform[0] &&
            (isalnum((unsigned char)decl[k])||decl[k]=='_')){
        sp->quantise4_uniform[n][k]=decl[k]; k++;
      }
      sp->quantise4_uniform[n][k]='\0';
      if(k) n++;
    }
    if(n!=4 || strstr(decl,"uniform vec3 ")) break;
    sp->quantise4=1;
    sp->quantise4_set=0;
    for(int i=0;i<3;i++) sp->quantise4_threshold[i]=threshold[i];
    ok=1;
  } while(0);
  free(compact);
  return ok;
}

/* Recognize the ordered-dither cutout family. The accepted graph is deliberately narrow: exactly
 * one gm_BaseTexture sample multiplied by the vertex colour, a 4x4 matrix whose cell count comes
 * from one float uniform quantised by sixteen, both position axes wrapped to that matrix by the
 * floor(p/4)*4 identity, and a final alpha assignment that keeps a cell's value only where the
 * sampled alpha is already non-zero. The uniform's name is taken from the call that builds the
 * pattern and confirmed against its declaration, so nothing here depends on an asset name. */
static int glsl_parse_ordered_dither(const char *src, struct GmlShaderPal *sp){
  char *compact=glsl_compact_source(src);
  if(!compact) return 0;
  int ok=0;
  do {
    /* One sample of the base texture, and only one. */
    const char *sample=strstr(compact,"texture2D(gm_BaseTexture");
    if(!sample || strstr(sample+8,"texture2D(")) break;
    /* The seventeen-level quantiser is what makes the pattern reproducible exactly. */
    const char *quant=strstr(compact,"*16.0)");
    if(!quant) break;
    /* Both axes wrapped into the matrix by the same floor identity. */
    if(!strstr(compact,"/4.0)*4.0")) break;
    /* The alpha assignment: a matrix cell gated by the sign of the sampled alpha. */
    const char *assign=strstr(compact,"gl_FragColor.a=");
    if(!assign || !strstr(assign,"sign(gl_FragColor.a)")) break;
    if(!memchr(assign,'[',(size_t)(strstr(assign,"sign(gl_FragColor.a)")-assign))) break;
    /* The uniform: named by the call that builds the pattern, confirmed by its declaration. The
     * compactor keeps one space between adjacent identifiers, so the declaration is spelled with
     * it; the preamble's own float uniforms appear first and are rejected by the tests below. */
    (void)quant;
    char name[32]="";
    const char *decl=compact;
    while((decl=strstr(decl,"uniform float "))!=NULL){
      decl+=14;
      size_t n=0;
      while(n+1<sizeof name && (isalnum((unsigned char)decl[n])||decl[n]=='_')) { name[n]=decl[n]; n++; }
      name[n]='\0';
      if(!n) continue;
      /* The declared uniform has to be the value the quantiser consumes. */
      char consumed[48];
      snprintf(consumed,sizeof consumed,"%s*16.0)",name);
      if(strstr(compact,consumed)) break;
      char called[40];
      snprintf(called,sizeof called,"(%s)",name);
      if(strstr(compact,called)) break;
      name[0]='\0';
    }
    if(!name[0]) break;
    sp->ordered_dither=1;
    snprintf(sp->ordered_dither_uniform,sizeof sp->ordered_dither_uniform,"%s",name);
    /* Until the run sets it, nothing is asked for and nothing is drawn: that is what a zero fade
     * means, and guessing full coverage instead is the defect this family exists to remove. */
    sp->ordered_dither_alpha=0.0f;
    ok=1;
  } while(0);
  free(compact);
  return ok;
}

/* Read one GLSL identifier ending at `end`, walking back over its characters. */
static int glsl_identifier_before(const char *start,const char *end,char *out,size_t size){
  const char *cursor=end;
  while(cursor>start && (isalnum((unsigned char)cursor[-1]) || cursor[-1]=='_')) cursor--;
  size_t length=(size_t)(end-cursor);
  if(!length || length>=size) return 0;
  memcpy(out,cursor,length);
  out[length]=0;
  return 1;
}
/* Recognize a vertex program that modulates its outgoing colour varying by a uniform vec4, and
 * name that uniform. Both forms text libraries emit are accepted: the whole varying multiplied by
 * the uniform, and the alpha channel alone multiplied by the uniform's alpha. The uniform must be
 * declared vec4 in this same program, so an unrelated multiply by a varying or an attribute is not
 * mistaken for one. Everything else about the program is deliberately left free: what matters to
 * this renderer is only that the vertex colour reaching the rasterizer is the authored colour
 * times that uniform. */
static int glsl_parse_vertex_colour_blend(const char *src,struct GmlShaderPal *sp){
  char *compact=glsl_compact_source(src);
  if(!compact) return 0;
  int ok=0;
  const char *mainfn=strstr(compact,"void main()");
  if(!mainfn) goto done;
  for(const char *cursor=strstr(mainfn,"*=");cursor;cursor=strstr(cursor+2,"*=")){
    char target[64],uniform[64];
    const char *target_end=cursor;
    int alpha_only=0;
    if(target_end>mainfn && target_end[-1]=='a' && target_end-1>mainfn && target_end[-2]=='.'){
      alpha_only=1;
      target_end-=2;
    }
    if(!glsl_identifier_before(mainfn,target_end,target,sizeof target)) continue;
    const char *value=cursor+2;
    const char *value_end=value;
    while(isalnum((unsigned char)*value_end) || *value_end=='_') value_end++;
    if(!glsl_identifier_before(value,value_end,uniform,sizeof uniform)) continue;
    /* The alpha-only form may spell the right-hand side as `<uniform>.a`; either is the same
     * uniform, and the channel it reads is decided by the assignment target. */
    if(*value_end=='.' && value_end[1]=='a') value_end+=2;
    if(*value_end!=';') continue;
    char declaration[96];
    snprintf(declaration,sizeof declaration,"uniform vec4 %s;",uniform);
    if(!strstr(compact,declaration)) continue;
    /* The target must be a varying this program writes out, not a local scratch value. */
    char varying[96];
    snprintf(varying,sizeof varying,"varying vec4 %s;",target);
    if(!strstr(compact,varying)) continue;
    sp->vertex_colour_blend=1;
    sp->vertex_colour_blend_alpha_only=alpha_only;
    snprintf(sp->vertex_colour_blend_uniform,sizeof sp->vertex_colour_blend_uniform,"%s",uniform);
    /* Until the content sets it, the modulation is the identity: a shader whose uniform never
     * arrives must not silently blank every string drawn through it. */
    for(int channel=0;channel<4;channel++) sp->vertex_colour_blend_value[channel]=1.0f;
    sp->vertex_colour_blend_set=0;
    ok=1;
    /* A program commonly carries both forms behind a compile-time switch: the alpha-only line and
     * the whole-colour one. The whole-colour form is the more complete statement of the same
     * modulation, so keep looking and prefer it when the program has one. */
    if(!alpha_only) break;
  }
done:
  free(compact);
  return ok;
}

/* Recognize a single-sample constant-colour alpha mask by its complete main-body graph. Identifier
 * spelling and insignificant formatting are intentionally free, while additional samples, colour
 * operations or statements keep a fragment on the unknown-shader path. */
static int glsl_parse_solid_alpha_mask(const char *src, struct GmlShaderPal *sp){
  char *compact=glsl_compact_source(src);
  if(!compact) return 0;
  int ok=0,inclusive=0,braced=0;
  float cutoff=0.0f,zero=1.0f;
  char sample_name[32]="",uniform_name[32]="",declaration[96],expected[128];
  const char *mainfn=strstr(compact,"void main()");
  const char *cursor=mainfn?strchr(mainfn,'{'):NULL;
  const char *body_end=NULL;
  if(!cursor) goto done;
  {
    int depth=1;
    body_end=cursor+1;
    while(*body_end && depth){
      if(*body_end=='{') depth++;
      else if(*body_end=='}') depth--;
      if(depth) body_end++;
    }
    if(depth || !body_end) goto done;
  }
  cursor++;
  if(!strncmp(cursor,"lowp ",5)) cursor+=5;
  else if(!strncmp(cursor,"mediump ",8)) cursor+=8;
  else if(!strncmp(cursor,"highp ",6)) cursor+=6;
  if(!glsl_take_text(&cursor,"vec4 ")) goto done;
  {
    int length=0;
    while(length<(int)sizeof(sample_name)-1 &&
          (isalnum((unsigned char)cursor[length])||cursor[length]=='_')){
      sample_name[length]=cursor[length]; length++;
    }
    sample_name[length]=0;
    if(!length) goto done;
    cursor+=length;
  }
  if(!glsl_take_text(&cursor,"=texture2D(gm_BaseTexture,")) goto done;
  {
    int depth=1;
    while(*cursor && depth){
      if(!strncmp(cursor,"texture2D(",10)) goto done;
      if(*cursor=='(') depth++;
      else if(*cursor==')') depth--;
      cursor++;
    }
    if(depth || !glsl_take_text(&cursor,";")) goto done;
  }
  snprintf(expected,sizeof expected,"if(%s.a",sample_name);
  if(!glsl_take_text(&cursor,expected)) goto done;
  if(glsl_take_text(&cursor,"<=")) inclusive=1;
  else if(!glsl_take_text(&cursor,"<")) goto done;
  if(!glsl_take_float(&cursor,&cutoff) || cutoff<0.0f || cutoff>1.0f ||
     !glsl_take_text(&cursor,")")) goto done;
  if(glsl_take_text(&cursor,"{")) braced=1;
  snprintf(expected,sizeof expected,"%s.a=",sample_name);
  if(!glsl_take_text(&cursor,expected) || !glsl_take_float(&cursor,&zero) ||
     fabsf(zero)>1e-8f || !glsl_take_text(&cursor,";")) goto done;
  if(braced && !glsl_take_text(&cursor,"}")) goto done;
  if(!glsl_take_text(&cursor,"gl_FragColor=vec4(")) goto done;
  {
    int length=0;
    while(length<(int)sizeof(uniform_name)-1 &&
          (isalnum((unsigned char)cursor[length])||cursor[length]=='_')){
      uniform_name[length]=cursor[length]; length++;
    }
    uniform_name[length]=0;
    if(!length) goto done;
    cursor+=length;
  }
  snprintf(expected,sizeof expected,".rgb,%s.a);",sample_name);
  if(!glsl_take_text(&cursor,expected) || cursor!=body_end) goto done;
  snprintf(declaration,sizeof declaration,"uniform vec3 %s;",uniform_name);
  if(!strstr(compact,declaration)) goto done;
  sp->solid_alpha_mask=1;
  sp->solid_alpha_mask_inclusive=inclusive;
  sp->solid_alpha_mask_cutoff=cutoff;
  sp->solid_alpha_mask_cutoff_step=inclusive
    ? (int)floorf(cutoff*255.0f)
    : (int)ceilf(cutoff*255.0f);
  snprintf(sp->solid_alpha_mask_uniform,sizeof sp->solid_alpha_mask_uniform,
           "%s",uniform_name);
  ok=1;
done:
  free(compact);
  return ok;
}
static int glsl_take_identifier(const char **cursor,char *out,size_t capacity){
  size_t length=0;
  if(!cursor || !*cursor || !out || capacity<2) return 0;
  while((isalnum((unsigned char)(*cursor)[length])||(*cursor)[length]=='_') &&
        length+1<capacity){
    out[length]=(*cursor)[length];
    length++;
  }
  if(!length || isdigit((unsigned char)out[0])) return 0;
  out[length]=0;
  *cursor+=length;
  return 1;
}
static int glsl_take_vec2(const char **cursor,float *x,float *y){
  return glsl_take_text(cursor,"vec2(") && glsl_take_float(cursor,x) &&
         glsl_take_text(cursor,",") && glsl_take_float(cursor,y) &&
         glsl_take_text(cursor,")");
}
static int glsl_take_vec4_zero(const char **cursor){
  float value[4];
  if(!glsl_take_text(cursor,"vec4(")) return 0;
  for(int component=0;component<4;component++){
    if(!glsl_take_float(cursor,&value[component]) ||
       fabsf(value[component])>1e-8f ||
       (component<3 && !glsl_take_text(cursor,","))) return 0;
  }
  return glsl_take_text(cursor,")");
}
static int glsl_take_weighted_alpha_sample(
    const char **cursor,const char *accumulator,const char *offset_name,
    char coordinate_name[32],int multiply_accumulator,float *offset,float *weight){
  char expected[96],identifier[32]="";
  snprintf(expected,sizeof expected,"%s+=texture2D(gm_BaseTexture,",accumulator);
  if(!glsl_take_text(cursor,expected) ||
     !glsl_take_identifier(cursor,identifier,sizeof identifier)) return 0;
  if(coordinate_name[0]){
    if(strcmp(identifier,coordinate_name)) return 0;
  } else {
    snprintf(coordinate_name,32,"%s",identifier);
  }
  *offset=0.0f;
  if(**cursor=='+' || **cursor=='-'){
    int sign=*(*cursor)++=='-'?-1:1;
    float magnitude=0.0f;
    if(!glsl_take_float(cursor,&magnitude) || magnitude<0.0f ||
       !glsl_take_text(cursor,"*") ||
       !glsl_take_text(cursor,offset_name)) return 0;
    *offset=sign*magnitude;
  }
  if(!glsl_take_text(cursor,")*") || !glsl_take_float(cursor,weight) ||
     *weight<0.0f || *weight>4.0f) return 0;
  if(multiply_accumulator){
    if(!glsl_take_text(cursor,"*") ||
       !glsl_take_text(cursor,accumulator)) return 0;
  }
  return glsl_take_text(cursor,";");
}
/* Recognize a constant-colour alpha-convolution fragment from its complete main-body graph. The
 * first pass is a weighted one-dimensional texture sum. The second pass feeds weighted samples
 * back into that accumulator multiplicatively. Identifiers, steps, offsets and weights are all
 * parsed from the embedded program; extra statements or samples reject the match. */
static int glsl_parse_solid_blur_alpha(const char *src,struct GmlShaderPal *sp){
  char *compact=glsl_compact_source(src);
  if(!compact) return 0;
  int ok=0;
  char accumulator[32]="",offset_name[32]="",coordinate_name[32]="";
  char uniform_name[32]="",expected[128],declaration[96];
  const char *mainfn=strstr(compact,"void main()");
  const char *cursor=mainfn?strchr(mainfn,'{'):NULL;
  const char *body_end=NULL;
  float zero=0.0f;
  if(!cursor) goto done;
  {
    int depth=1;
    body_end=cursor+1;
    while(*body_end && depth){
      if(*body_end=='{') depth++;
      else if(*body_end=='}') depth--;
      if(depth) body_end++;
    }
    if(depth) goto done;
  }
  cursor++;
  if(!strncmp(cursor,"lowp ",5)) cursor+=5;
  else if(!strncmp(cursor,"mediump ",8)) cursor+=8;
  else if(!strncmp(cursor,"highp ",6)) cursor+=6;
  if(!glsl_take_text(&cursor,"vec4 ") ||
     !glsl_take_identifier(&cursor,accumulator,sizeof accumulator) ||
     !glsl_take_text(&cursor,"=") || !glsl_take_vec4_zero(&cursor) ||
     !glsl_take_text(&cursor,";vec2 ") ||
     !glsl_take_identifier(&cursor,offset_name,sizeof offset_name) ||
     !glsl_take_text(&cursor,"=") ||
     !glsl_take_vec2(&cursor,&sp->solid_blur_alpha_step_x,&zero) ||
     fabsf(zero)>1e-8f || sp->solid_blur_alpha_step_x<=0.0f ||
     sp->solid_blur_alpha_step_x>1.0f ||
     !glsl_take_text(&cursor,";")) goto done;
  while(sp->solid_blur_alpha_x_count<16){
    snprintf(expected,sizeof expected,"%s+=",accumulator);
    if(strncmp(cursor,expected,strlen(expected))) break;
    int index=sp->solid_blur_alpha_x_count;
    if(!glsl_take_weighted_alpha_sample(
         &cursor,accumulator,offset_name,coordinate_name,0,
         &sp->solid_blur_alpha_x_offset[index],
         &sp->solid_blur_alpha_x_weight[index])) goto done;
    sp->solid_blur_alpha_x_count++;
  }
  if(sp->solid_blur_alpha_x_count<3) goto done;
  snprintf(expected,sizeof expected,"%s=vec2(",offset_name);
  if(!glsl_take_text(&cursor,expected) ||
     !glsl_take_float(&cursor,&zero) || fabsf(zero)>1e-8f ||
     !glsl_take_text(&cursor,",") ||
     !glsl_take_float(&cursor,&sp->solid_blur_alpha_step_y) ||
     sp->solid_blur_alpha_step_y<=0.0f || sp->solid_blur_alpha_step_y>1.0f ||
     !glsl_take_text(&cursor,");")) goto done;
  while(sp->solid_blur_alpha_y_count<16){
    snprintf(expected,sizeof expected,"%s+=",accumulator);
    if(strncmp(cursor,expected,strlen(expected))) break;
    int index=sp->solid_blur_alpha_y_count;
    if(!glsl_take_weighted_alpha_sample(
         &cursor,accumulator,offset_name,coordinate_name,1,
         &sp->solid_blur_alpha_y_offset[index],
         &sp->solid_blur_alpha_y_weight[index])) goto done;
    sp->solid_blur_alpha_y_count++;
  }
  if(sp->solid_blur_alpha_y_count<2 ||
     !glsl_take_text(&cursor,"gl_FragColor=vec4(") ||
     !glsl_take_identifier(&cursor,uniform_name,sizeof uniform_name)) goto done;
  snprintf(expected,sizeof expected,".rgb,%s.a);",accumulator);
  if(!glsl_take_text(&cursor,expected) || cursor!=body_end) goto done;
  snprintf(declaration,sizeof declaration,"uniform vec3 %s;",uniform_name);
  if(!strstr(compact,declaration)) goto done;
  {
    int horizontal_negative=0,horizontal_positive=0,horizontal_centre=0;
    int vertical_negative=0,vertical_positive=0;
    for(int i=0;i<sp->solid_blur_alpha_x_count;i++){
      float offset=sp->solid_blur_alpha_x_offset[i];
      if(offset<0.0f) horizontal_negative=1;
      else if(offset>0.0f) horizontal_positive=1;
      else horizontal_centre=1;
    }
    for(int i=0;i<sp->solid_blur_alpha_y_count;i++){
      float offset=sp->solid_blur_alpha_y_offset[i];
      if(offset<0.0f) vertical_negative=1;
      else if(offset>0.0f) vertical_positive=1;
    }
    if(!horizontal_negative || !horizontal_positive || !horizontal_centre ||
       !vertical_negative || !vertical_positive) goto done;
  }
  snprintf(sp->solid_blur_alpha_uniform,sizeof sp->solid_blur_alpha_uniform,
           "%s",uniform_name);
  sp->solid_blur_alpha=1;
  ok=1;
done:
  if(!ok){
    sp->solid_blur_alpha=0;
    sp->solid_blur_alpha_x_count=0;
    sp->solid_blur_alpha_y_count=0;
  }
  free(compact);
  return ok;
}
/* Recognize a radial sine-displacement sampler. The fragment derives an aspect-corrected distance
 * from a user centre, multiplies the source-centred direction by sin(distance*amount-time*speed),
 * divides it by a user scalar, and adds that vector to the base texture coordinate. */
static int glsl_parse_radial_wave(const char *src, struct GmlShaderPal *sp){
  const char *mainfn=strstr(src,"void main");
  if(!mainfn || !strstr(mainfn,"texture2D(gm_BaseTexture") || !strstr(mainfn,"distance(") ||
     !strstr(mainfn,"sin(") || !strstr(mainfn,"vec2(0.5") ||
     !strstr(mainfn,"+ offset")) return 0;
  const char *decl=src;
  for(const char *vary=strstr(src,"varying ");vary && vary<mainfn;vary=strstr(vary+1,"varying "))
    decl=vary;
  char scalar[5][32]={{0}}, vector[2][32]={{0}}; int ns=0,nv=0;
  for(const char *p=decl;p && p<mainfn;){
    const char *sf=strstr(p,"uniform float "),*sv=strstr(p,"uniform vec2 ");
    const char *next=NULL; int vector_decl=0;
    if(sf && sf<mainfn) next=sf;
    if(sv && sv<mainfn && (!next || sv<next)){ next=sv; vector_decl=1; }
    if(!next) break;
    const char *name=next+(vector_decl?13:14);
    char *out=vector_decl ? (nv<2?vector[nv]:NULL) : (ns<5?scalar[ns]:NULL);
    if(out){ int j=0; for(;j<31 && (isalnum((unsigned char)name[j])||name[j]=='_');j++) out[j]=name[j];
      out[j]=0; if(j){ if(vector_decl)nv++; else ns++; } }
    p=next+1;
  }
  if(ns<4 || nv<2) return 0;
  sp->radial_wave=1;
  snprintf(sp->radial_wave_uniform[0],32,"%s",scalar[0]);
  snprintf(sp->radial_wave_uniform[1],32,"%s",vector[0]);
  snprintf(sp->radial_wave_uniform[2],32,"%s",vector[1]);
  snprintf(sp->radial_wave_uniform[3],32,"%s",scalar[1]);
  snprintf(sp->radial_wave_uniform[4],32,"%s",scalar[2]);
  snprintf(sp->radial_wave_uniform[5],32,"%s",scalar[3]);
  return 1;
}

/* Find the coordinate identifier consumed by the base-texture lookup, then copy its last active
 * assignment in main with whitespace removed.  Ignoring // lines matters because shader authors
 * often retain alternate displacement formulas beside the live one. */
static int glsl_base_coord_assignment(const char *src,char *out,size_t outsz){
  const char *mainfn=strstr(src,"void main");
  if(!mainfn || !out || outsz<2) return 0;
  const char *sample=NULL;
  for(const char *p=strstr(mainfn,"texture2D");p;p=strstr(p+1,"texture2D")){
    const char *close=strchr(p,')');
    if(close && mem_find(p,(size_t)(close-p),"gm_BaseTexture",14)) sample=p;
  }
  if(!sample) return 0;
  const char *comma=strchr(sample,','); if(!comma) return 0;
  const char *name=comma+1; while(*name==' '||*name=='\t'||*name=='\r'||*name=='\n') name++;
  char coord[48]; int cn=0;
  while(cn<(int)sizeof(coord)-1 && (isalnum((unsigned char)name[cn])||name[cn]=='_')){
    coord[cn]=name[cn]; cn++;
  }
  coord[cn]=0; if(!cn) return 0;
  const char *assignment=NULL;
  for(const char *p=strstr(mainfn,coord);p && p<sample;p=strstr(p+cn,coord)){
    if((p>mainfn && (isalnum((unsigned char)p[-1])||p[-1]=='_')) ||
       isalnum((unsigned char)p[cn]) || p[cn]=='_') continue;
    const char *q=p+cn; while(q<sample && (*q==' '||*q=='\t')) q++;
    if(q>=sample || *q!='=' || q[1]=='=') continue;
    const char *line=p; while(line>mainfn && line[-1]!='\n' && line[-1]!='\r') line--;
    if(mem_find(line,(size_t)(p-line),"//",2)) continue;
    assignment=p;
  }
  if(!assignment) return 0;
  const char *end=strchr(assignment,';'); if(!end || end>sample) return 0;
  size_t used=0;
  for(const char *p=assignment;p<end && used+1<outsz;p++)
    if(!isspace((unsigned char)*p)) out[used++]=*p;
  out[used]=0;
  return used>0;
}

/* Recognize two single-lookup horizontal displacement graphs.  The varying names are GameMaker's
 * fixed shader ABI; local names, the custom uniform and every numeric control come from GLSL. */
static int glsl_parse_uv_wave(const char *src,struct GmlShaderPal *sp){
  char expr[640],lhs[64],time_name[32]; int consumed=0;
  if(!glsl_base_coord_assignment(src,expr,sizeof expr)) return 0;
  float uv_factor=0,time_factor=0,divisor=0;
  int matched=sscanf(expr,
    "%63[A-Za-z0-9_]=v_vTexcoord+vec2(cos(v_vTexcoord.y*%f+%31[A-Za-z0-9_]*%f)/%f,0)%n",
    lhs,&uv_factor,time_name,&time_factor,&divisor,&consumed);
  if(matched==5 && consumed==(int)strlen(expr) && fabsf(divisor)>1e-8f &&
     glsl_uniform_float_named(src,time_name)){
    sp->uv_wave_mode=1;
    snprintf(sp->uv_wave_uniform,sizeof sp->uv_wave_uniform,"%s",time_name);
    sp->uv_wave_uv_factor=uv_factor;
    sp->uv_wave_time_factor=time_factor;
    sp->uv_wave_divisor=divisor;
    return 1;
  }
  char wave_x[32],wave_y[32],size_name[32]; float taper=0;
  consumed=0;
  matched=sscanf(expr,
    "%63[A-Za-z0-9_]=v_vTexcoord+vec2(sin((v_vPosition.y/%31[A-Za-z0-9_].x+%31[A-Za-z0-9_])*%f)*(%31[A-Za-z0-9_].y*v_vTexcoord.y),0)/%31[A-Za-z0-9_]*(%f-v_vTexcoord.x)%n",
    lhs,wave_x,time_name,&time_factor,wave_y,size_name,&taper,&consumed);
  float spatial=0,amplitude=0,size_x=0,size_y=0;
  if(matched==7 && consumed==(int)strlen(expr) && !strcmp(wave_x,wave_y) &&
     glsl_uniform_float_named(src,time_name) &&
     glsl_parse_vec2(src,wave_x,&spatial,&amplitude) &&
     glsl_parse_vec2(src,size_name,&size_x,&size_y) &&
     fabsf(spatial)>1e-8f && fabsf(size_x)>1e-8f){
    sp->uv_wave_mode=2;
    snprintf(sp->uv_wave_uniform,sizeof sp->uv_wave_uniform,"%s",time_name);
    sp->uv_wave_time_factor=time_factor;
    sp->uv_wave_size_x=size_x;
    sp->uv_wave_spatial=spatial;
    sp->uv_wave_amplitude=amplitude;
    sp->uv_wave_taper=taper;
    return 1;
  }
  return 0;
}

static int glsl_token_count(const char *source,const char *token){
  int count=0;
  size_t length=strlen(token);
  if(!length) return 0;
  for(const char *at=source;(at=strstr(at,token));at+=length) count++;
  return count;
}


void parse_shader_palettes(GmlRender *r){
  r->active_shader=-1;
  const GmlChunk *c=gml_chunk(r->win,"SHDR"); if(!c) return;
  const uint8_t *d=r->win->data;
  uint32_t n=u32(d,c->off);
  if(n==0 || n>4096) return;
  r->shader_pal=calloc(n,sizeof(*r->shader_pal)); if(!r->shader_pal) return;
  r->n_shader_pal=(int)n;
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    if(p==0 || (size_t)p+28>r->win->size) continue;
    /* Shader records in older and newer Studio containers place the GLSL-ES fields at slightly
     * different slots. Select a bounded source which actually has fragment output semantics;
     * this also prevents a valid vertex string from being mistaken for the fragment program. */
    uint32_t fp=0;
    static const unsigned fragment_slots[]={16,12,24,20};
    for(unsigned k=0;k<sizeof fragment_slots/sizeof fragment_slots[0];k++){
      uint32_t candidate=u32(d,p+fragment_slots[k]);
      if(!candidate || (size_t)candidate>=r->win->size) continue;
      size_t available=r->win->size-(size_t)candidate;
      size_t length=strnlen((const char*)d+candidate,available);
      if(length<available && mem_find(d+candidate,length,"gl_FragColor",12)){ fp=candidate; break; }
    }
    if(!fp) continue;
    /* The vertex program sits in one of the same candidate slots and is told apart by writing a
     * position rather than a fragment colour. A text library carries its tint and fade there. */
    for(unsigned k=0;k<sizeof fragment_slots/sizeof fragment_slots[0];k++){
      uint32_t candidate=u32(d,p+fragment_slots[k]);
      if(!candidate || candidate==fp || (size_t)candidate>=r->win->size) continue;
      size_t available=r->win->size-(size_t)candidate;
      size_t length=strnlen((const char*)d+candidate,available);
      if(length>=available || !mem_find(d+candidate,length,"gl_Position",11) ||
         mem_find(d+candidate,length,"gl_FragColor",12)) continue;
      if(glsl_parse_vertex_colour_blend((const char*)(d+candidate),&r->shader_pal[i]) &&
         render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
          "[shader] vertex-colour-blend[%u] uniform %s%s\n",i,
          r->shader_pal[i].vertex_colour_blend_uniform,
          r->shader_pal[i].vertex_colour_blend_alpha_only?" (alpha only)":"");
      break;
    }
    const char *src=(const char*)(d+fp);
    /* The four program texts, for the host's graphics context: a Studio shader record carries the
     * OpenGL ES pair at +8/+12 and the desktop GLSL pair at +16/+20. Each is accepted only when it
     * is a bounded string inside the image, and the fragment ones only when they write a fragment
     * colour, so a record laid out differently yields no program rather than a wrong one. */
    {
      static const unsigned source_slots[4]={8,12,16,20};
      const char *texts[4]={NULL,NULL,NULL,NULL};
      for(unsigned k=0;k<4;k++){
        uint32_t candidate=u32(d,p+source_slots[k]);
        size_t available,length;
        if(!candidate || (size_t)candidate>=r->win->size) continue;
        available=r->win->size-(size_t)candidate;
        length=strnlen((const char*)d+candidate,available);
        if(length>=available || length==0) continue;
        if((k&1u) && !mem_find(d+candidate,length,"gl_FragColor",12) &&
           !mem_find(d+candidate,length,"gl_FragData",11)) continue;
        if(!(k&1u) && !mem_find(d+candidate,length,"gl_Position",11)) continue;
        texts[k]=(const char*)(d+candidate);
      }
      r->shader_pal[i].source_vertex_es=texts[0];
      r->shader_pal[i].source_fragment_es=texts[1];
      r->shader_pal[i].source_vertex_gl=texts[2];
      r->shader_pal[i].source_fragment_gl=texts[3];
      /* The samplers the fragment program declares, in declaration order, beyond the base
       * texture the runtime binds itself; these are what shader_get_sampler_index can name. */
      if(texts[1]){
        const char *scan=texts[1];
        r->shader_pal[i].generic_sampler_count=0;
        while((scan=strstr(scan,"uniform"))){
          const char *cursor=scan+7;
          char name[32];
          int length=0;
          scan+=7;
          while(*cursor==' '||*cursor=='\t') cursor++;
          if(strncmp(cursor,"sampler2D",9)) continue;
          cursor+=9;
          while(*cursor==' '||*cursor=='\t') cursor++;
          while((isalnum((unsigned char)*cursor) || *cursor=='_') && length<31) name[length++]=*cursor++;
          name[length]='\0';
          if(!length || !strcmp(name,"gm_BaseTexture")) continue;
          if(r->shader_pal[i].generic_sampler_count>=GML_SHADER_GENERIC_SAMPLERS) break;
          {
            int index=r->shader_pal[i].generic_sampler_count++;
            memset(&r->shader_pal[i].generic_sampler[index],0,sizeof r->shader_pal[i].generic_sampler[index]);
            snprintf(r->shader_pal[i].generic_sampler[index].name,
                     sizeof r->shader_pal[i].generic_sampler[index].name,"%s",name);
          }
        }
      }
    }
    /* A fragment that calls no sampling function reads no pixels from anywhere: neither the ones
     * the draw covers nor a surface the content bound to a stage. Asking only about gm_BaseTexture
     * is not enough, because a post-process that samples a bound uniform sampler2D transforms
     * a picture and would be misread as painting one from nothing. Every sampling builtin is named
     * texture, texture2D, textureCube, textureLod and so on, so one word-boundary scan for a call
     * to an identifier beginning with "texture" covers them all. A function definition whose name
     * begins that way counts as one too, which errs toward leaving the answer to the host.
     *
     * Decided here, before the family parsers run, because each of them returns as soon as it
     * recognizes the fragment. */
    { int samples=0;
      for(const char *scan=src;(scan=strstr(scan,"texture"));){
        int boundary=(scan==src) || !(isalnum((unsigned char)scan[-1]) || scan[-1]=='_');
        const char *after=scan+7;
        while(isalnum((unsigned char)*after) || *after=='_') after++;
        while(*after==' '||*after=='\t'||*after=='\n'||*after=='\r') after++;
        scan+=7;
        if(boundary && *after=='('){ samples=1; break; }
      }
      r->shader_pal[i].procedural=!samples; }
    /* gl_FragCoord is the pixel's place on the render target. A fragment that reads it produces
     * something whose value moves with the draw, so its answer is not a property of the picture it
     * samples and cannot be evaluated once for a texture-page rectangle and reused. */
    r->shader_pal[i].position_dependent=strstr(src,"gl_FragCoord")?1:0;
    if(glsl_parse_indexed_brightness(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
          "[shader] indexed-brightness[%u] split=%.4f uniform=%s\n",
          i,sp->indexed_brightness_red,sp->indexed_brightness_uniform);
      continue;
    }
    if(glsl_parse_threshold_palette(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
          "[shader] threshold-palette[%u] split=%.4f uniforms=%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
          i,sp->threshold_palette_red,
          sp->threshold_palette_uniform[0],sp->threshold_palette_uniform[1],
          sp->threshold_palette_uniform[2],sp->threshold_palette_uniform[3],
          sp->threshold_palette_uniform[4],sp->threshold_palette_uniform[5],
          sp->threshold_palette_uniform[6],sp->threshold_palette_uniform[7],
          sp->threshold_palette_uniform[8],sp->threshold_palette_uniform[9]);
      continue;
    }
    if(glsl_parse_channel_mask(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
          "[shader] channel-mask[%u] keep=%d\n",i,sp->channel_mask_keep);
      continue;
    }
    if(glsl_parse_channel_alpha_key(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
          "[shader] channel-alpha-key[%u] channel=%d alpha %s %.5f\n",i,
          sp->channel_alpha_key_channel,sp->channel_alpha_key_inclusive?"<=":"<",
          sp->channel_alpha_key_cutoff);
      continue;
    }
    if(glsl_parse_alpha_discard_passthrough(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] alpha-discard[%u] alpha %s %.5f\n",i,
                sp->alpha_discard_inclusive?"<=":"<",sp->alpha_discard_cutoff);
      continue;
    }
    if(glsl_parse_quantise4(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
                "[shader] quantise4[%u] uniforms %s,%s,%s,%s thresholds %.3f/%.3f/%.3f\n",i,
                sp->quantise4_uniform[0],sp->quantise4_uniform[1],
                sp->quantise4_uniform[2],sp->quantise4_uniform[3],
                sp->quantise4_threshold[0],sp->quantise4_threshold[1],sp->quantise4_threshold[2]);
      continue;
    }
    if(glsl_parse_ordered_dither(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
                "[shader] ordered-dither[%u] uniform %s\n",i,sp->ordered_dither_uniform);
      continue;
    }
    if(glsl_parse_solid_alpha_mask(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
          "[shader] solid-alpha-mask[%u] colour='%s' alpha %s %.5f\n",i,
          sp->solid_alpha_mask_uniform,sp->solid_alpha_mask_inclusive?"<=":"<",
          sp->solid_alpha_mask_cutoff);
      continue;
    }
    if(glsl_parse_solid_blur_alpha(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
          "[shader] solid-blur-alpha[%u] colour='%s' taps=%d+%d steps=%.7f,%.7f\n",
          i,sp->solid_blur_alpha_uniform,sp->solid_blur_alpha_x_count,
          sp->solid_blur_alpha_y_count,sp->solid_blur_alpha_step_x,
          sp->solid_blur_alpha_step_y);
      continue;
    }
    if(glsl_parse_grayscale(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] grayscale[%u] weights=%.4f,%.4f,%.4f alpha='%s'\n",i,
          sp->grayscale_weight[0],sp->grayscale_weight[1],sp->grayscale_weight[2],
          sp->grayscale_alpha_uniform);
      continue;
    }
    if(glsl_parse_radial_wave(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] radial-wave[%u] controls='%s','%s','%s','%s','%s','%s'\n",i,
          sp->radial_wave_uniform[0],sp->radial_wave_uniform[1],sp->radial_wave_uniform[2],
          sp->radial_wave_uniform[3],sp->radial_wave_uniform[4],sp->radial_wave_uniform[5]);
      continue;
    }
    if(glsl_parse_uv_wave(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] uv-wave[%u] mode=%d time='%s' factor=%.4f\n",i,
                sp->uv_wave_mode,sp->uv_wave_uniform,sp->uv_wave_time_factor);
      continue;
    }
    /* Two samples of the base texture, with a normalized-coordinate offset and per-channel
     * multiply/add. This covers chromatic-offset post-processes without executing arbitrary GLSL. */
    if(glsl_parse_dual_sample(src,&r->shader_pal[i])){
      const struct GmlShaderPal *sp=&r->shader_pal[i];
      if(render_setting(r,"GML_LOG_SHADER"))
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] dual[%u] axis=%c sign=%d uniforms='%s','%s' "
          "base=%.3f,%.3f,%.3f,%.3f shifted=%.3f,%.3f,%.3f,%.3f\n",i,
          sp->dual_axis?'y':'x',sp->dual_sign,sp->dual_uniform[0],sp->dual_uniform[1],
          sp->dual_base_gain[0],sp->dual_base_gain[1],sp->dual_base_gain[2],sp->dual_base_gain[3],
          sp->dual_shift_gain[0],sp->dual_shift_gain[1],sp->dual_shift_gain[2],sp->dual_shift_gain[3]);
      continue;
    }
    {
      /* Palette-grid family: search column zero for an exact source color, then sample a selected
       * palette column (optionally interpolating fractional ids). Recognize the algorithm and read
       * its uniforms from the shader rather than relying on asset or identifier names. */
      int interpolated_grid=strstr(src,"fract(") && strstr(src,"floor(");
      int scanned_grid=strstr(src,"for (") && strstr(src,"+=") && strstr(src,".y");
      if(strstr(src,"distance(") && (interpolated_grid || scanned_grid) &&
         strstr(src,"texture2D") && strstr(src,"sampler2D")){
        struct GmlShaderPal *sp=&r->shader_pal[i];
        const char *us=src, *samp=NULL;
        while((us=strstr(us,"uniform sampler2D "))){
          us+=18; if(strncmp(us,"gm_",3)){ samp=us; break; }
        }
        char sname[32]="", uvname[32]="", idname[32]="", pxname[32]="";
        if(samp){ int k=0; for(;k<31 && (isalnum((unsigned char)samp[k])||samp[k]=='_');k++) sname[k]=samp[k]; sname[k]=0; }
        glsl_uniform_name(src,"vec4",0,uvname,sizeof uvname);
        glsl_uniform_name(src,"float",0,idname,sizeof idname);
        glsl_uniform_name(src,"vec2",0,pxname,sizeof pxname);
        if(sname[0] && uvname[0] && idname[0] && pxname[0]){
          sp->grid=1; sp->grid_id=-1.0f;
          snprintf(sp->grid_sampler,sizeof sp->grid_sampler,"%s",sname);
          snprintf(sp->grid_uvs_uniform,sizeof sp->grid_uvs_uniform,"%s",uvname);
          snprintf(sp->grid_id_uniform,sizeof sp->grid_id_uniform,"%s",idname);
          snprintf(sp->grid_pixel_uniform,sizeof sp->grid_pixel_uniform,"%s",pxname);
          if(render_setting(r,"GML_LOG_SHADER"))
            anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] grid[%u] sampler=%s uvs=%s id=%s pixel=%s\n",
                    i,sname,uvname,idname,pxname);
          continue;
        }
      }
      /* Palette-LUT template: the fragment samples a palette texture at
       * (a base-colour channel, <row uniform>). Detect a sampler2D besides gm_BaseTexture,
       * a float uniform, and a vec2( ... .r, <row>) build feeding texture2D(<sampler>, ...). */
      struct GmlShaderPal *sp=&r->shader_pal[i];
      const char *us=src, *samp=NULL;
      while((us=strstr(us,"uniform sampler2D "))){
        us+=18; if(strncmp(us,"gm_BaseTexture",14)){ samp=us; break; }
      }
      const char *v2=strstr(src,"vec2(");
      /* find the LAST vec2( build that references .r — the uv for the palette lookup */
      for(const char *w2=v2; w2; ){
        const char *nx=strstr(w2+1,"vec2(");
        const char *cl=strchr(w2,')');
        if(cl && mem_find(w2,(size_t)(cl-w2),".r",2)) v2=w2;
        if(!nx) break;
        w2=nx;
      }
      if(samp && v2){
        char sname[32], rname[32]="" ; int k=0;
        for(;k<31 && (isalnum((unsigned char)samp[k])||samp[k]=='_');k++) sname[k]=samp[k];
        sname[k]=0;
        const char *close=strchr(v2,')');
        /* pick the float uniform that is referenced inside that vec2's argument list */
        for(const char *uf=strstr(src,"uniform float "); uf && close; uf=strstr(uf+1,"uniform float ")){
          char cand[32]; int j=0; const char *un=uf+14;
          for(;j<31 && (isalnum((unsigned char)un[j])||un[j]=='_');j++) cand[j]=un[j];
          cand[j]=0;
          if(cand[0] && mem_find(v2,(size_t)(close-v2),cand,strlen(cand))){ snprintf(rname,sizeof rname,"%s",cand); break; }
        }
        int uses_r = close && mem_find(v2,(size_t)(close-v2),".r",2)!=NULL;
        int uses_row = rname[0]!=0;
        if(sname[0] && uses_r && uses_row){
          sp->lut=1; sp->lut_row=-1;
          snprintf(sp->lut_sampler,sizeof sp->lut_sampler,"%s",sname);
          snprintf(sp->lut_row_uniform,sizeof sp->lut_row_uniform,"%s",rname);
          /* A second common family maps grayscale indices down a palette column. It is distinct
           * from the simple red/x lookup above: three float controls build a normalized vec2,
           * and the palette sprite's UV rectangle is interpolated with that coordinate. */
          const char *mainp=strstr(samp,"void main");
          const char *mixp=NULL;
          for(const char *m=strstr(src,"mix(");m;m=strstr(m+4,"mix(")){
            const char *mc=strchr(m,')');
            if(mc && mem_find(m,(size_t)(mc-m),".xy",3) && mem_find(m,(size_t)(mc-m),".zw",3)) mixp=m;
          }
          char floats[3][32]={{0}}, uvname[32]=""; int nf=0;
          if(mainp) for(const char *uf=strstr(samp,"uniform float ");uf && uf<mainp && nf<3;
                         uf=strstr(uf+1,"uniform float ")){
            const char *un=uf+14; int j=0;
            for(;j<31 && (isalnum((unsigned char)un[j])||un[j]=='_');j++) floats[nf][j]=un[j];
            floats[nf][j]=0; if(j) nf++;
          }
          if(mixp){
            const char *un=mixp+4; while(*un && isspace((unsigned char)*un)) un++;
            int j=0; for(;j<31 && (isalnum((unsigned char)un[j])||un[j]=='_');j++) uvname[j]=un[j];
            uvname[j]=0;
          }
          if(nf==3 && uvname[0] && strstr(src,"255.0") && strstr(src,".xy") &&
             strstr(src,".zw") && (strstr(src,"==") || strstr(src,"equal("))){
            sp->lut_indexed=1;
            snprintf(sp->lut_offset_uniform,sizeof sp->lut_offset_uniform,"%s",floats[0]);
            snprintf(sp->lut_colors_uniform,sizeof sp->lut_colors_uniform,"%s",floats[1]);
            /* The vec2's first scalar is the normalized palette column. Prefer the already
             * inferred row handle, falling back to declaration order for equivalent shaders. */
            snprintf(sp->lut_row_uniform,sizeof sp->lut_row_uniform,"%s",rname[0]?rname:floats[2]);
            snprintf(sp->lut_uvs_uniform,sizeof sp->lut_uvs_uniform,"%s",uvname);
            sp->lut_uvs[2]=sp->lut_uvs[3]=1.0f;
            /* Optional post-palette tint and rectangle clipping are vec4 controls declared near
             * the custom sampler. Identify them by their use in alpha mixing / fragment position. */
            for(const char *uv=strstr(samp,"uniform vec4 ");uv && (!mainp || uv<mainp);
                uv=strstr(uv+1,"uniform vec4 ")){
              char name[32]=""; const char *un=uv+13; int j=0;
              for(;j<31 && (isalnum((unsigned char)un[j])||un[j]=='_');j++) name[j]=un[j];
              name[j]=0; if(!name[0] || !strcmp(name,uvname)) continue;
              char alpha_ref[40], bounds_ref[40];
              snprintf(alpha_ref,sizeof alpha_ref,"%s.a",name);
              snprintf(bounds_ref,sizeof bounds_ref,"%s[",name);
              if(strstr(src,"v_vPosition") && strstr(src,bounds_ref)){
                sp->lut_has_bounds=1;
                snprintf(sp->lut_bounds_uniform,sizeof sp->lut_bounds_uniform,"%s",name);
              } else if(strstr(src,alpha_ref)){
                sp->lut_has_colorise=1;
                snprintf(sp->lut_colorise_uniform,sizeof sp->lut_colorise_uniform,"%s",name);
              }
            }
          }
          if(render_setting(r,"GML_LOG_SHADER"))
            anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] lut[%u] sampler=%s row=%s indexed=%d controls=%s,%s,%s\n",
                    i,sname,sp->lut_row_uniform,sp->lut_indexed,sp->lut_uvs_uniform,
                    sp->lut_offset_uniform,sp->lut_colors_uniform);
        }
      }
      continue;
    }
  }
}
