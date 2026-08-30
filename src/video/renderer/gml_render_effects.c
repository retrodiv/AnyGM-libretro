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

/* Glow: the capture, raised to the gamma so only its bright parts survive, blurred by the radius
 * as many times as the quality asks, scaled by the intensity, and merged by the brighter channel. */
static void layer_glow_prepare(const uint32_t *src,uint32_t *dst,size_t count,float gamma){
  float exponent=gamma>1e-3f?gamma:1e-3f;
  for(size_t i=0;i<count;i++){
    float c[4]; layer_unpack(src[i],c);
    for(int k=0;k<3;k++) c[k]=powf(layer_clamp01(c[k]),exponent);
    dst[i]=layer_pack(c);
  }
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
    layer_composite_normal(r,src,w,h,filter->u.glow.alpha);
    int quality=(int)floor(filter->u.glow.quality+0.5); if(quality<1)quality=1;if(quality>16)quality=16;
    layer_glow_prepare(src,work,count,(float)filter->u.glow.gamma);
    layer_blur(work,work,aux,w,h,(float)filter->u.glow.radius,quality);
    layer_composite_max(r,work,w,h,filter->u.glow.intensity);
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

/* ---- recognized display post-processes: none retained in this revision ---- */

