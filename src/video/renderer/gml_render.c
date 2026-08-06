/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer lifecycle, frame coordination, draw state, and software draw facade. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_blit_internal.h"
#include "gml_render_pixel_internal.h"
#include "gml_render_sampling_internal.h"
#include "gml_render_backend.h"
#include "anygm_compatibility.h"
#include "gml_thread.h"
#include "anygm_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
GML_THREAD_BRIDGE_IMPL

int rprof_enabled(void){ return 0; }
const char *render_setting(const GmlRender *r,const char *name){
  return anygm_host_development_setting(r&&r->win?r->win->host:NULL,name);
}

/* Cardinal rotations must stay exactly on the pixel lattice.  libm leaves tiny residuals for
 * sin/cos(90*n) (for example cos(270) ~= -1.8e-16); inverse texture mapping then floors a sample
 * on the wrong side of an integer boundary and shifts the quadrants with a negative axis by one
 * pixel.  Hardware vertex transforms preserve these literal cardinal matrices. */
void render_rotation_sincos(double degrees,double *cosine,double *sine){
  double quadrant=nearbyint(degrees/90.0);
  if(fabs(degrees-quadrant*90.0)<1e-10){
    switch(((int)quadrant%4+4)%4){
      case 0: *cosine=1.0;  *sine=0.0;  return;
      case 1: *cosine=0.0;  *sine=1.0;  return;
      case 2: *cosine=-1.0; *sine=0.0;  return;
      default:*cosine=0.0;  *sine=-1.0; return;
    }
  }
  double radians=degrees*M_PI/180.0;
  *cosine=cos(radians);
  *sine=sin(radians);
}
double rprof_now(void){
  return 0.0;
}
int rprof_tpag_id(GmlRender *r, GmlTpag *t){
  if(!r || !t || !r->tpag || r->n_tpag<=0) return -1;
  uintptr_t pointer=(uintptr_t)t, begin=(uintptr_t)r->tpag;
  uintptr_t end=begin+(uintptr_t)r->n_tpag*sizeof(GmlTpag);
  return pointer>=begin && pointer<end ? (int)((pointer-begin)/sizeof(GmlTpag)) : -1;
}
void rprof_add(const char *label, GmlRender *r, GmlTpag *t, double ms, unsigned long long pixels){
  (void)label; (void)r; (void)t; (void)ms; (void)pixels;
}

uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
uint16_t u16(const uint8_t *d, uint32_t o){ return (uint16_t)(d[o]|d[o+1]<<8); }
uint32_t be32(const uint8_t *d){ return (uint32_t)d[0]<<24|(uint32_t)d[1]<<16|(uint32_t)d[2]<<8|(uint32_t)d[3]; }
/* ---- TPAG ---- */
static void parse_tpag(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"TPAG"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_tpag=(int)n; r->tpag=calloc(n,sizeof(GmlTpag)); r->tpag_ptr=calloc(n,sizeof(uint32_t));
  if(!r->tpag || !r->tpag_ptr){ free(r->tpag); free(r->tpag_ptr); r->tpag=NULL; r->tpag_ptr=NULL; r->n_tpag=0; return; }
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4); r->tpag_ptr[i]=p;
    GmlTpag *t=&r->tpag[i];
    t->sx=u16(d,p); t->sy=u16(d,p+2); t->sw=u16(d,p+4); t->sh=u16(d,p+6);
    t->tx=u16(d,p+8); t->ty=u16(d,p+10); t->bw=u16(d,p+16); t->bh=u16(d,p+18);
    t->atlas=(int16_t)u16(d,p+20);
  }
}
int tpag_index_for_ptr(GmlRender *r, uint32_t ptr){
  for(int i=0;i<r->n_tpag;i++) if(r->tpag_ptr[i]==ptr) return i;
  return -1;
}
uint32_t gml_render_named_tpag_ptr(GmlRender *r, const char *name){
  if(!r || !name || !*name || !r->tpag_ptr) return 0;
  for(int i=0;i<r->n_spr;i++){
    GmlSprite *s=&r->spr[i];
    if(!s->name || strcmp(s->name,name) || !s->frame || s->n_frames<=0) continue;
    int ti=s->frame[0];
    return ti>=0 && ti<r->n_tpag ? r->tpag_ptr[ti] : 0;
  }
  return 0;
}

int gml_render_named_sprite(GmlRender *r, const char *name){
  if(!r || !name || !*name) return -1;
  for(int i=0;i<r->n_spr;i++)
    if(r->spr[i].name && !strcmp(r->spr[i].name,name)) return i;
  return -1;
}

/* ---- SPRT ---- */
static void parse_sprt(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"SPRT"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_spr=(int)n; r->spr=calloc(n,sizeof(GmlSprite)); r->spr_cap=(int)n; r->spr_has_free=0;
  if(!r->spr){ r->n_spr=0; r->spr_cap=0; return; }
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    GmlSprite *s=&r->spr[i];
    s->name=gml_str_by_ptr(r->win,u32(d,p));
    s->playback_speed=1.0f; s->playback_speed_type=1; s->playback_speed_valid=0;
    s->w=(int)u32(d,p+4); s->h=(int)u32(d,p+8);
    s->ml=(int)u32(d,p+12); s->mr=(int)u32(d,p+16); s->mb=(int)u32(d,p+20); s->mt=(int)u32(d,p+24);
    s->collision_kind=0; s->collision_tolerance=63; /* preserve the old alpha>=64 fallback */
    /* margins(16) transparent/smooth/preload(12) bboxmode(4) sepmasks(4) -> originX/Y */
    /* GMS1 sprite header: ...,BBoxMode(40),SepMasks(44),OriginX(48),OriginY(52),frameList(56) */
    s->originx=(int)u32(d,p+48); s->originy=(int)u32(d,p+52);
    uint32_t list=p+56;             /* GMS1: SimpleList<TextureEntry> here (count + pointers) */
    /* GMS2 sprite: a -1 marker at +56, then SVersion(+60), SpriteType(+64), and for a normal sprite
     * PlaybackSpeed(+68 float)+PlaybackSpeedType(+72), plus SequenceOffset (SVersion>=2) and
     * NineSliceOffset (SVersion>=3) — the texture list only starts after all that. Reading +56 as the
     * frame count (=-1) would zero the frame list and blank every sprite. */
    if(u32(d,p+56)==0xFFFFFFFFu){
      uint32_t sver=u32(d,p+60), stype=u32(d,p+64);
      float playback; memcpy(&playback,d+p+68,sizeof playback);
      uint32_t playback_type=u32(d,p+72);
      if(isfinite(playback) && playback>=0.0f && playback_type<=1){
        s->playback_speed=playback;
        s->playback_speed_type=(int)playback_type;
        s->playback_speed_valid=1;
      }
      if(stype!=0){
        s->n_frames=0;
        s->frame=calloc(1,sizeof(int));
        if(stype==2){
          uint32_t header=p+76+(sver>=2?4:0)+(sver>=3?4:0);
          /* Some packages place a texture-page list before the skeletal record. */
          if(header+12<=r->win->size){
            uint32_t count=u32(d,header),candidate=header+4+count*4u;
            if(count>0 && count<=16 && candidate+20<=r->win->size &&
               u32(d,candidate)>=1 && u32(d,candidate)<=3)
              header=candidate;
          }
          (void)gml_render_parse_spine(r,s,p,header);
        }
        continue;
      }
      uint32_t fl=76;                         /* after PlaybackSpeed(+68)+PlaybackSpeedType(+72) */
      if(sver>=2) fl+=4;                       /* SequenceOffset */
      if(sver>=3){                             /* NineSliceOffset */
        uint32_t nso=u32(d,p+80);
        if(nso && nso+40<=r->win->size && u32(d,nso+16)){
          /* NineSlice layout: Left,Top,Right,Bottom (i32), Enabled, TileModes[5]
           * (left,top,right,bottom,center). GM keeps the borders at native size when the
           * sprite draws scaled — stretching them uniformly deforms UI boards/bubbles. */
          s->ns_l=(int)(int32_t)u32(d,nso); s->ns_t=(int)(int32_t)u32(d,nso+4);
          s->ns_r=(int)(int32_t)u32(d,nso+8); s->ns_b=(int)(int32_t)u32(d,nso+12);
          for(int k=0;k<5;k++) s->ns_tile[k]=(int)(int32_t)u32(d,nso+20+4u*k);
          if(s->ns_l>=0 && s->ns_t>=0 && s->ns_r>=0 && s->ns_b>=0 &&
             s->ns_l+s->ns_r<=s->w && s->ns_t+s->ns_b<=s->h)
            s->ns_enabled=1;
        }
        fl+=4;
      }
      list=p+fl;
    }
    uint32_t fn=u32(d,list);
    if(fn>10000) fn=0;              /* guard against special-type sprites */
    s->n_frames=(int)fn; s->frame=calloc(fn?fn:1,sizeof(int));
    if(!s->frame){ s->n_frames=0; continue; }
    for(uint32_t f=0;f<fn;f++){
      uint32_t tptr=u32(d,list+4+f*4);
      s->frame[f]=tpag_index_for_ptr(r,tptr);
    }
    /* SPRT collision masks (after the frame list): count + count×(rowbytes·height) of 1bpp data.
     * GameMaker collides with THESE, not the visible sprite alpha (which can be decorative). */
    uint32_t maskoff=list+4+fn*4; uint32_t mc=u32(d,maskoff);
    if(mc>0 && mc<100000 && s->w>0 && s->h>0){
      s->mask_count=(int)mc; s->mask_rowb=(s->w+7)/8; s->mask=d+maskoff+4;
    }
  }
}
double gml_sprite_animation_delta(GmlRender *r, int sprite, double image_speed, double game_fps){
  if(!r || sprite<0 || sprite>=r->n_spr) return image_speed;
  GmlSprite *s=&r->spr[sprite];
  if(!s->playback_speed_valid) return image_speed;
  double delta=image_speed*(double)s->playback_speed;
  if(s->playback_speed_type==0){
    if(!(game_fps>0.0) || !isfinite(game_fps)) game_fps=60.0;
    delta/=game_fps;
  }
  return delta;
}
/* whether sprite's COLLISION MASK is solid at sprite-local (lx,ly). Asset sprites without a
 * serialized 1bpp mask use their bounding box; runtime sprites can still use alpha precision
 * through sprite_collision_mask(kind=bboxkind_precise). */
int gml_sprite_collision(GmlRender *r, int sprite, int frame, int lx, int ly){
  if(sprite<0||sprite>=r->n_spr) return 0;
  GmlSprite *s=&r->spr[sprite];
  if(lx<0||ly<0||lx>=s->w||ly>=s->h) return 0;
  if(lx<s->ml||lx>s->mr||ly<s->mt||ly>s->mb) return 0;
  if(s->collision_kind==1) return 1;
  if(s->collision_kind==2){
    double hw=(s->mr-s->ml+1)/2.0, hh=(s->mb-s->mt+1)/2.0;
    if(hw<=0||hh<=0) return 0;
    double cx=s->ml+hw-0.5, cy=s->mt+hh-0.5;
    double dx=(lx-cx)/hw, dy=(ly-cy)/hh;
    return dx*dx+dy*dy<=1.0;
  }
  if(s->collision_kind==3){
    double hw=(s->mr-s->ml+1)/2.0, hh=(s->mb-s->mt+1)/2.0;
    if(hw<=0||hh<=0) return 0;
    double cx=s->ml+hw-0.5, cy=s->mt+hh-0.5;
    return fabs((lx-cx)/hw)+fabs((ly-cy)/hh)<=1.0;
  }
  if(s->runtime_rgba) return gml_sprite_alpha(r,sprite,frame,lx,ly)>s->collision_tolerance;
  if(!s->mask||s->mask_count<=0) return 1;
  int mi=(s->mask_count>1 && frame>=0 && frame<s->mask_count)? frame : 0;
  const uint8_t *m=s->mask + (size_t)mi*s->mask_rowb*s->h;
  return (m[(size_t)ly*s->mask_rowb + lx/8] >> (7-(lx%8))) & 1;
}

/* ---- BGND ---- */
static void parse_bgnd(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"BGND"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_bg=(int)n; r->bg=calloc(n,sizeof(GmlBg));
  if(!r->bg){ r->n_bg=0; return; }
  for(uint32_t i=0;i<n;i++){
	    uint32_t p=u32(d,c->off+4+i*4);
	    /* name, transparent, smooth, preload, texture(TPAG ptr) */
	    uint32_t tptr=u32(d,p+16);
	    r->bg[i].tpag=tpag_index_for_ptr(r,tptr);
	    if(anygm_policy_has_modern_layer_semantics(r->win) && p+64<c->off+c->size){
	      int ver=(int)u32(d,p+20), tw=(int)u32(d,p+24), th=(int)u32(d,p+28);
          /* The separated-border layout inserts separationX/Y before the output-border fields.
           * Validate both layouts structurally. Interpreting an older record as the new layout
           * makes its exported-sprite slot become ItemsPerTile (normally zero); a new record has
           * a complete count*frames table at +72. */
          int obx=(int)u32(d,p+32), oby=(int)u32(d,p+36), ocols=(int)u32(d,p+40);
          int oitems=(int)u32(d,p+44), ocount=(int)u32(d,p+48);
          uint64_t obytes=(uint64_t)(uint32_t)oitems*(uint64_t)(uint32_t)ocount*4u;
          int old_ok=ver>0 && tw>0 && th>0 && tw<=4096 && th<=4096 && obx>=0 && oby>=0 &&
                     ocols>0 && ocols<=4096 && oitems>0 && oitems<=1024 && ocount>0 &&
                     obytes<=4000000u && (uint64_t)p+64u+obytes<=(uint64_t)c->off+c->size;
          int nsep_x=(int)u32(d,p+32), nsep_y=(int)u32(d,p+36);
          int nbx=(int)u32(d,p+40), nby=(int)u32(d,p+44), ncols=(int)u32(d,p+48);
          int nitems=(int)u32(d,p+52), ncount=(int)u32(d,p+56);
          uint64_t nbytes=(uint64_t)(uint32_t)nitems*(uint64_t)(uint32_t)ncount*4u;
          int new_ok=ver>0 && tw>0 && th>0 && tw<=4096 && th<=4096 &&
                     nsep_x>=0 && nsep_x<=4096 && nsep_y>=0 && nsep_y<=4096 &&
                     nbx>=0 && nby>=0 && nbx<=4096 && nby<=4096 &&
                     ncols>0 && ncols<=4096 && nitems>0 && nitems<=1024 && ncount>0 &&
                     nbytes<=4000000u && (uint64_t)p+72u+nbytes<=(uint64_t)c->off+c->size;
          int modern=new_ok && (!old_ok || (ncount>ocount && ncols>ocols));
          if(old_ok || modern){
            int bx=modern?nbx:obx, by=modern?nby:oby, cols=modern?ncols:ocols;
            int items=modern?nitems:oitems, count=modern?ncount:ocount;
            r->bg[i].tile_w=tw; r->bg[i].tile_h=th;
            r->bg[i].tile_border_x=bx; r->bg[i].tile_border_y=by;
            r->bg[i].tile_separation_x=modern?nsep_x:0;
            r->bg[i].tile_separation_y=modern?nsep_y:0;
            r->bg[i].tile_columns=cols; r->bg[i].tile_items_per_tile=items; r->bg[i].tile_count=count;
            {
              uint32_t frame_off=p+(modern?64:56);
              r->bg[i].tile_frame_length_us=
                (uint64_t)u32(d,frame_off)|((uint64_t)u32(d,frame_off+4)<<32);
            }
            r->bg[i].tile_ids=d+p+(modern?72:64);
          }
	    }
	  }
	}

/* Persistent row-worker pool shared by the full-screen software compositor passes. Creating and
 * joining 15-31 OS threads for every frame is particularly expensive on Windows and dominated
 * fast-forward at high resolutions. The renderer is single-caller, so one small global pool can
 * safely serve every renderer instance, growing lazily and shutting down with renderer teardown. */
typedef struct GmlRowPool GmlRowPool;
typedef struct { GmlRowPool *pool; int slot; unsigned seen_epoch; } GmlRowWorker;
struct GmlRowPool {
  gml_thread_t thread[GML_ROW_THREADS_MAX-1];
  GmlRowWorker worker[GML_ROW_THREADS_MAX-1];
  int initialized, nworkers, stop, nt, H, remaining;
  unsigned epoch;
  GmlRowBandFn fn;
  void *ctx;
  gml_mutex_t mutex;
  gml_cond_t start_cond, done_cond;
};
static void *gml_rowband_worker(void *p){
  GmlRowWorker *w=(GmlRowWorker*)p;
  GmlRowPool *pool=w->pool;
  gml_mutex_lock(&pool->mutex);
  for(;;){
    while(!pool->stop && w->seen_epoch==pool->epoch) gml_cond_wait(&pool->start_cond,&pool->mutex);
    if(pool->stop){ gml_mutex_unlock(&pool->mutex); return NULL; }
    w->seen_epoch=pool->epoch;
    int active=w->slot<pool->nt;
    GmlRowBandFn fn=pool->fn;
    void *ctx=pool->ctx;
    int H=pool->H, nt=pool->nt, slot=w->slot;
    gml_mutex_unlock(&pool->mutex);
    if(active) fn(ctx,(int)((long)H*slot/nt),(int)((long)H*(slot+1)/nt),slot);
    gml_mutex_lock(&pool->mutex);
    if(active && --pool->remaining==0) gml_cond_broadcast(&pool->done_cond);
  }
}
static GmlRowPool *gml_row_pool_init(GmlRender *r){
  if(!r) return NULL;
  if(!r->row_pool) r->row_pool=calloc(1,sizeof(GmlRowPool));
  GmlRowPool *p=(GmlRowPool*)r->row_pool;
  if(!p || p->initialized) return p;
  memset(p,0,sizeof *p);
  gml_mutex_init(&p->mutex);
  gml_cond_init(&p->start_cond);
  gml_cond_init(&p->done_cond);
  p->initialized=1;
  return p;
}
static int gml_row_pool_ensure(GmlRender *r,int nt){
  GmlRowPool *p=gml_row_pool_init(r);
  if(!p) return 1;
  int want=nt-1;
  if(want>GML_ROW_THREADS_MAX-1) want=GML_ROW_THREADS_MAX-1;
  while(p->nworkers<want){
    int i=p->nworkers;
    GmlRowWorker *w=&p->worker[i];
    w->pool=p; w->slot=i+1; w->seen_epoch=p->epoch;
    if(gml_thread_create(&p->thread[i],gml_rowband_worker,w)!=0) break;
    p->nworkers++;
  }
  return p->nworkers+1;
}
static void gml_row_pool_free(GmlRender *r){
  GmlRowPool *p=r?(GmlRowPool*)r->row_pool:NULL;
  if(!p) return;
  if(!p->initialized){ free(p); r->row_pool=NULL; return; }
  gml_mutex_lock(&p->mutex);
  p->stop=1;
  p->epoch++;
  gml_cond_broadcast(&p->start_cond);
  gml_mutex_unlock(&p->mutex);
  for(int i=0;i<p->nworkers;i++) gml_thread_join(p->thread[i]);
  gml_cond_destroy(&p->done_cond);
  gml_cond_destroy(&p->start_cond);
  gml_mutex_destroy(&p->mutex);
  free(p);
  r->row_pool=NULL;
}
void gml_run_row_bands_n(GmlRender *r,int H,int nt,GmlRowBandFn fn,void *ctx){
  if(nt<=1){ fn(ctx,0,H,0); return; }
  int actual=gml_row_pool_ensure(r,nt);
  GmlRowPool *p=(GmlRowPool*)r->row_pool;
  nt=actual;
  if(nt<=1){ fn(ctx,0,H,0); return; }
  gml_mutex_lock(&p->mutex);
  p->fn=fn; p->ctx=ctx; p->H=H; p->nt=nt; p->remaining=nt-1;
  p->epoch++;
  gml_cond_broadcast(&p->start_cond);
  gml_mutex_unlock(&p->mutex);
  fn(ctx,0,(int)((long)H/nt),0);
  gml_mutex_lock(&p->mutex);
  while(p->remaining>0) gml_cond_wait(&p->done_cond,&p->mutex);
  gml_mutex_unlock(&p->mutex);
}
void gml_run_row_bands(GmlRender *r,int H,GmlRowBandFn fn,void *ctx){
  int nt=1;
  if(H>=128){ nt=gml_ncpu(); if(nt>GML_ROW_THREADS_AUTO)nt=GML_ROW_THREADS_AUTO; if(nt<1)nt=1; }
  gml_run_row_bands_n(r,H,nt,fn,ctx);
}
static int env_fast_alpha_cull(const GmlRender *r){
  const char *e=render_setting(r,"GML_FAST_ALPHA_CULL");
  int v=e?atoi(e):0;
  if(v<0) v=0;
  if(v>32) v=32;
  return v;
}
int gml_render_init(GmlRender *r, GmlWin *win){
  memset(r,0,sizeof(*r)); r->win=win;
  r->surface_draw_logging=-1;
  r->classic=win&&anygm_policy_uses_classic_runtime(win);
  /* GM6/7/8 starts the shared drawing colour at black; Studio starts it at white. The state is
   * intentionally persistent across Draw events, so choosing the right initial value matters to
   * every project which draws text or primitives without an explicit draw_set_colour call. */
  r->color=(win && anygm_policy_uses_classic_runtime(win))?0:0xFFFFFF;
  r->alpha=1; r->font=-1; r->alphablend=1; r->circle_precision=24; r->color_write_mask=0x0F;
  r->alpha_test_enable=0; r->alpha_test_ref=0;
  r->blend_equation=r->blend_equation_alpha=1;
  r->app_draw_enable=1; r->next_surface_id=1; r->crt_shader_enable=1; r->crt_mask_enable=1; r->crt_scanlines_enable=1; r->crt_gamma_enable=1; r->crt_curvature=-1; r->crt_vignette=-1;
  r->interp=anygm_policy_classic_interpolate(win);
  r->composites_app=0;
  r->fast_alpha_cull=env_fast_alpha_cull(r);
  r->lut_pal_sprite=-1; r->lut_pal_frame=0;
  parse_txtr(r); parse_tpag(r); parse_sprt(r); parse_bgnd(r); parse_font(r); build_default_font(r);
  parse_shader_palettes(r);
  r->base_n_spr=r->n_spr;
  return 0;
}
void gml_render_bind_software3d(GmlRender *r,GmlSoftware3D *software3d){
  if(r) r->software3d=software3d;
}
GmlSoftware3D *gml_render_backend_software3d(GmlRender *r){
  return r?r->software3d:NULL;
}
GmlSoftware3D *gml_render_software3d(GmlRender *r){
  return gml_render_backend_software3d(r);
}
void gml_render_set_frame(GmlRender *r,long frame){
  if(r) r->frame=frame;
}
void gml_render_control_update(GmlRender *r,const GmlRenderControl *control,
                               unsigned fields){
  if(!r || !control) return;
  if(fields&GML_RENDER_CONTROL_REQUESTED_SIZE){
    r->resolution_w=control->requested_width;
    r->resolution_h=control->requested_height;
  }
  if(fields&GML_RENDER_CONTROL_CRT){
    r->crt_shader_enable=control->crt_shader_enabled;
    r->crt_mask_enable=control->crt_mask_enabled;
    r->crt_scanlines_enable=control->crt_scanlines_enabled;
    r->crt_gamma_enable=control->crt_gamma_enabled;
    r->crt_curvature=control->crt_curvature;
    r->crt_vignette=control->crt_vignette;
  }
  if(fields&GML_RENDER_CONTROL_FAST_FORWARD)
    r->crt_ff=control->fast_forward;
  if(fields&GML_RENDER_CONTROL_FAST_ALPHA)
    r->fast_alpha_cull=control->fast_alpha_cull;
  if(fields&GML_RENDER_CONTROL_WIDE_ASPECT){
    r->aspect_fullwidth=control->wide_aspect_active;
    r->aspect_wide_w=control->wide_width;
    r->aspect_wide_h=control->wide_height;
  }
}
int gml_render_resource_metrics(const GmlRender *r,
                                GmlRenderResourceMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!r) return 0;
  if(metrics){
    metrics->atlas_count=r->n_atlas;
    metrics->sprite_count=r->n_spr;
    metrics->texture_page_count=r->n_tpag;
  }
  return 1;
}
int gml_render_diagnostic_metrics(const GmlRender *r,
                                  GmlRenderDiagnosticMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!r) return 0;
  if(metrics){
    metrics->active_shader=r->active_shader;
    metrics->pending_fill=r->pending_fill;
    metrics->pending_underlay=r->pending_underlay;
  }
  return 1;
}
/* The world rectangle the target currently shows.
 *
 * Target metrics are in target pixels, and a view that does not match the target introduces a
 * scale between those and the coordinates content is authored in. Anything that selects world-space
 * content by what is visible — the tile grid above all — needs the rectangle in authored units, and
 * only the renderer knows the transform relating the two. Reading the target metrics directly and
 * dividing by a cell size in authored units mixes the two spaces and selects the wrong region. */
int gml_render_world_view(const GmlRender *r,double *x,double *y,
                          double *width,double *height){
  if(!r) return 0;
  double scale_x=r->world_transform_active && r->world_scale_x>0.0 ? r->world_scale_x : 1.0;
  double scale_y=r->world_transform_active && r->world_scale_y>0.0 ? r->world_scale_y : 1.0;
  if(x) *x=r->cam_x/scale_x;
  if(y) *y=r->cam_y/scale_y;
  if(width) *width=(double)r->fbw/scale_x;
  if(height) *height=(double)r->fbh/scale_y;
  return 1;
}
int gml_render_target_metrics(const GmlRender *r,GmlRenderTargetMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!r) return 0;
  if(metrics){
    metrics->width=r->fbw; metrics->height=r->fbh;
    metrics->camera_x=r->cam_x; metrics->camera_y=r->cam_y;
  }
  return 1;
}
void gml_render_target_metrics_update(GmlRender *r,
                                      const GmlRenderTargetMetrics *metrics,
                                      unsigned fields){
  if(!r || !metrics) return;
  if(fields&GML_RENDER_TARGET_WIDTH) r->fbw=metrics->width;
  if(fields&GML_RENDER_TARGET_HEIGHT) r->fbh=metrics->height;
  if(fields&GML_RENDER_TARGET_CAMERA){
    r->cam_x=metrics->camera_x;
    r->cam_y=metrics->camera_y;
  }
}
int gml_render_presentation_metrics(const GmlRender *r,
                                    GmlRenderPresentationMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!r) return 0;
  if(metrics){
    metrics->requested_width=r->resolution_w;
    metrics->requested_height=r->resolution_h;
    metrics->effective_width=r->presentation_w;
    metrics->effective_height=r->presentation_h;
    metrics->wide_aspect_active=r->aspect_fullwidth;
    metrics->wide_width=r->aspect_wide_w;
    metrics->wide_height=r->aspect_wide_h;
    metrics->gui_pass_active=r->gui_pass_active;
    metrics->gui_base_width=r->gui_base_logical_w;
    metrics->gui_base_height=r->gui_base_logical_h;
    metrics->application_width=r->app_w;
    metrics->application_height=r->app_h;
    metrics->application_owned=r->app_surface_owned!=NULL;
    metrics->application_draw_enabled=r->app_draw_enable;
    metrics->interpolation=r->interp;
    metrics->application_phase_active=r->app_phase_y!=NULL;
  }
  return 1;
}
void gml_render_presentation_effective_set(GmlRender *r,int width,int height){
  if(!r) return;
  r->presentation_w=width;
  r->presentation_h=height;
}
void gml_render_sample_planes_update(GmlRender *r,
                                     const GmlRenderSamplePlanes *planes,
                                     unsigned fields){
  if(!r || !planes) return;
  if(fields&GML_RENDER_SAMPLE_PLANES_APPLICATION){
    r->app_phase_y=planes->application_vertical;
    for(int i=0;i<3;i++)
      r->app_interp_phase[i]=planes->application_interpolated[i];
  }
  if(fields&GML_RENDER_SAMPLE_PLANES_CLASSIC){
    r->classic_phase_y=planes->classic_vertical;
    for(int i=0;i<3;i++)
      r->classic_interp_phase[i]=planes->classic_interpolated[i];
  }
}
int gml_render_target_coverage(const GmlRender *r,
                               GmlRenderTargetCoverage *coverage){
  if(coverage) memset(coverage,0,sizeof(*coverage));
  if(!r) return 0;
  if(coverage){
    coverage->opaque_known=r->fb_opaque_known;
    coverage->all_opaque=r->fb_all_opaque;
    coverage->all_transparent=r->fb_all_transparent;
  }
  return 1;
}
void gml_render_target_coverage_update(GmlRender *r,
                                       const GmlRenderTargetCoverage *coverage,
                                       unsigned fields){
  if(!r || !coverage) return;
  if(fields&GML_RENDER_COVERAGE_OPAQUE_KNOWN)
    r->fb_opaque_known=coverage->opaque_known;
  if(fields&GML_RENDER_COVERAGE_ALL_OPAQUE)
    r->fb_all_opaque=coverage->all_opaque;
  if(fields&GML_RENDER_COVERAGE_ALL_TRANSPARENT)
    r->fb_all_transparent=coverage->all_transparent;
}
void gml_render_application_surface_bind(GmlRender *r,uint32_t *pixels,
                                         int width,int height,int opaque){
  if(!r) return;
  r->app_surface=pixels;
  r->app_w=width;
  r->app_h=height;
  r->app_surface_opaque=opaque!=0;
}
int gml_render_application_surface_owned_clear(
  GmlRender *r,uint32_t color,GmlRenderApplicationWriteView *view){
  if(view) memset(view,0,sizeof(*view));
  if(!r || !r->app_surface_owned || r->app_w<=0 || r->app_h<=0) return 0;
  size_t count=(size_t)r->app_w*(size_t)r->app_h;
  for(size_t i=0;i<count;i++) r->app_surface_owned[i]=color;
  if(view){
    view->pixels=r->app_surface_owned;
    view->width=r->app_w;
    view->height=r->app_h;
  }
  return 1;
}
int gml_render_application_surface_owned_view(
  GmlRender *r,GmlRenderApplicationWriteView *view){
  if(view) memset(view,0,sizeof(*view));
  if(!r || !r->app_surface_owned || r->app_w<=0 || r->app_h<=0) return 0;
  if(view){
    view->pixels=r->app_surface_owned;
    view->width=r->app_w;
    view->height=r->app_h;
  }
  return 1;
}
int gml_render_application_surface_select_owned(GmlRender *r,int opaque){
  if(!r || !r->app_surface_owned) return 0;
  r->app_surface=r->app_surface_owned;
  r->app_surface_opaque=opaque!=0;
  return 1;
}
int gml_render_surface_mirror_pixels(GmlRender *r,int destination,
                                     uint32_t *pixels,int width,int height,
                                     int opaque){
  if(!r || destination<=0 || !pixels || width<=0 || height<=0 ||
     !gml_surface_exists(r,destination)) return 0;
  uint32_t *saved_pixels=r->app_surface;
  int saved_width=r->app_w;
  int saved_height=r->app_h;
  int saved_opaque=r->app_surface_opaque;
  gml_render_application_surface_bind(r,pixels,width,height,opaque);
  int mirrored=0;
  if(gml_surface_set_target(r,destination)){
    gml_draw_surface_stretched(r,0,0,0,
      gml_surface_width(r,destination),gml_surface_height(r,destination),
      0xFFFFFF,1.0);
    gml_surface_reset_target(r);
    mirrored=1;
  }
  gml_render_application_surface_bind(
    r,saved_pixels,saved_width,saved_height,saved_opaque);
  return mirrored;
}
int gml_render_draw_state_get(const GmlRender *r,GmlRenderDrawState *state){
  if(!state) return 0;
  memset(state,0,sizeof(*state));
  state->color=0xFFFFFFu;
  state->alpha=1.0;
  state->font=-1;
  state->alpha_blend=1;
  state->circle_precision=24;
  state->blend_equation=1;
  state->blend_equation_alpha=1;
  state->color_write_mask=0x0Fu;
  if(!r) return 0;
  state->color=r->color;
  state->alpha=r->alpha;
  state->font=r->font;
  state->horizontal_alignment=r->halign;
  state->vertical_alignment=r->valign;
  state->alpha_blend=r->alphablend;
  state->circle_precision=r->circle_precision;
  state->interpolation=r->interp;
  state->blend_mode=r->blendmode;
  state->blend_equation=r->blend_equation;
  state->blend_equation_alpha=r->blend_equation_alpha;
  state->alpha_test_enable=r->alpha_test_enable;
  state->alpha_test_reference=r->alpha_test_ref;
  state->color_write_mask=r->color_write_mask;
  return 1;
}
void gml_render_draw_state_update(GmlRender *r,const GmlRenderDrawState *state,
                                  unsigned fields){
  if(!r || !state) return;
  if(fields&GML_RENDER_DRAW_STATE_COLOR) r->color=state->color;
  if(fields&GML_RENDER_DRAW_STATE_ALPHA) r->alpha=state->alpha;
  if(fields&GML_RENDER_DRAW_STATE_FONT) r->font=state->font;
  if(fields&GML_RENDER_DRAW_STATE_HORIZONTAL_ALIGNMENT)
    r->halign=state->horizontal_alignment;
  if(fields&GML_RENDER_DRAW_STATE_VERTICAL_ALIGNMENT)
    r->valign=state->vertical_alignment;
  if(fields&GML_RENDER_DRAW_STATE_ALPHA_BLEND) r->alphablend=state->alpha_blend;
  if(fields&GML_RENDER_DRAW_STATE_CIRCLE_PRECISION)
    r->circle_precision=state->circle_precision;
  if(fields&GML_RENDER_DRAW_STATE_INTERPOLATION) r->interp=state->interpolation;
  if(fields&GML_RENDER_DRAW_STATE_BLEND_MODE) r->blendmode=state->blend_mode;
  if(fields&GML_RENDER_DRAW_STATE_BLEND_EQUATION)
    r->blend_equation=state->blend_equation;
  if(fields&GML_RENDER_DRAW_STATE_BLEND_EQUATION_ALPHA)
    r->blend_equation_alpha=state->blend_equation_alpha;
  if(fields&GML_RENDER_DRAW_STATE_ALPHA_TEST_ENABLE)
    r->alpha_test_enable=state->alpha_test_enable;
  if(fields&GML_RENDER_DRAW_STATE_ALPHA_TEST_REFERENCE)
    r->alpha_test_ref=(uint8_t)state->alpha_test_reference;
  if(fields&GML_RENDER_DRAW_STATE_COLOR_WRITE_MASK)
    r->color_write_mask=(uint8_t)state->color_write_mask;
}
int gml_render_gpu_state_push(GmlRender *r){
  if(!r || r->gpu_state_sp>=16) return 0;
  struct GmlGpuState *state=&r->gpu_state_stack[r->gpu_state_sp++];
  state->alphablend=r->alphablend;
  state->alpha_test_enable=r->alpha_test_enable;
  state->alpha_test_ref=r->alpha_test_ref;
  state->blendmode=r->blendmode;
  state->blend_equation=r->blend_equation;
  state->blend_equation_alpha=r->blend_equation_alpha;
  state->interp=r->interp;
  state->color_write_mask=r->color_write_mask;
  return 1;
}
int gml_render_gpu_state_pop(GmlRender *r){
  if(!r || r->gpu_state_sp<=0) return 0;
  struct GmlGpuState *state=&r->gpu_state_stack[--r->gpu_state_sp];
  r->alphablend=state->alphablend;
  r->alpha_test_enable=state->alpha_test_enable;
  r->alpha_test_ref=state->alpha_test_ref;
  r->blendmode=state->blendmode;
  r->blend_equation=state->blend_equation;
  r->blend_equation_alpha=state->blend_equation_alpha;
  r->interp=state->interp;
  r->color_write_mask=state->color_write_mask;
  return 1;
}
void gml_render_application_surface_set_draw_enabled(GmlRender *r,int enabled){
  if(r) r->app_draw_enable=enabled;
}
int gml_render_target_pixel(GmlRender *r,int x,int y,uint32_t *pixel){
  if(pixel) *pixel=0;
  if(!r || !pixel) return 0;
  gml_render_maybe_prepare_draw(r);
  if(!r->fb || x<0 || y<0 || x>=r->fbw || y>=r->fbh) return 0;
  *pixel=r->fb[(size_t)y*r->fbw+x];
  return 1;
}
void gml_render_shader_set_current(GmlRender *r,int shader){
  if(r) r->active_shader=shader;
}
int gml_render_shader_current(const GmlRender *r){
  return r?r->active_shader:-1;
}

#define GML_RENDER_SHADER_HANDLE_STRIDE 64
#define GML_RENDER_SHADER_HANDLE(shader,slot) \
  ((shader)*GML_RENDER_SHADER_HANDLE_STRIDE+(slot))
#define GML_RENDER_NOISE_JUMBLE_HANDLE_BASE 20

int gml_render_shader_is_compiled(const GmlRender *r,int shader){
  if(!r || shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return 0;
  const struct GmlShaderPal *recognized=&r->shader_pal[shader];
  return recognized->has || recognized->lut || recognized->grid ||
         recognized->alpha_discard || recognized->dual_sample ||
         recognized->paint || recognized->grayscale ||
         recognized->solid_alpha_mask ||
         recognized->solid_blur_alpha ||
         recognized->radial_wave ||
         recognized->noise_jumble ||
         (recognized->hsv_scan && r->crt_shader_enable) ||
         (recognized->sampled_crt && r->crt_shader_enable) ||
         (recognized->crt && r->crt_shader_enable);
}

int gml_render_shader_uniform_handle(const GmlRender *r,int shader,const char *name){
  if(r && name && shader>=0 && shader<r->n_shader_pal && r->shader_pal){
    const struct GmlShaderPal *recognized=&r->shader_pal[shader];
    if(recognized->noise_jumble)
      for(int index=0;index<GML_NOISE_JUMBLE_UNIFORM_COUNT;index++)
        if(!strcmp(name,recognized->noise_jumble_uniform[index]))
          return GML_RENDER_SHADER_HANDLE(
            shader,GML_RENDER_NOISE_JUMBLE_HANDLE_BASE+index);
    if(recognized->lut && !strcmp(name,recognized->lut_row_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,1);
    if(recognized->lut_indexed){
      if(!strcmp(name,recognized->lut_uvs_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,15);
      if(!strcmp(name,recognized->lut_offset_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,16);
      if(!strcmp(name,recognized->lut_colors_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,17);
      if(recognized->lut_has_colorise &&
         !strcmp(name,recognized->lut_colorise_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,18);
      if(recognized->lut_has_bounds &&
         !strcmp(name,recognized->lut_bounds_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,19);
    }
    if(recognized->grid){
      if(!strcmp(name,recognized->grid_pixel_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,7);
      if(!strcmp(name,recognized->grid_uvs_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,8);
      if(!strcmp(name,recognized->grid_id_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,9);
    }
    if(recognized->crt){
      if(recognized->crt_sizes_uniform[0] &&
         !strcmp(name,recognized->crt_sizes_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,3);
      if(recognized->crt_distortion_uniform[0] &&
         !strcmp(name,recognized->crt_distortion_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,4);
      if(recognized->crt_distort_uniform[0] &&
         !strcmp(name,recognized->crt_distort_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,5);
      if(recognized->crt_border_uniform[0] &&
         !strcmp(name,recognized->crt_border_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,6);
    }
    if(recognized->sampled_crt){
      for(int index=0;index<20;index++)
        if(!strcmp(name,recognized->sampled_crt_uniform[index]))
          return GML_RENDER_SHADER_HANDLE(shader,20+index);
    }
    if(recognized->dual_sample){
      if(!strcmp(name,recognized->dual_uniform[0]))
        return GML_RENDER_SHADER_HANDLE(shader,10);
      if(!strcmp(name,recognized->dual_uniform[1]))
        return GML_RENDER_SHADER_HANDLE(shader,11);
    }
    if(recognized->radial_wave)
      for(int index=0;index<6;index++)
        if(!strcmp(name,recognized->radial_wave_uniform[index]))
          return GML_RENDER_SHADER_HANDLE(shader,44+index);
    if(recognized->uv_wave_mode && !strcmp(name,recognized->uv_wave_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,50);
    if(recognized->paint){
      if(!strcmp(name,recognized->paint_resolution_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,12);
      if(!strcmp(name,recognized->paint_time_uniform))
        return GML_RENDER_SHADER_HANDLE(shader,13);
    }
    if(recognized->grayscale &&
       recognized->grayscale_has_alpha_uniform &&
       !strcmp(name,recognized->grayscale_alpha_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,14);
    if(recognized->solid_alpha_mask &&
       !strcmp(name,recognized->solid_alpha_mask_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,51);
    if(recognized->solid_blur_alpha &&
       !strcmp(name,recognized->solid_blur_alpha_uniform))
      return GML_RENDER_SHADER_HANDLE(shader,52);
  }
  return shader>=0?GML_RENDER_SHADER_HANDLE(shader,63):-1;
}

int gml_render_shader_sampler_handle(const GmlRender *r,int shader,const char *name){
  if(r && name && shader>=0 && shader<r->n_shader_pal && r->shader_pal){
    const struct GmlShaderPal *recognized=&r->shader_pal[shader];
    if(recognized->sampled_crt)
      for(int index=0;index<3;index++)
        if(!strcmp(name,recognized->sampled_crt_sampler[index]))
          return GML_RENDER_SHADER_HANDLE(shader,40+index);
  }
  return shader>=0?GML_RENDER_SHADER_HANDLE(shader,2):-1;
}

void gml_render_shader_uniform_set(GmlRender *r,int handle,const double values[4]){
  if(!r || !values || handle<0) return;
  int shader=handle/GML_RENDER_SHADER_HANDLE_STRIDE;
  int slot=handle%GML_RENDER_SHADER_HANDLE_STRIDE;
  if(shader<0 || shader>=r->n_shader_pal || !r->shader_pal) return;
  struct GmlShaderPal *recognized=&r->shader_pal[shader];
  if(recognized->noise_jumble &&
     slot>=GML_RENDER_NOISE_JUMBLE_HANDLE_BASE &&
     slot<GML_RENDER_NOISE_JUMBLE_HANDLE_BASE+GML_NOISE_JUMBLE_UNIFORM_COUNT){
    int index=slot-GML_RENDER_NOISE_JUMBLE_HANDLE_BASE;
    recognized->noise_jumble_value[index][0]=(float)values[0];
    if(index==GML_NOISE_JUMBLE_RESOLUTION)
      recognized->noise_jumble_value[index][1]=(float)values[1];
    return;
  }
  if(recognized->sampled_crt && slot>=20 && slot<40){
    int index=slot-20;
    for(int component=0;component<4;component++)
      recognized->sampled_crt_value[index][component]=(float)values[component];
    return;
  }
  if(slot==1){
    if(recognized->lut) recognized->lut_row=(float)values[0];
    return;
  }
  if(recognized->lut_indexed && slot>=15 && slot<=19){
    if(slot==15)
      for(int component=0;component<4;component++)
        recognized->lut_uvs[component]=(float)values[component];
    else if(slot==16) recognized->lut_offset=(float)values[0];
    else if(slot==17) recognized->lut_colors=(float)values[0];
    else if(slot==18)
      for(int component=0;component<4;component++)
        recognized->lut_colorise[component]=(float)values[component];
    else
      for(int component=0;component<4;component++)
        recognized->lut_bounds[component]=(float)values[component];
    return;
  }
  if(recognized->grid){
    if(slot==7){
      recognized->grid_pixel[0]=(float)values[0];
      recognized->grid_pixel[1]=(float)values[1];
      return;
    }
    if(slot==8){
      for(int component=0;component<4;component++)
        recognized->grid_uvs[component]=(float)values[component];
      return;
    }
    if(slot==9){ recognized->grid_id=(float)values[0]; return; }
  }
  if(recognized->dual_sample){
    if(slot==10){ recognized->dual_value[0]=(float)values[0]; return; }
    if(slot==11){ recognized->dual_value[1]=(float)values[0]; return; }
  }
  if(recognized->radial_wave && slot>=44 && slot<50){
    int index=slot-44;
    recognized->radial_wave_value[index][0]=(float)values[0];
    if(index==1 || index==2)
      recognized->radial_wave_value[index][1]=(float)values[1];
    return;
  }
  if(recognized->uv_wave_mode && slot==50){
    recognized->uv_wave_time=(float)values[0];
    return;
  }
  if(recognized->paint){
    if(slot==12){
      for(int component=0;component<3;component++)
        recognized->paint_resolution[component]=(float)values[component];
      return;
    }
    if(slot==13){ recognized->paint_time=(float)values[0]; return; }
  }
  if(recognized->grayscale && slot==14){
    recognized->grayscale_alpha=(float)values[0];
    return;
  }
  if(recognized->solid_alpha_mask && slot==51){
    uint32_t packed=0;
    for(int component=0;component<3;component++){
      recognized->solid_alpha_mask_colour[component]=(float)values[component];
      int channel=(int)floor(values[component]*255.0+0.5);
      if(channel<0) channel=0;
      else if(channel>255) channel=255;
      packed|=(uint32_t)channel<<(16-component*8);
    }
    recognized->solid_alpha_mask_rgb=packed;
    return;
  }
  if(recognized->solid_blur_alpha && slot==52){
    uint32_t packed=0;
    for(int component=0;component<3;component++){
      recognized->solid_blur_alpha_colour[component]=(float)values[component];
      int channel=(int)floor(values[component]*255.0+0.5);
      if(channel<0) channel=0;
      else if(channel>255) channel=255;
      packed|=(uint32_t)channel<<(16-component*8);
    }
    recognized->solid_blur_alpha_rgb=packed;
    return;
  }
  if(!recognized->crt) return;
  switch(slot){
    case 3:
      for(int component=0;component<4;component++)
        recognized->crt_sizes[component]=(float)values[component];
      break;
    case 4: recognized->crt_distortion=(float)values[0]; break;
    case 5: recognized->crt_distort=values[0]!=0.0; break;
    case 6: recognized->crt_border=values[0]!=0.0; break;
    default: break;
  }
}

int gml_render_shader_texture_stage_set(
  GmlRender *r,int stage,int texture,GmlRenderShaderTextureBinding *binding){
  if(binding) memset(binding,0,sizeof(*binding));
  if(!r || (((uint32_t)texture&GML_TEX_KIND_MASK)!=GML_TEX_SPR_TAG)) return 0;
  int shader=stage/GML_RENDER_SHADER_HANDLE_STRIDE;
  int slot=stage%GML_RENDER_SHADER_HANDLE_STRIDE;
  int sprite=(texture>>10)&0xFFFF;
  int frame=texture&0x3FF;
  if(shader>=0 && shader<r->n_shader_pal && r->shader_pal &&
     r->shader_pal[shader].sampled_crt && slot>=40 && slot<43){
    int sampler=slot-40;
    r->shader_pal[shader].sampled_crt_sprite[sampler]=sprite;
    r->shader_pal[shader].sampled_crt_frame[sampler]=frame;
    if(binding){
      binding->kind=GML_RENDER_SHADER_TEXTURE_SAMPLED;
      binding->sampler=sampler;
      binding->sprite=sprite;
      binding->frame=frame;
    }
  } else {
    r->lut_pal_sprite=sprite;
    r->lut_pal_frame=frame;
    if(binding){
      binding->kind=GML_RENDER_SHADER_TEXTURE_PALETTE;
      binding->sprite=sprite;
      binding->frame=frame;
    }
  }
  return 1;
}

#undef GML_RENDER_SHADER_HANDLE
#undef GML_RENDER_SHADER_HANDLE_STRIDE
#undef GML_RENDER_NOISE_JUMBLE_HANDLE_BASE
int gml_render_backend_draw_view(GmlRender *r,GmlRenderBackendDrawView *view){
  if(view) memset(view,0,sizeof(*view));
  if(!r || !view) return 0;
  gml_render_flush_rotated_batch(r);
  view->host=r->win?r->win->host:NULL;
  view->pixels=r->fb;
  if(r->target_sp==0 && r->fb==r->base_fb){
    view->interpolation_pixels[0]=r->classic_interp_phase[0];
    view->interpolation_pixels[1]=r->classic_interp_phase[1];
    view->interpolation_pixels[2]=r->classic_interp_phase[2];
  }
  view->frame=r->frame;
  view->camera_x=r->cam_x; view->camera_y=r->cam_y;
  int world_coordinates=r->world_transform_active && !r->gui_pass_active &&
    (r->target_sp==0 || r->target_id==-2) && r->target_id<0;
  view->coordinate_scale_x=world_coordinates?r->world_scale_x:1.0;
  view->coordinate_scale_y=world_coordinates?r->world_scale_y:1.0;
  view->alpha=r->alpha;
  view->width=r->fbw; view->height=r->fbh;
  view->interpolate=r->interp;
  view->alpha_blend=r->alphablend; view->blend_mode=r->blendmode;
  view->circle_precision=r->circle_precision;
  return 1;
}
void gml_render_backend_prepare_draw(GmlRender *r){
  gml_render_maybe_prepare_draw(r);
}
int gml_render_backend_prepare_draw_view(GmlRender *r,GmlRenderBackendDrawView *view){
  gml_render_backend_prepare_draw(r);
  return gml_render_backend_draw_view(r,view);
}
void gml_render_backend_sync_camera(GmlRender *r,double translation_x,double translation_y){
  if(!r) return;
  if(r->world_transform_active && !r->gui_pass_active &&
     (r->target_sp==0 || r->target_id==-2) && r->target_id<0){
    translation_x*=r->world_scale_x;
    translation_y*=r->world_scale_y;
  }
  r->cam_x=r->projection_cam_x-translation_x;
  r->cam_y=r->projection_cam_y-translation_y;
}
void gml_render_texture_page_cache_clear(GmlTpag *page){
  if(!page) return;
  int sx=page->sx,sy=page->sy,sw=page->sw,sh=page->sh;
  int tx=page->tx,ty=page->ty,bw=page->bw,bh=page->bh,atlas=page->atlas;
  free(page->alpha_row_min);
  free(page->alpha_row_max);
  free(page->alpha_qrow_min);
  free(page->alpha_qrow_max);
  free(page->alpha_qrow_built);
  free(page->alpha_runs);
  free(page->alpha8_cache);
  free(page->argb_cache);
  free(page->solid_blur_alpha_cache);
  for(int phase=0;phase<3;phase++) free(page->interp_phase_cache[phase]);
  free(page->fast8_draw_cache);
  memset(page,0,sizeof(*page));
  page->sx=sx; page->sy=sy; page->sw=sw; page->sh=sh;
  page->tx=tx; page->ty=ty; page->bw=bw; page->bh=bh; page->atlas=atlas;
}
void gml_render_interpolated_subrect_cache_clear(GmlRender *render){
  if(!render) return;
  for(int index=0;index<render->interp_subrect_count;index++)
    for(int phase=0;phase<3;phase++)
      free(render->interp_subrect_cache[index].phase[phase]);
  free(render->interp_subrect_cache);
  render->interp_subrect_cache=NULL;
  render->interp_subrect_count=0;
  render->interp_subrect_capacity=0;
}
void gml_render_backend_draw_map_point(const GmlRender *r,double *x,double *y){
  gml_render_draw_map_point(r,x,y);
}
void gml_render_free(GmlRender *r){
  gml_render_flush_rotated_batch(r);
  atlas_pool_free(r);   /* before atlas teardown: workers read r->atlas/win */
  gml_row_pool_free(r);  /* compositor workers must stop before their renderer workspaces vanish */
  for(int i=0;i<GML_MAX_SURFACES;i++) free(r->surface[i].px);
  for(int i=0;i<GML_MAX_FONTS;i++){ free(r->fonts[i].map); free(r->fonts[i].glyphs); }
  free(r->default_font.map); free(r->default_font.glyphs);
  for(int i=0;i<r->n_spr;i++){
    gml_render_sprite_cache_free(&r->spr[i]);
    gml_render_free_spine(r->spr[i].spine);
    free(r->spr[i].runtime_rgba);
    free(r->spr[i].runtime_row_min);
    free(r->spr[i].runtime_row_max);
    free(r->spr[i].owned_name);
    free(r->spr[i].runtime_source_path);
  }
  for(int i=0;i<r->n_tpag;i++){
    gml_render_texture_page_cache_clear(&r->tpag[i]);
  }
  gml_render_interpolated_subrect_cache_clear(r);
  for(int i=0;i<r->n_atlas;i++){
    free(r->atlas[i].px);
    free(r->atlas[i].external_blob);
  }
  for(int i=0;i<r->n_spr;i++) free(r->spr[i].frame);
  free(r->classic_info_native_pixels);
  free(r->crt_gamma_scratch); free(r->crt_cols_scratch); free(r->crt_conv_scratch);
  crt_tables_free(r);
  crt_warp_geometry_cache_free(r);
  hsv_binary_lut_cache_free(r);
  free(r->layer_noise_rgb); r->layer_noise_rgb=NULL;
  free(r->layer_filter_src); free(r->layer_filter_work); free(r->layer_filter_aux);
  free(r->layer_blur_taps);
  r->layer_filter_src=r->layer_filter_work=r->layer_filter_aux=NULL;
  r->layer_blur_taps=NULL; r->layer_blur_tap_capacity=0;
  r->layer_filter_capacity=0; r->layer_filter_active=0;
  free(r->app_surface_owned); r->app_surface_owned=NULL;
  free(r->rotated_batch); r->rotated_batch=NULL;
  r->rotated_batch_count=r->rotated_batch_capacity=0;
  free(r->atlas); free(r->spr); free(r->tpag); free(r->bg); free(r->shader_pal);
  free(r->tpag_ptr); r->tpag_ptr=NULL;
}
void gml_render_begin(GmlRender *r, uint32_t *fb, int w, int h, double cx, double cy){
  gml_render_flush_rotated_batch(r);
  r->fb=fb; r->fbw=w; r->fbh=h; r->base_fb=fb; r->base_fbw=w; r->base_fbh=h;
  r->gui_pass_active=0;
  r->gui_base_logical_w=r->gui_base_logical_h=0;
  r->gui_logical_w=r->gui_logical_h=0;
  r->gui_scale_x=r->gui_scale_y=1.0;
  r->gui_maximise_active=0;
  r->gui_maximise_xscale=r->gui_maximise_yscale=0.0;
  r->gui_maximise_xoffset=r->gui_maximise_yoffset=0.0;
  r->classic_phase_y=NULL;
  r->classic_interp_phase[0]=NULL;
  r->classic_interp_phase[1]=NULL;
  r->classic_interp_phase[2]=NULL;
  r->target_sp=0; r->target_id=-1;
  r->world_transform_active=0;
  r->world_scale_x=r->world_scale_y=1.0;
  r->projection_cam_x=cx; r->projection_cam_y=cy; r->cam_x=cx; r->cam_y=cy;
  gml_d3_sync_render_camera(r);
  r->blendmode=0;
  r->blend_equation=r->blend_equation_alpha=1; r->gpu_state_sp=0; r->active_shader=-1;   /* GM resets blend mode/shader each frame */
  r->fb_opaque_known=0; r->fb_all_opaque=0; r->fb_all_transparent=0;
  r->pending_underlay=0; r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
  r->pending_fill=0; r->pending_fill_color=0;
}
void gml_render_world_set_logical_extent(GmlRender *r,int width,int height){
  if(!r || width<=0 || height<=0 || r->fbw<=0 || r->fbh<=0 ||
     r->target_sp!=0 || r->target_id>=0) return;
  double scale_x=(double)r->fbw/(double)width;
  double scale_y=(double)r->fbh/(double)height;
  if(!isfinite(scale_x) || !isfinite(scale_y) || scale_x<=0.0 || scale_y<=0.0) return;
  r->world_transform_active=1;
  r->world_scale_x=scale_x;
  r->world_scale_y=scale_y;
  r->projection_cam_x*=scale_x;
  r->projection_cam_y*=scale_y;
  r->cam_x*=scale_x;
  r->cam_y*=scale_y;
  gml_d3_sync_render_camera(r);
}
void gml_render_gui_begin(GmlRender *r, int logical_w, int logical_h){
  if(!r) return;
  if(logical_w<=0) logical_w=r->fbw;
  if(logical_h<=0) logical_h=r->fbh;
  r->gui_pass_active=1;
  r->gui_base_logical_w=logical_w;
  r->gui_base_logical_h=logical_h;
  r->gui_logical_w=logical_w;
  r->gui_logical_h=logical_h;
  r->gui_scale_x=r->gui_scale_y=1.0;
  r->gui_maximise_active=0;
  r->gui_maximise_xscale=r->gui_maximise_yscale=0.0;
  r->gui_maximise_xoffset=r->gui_maximise_yoffset=0.0;
}
static void gui_transform_update(GmlRender *r){
  if(!r || !r->gui_pass_active) return;
  double default_x=r->gui_base_logical_w>0 && r->gui_logical_w>0
    ? (double)r->gui_base_logical_w/(double)r->gui_logical_w : 1.0;
  double default_y=r->gui_base_logical_h>0 && r->gui_logical_h>0
    ? (double)r->gui_base_logical_h/(double)r->gui_logical_h : 1.0;
  r->gui_scale_x=r->gui_maximise_active ? r->gui_maximise_xscale : default_x;
  r->gui_scale_y=r->gui_maximise_active ? r->gui_maximise_yscale : default_y;
}
void gml_render_gui_set_size(GmlRender *r, int logical_w, int logical_h){
  if(!r || !r->gui_pass_active || logical_w<=0 || logical_h<=0) return;
  r->gui_logical_w=logical_w;
  r->gui_logical_h=logical_h;
  gui_transform_update(r);
}
void gml_render_gui_set_maximise(GmlRender *r, int active, double xscale, double yscale,
                                 double xoffset, double yoffset,
                                 int window_w, int window_h){
  if(!r || !r->gui_pass_active) return;
  r->gui_maximise_active=active!=0;
  /* This transform is defined in window pixels. A frontend can publish a smaller
   * presentation raster for the same window image, so convert both scale and offset into the
   * current target before drawing. Zero is the no-argument form and therefore means 1:1 with
   * the logical window, not the display_set_gui_size-derived transform. */
  double target_per_window_x=window_w>0 && r->gui_base_logical_w>0
    ? (double)r->gui_base_logical_w/(double)window_w : 1.0;
  double target_per_window_y=window_h>0 && r->gui_base_logical_h>0
    ? (double)r->gui_base_logical_h/(double)window_h : 1.0;
  r->gui_maximise_xscale=r->gui_maximise_active
    ? (xscale>0.0?xscale:1.0)*target_per_window_x : 0.0;
  r->gui_maximise_yscale=r->gui_maximise_active
    ? (yscale>0.0?yscale:1.0)*target_per_window_y : 0.0;
  r->gui_maximise_xoffset=r->gui_maximise_active?xoffset*target_per_window_x:0.0;
  r->gui_maximise_yoffset=r->gui_maximise_active?yoffset*target_per_window_y:0.0;
  gui_transform_update(r);
}
void gml_render_gui_end(GmlRender *r){
  if(!r) return;
  r->gui_pass_active=0;
  r->gui_base_logical_w=r->gui_base_logical_h=0;
  r->gui_logical_w=r->gui_logical_h=0;
  r->gui_scale_x=r->gui_scale_y=1.0;
  r->gui_maximise_active=0;
  r->gui_maximise_xscale=r->gui_maximise_yscale=0.0;
  r->gui_maximise_xoffset=r->gui_maximise_yoffset=0.0;
}
static inline int render_gui_transform_active_local(const GmlRender *r){
  return r && r->gui_pass_active && r->target_sp==0 && r->target_id<0;
}
static inline int render_world_transform_active_local(const GmlRender *r){
  return r && r->world_transform_active && !r->gui_pass_active &&
         (r->target_sp==0 || r->target_id==-2) && r->target_id<0;
}
static inline void render_gui_map_point_local(const GmlRender *r, double *x, double *y){
  if(!render_gui_transform_active_local(r)) return;
  if(x) *x = *x*r->gui_scale_x+r->gui_maximise_xoffset;
  if(y) *y = *y*r->gui_scale_y+r->gui_maximise_yoffset;
}
static inline void render_gui_map_scale_local(const GmlRender *r, double *xscale, double *yscale){
  if(!render_gui_transform_active_local(r)) return;
  if(xscale) *xscale *= r->gui_scale_x;
  if(yscale) *yscale *= r->gui_scale_y;
}
static inline void render_draw_map_point_local(const GmlRender *r,double *x,double *y){
  if(render_gui_transform_active_local(r)){
    render_gui_map_point_local(r,x,y);
    return;
  }
  if(!render_world_transform_active_local(r)) return;
  if(x) *x*=r->world_scale_x;
  if(y) *y*=r->world_scale_y;
}
static inline void render_draw_map_scale_local(
    const GmlRender *r,double *xscale,double *yscale){
  if(render_gui_transform_active_local(r)){
    render_gui_map_scale_local(r,xscale,yscale);
    return;
  }
  if(!render_world_transform_active_local(r)) return;
  if(xscale) *xscale*=r->world_scale_x;
  if(yscale) *yscale*=r->world_scale_y;
}
static inline double render_gui_logical_width_local(const GmlRender *r){
  return render_gui_transform_active_local(r) && r->gui_scale_x>0.0
       ? (double)r->fbw/r->gui_scale_x : (double)(r?r->fbw:0);
}
static inline double render_gui_logical_height_local(const GmlRender *r){
  return render_gui_transform_active_local(r) && r->gui_scale_y>0.0
       ? (double)r->fbh/r->gui_scale_y : (double)(r?r->fbh:0);
}
static inline double render_gui_logical_x_local(const GmlRender *r, double physical_x){
  return render_gui_transform_active_local(r) && r->gui_scale_x>0.0
       ? (physical_x-r->gui_maximise_xoffset)/r->gui_scale_x : physical_x;
}
static inline double render_gui_logical_y_local(const GmlRender *r, double physical_y){
  return render_gui_transform_active_local(r) && r->gui_scale_y>0.0
       ? (physical_y-r->gui_maximise_yoffset)/r->gui_scale_y : physical_y;
}
int gml_render_gui_transform_active(const GmlRender *r){
  return render_gui_transform_active_local(r);
}
void gml_render_draw_map_point(const GmlRender *r,double *x,double *y){
  render_draw_map_point_local(r,x,y);
}
void gml_render_draw_map_scale(const GmlRender *r,double *xscale,double *yscale){
  render_draw_map_scale_local(r,xscale,yscale);
}
void gml_render_gui_map_point(const GmlRender *r, double *x, double *y){
  render_gui_map_point_local(r,x,y);
}
void gml_render_gui_map_scale(const GmlRender *r, double *xscale, double *yscale){
  render_gui_map_scale_local(r,xscale,yscale);
}
double gml_render_gui_logical_width(const GmlRender *r){
  return render_gui_logical_width_local(r);
}
double gml_render_gui_logical_height(const GmlRender *r){
  return render_gui_logical_height_local(r);
}
double gml_render_gui_logical_x(const GmlRender *r, double physical_x){
  return render_gui_logical_x_local(r,physical_x);
}
double gml_render_gui_logical_y(const GmlRender *r, double physical_y){
  return render_gui_logical_y_local(r,physical_y);
}
#define gml_render_gui_transform_active render_gui_transform_active_local
#define gml_render_gui_map_point render_gui_map_point_local
#define gml_render_gui_map_scale render_gui_map_scale_local
#define gml_render_gui_logical_width render_gui_logical_width_local
#define gml_render_gui_logical_height render_gui_logical_height_local
#define gml_render_gui_logical_x render_gui_logical_x_local
#define gml_render_gui_logical_y render_gui_logical_y_local
#define gml_render_draw_map_point render_draw_map_point_local
#define gml_render_draw_map_scale render_draw_map_scale_local
void gml_render_set_pending_underlay(GmlRender *r, int x, int y, int w, int h){
  gml_render_flush_rotated_batch(r);
  if(!r || !r->app_surface || w<=0 || h<=0){
    if(r) r->pending_underlay=0;
    return;
  }
  r->pending_underlay=1;
  r->underlay_x=x;
  r->underlay_y=y;
  r->underlay_w=w;
  r->underlay_h=h;
}
void gml_render_cancel_pending_underlay(GmlRender *r){
  if(!r) return;
  r->pending_underlay=0;
  r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
}
void gml_render_cancel_pending_fill(GmlRender *r){
  if(!r) return;
  r->pending_fill=0;
  r->pending_fill_color=0;
}
void gml_render_set_pending_fill(GmlRender *r, uint32_t color){
  if(!r || !r->fb || r->fbw<=0 || r->fbh<=0) return;
  gml_render_flush_rotated_batch(r);
  gml_render_cancel_pending_underlay(r);
  r->pending_fill=1;
  r->pending_fill_color=color;
  r->fb_opaque_known=1;
  r->fb_all_opaque=((color>>24)==255u);
  r->fb_all_transparent=((color>>24)==0u);
  if(r->target_sp==0 && r->classic_phase_y){
    size_t n=(size_t)r->fbw*(size_t)r->fbh;
    uint32_t *p=r->classic_phase_y; size_t left=n;
    while(left>0){
      int run=left>(size_t)INT_MAX?INT_MAX:(int)left;
      gml_render_backend_fill_xrgb(p,run,color); p+=run; left-=(size_t)run;
    }
  }
  if(r->target_sp==0){
    size_t n=(size_t)r->fbw*(size_t)r->fbh;
    for(int q=0;q<3;q++) if(r->classic_interp_phase[q]){
      uint32_t *p=r->classic_interp_phase[q]; size_t left=n;
      while(left>0){
        int run=left>(size_t)INT_MAX?INT_MAX:(int)left;
        gml_render_backend_fill_xrgb(p,run,color); p+=run; left-=(size_t)run;
      }
    }
  }
}
void gml_render_flush_pending_fill(GmlRender *r){
  gml_render_flush_rotated_batch(r);
  if(!r || !r->pending_fill || !r->fb || r->fbw<=0 || r->fbh<=0) return;
  uint32_t color=r->pending_fill_color;
  r->pending_fill=0;
  r->fb_opaque_known=1;
  r->fb_all_opaque=((color>>24)==255u);
  r->fb_all_transparent=((color>>24)==0u);
  size_t n=(size_t)r->fbw*(size_t)r->fbh;
  uint32_t *p=r->fb;
  while(n>0){
    int run=n>(size_t)INT_MAX?INT_MAX:(int)n;
    gml_render_backend_fill_xrgb(p,run,color);
    p+=run;
    n-=(size_t)run;
  }
}
void gml_render_flush_pending_underlay(GmlRender *r){
  gml_render_flush_rotated_batch(r);
  if(!r || !r->pending_underlay) return;
  int x=r->underlay_x, y=r->underlay_y, w=r->underlay_w, h=r->underlay_h;
  r->pending_underlay=0;
  /* This is the host's internal application-surface presentation, not a GML draw call.
   * Routing it through gml_draw_surface_stretched while d3d is active projects the already-rendered
   * frame through the active camera a second time, producing missing scanline bands. Bypass
   * content shaders and projection, and perform the ordinary screen-space copy directly. */
  if(w>0 && h>0){
    int sw=r->app_w,sh=r->app_h;
    if(sw>0 && sh>0) draw_surface_region(r,0,0,0,sw,sh,x,y,w,h,0xFFFFFF,1.0);
  }
}
int rect_covers_target(GmlRender *r, int x0, int y0, int x1, int y1){
  if(!r || r->fbw<=0 || r->fbh<=0) return 0;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  return x0<=0 && y0<=0 && x1>=r->fbw && y1>=r->fbh;
}
static int pending_underlay_covered(GmlRender *r, int x0, int y0, int x1, int y1){
  if(!r || !r->pending_underlay) return 0;
  int ux0=r->underlay_x, uy0=r->underlay_y;
  int ux1=ux0+r->underlay_w, uy1=uy0+r->underlay_h;
  if(ux0<0) ux0=0;
  if(uy0<0) uy0=0;
  if(ux1>r->fbw) ux1=r->fbw;
  if(uy1>r->fbh) uy1=r->fbh;
  if(ux1<=ux0 || uy1<=uy0) return 1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  return x0<=ux0 && y0<=uy0 && x1>=ux1 && y1>=uy1;
}
void gml_render_prepare_draw(GmlRender *r){
  gml_render_flush_pending_underlay(r);
  gml_render_flush_pending_fill(r);
}
void gml_render_prepare_opaque_rect(GmlRender *r, int x0, int y0, int x1, int y1){
  if(!r) return;
  if(r->pending_underlay){
    if(pending_underlay_covered(r,x0,y0,x1,y1)) gml_render_cancel_pending_underlay(r);
    else gml_render_flush_pending_underlay(r);
  }
  if(r->pending_fill){
    if(rect_covers_target(r,x0,y0,x1,y1)) gml_render_cancel_pending_fill(r);
    else gml_render_flush_pending_fill(r);
  }
}
static inline void render_maybe_prepare_draw_local(GmlRender *r){
  if(r){
    if(!r->rotated_batch_building) gml_render_flush_rotated_batch(r);
    if(r->pending_underlay || r->pending_fill) gml_render_prepare_draw(r);
    r->fb_all_transparent=0;
  }
}
static inline void render_maybe_prepare_opaque_rect_local(GmlRender *r, int x0, int y0, int x1, int y1){
  if(r){
    if(r->pending_underlay || r->pending_fill) gml_render_prepare_opaque_rect(r,x0,y0,x1,y1);
    r->fb_all_transparent=0;
  }
}
void gml_render_maybe_prepare_draw(GmlRender *r){
  render_maybe_prepare_draw_local(r);
}
void gml_render_maybe_prepare_opaque_rect(GmlRender *r, int x0, int y0, int x1, int y1){
  render_maybe_prepare_opaque_rect_local(r,x0,y0,x1,y1);
}
#define gml_render_maybe_prepare_draw render_maybe_prepare_draw_local
#define gml_render_maybe_prepare_opaque_rect render_maybe_prepare_opaque_rect_local
int gml_classic_present_explicit_port(const GmlWin *win, int explicit_window,
                                      int canvas_w, int canvas_h,
                                      int port_x, int port_y, int port_w, int port_h,
                                      int *target_x, int *target_y,
                                      int *target_w, int *target_h){
  if(!win || !anygm_policy_uses_classic_runtime(win) || !explicit_window ||
     canvas_w<=0 || canvas_h<=0 || port_w<=0 || port_h<=0 ||
     !target_x || !target_y || !target_w || !target_h) return 0;
  /* window_set_size changes the host window, not the view port.  Classic GM
   * therefore leaves a smaller port at its declared coordinates and clears
   * the rest of the window instead of fitting that port to the new window. */
  *target_x=port_x;
  *target_y=port_y;
  *target_w=port_w;
  *target_h=port_h;
  return 1;
}

void gml_classic_room_window_size(int room_w, int room_h,
                                  int fixed_scale_pct,
                                  int configured_w, int configured_h,
                                  const int visible[8],
                                  const int xport[8], const int yport[8],
                                  const int wport[8], const int hport[8],
                                  int *window_w, int *window_h){
  int width=room_w>0?room_w:1;
  int height=room_h>0?room_h:1;
  int have_view=0;
  if(fixed_scale_pct>0 && configured_w>0 && configured_h>0){
    if(window_w) *window_w=configured_w;
    if(window_h) *window_h=configured_h;
    return;
  }
  if(visible && xport && yport && wport && hport){
    for(int i=0;i<8;i++) if(visible[i] && wport[i]>0 && hport[i]>0){
      int right=xport[i]+wport[i];
      int bottom=yport[i]+hport[i];
      if(!have_view){ width=right; height=bottom; have_view=1; }
      else { if(right>width) width=right; if(bottom>height) height=bottom; }
    }
  }
  if(width<1) width=1;
  if(height<1) height=1;
  if(window_w) *window_w=width;
  if(window_h) *window_h=height;
}

void gml_classic_present_adjust(const GmlWin *win, int source_w, int source_h,
                                int target_w, int target_h, int interpolated,
                                int *target_x, int *target_y){
  if(!win || !anygm_policy_uses_classic_runtime(win)) return;
  int gm8=anygm_policy_classic_modern_presentation(win);
  int centred_aspect=gm8 && win->classic_scaling<0;
  interpolated=gm8 && interpolated;
  if(target_x && source_w>0 && target_w>source_w){
    if(!interpolated && ((gm8 && target_w==source_w*2) || (!centred_aspect && target_w%source_w!=0)))
      (*target_x)--;
  }
  if(target_y && source_h>0 && target_h>source_h){
    if(!interpolated && ((gm8 && target_h==source_h*2) || (!centred_aspect && target_h%source_h!=0)))
      (*target_y)--;
  }
}
