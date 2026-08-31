/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer-owned surface storage, pool, copy, resize, and target-stack operations. */
#include "gml_render.h"
#include "gml_render_backend.h"
#include "gml_render_blit_internal.h"
#include "gml_render_internal.h"
#include "gml_render_pixel_internal.h"

#include "anygm_compatibility.h"
#include "anygm_host.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Colour and coverage counted apart. A surface can be fully authored and hold no colour at all:
 * a scanline or shadow mask is black with an alpha ramp, so a count of non-black RGB reads zero
 * over a mask that drew perfectly, and reading that zero as "nothing was drawn" is a wrong answer
 * an instrument handed out rather than a defect in the renderer. */
static void surface_coverage_counts(const uint32_t *px,int w,int h,int *colour,int *covered){
  int c=0,a=0;
  for(size_t i=0;i<(size_t)w*(size_t)h;i++){
    if(px[i]&0x00FFFFFFu) c++;
    if(px[i]&0xFF000000u) a++;
  }
  *colour=c; *covered=a;
}
static int surface_slot(int id){ return id>0 && id<=GML_MAX_SURFACES ? id-1 : -1; }
int surface_known_opaque(GmlRender *r, int id){
  if(!r) return 0;
  if(id==0) return r->app_surface_opaque;
  int i=surface_slot(id);
  return i>=0 && r->surface[i].live && r->surface[i].opaque_known && r->surface[i].all_opaque;
}
int surface_known_transparent(GmlRender *r, int id){
  if(!r || id==0) return 0;
  int i=surface_slot(id);
  return i>=0 && r->surface[i].live && r->surface[i].all_transparent;
}
static int surface_alpha_all_zero(const uint32_t *px, size_t n){
  if(!px) return 0;
  for(size_t i=0;i<n;i++) if(px[i]>>24) return 0;
  return 1;
}
static void surface_store_target_coverage(GmlRender *r){
  if(!r) return;
  int i=surface_slot(r->target_id);
  if(i>=0 && r->surface[i].live){
    /* No-op draws and readbacks can conservatively dirty this bit; verify cleared targets
     * at target close so later composites can skip surfaces with no covered pixels. */
    if(!r->fb_all_transparent && r->fb_opaque_known && !r->fb_all_opaque &&
       r->fb && r->fbw>0 && r->fbh>0 &&
       surface_alpha_all_zero(r->fb,(size_t)r->fbw*(size_t)r->fbh)){
      r->fb_all_transparent=1;
    }
    r->surface[i].opaque_known=r->fb_opaque_known;
    r->surface[i].all_opaque=r->fb_all_opaque;
    r->surface[i].all_transparent=r->fb_all_transparent;
  }
}
/* A read must realize a deferred fill on a suspended target before sampling.
 * Match the backing buffer: application surface 0 may alias base target -1. */
static void realize_suspended_fill(GmlRender *r,const uint32_t *px){
  if(!r || !px) return;
  for(int i=0;i<r->target_sp;i++){
    if(r->target_stack[i].fb!=px || !r->target_stack[i].pending_fill) continue;
    if(r->target_stack[i].w<=0 || r->target_stack[i].h<=0) continue;
    size_t n=(size_t)r->target_stack[i].w*(size_t)r->target_stack[i].h;
    uint32_t *p=r->target_stack[i].fb;
    while(n>0){
      int run=n>(size_t)INT_MAX?INT_MAX:(int)n;
      gml_render_backend_fill_xrgb(p,run,r->target_stack[i].fill_color);
      p+=run; n-=(size_t)run;
    }
    r->target_stack[i].pending_fill=0;
  }
}
uint32_t *surface_pixels(GmlRender *r, int id, int *w, int *h){
  if(id==GML_RENDER_SHADED_SURFACE){
    if(r->shaded_plane_width<=0 || r->shaded_plane_height<=0) return NULL;
    if(w) *w=r->shaded_plane_width;
    if(h) *h=r->shaded_plane_height;
    if(r->shaded_plane_borrowed) return (uint32_t*)(uintptr_t)r->shaded_plane_borrowed;
    return r->shaded_plane;
  }
  if(id==0){
    if(!r->app_surface) return NULL;
    /* the app surface has its own dims (the view render); the current target may be the larger
     * presentation canvas — using target dims here read the view buffer with the wrong stride */
    if(w) *w=r->app_w?r->app_w:(r->base_fbw?r->base_fbw:r->fbw);
    if(h) *h=r->app_h?r->app_h:(r->base_fbh?r->base_fbh:r->fbh);
    realize_suspended_fill(r,r->app_surface);
    return r->app_surface;
  }
  int i=surface_slot(id);
  if(i<0 || !r->surface[i].live || !r->surface[i].px) return NULL;
  if(w) *w=r->surface[i].w;
  if(h) *h=r->surface[i].h;
  realize_suspended_fill(r,r->surface[i].px);
  return r->surface[i].px;
}


int gml_surface_exists(GmlRender *r, int id){
  int w=0,h=0; return surface_pixels(r,id,&w,&h)!=NULL && w>0 && h>0;
}
int gml_surface_width(GmlRender *r, int id){
  int w=0,h=0; return surface_pixels(r,id,&w,&h)?w:0;
}
int gml_surface_height(GmlRender *r, int id){
  int w=0,h=0; return surface_pixels(r,id,&w,&h)?h:0;
}
const uint32_t *gml_surface_pixels_read(GmlRender *r, int id, int *w, int *h){
  if(!r) return NULL;
  if(id==r->target_id) gml_render_maybe_prepare_draw(r);
  return surface_pixels(r,id,w,h);
}
void gml_surface_copy(GmlRender *r, int dst, int x, int y, int src){
  if(!r) return;
  int sw=0, sh=0, dw=0, dh=0;
  uint32_t *sp=surface_pixels(r,src,&sw,&sh);
  uint32_t *dp=surface_pixels(r,dst,&dw,&dh);
  if(!sp || !dp || sw<=0 || sh<=0 || dw<=0 || dh<=0) return;
  if((r->fb && (sp==r->fb || dp==r->fb)) || src==r->target_id || dst==r->target_id){
    gml_render_maybe_prepare_draw(r);
    sp=surface_pixels(r,src,&sw,&sh);
    dp=surface_pixels(r,dst,&dw,&dh);
    if(!sp || !dp) return;
  }

  int sx0=0, sy0=0, dx0=x, dy0=y, cw=sw, ch=sh;
  if(dx0<0){ sx0=-dx0; cw+=dx0; dx0=0; }
  if(dy0<0){ sy0=-dy0; ch+=dy0; dy0=0; }
  if(dx0+cw>dw) cw=dw-dx0;
  if(dy0+ch>dh) ch=dh-dy0;
  if(cw<=0 || ch<=0) return;

  if(sp==dp){
    uint32_t *tmp=malloc((size_t)cw*(size_t)ch*sizeof(uint32_t));
    if(!tmp) return;
    for(int yy=0; yy<ch; yy++)
      memcpy(tmp+(size_t)yy*cw, sp+(size_t)(sy0+yy)*sw+sx0, (size_t)cw*sizeof(uint32_t));
    for(int yy=0; yy<ch; yy++)
      memcpy(dp+(size_t)(dy0+yy)*dw+dx0, tmp+(size_t)yy*cw, (size_t)cw*sizeof(uint32_t));
    free(tmp);
  } else {
    for(int yy=0; yy<ch; yy++)
      memcpy(dp+(size_t)(dy0+yy)*dw+dx0, sp+(size_t)(sy0+yy)*sw+sx0, (size_t)cw*sizeof(uint32_t));
  }

  int di=surface_slot(dst);
  if(di>=0 && r->surface[di].live){
    r->surface[di].dirty=1;
    r->surface[di].opaque_known=0;
    r->surface[di].all_opaque=0;
    r->surface[di].all_transparent=0;
  }
  if(dst==0) r->app_surface_opaque=0;
  if(dst==r->target_id){
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    r->fb_all_transparent=0;
  }
}
void gml_surface_copy_part(GmlRender *r, int dst, int x, int y, int src,
                           int source_x, int source_y, int width, int height){
  if(!r || width<=0 || height<=0) return;
  int sw=0,sh=0,dw=0,dh=0;
  uint32_t *sp=surface_pixels(r,src,&sw,&sh);
  uint32_t *dp=surface_pixels(r,dst,&dw,&dh);
  if(!sp || !dp || sw<=0 || sh<=0 || dw<=0 || dh<=0) return;
  if((r->fb && (sp==r->fb || dp==r->fb)) || src==r->target_id || dst==r->target_id){
    gml_render_maybe_prepare_draw(r);
    sp=surface_pixels(r,src,&sw,&sh);
    dp=surface_pixels(r,dst,&dw,&dh);
    if(!sp || !dp) return;
  }

  int64_t sx=source_x,sy=source_y,dx=x,dy=y,cw=width,ch=height;
  if(sx<0){ int64_t skip=-sx; sx=0; dx+=skip; cw-=skip; }
  if(sy<0){ int64_t skip=-sy; sy=0; dy+=skip; ch-=skip; }
  if(dx<0){ int64_t skip=-dx; dx=0; sx+=skip; cw-=skip; }
  if(dy<0){ int64_t skip=-dy; dy=0; sy+=skip; ch-=skip; }
  if(cw<=0 || ch<=0 || sx>=sw || sy>=sh || dx>=dw || dy>=dh) return;
  if(cw>(int64_t)sw-sx) cw=(int64_t)sw-sx;
  if(ch>(int64_t)sh-sy) ch=(int64_t)sh-sy;
  if(cw>(int64_t)dw-dx) cw=(int64_t)dw-dx;
  if(ch>(int64_t)dh-dy) ch=(int64_t)dh-dy;
  if(cw<=0 || ch<=0) return;

  if(sp==dp){
    uint32_t *tmp=malloc((size_t)cw*(size_t)ch*sizeof(uint32_t));
    if(!tmp) return;
    for(int64_t yy=0;yy<ch;yy++)
      memcpy(tmp+(size_t)yy*(size_t)cw,
             sp+(size_t)(sy+yy)*(size_t)sw+(size_t)sx,
             (size_t)cw*sizeof(uint32_t));
    for(int64_t yy=0;yy<ch;yy++)
      memcpy(dp+(size_t)(dy+yy)*(size_t)dw+(size_t)dx,
             tmp+(size_t)yy*(size_t)cw,(size_t)cw*sizeof(uint32_t));
    free(tmp);
  } else {
    for(int64_t yy=0;yy<ch;yy++)
      memcpy(dp+(size_t)(dy+yy)*(size_t)dw+(size_t)dx,
             sp+(size_t)(sy+yy)*(size_t)sw+(size_t)sx,
             (size_t)cw*sizeof(uint32_t));
  }

  int di=surface_slot(dst);
  if(di>=0 && r->surface[di].live){
    r->surface[di].dirty=1;
    r->surface[di].opaque_known=0;
    r->surface[di].all_opaque=0;
    r->surface[di].all_transparent=0;
  }
  if(dst==0) r->app_surface_opaque=0;
  if(dst==r->target_id){
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    r->fb_all_transparent=0;
  }
}
int gml_surface_create(GmlRender *r, int w, int h){
  if(w<=0||h<=0||w>4096||h>4096) return -1;
  for(int k=0;k<GML_MAX_SURFACES;k++){
    int i=(r->next_surface_id+k-1)%GML_MAX_SURFACES;
    if(!r->surface[i].live){
      uint32_t *px=calloc((size_t)w*h,sizeof(uint32_t));
      if(!px) return -1;
      { uint8_t *rle=r->surface[i].rle; size_t cap=r->surface[i].rle_cap;   /* keep cache alloc across reuse */
        r->surface[i]=(GmlSurface){px,w,h,1,1,1,0,1,rle,0,cap}; }
      r->next_surface_id=i+2; if(r->next_surface_id>GML_MAX_SURFACES) r->next_surface_id=1;
      return i+1;
    }
  }
  return -1;
}
void gml_surface_free(GmlRender *r, int id){
  int i=surface_slot(id); if(i<0) return;
  free(r->surface[i].px); free(r->surface[i].rle); memset(&r->surface[i],0,sizeof(r->surface[i]));
}
static int application_surface_resize(GmlRender *r,int w,int h){
  if(!r || w<=0 || h<=0 || w>4096 || h>4096 ||
     (size_t)w>SIZE_MAX/(size_t)h || (size_t)w*(size_t)h>SIZE_MAX/sizeof(uint32_t))
    return 0;
  if(r->app_surface_owned && r->app_w==w && r->app_h==h) return 1;
  uint32_t *old=r->app_surface;
  int oldw=r->app_w, oldh=r->app_h;
  uint32_t *px=calloc((size_t)w*(size_t)h,sizeof(uint32_t));
  if(!px) return 0;
  int cw=oldw<w?oldw:w, ch=oldh<h?oldh:h;
  if(old && cw>0 && ch>0)
    for(int y=0;y<ch;y++) memcpy(px+(size_t)y*w,old+(size_t)y*oldw,(size_t)cw*sizeof(uint32_t));
  uint32_t *owned=r->app_surface_owned;
  /* A game can resize surface 0 while it is the current/nested target. Keep every borrowed
   * target reference coherent before releasing the previous owned allocation. */
  if(r->fb==old){ r->fb=px; r->fbw=w; r->fbh=h; }
  if(r->base_fb==old){ r->base_fb=px; r->base_fbw=w; r->base_fbh=h; }
  for(int i=0;i<r->target_sp;i++) if(r->target_stack[i].fb==old){
    r->target_stack[i].fb=px; r->target_stack[i].w=w; r->target_stack[i].h=h;
  }
  r->app_surface_owned=px;
  r->app_surface=px; r->app_w=w; r->app_h=h;
  r->app_surface_opaque=0;
  free(owned);
  return 1;
}
int gml_render_application_surface_ensure_owned(GmlRender *r,int w,int h){
  return application_surface_resize(r,w,h);
}
void gml_surface_resize(GmlRender *r, int id, int w, int h){
  if(!r || w<=0 || h<=0 || w>4096 || h>4096) return;
  if(id==0){
    /* Surface zero follows the same explicit resize request in every generation. */
    if(!r->win) return;
    if(render_setting(r,"GML_LOG_SURF")) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[surf] resize application_surface %dx%d (was %dx%d)\n",w,h,r->app_w,r->app_h);
    (void)application_surface_resize(r,w,h);
    return;
  }
  int i=surface_slot(id); if(i<0 || !r->surface[i].live || w<=0 || h<=0 || w>4096 || h>4096) return;
  uint32_t *px=calloc((size_t)w*h,sizeof(uint32_t));
  if(!px) return;
  int was_transparent=r->surface[i].all_transparent;
  int cw=w<r->surface[i].w?w:r->surface[i].w, ch=h<r->surface[i].h?h:r->surface[i].h;
  for(int y=0;y<ch;y++) memcpy(px+(size_t)y*w,r->surface[i].px+(size_t)y*r->surface[i].w,(size_t)cw*sizeof(uint32_t));
  free(r->surface[i].px); r->surface[i].px=px; r->surface[i].w=w; r->surface[i].h=h;
  r->surface[i].dirty=1;
  r->surface[i].opaque_known=0;
  r->surface[i].all_opaque=0;
  r->surface[i].all_transparent=was_transparent;
}
int gml_surface_set_target(GmlRender *r, int id){
  int w=0,h=0; uint32_t *px=surface_pixels(r,id,&w,&h);
  if(!px || r->target_sp>=GML_SURFACE_STACK) return 0;
  /* Classic target assignment replaces the current target; one reset returns to the base. Other runtime families retain nested targets. */
  if(r->classic && r->target_id>=0){
    gml_surface_reset_target(r);
    px=surface_pixels(r,id,&w,&h);
    if(!px || r->target_sp>=GML_SURFACE_STACK) return 0;
  }
  int si=surface_slot(id);
  { if(si>=0) r->surface[si].dirty=1; }   /* about to be drawn into */
  r->target_stack[r->target_sp++]=(typeof(r->target_stack[0])){
    r->fb,r->fbw,r->fbh,r->cam_x,r->cam_y,r->projection_cam_x,r->projection_cam_y,
    r->target_id,r->fb_opaque_known,r->fb_all_opaque,r->fb_all_transparent,
    r->pending_underlay,r->underlay_x,r->underlay_y,r->underlay_w,r->underlay_h,
    r->pending_fill,r->pending_fill_color
  };
  if(render_setting(r,"GML_LOG_SURF")){
    /* Count existing RGB and alpha before this surface becomes the active target. */
    int lit_=0,cov_=0;
    surface_coverage_counts(px,w,h,&lit_,&cov_);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surf] set_target %d px=%p lit_on_entry=%d covered_on_entry=%d of %d\n",
      id,(const void *)px,lit_,cov_,w*h);
  }
  r->fb=px; r->fbw=w; r->fbh=h;
  r->target_id=id;
  r->fb_opaque_known=(si>=0)?r->surface[si].opaque_known:0;
  r->fb_all_opaque=(si>=0)?r->surface[si].all_opaque:0;
  r->fb_all_transparent=(si>=0)?r->surface[si].all_transparent:0;
  r->pending_underlay=0;
  r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
  r->pending_fill=0;
  r->pending_fill_color=0;
  /* GM resets the projection to the surface's own coordinates while a surface target is
   * active: draws inside it must NOT be shifted by the room camera/GUI offset. */
  r->projection_cam_x=0; r->projection_cam_y=0;
  r->cam_x=0; r->cam_y=0;
  gml_d3_sync_render_camera(r);
  if(render_setting(r,"GML_LOG_SURF")){
    /* Count both channels again at function exit, paired with the entry counts. */
    int lit_=0,cov_=0;
    surface_coverage_counts(px,w,h,&lit_,&cov_);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surf] set_target %d lit_on_exit=%d covered_on_exit=%d of %d\n",id,lit_,cov_,w*h);
  }
  return 1;
}
void gml_surface_reset_target(GmlRender *r){
  /* Count colour coverage on both sides of the pending-draw flush while the
   * active surface is still the render target. */
  int dbg_=r && render_setting(r,"GML_LOG_SURF")!=NULL && r->fb && r->fbw>0 && r->fbh>0;
  int released_=r?r->target_id:-1;   /* captured before the stack pop restores the previous one */
  int before_=0,before_cov_=0;
  if(dbg_) surface_coverage_counts(r->fb,r->fbw,r->fbh,&before_,&before_cov_);
  if(r && surface_slot(r->target_id)>=0){
    /* Remember that content really authored this surface before a deferred transparent clear is
     * resolved. A later screen blit of that now-transparent surface is still a compositor action;
     * this is distinct from blitting a scratch surface that received no draw at all. */
    if(!r->fb_all_transparent)
      r->content_authored_surfaces|=UINT64_C(1)<<(r->target_id-1);
    gml_render_flush_pending_underlay(r);
    gml_render_flush_pending_fill(r);
    surface_store_target_coverage(r);
  }
  if(dbg_){
    int after_=0,after_cov_=0;
    surface_coverage_counts(r->fb,r->fbw,r->fbh,&after_,&after_cov_);
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surf] reset_target released=%d px=%p lit_before=%d lit_after=%d"
      " covered_before=%d covered_after=%d of %d\n",
      released_,(const void *)r->fb,before_,after_,before_cov_,after_cov_,r->fbw*r->fbh);
  }
  if(r->target_sp>0){
    typeof(r->target_stack[0]) t=r->target_stack[--r->target_sp];
    r->fb=t.fb; r->fbw=t.w; r->fbh=t.h;
    r->projection_cam_x=t.projection_cx; r->projection_cam_y=t.projection_cy;
    r->cam_x=t.cx; r->cam_y=t.cy;
    gml_d3_sync_render_camera(r);
    r->target_id=t.target_id;
    r->fb_opaque_known=t.opaque_known;
    r->fb_all_opaque=t.all_opaque;
    r->fb_all_transparent=t.all_transparent;
    r->pending_underlay=t.pending_underlay;
    r->underlay_x=t.underlay_x;
    r->underlay_y=t.underlay_y;
    r->underlay_w=t.underlay_w;
    r->underlay_h=t.underlay_h;
    r->pending_fill=t.pending_fill;
    r->pending_fill_color=t.fill_color;
  } else if(r->base_fb){
    r->fb=r->base_fb; r->fbw=r->base_fbw; r->fbh=r->base_fbh;
    r->target_id=-1;
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    r->fb_all_transparent=0;
    r->pending_underlay=0;
    r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
    r->pending_fill=0;
    r->pending_fill_color=0;
  } else {
    r->target_id=-1;
    r->fb_opaque_known=0;
    r->fb_all_opaque=0;
    r->fb_all_transparent=0;
    r->pending_underlay=0;
    r->underlay_x=r->underlay_y=r->underlay_w=r->underlay_h=0;
    r->pending_fill=0;
    r->pending_fill_color=0;
  }
}
int gml_surface_get_target(GmlRender *r){
  return r ? r->target_id : -1;
}
/* Retain a bound program target across a new pass while replacing the base buffer that a later target reset returns to. With no target, use ordinary pass initialization. */
void gml_render_begin_retaining_target(GmlRender *r, uint32_t *fb, int w, int h,
                                       double cx, double cy){
  if(!r || r->target_id<0 || r->target_sp<=0){ gml_render_begin(r,fb,w,h,cx,cy); return; }
  int retained=r->target_id;
  int depth=r->target_sp;
  /* The coverage of a bound target is tracked live and only written back to the surface when the
   * target closes, so it has to cross this call rather than be re-read: a surface the draw phase
   * has just filled still reads transparent in its own record, and a later composite of it would
   * be skipped as empty. */
  int retained_opaque_known=r->fb_opaque_known;
  int retained_all_opaque=r->fb_all_opaque;
  int retained_all_transparent=r->fb_all_transparent;
  typeof(r->target_stack[0]) saved[GML_SURFACE_STACK];
  memcpy(saved,r->target_stack,sizeof(saved));
  gml_render_begin(r,fb,w,h,cx,cy);
  int sw=0,sh=0;
  uint32_t *px=surface_pixels(r,retained,&sw,&sh);
  if(!px) return;   /* the surface was freed under the binding: the pass owns the base buffer */
  memcpy(r->target_stack,saved,sizeof(r->target_stack));
  r->target_sp=depth;
  /* The bottom entry described the base of the pass that pushed it, which no longer exists. */
  r->target_stack[0].fb=fb;
  r->target_stack[0].w=w;
  r->target_stack[0].h=h;
  r->target_stack[0].cx=cx;
  r->target_stack[0].cy=cy;
  r->target_stack[0].projection_cx=cx;
  r->target_stack[0].projection_cy=cy;
  r->target_stack[0].target_id=-1;
  r->target_stack[0].opaque_known=0;
  r->target_stack[0].all_opaque=0;
  r->target_stack[0].all_transparent=0;
  r->target_stack[0].pending_underlay=0;
  r->target_stack[0].underlay_x=r->target_stack[0].underlay_y=0;
  r->target_stack[0].underlay_w=r->target_stack[0].underlay_h=0;
  r->target_stack[0].pending_fill=0;
  r->target_stack[0].fill_color=0;
  int si=surface_slot(retained);
  r->fb=px; r->fbw=sw; r->fbh=sh;
  r->target_id=retained;
  r->fb_opaque_known=retained_opaque_known;
  r->fb_all_opaque=retained_all_opaque;
  r->fb_all_transparent=retained_all_transparent;
  if(si>=0) r->surface[si].dirty=1;
}
int gml_surface_target_lit(GmlRender *r){
  /* Count nonzero RGB in the active surface; alpha is not part of this probe. */
  if(!r || r->target_id<=0 || !r->fb || r->fbw<=0 || r->fbh<=0) return -1;
  int lit=0;
  for(size_t i=0;i<(size_t)r->fbw*(size_t)r->fbh;i++) if(r->fb[i]&0x00FFFFFFu) lit++;
  return lit;
}
/* Select one source texel for a point-sampled destination pixel.
 * Magnification retains the established leading-edge phase. Reduction
 * samples the pixel centre, with an exact boundary assigned to the
 * preceding texel, so fractional reductions do not shift source rows. */
static inline int surface_point_index(int64_t destination,int64_t source_extent,
                                      int64_t destination_extent){
  if(destination_extent<=0) return 0;
  if(source_extent<=destination_extent)
    return (int)((destination*source_extent)/destination_extent);
  /* Centre of the destination pixel, with a sample that lands exactly on a texel boundary
   * belonging to the preceding texel -- the tie this renderer resolves the same way everywhere
   * else. An exact 2:1 reduction is entirely ties, so it keeps selecting the even texels it
   * always did; a fractional reduction has no ties at all and moves by the half pixel. */
  return (int)((((destination*2+1)*source_extent)-1)/(destination_extent*2));
}


/* One band of the generic surface mapper. A row derives its source line and its destination row
 * from the row index alone and writes only that row, so bands are independent and each row selects
 * exactly the texel it would have selected on its own. */
typedef struct ScaledBand {
  GmlRender *r;
  const uint32_t *src;
  int sw,sh,W,H,x0,y0,px0,px1,py_first;
  const int *xs0,*xs1;
} ScaledBand;

static void scaled_band(void *context,int row_start,int row_end,int slot){
  const ScaledBand *b=(const ScaledBand*)context;
  GmlRender *r=b->r;
  const uint32_t *src=b->src;
  const int sw=b->sw,sh=b->sh,H=b->H,x0=b->x0,y0=b->y0,px0=b->px0,px1=b->px1;
  const int *xs0=b->xs0,*xs1=b->xs1;
  (void)slot;
  for(int py=b->py_first+row_start; py<b->py_first+row_end; py++){
    int sy0, sy1;
    if(!r->interp){
      sy0=surface_point_index(py,sh,H);
      sy1=sy0+1;
    } else if(H>=sh){
      sy0=(int)(((int64_t)py*sh)/H);
      sy1=sy0+1;
    } else {
      sy0 = (int)(((int64_t)py * sh) / H);
      sy1 = (int)((((int64_t)py + 1) * sh) / H);
    }
    if(sy1 <= sy0) sy1 = sy0 + 1;
    if(sy0 < 0) sy0 = 0;
    if(sy1 > sh) sy1 = sh;
    uint32_t *dp = r->fb + (size_t)(y0 + py) * r->fbw + (x0 + px0);
    for(int px=px0; px<px1; px++){
      int sx0 = xs0[px], sx1 = xs1[px];
      if(sx1 == sx0 + 1 && sy1 == sy0 + 1){
        uint32_t s = src[(size_t)sy0 * sw + sx0];
        uint32_t a = s >> 24;
        if(a == 255){
          *dp = s | 0xFF000000u;
        } else if(a){
          double pa = a / 255.0;
          int sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
          if(pa >= 1.0) *dp = 0xFF000000u | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | (uint32_t)sb;
          else {
            int dr = (*dp >> 16) & 0xFF, dg = (*dp >> 8) & 0xFF, db = *dp & 0xFF;
            int round_target=r->win && anygm_policy_uses_first_generation_studio(r->win) &&
                             r->app_surface==src;
            if(round_target){
              uint32_t inverse=255u-a;
              *dp=0xFF000000u|
                ((uint32_t)(((uint32_t)sr*a+(uint32_t)dr*inverse+127u)/255u)<<16)|
                ((uint32_t)(((uint32_t)sg*a+(uint32_t)dg*inverse+127u)/255u)<<8)|
                (uint32_t)(((uint32_t)sb*a+(uint32_t)db*inverse+127u)/255u);
            } else {
              *dp = 0xFF000000u | ((int)(sr * pa + dr * (1.0 - pa)) << 16) |
                    ((int)(sg * pa + dg * (1.0 - pa)) << 8) |
                    (int)(sb * pa + db * (1.0 - pa));
            }
          }
        }
        dp++;
        continue;
      }
      unsigned R=0, G=0, B=0, A=0, n=0;
      for(int sy=sy0; sy<sy1; sy++){
        const uint32_t *sp = src + (size_t)sy * sw + sx0;
        for(int sx=sx0; sx<sx1; sx++){
          uint32_t s = *sp++;
          R += (s >> 16) & 0xFF;
          G += (s >> 8) & 0xFF;
          B += s & 0xFF;
          A += s >> 24;
          n++;
        }
      }
      if(!n) n = 1;
      double pa = (A / (double)n) / 255.0;
      if(pa > 0.0){
        int sr = (int)(R / n), sg = (int)(G / n), sb = (int)(B / n);
        if(pa >= 1.0) *dp = 0xFF000000u | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | (uint32_t)sb;
        else {
          int dr = (*dp >> 16) & 0xFF, dg = (*dp >> 8) & 0xFF, db = *dp & 0xFF;
          *dp = 0xFF000000u | ((int)(sr * pa + dr * (1.0 - pa)) << 16) |
                ((int)(sg * pa + dg * (1.0 - pa)) << 8) |
                (int)(sb * pa + db * (1.0 - pa));
        }
      }
      dp++;
    }
    }
}

static int draw_scaled_full_surface_normal(GmlRender *r, const uint32_t *src, int sw, int sh,
                                           int x0, int y0, int W, int H,
                                           int source_all_opaque){
  if(!r || !src || !r->fb || sw<=0 || sh<=0 || W<=0 || H<=0) return 0;
  int px0 = x0 < 0 ? -x0 : 0;
  int py0 = y0 < 0 ? -y0 : 0;
  int px1 = x0 + W > r->fbw ? r->fbw - x0 : W;
  int py1 = y0 + H > r->fbh ? r->fbh - y0 : H;
  if(px0 >= px1 || py0 >= py1) return 1;

  /* A classic view whose port is exactly twice its logical size is rasterized at four sample
   * phases by the fixed-function viewport.  The auxiliary planes keep texture filtering and
   * alpha composition in draw order; filtering the already-composited application surface cannot
   * reconstruct those edge samples. */
  if(r->classic && r->interp && r->app_surface==src && W==sw*2 && H==sh*2 &&
     r->app_interp_phase[0] && r->app_interp_phase[1] && r->app_interp_phase[2]){
    for(int py=py0;py<py1;py++){
      int sy=py>>1, oddy=py&1;
      uint32_t *dp=r->fb+(size_t)(y0+py)*r->fbw+(x0+px0);
      for(int px=px0;px<px1;px++,dp++){
        int sx=px>>1, oddx=px&1;
        const uint32_t *plane=!oddx&&!oddy?src:
          (oddx&&!oddy?r->app_interp_phase[0]:
           (!oddx&&oddy?r->app_interp_phase[1]:r->app_interp_phase[2]));
        *dp=plane[(size_t)sy*sw+sx]|0xFF000000u;
      }
    }
    return 1;
  }

  /* The classic interpolation option also filters the final application-surface
   * magnification.  That screen blit uses leading-edge texture coordinates; routing it through
   * the ordinary centred sprite/surface sampler shifts every transition by half a texel.  Keep
   * this host-owned path separate from texture_set_interpolation() draws made by game code. */
  if(r->classic && r->interp && r->app_surface==src && W>sw && H>sh){
    int *cxa=malloc((size_t)W*sizeof(int)), *cxb=malloc((size_t)W*sizeof(int));
    float *cfx=malloc((size_t)W*sizeof(float));
    if(!cxa || !cxb || !cfx){ free(cxa); free(cxb); free(cfx); return 0; }
    for(int px=0;px<W;px++){
      double fsx=(double)px*sw/W; int a=(int)floor(fsx), b=a+1;
      cfx[px]=(float)(fsx-a);
      if(a<0)a=0; else if(a>=sw)a=sw-1;
      if(b<0)b=0; else if(b>=sw)b=sw-1;
      cxa[px]=a; cxb[px]=b;
    }
    gml_render_maybe_prepare_draw(r);
    SurfBiCtx ctx={ r,src,cxa,cxb,cfx,sw,sh,W,H,x0,y0,0.0,(double)sh,0.0,0.0,
      1.0f,1.0f,1.0f,1.0f,!r->alphablend };
    gml_run_row_bands(r,H,surf_bi_band,&ctx);
    free(cxa); free(cxb); free(cfx);
    return 1;
  }

  /* GM8's fixed-function aspect blit keeps the half-pixel phase of the projected
   * logical view.  At a fractional magnification that selects the nearest logical
   * texel to the projected output coordinate (round(dst * src / target)); using
   * the ordinary output-centre formula instead repeats the preceding texel at
   * each fractional boundary.  Exact integer magnifications retain the centred
   * mapping because the host applies their historical viewport-origin offset.
   * Full/fixed scaling, older format families and Studio use the generic path. */
  if(r->classic && r->win && anygm_policy_classic_modern_presentation(r->win) &&
     r->win->classic_scaling<0 && W>=sw && H>=sh){
    int xbuf[2048];
    int *xmap=W<=(int)(sizeof xbuf/sizeof *xbuf)?xbuf:malloc((size_t)W*sizeof(*xmap));
    if(!xmap) return 0;
    int fractional_x=W%sw!=0, fractional_y=H%sh!=0;
    for(int px=0;px<W;px++){
      int64_t numerator=fractional_x
        ? (int64_t)px*sw*2+W
        : ((int64_t)px*2+1)*sw;
      int sx=(int)(numerator/((int64_t)W*2));
      xmap[px]=sx<0?0:(sx>=sw?sw-1:sx);
    }
    gml_render_maybe_prepare_draw(r);
    for(int py=py0;py<py1;py++){
      int64_t numerator=fractional_y
        ? (int64_t)py*sh*2+H
        : ((int64_t)py*2+1)*sh;
      int sy=(int)(numerator/((int64_t)H*2));
      if(sy<0) sy=0; else if(sy>=sh) sy=sh-1;
      uint32_t *dp=r->fb+(size_t)(y0+py)*r->fbw+(x0+px0);
      for(int px=px0;px<px1;px++){
        int sx=xmap[px];
        uint32_t s=src[(size_t)sy*sw+sx],a=s>>24;
        if(a==255) *dp=s|0xFF000000u;
        else if(a){
          if(r->classic && src==r->app_surface){
            /* The classic automatic present models an alphaless backbuffer: partial coverage
             * never dims the presented colour, at this scale as at any other. */
            *dp=s|0xFF000000u;
          } else {
            uint32_t d=*dp,ia=255-a;
            int sr=(s>>16)&0xFF,sg=(s>>8)&0xFF,sb=s&0xFF;
            int dr=(d>>16)&0xFF,dg=(d>>8)&0xFF,db=d&0xFF;
            *dp=0xFF000000u|((uint32_t)((sr*a+dr*ia)/255)<<16)|
                ((uint32_t)((sg*a+dg*ia)/255)<<8)|(uint32_t)((sb*a+db*ia)/255);
          }
        }
        dp++;
      }
    }
    if(xmap!=xbuf) free(xmap);
    return 1;
  }

  /* Integer-magnified opaque presentation is a pure nearest-neighbour copy. Build each expanded
   * source row once, then duplicate it vertically. The general mapper below must retain its
   * coverage checks for transparent surfaces and fractional/downscaled presentation, but doing
   * those checks for every pixel of a certified opaque full-target blit is unnecessary work. */
  if(source_all_opaque && x0==0 && y0==0 && W==r->fbw && H==r->fbh &&
     W%sw==0 && H%sh==0){
    int xscale=W/sw,yscale=H/sh;
    for(int sy=0;sy<sh;sy++){
      uint32_t *first=r->fb+(size_t)(sy*yscale)*W;
      const uint32_t *source_row=src+(size_t)sy*sw;
      for(int sx=0;sx<sw;sx++)
        gml_render_backend_fill_xrgb(first+(size_t)sx*xscale,xscale,
                                     source_row[sx]|0xFF000000u);
      for(int repeat=1;repeat<yscale;repeat++)
        memcpy(first+(size_t)repeat*W,first,(size_t)W*sizeof(*first));
    }
    r->fb_opaque_known=1;
    r->fb_all_opaque=1;
    r->fb_all_transparent=0;
    return 1;
  }

  /* A near-identity point reduction often selects one contiguous source span per output row
   * (for example, a 1920-wide surface presented into 1919 pixels).  The generic mapper below
   * performs the same leading-edge lookup and alpha checks per pixel.  Copy a certified opaque
   * span directly; when coverage metadata is conservative, a vectorized alpha scan can certify
   * the sampled spans much more cheaply while preserving the exact sampled texels. */
  if(!r->interp && W<=sw){
    int sx_first=surface_point_index(px0,sw,W);
    int sx_last=surface_point_index(px1-1,sw,W);
    int copy_width=px1-px0;
    if(sx_last-sx_first==copy_width-1){
      int sampled_all_opaque=source_all_opaque;
      if(!sampled_all_opaque){
        sampled_all_opaque=1;
        for(int py=py0;py<py1;py++){
          int sy=surface_point_index(py,sh,H);
          if(!row_all_opaque32(src+(size_t)sy*sw+sx_first,copy_width)){
            sampled_all_opaque=0;
            break;
          }
        }
      }
      if(sampled_all_opaque){
        for(int py=py0;py<py1;py++){
          int sy=surface_point_index(py,sh,H);
          memcpy(r->fb+(size_t)(y0+py)*r->fbw+(x0+px0),
                 src+(size_t)sy*sw+sx_first,
                 (size_t)copy_width*sizeof(*src));
        }
        if(rect_covers_target(r,x0+px0,y0+py0,x0+px1,y0+py1)){
          r->fb_opaque_known=1;
          r->fb_all_opaque=1;
          r->fb_all_transparent=0;
        }
        return 1;
      }
    }
  }

  int *xspan = (int*)malloc((size_t)W * 2u * sizeof(int));
  if(!xspan) return 0;
  int *xs0 = xspan, *xs1 = xspan + W;
  for(int px=0; px<W; px++){
    int sx0, sx1;
    if(!r->interp){
      /* Disabled texture interpolation is point sampling for both magnification and reduction:
       * one source texel per destination pixel, never an average of every texel a reduced output
       * pixel covers. surface_point_index states which texel that is. */
      sx0=surface_point_index(px,sw,W);
      sx1=sx0+1;
    } else if(W>=sw){
      /* Studio and classic full/fixed presentation anchor point magnification at the leading
       * output edge. The fractional classic aspect path above is the centre-sampled exception. */
      sx0=(int)(((int64_t)px*sw)/W);
      sx1=sx0+1;
    } else {
      sx0 = (int)(((int64_t)px * sw) / W);
      sx1 = (int)((((int64_t)px + 1) * sw) / W);
    }
    if(sx1 <= sx0) sx1 = sx0 + 1;
    if(sx0 < 0) sx0 = 0;
    if(sx1 > sw) sx1 = sw;
    xs0[px] = sx0;
    xs1[px] = sx1;
  }

  /* Independent rows of a sufficiently large composite can use the renderer's row-band pool;
   * smaller composites retain the direct serial loop. */
  if(py1-py0>=64 && (px1-px0)>=64){
    ScaledBand band={r,src,sw,sh,W,H,x0,y0,px0,px1,py0,xs0,xs1};
    gml_run_row_bands(r,py1-py0,scaled_band,&band);
  } else
  for(int py=py0; py<py1; py++){
    int sy0, sy1;
    if(!r->interp){
      sy0=surface_point_index(py,sh,H);
      sy1=sy0+1;
    } else if(H>=sh){
      sy0=(int)(((int64_t)py*sh)/H);
      sy1=sy0+1;
    } else {
      sy0 = (int)(((int64_t)py * sh) / H);
      sy1 = (int)((((int64_t)py + 1) * sh) / H);
    }
    if(sy1 <= sy0) sy1 = sy0 + 1;
    if(sy0 < 0) sy0 = 0;
    if(sy1 > sh) sy1 = sh;
    uint32_t *dp = r->fb + (size_t)(y0 + py) * r->fbw + (x0 + px0);
    for(int px=px0; px<px1; px++){
      int sx0 = xs0[px], sx1 = xs1[px];
      if(sx1 == sx0 + 1 && sy1 == sy0 + 1){
        uint32_t s = src[(size_t)sy0 * sw + sx0];
        uint32_t a = s >> 24;
        if(a == 255){
          *dp = s | 0xFF000000u;
        } else if(a){
          double pa = a / 255.0;
          int sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
          if(pa >= 1.0) *dp = 0xFF000000u | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | (uint32_t)sb;
          else {
            int dr = (*dp >> 16) & 0xFF, dg = (*dp >> 8) & 0xFF, db = *dp & 0xFF;
            int round_target=r->win && anygm_policy_uses_first_generation_studio(r->win) &&
                             r->app_surface==src;
            if(round_target){
              uint32_t inverse=255u-a;
              *dp=0xFF000000u|
                ((uint32_t)(((uint32_t)sr*a+(uint32_t)dr*inverse+127u)/255u)<<16)|
                ((uint32_t)(((uint32_t)sg*a+(uint32_t)dg*inverse+127u)/255u)<<8)|
                (uint32_t)(((uint32_t)sb*a+(uint32_t)db*inverse+127u)/255u);
            } else {
              *dp = 0xFF000000u | ((int)(sr * pa + dr * (1.0 - pa)) << 16) |
                    ((int)(sg * pa + dg * (1.0 - pa)) << 8) |
                    (int)(sb * pa + db * (1.0 - pa));
            }
          }
        }
        dp++;
        continue;
      }
      unsigned R=0, G=0, B=0, A=0, n=0;
      for(int sy=sy0; sy<sy1; sy++){
        const uint32_t *sp = src + (size_t)sy * sw + sx0;
        for(int sx=sx0; sx<sx1; sx++){
          uint32_t s = *sp++;
          R += (s >> 16) & 0xFF;
          G += (s >> 8) & 0xFF;
          B += s & 0xFF;
          A += s >> 24;
          n++;
        }
      }
      if(!n) n = 1;
      double pa = (A / (double)n) / 255.0;
      if(pa > 0.0){
        int sr = (int)(R / n), sg = (int)(G / n), sb = (int)(B / n);
        if(pa >= 1.0) *dp = 0xFF000000u | ((uint32_t)sr << 16) | ((uint32_t)sg << 8) | (uint32_t)sb;
        else {
          int dr = (*dp >> 16) & 0xFF, dg = (*dp >> 8) & 0xFF, db = *dp & 0xFF;
          *dp = 0xFF000000u | ((int)(sr * pa + dr * (1.0 - pa)) << 16) |
                ((int)(sg * pa + dg * (1.0 - pa)) << 8) |
                (int)(sb * pa + db * (1.0 - pa));
        }
      }
      dp++;
    }
  }
  /* An opaque source point-sampled across the whole target leaves the target opaque whichever
   * texel each destination pixel selected. The contiguous-span shortcut above certifies that for
   * the reductions whose columns happen to be adjacent; a fractional reduction skips columns and
   * reaches this mapper instead, and must not lose the certificate the same blit used to carry.
   * The sampled rows are checked the way that shortcut checks them when the surface itself
   * carries no opaque certificate. */
  if(!r->interp && rect_covers_target(r,x0+px0,y0+py0,x0+px1,y0+py1)){
    int certified=source_all_opaque;
    if(!certified){
      certified=1;
      for(int py=py0;py<py1 && certified;py++){
        int sy=surface_point_index(py,sh,H);
        if(sy<0) sy=0; else if(sy>=sh) sy=sh-1;
        if(!row_all_opaque32(src+(size_t)sy*sw,sw)) certified=0;
      }
    }
    if(certified){
      r->fb_opaque_known=1;
      r->fb_all_opaque=1;
      r->fb_all_transparent=0;
    }
  }
  free(xspan);
  return 1;
}
static void draw_surface_interp_phase(GmlRender *r,uint32_t *plane,const uint32_t *src,
                                      int src_w,int src_h,
                                      double sx0d,double sy0d,double swd,double shd,
                                      double dx,double dy,int W,int H,
                                      uint32_t blend,double alpha,int phase_x,int phase_y){
  if(!r||!plane||!src||src_w<=0||src_h<=0||W<=0||H<=0||alpha<=0.0) return;
  int x0=(int)floor(dx),y0=(int)floor(dy);
  int px0=x0-(phase_x?1:0),py0=y0-(phase_y?1:0);
  int px1=px0+W,py1=py0+H;
  if(px0<0) px0=0;
  if(py0<0) py0=0;
  if(px1>r->base_fbw) px1=r->base_fbw;
  if(py1>r->base_fbh) py1=r->base_fbh;
  if(px0>=px1||py0>=py1) return;
  if(alpha>1.0) alpha=1.0;
  int bR=blend&0xFF,bG=(blend>>8)&0xFF,bB=(blend>>16)&0xFF;
  int sremap=indexed_brightness_active(r)!=NULL || threshold_palette_active(r)!=NULL;
  const struct GmlShaderPal *slut=lut_active(r);
  const struct GmlShaderPal *sgrid=grid_active(r);
  for(int py=py0;py<py1;py++){
    double sample_y=(double)py+(phase_y?0.5:0.0);
    double v=sy0d+(sample_y-(double)y0)*shd/(double)H;
    int va=(int)floor(v),vb=va+1; double fy=v-(double)va;
    if(va<0) va=0; else if(va>=src_h) va=src_h-1;
    if(vb<0) vb=0; else if(vb>=src_h) vb=src_h-1;
    uint32_t *drow=plane+(size_t)py*r->base_fbw;
    for(int px=px0;px<px1;px++){
      double sample_x=(double)px+(phase_x?0.5:0.0);
      double u=sx0d+(sample_x-(double)x0)*swd/(double)W;
      int ua=(int)floor(u),ub=ua+1; double fx=u-(double)ua;
      if(ua<0) ua=0; else if(ua>=src_w) ua=src_w-1;
      if(ub<0) ub=0; else if(ub>=src_w) ub=src_w-1;
      uint32_t p00=src[(size_t)va*src_w+ua],p01=src[(size_t)va*src_w+ub];
      uint32_t p10=src[(size_t)vb*src_w+ua],p11=src[(size_t)vb*src_w+ub];
      double ix=1.0-fx,iy=1.0-fy;
      double w00=ix*iy,w01=fx*iy,w10=ix*fy,w11=fx*fy;
      int sr=(int)(((p00>>16)&0xFF)*w00+((p01>>16)&0xFF)*w01+
                   ((p10>>16)&0xFF)*w10+((p11>>16)&0xFF)*w11);
      int sg=(int)(((p00>>8)&0xFF)*w00+((p01>>8)&0xFF)*w01+
                   ((p10>>8)&0xFF)*w10+((p11>>8)&0xFF)*w11);
      int sb=(int)((p00&0xFF)*w00+(p01&0xFF)*w01+(p10&0xFF)*w10+(p11&0xFF)*w11);
      int aa=(int)((p00>>24)*w00+(p01>>24)*w01+(p10>>24)*w10+(p11>>24)*w11);
      if(aa<=0 || shader_discards_alpha(r,(unsigned)aa)) continue;
      uint32_t sampled=((uint32_t)aa<<24)|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
      if(sremap) sampled=mapped_texture_pixel(r,sampled);
      else if(slut) sampled=lut_map_px(r,slut,sampled);
      else if(sgrid) sampled=grid_map_px(r,sgrid,sampled);
      sr=((sampled>>16)&0xFF)*bR/255;
      sg=((sampled>>8)&0xFF)*bG/255;
      sb=(sampled&0xFF)*bB/255;
      uint32_t *dp=&drow[px];
      double sa=(aa/255.0)*alpha;
      if(r->blendmode==4){
        unsigned source_alpha=(unsigned)lround((double)aa*alpha);
        uint32_t old=*dp;
        *dp=color_write_merge(r,old,
          blend_max_preset_pixel(r,old,sr,sg,sb,source_alpha));
      } else if(r->blendmode==1||r->blendmode==2){
        int dr=(*dp>>16)&0xFF,dg=(*dp>>8)&0xFF,db=*dp&0xFF;
        int rr,rg,rb;
        if(r->blendmode==1){
          rr=dr+(int)(sr*sa); rg=dg+(int)(sg*sa); rb=db+(int)(sb*sa);
          if(rr>255) rr=255;
          if(rg>255) rg=255;
          if(rb>255) rb=255;
        } else {
          rr=(int)gml_blend_inv_source_u8((unsigned)dr,(unsigned)sr);
          rg=(int)gml_blend_inv_source_u8((unsigned)dg,(unsigned)sg);
          rb=(int)gml_blend_inv_source_u8((unsigned)db,(unsigned)sb);
        }
        *dp=0xFF000000u|((uint32_t)rr<<16)|((uint32_t)rg<<8)|(uint32_t)rb;
      } else if(!r->alphablend||sa>=1.0){
        *dp=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
      } else {
        int dr=(*dp>>16)&0xFF,dg=(*dp>>8)&0xFF,db=*dp&0xFF;
        int rr,rg,rb;
        rr=(int)(sr*sa+0.5)+(int)(dr*(1.0-sa)+0.5);
        rg=(int)(sg*sa+0.5)+(int)(dg*(1.0-sa)+0.5);
        rb=(int)(sb*sa+0.5)+(int)(db*(1.0-sa)+0.5);
        if(rr<0) rr=0; else if(rr>255) rr=255;
        if(rg<0) rg=0; else if(rg>255) rg=255;
        if(rb<0) rb=0; else if(rb>255) rb=255;
        *dp=0xFF000000u|((uint32_t)rr<<16)|((uint32_t)rg<<8)|(uint32_t)rb;
      }
    }
  }
}

void draw_surface_region(GmlRender *r, int surf, double sx0d, double sy0d, double swd, double shd,
                                double dx, double dy, double dw, double dh, uint32_t blend, double alpha){
  int sw=0, sh=0; uint32_t *src=surface_pixels(r,surf,&sw,&sh);
  if(!src || !r->fb) return;
  dx-=r->cam_x; dy-=r->cam_y;   /* draws are camera/GUI-offset relative, like every other path */
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  int x0=(int)floor(dx), y0=(int)floor(dy), W=(int)lround(dw), H=(int)lround(dh);
  if(swd<=0||shd<=0||W==0||H==0) return;
  int flipx=W<0, flipy=H<0; if(W<0) W=-W; if(H<0) H=-H;
  if(alpha<=0) return;
  /* Mode 6 is (one, zero): everywhere a gate admits the plain normal path below, it admits the
   * replace pair too, because for the opaque pixels those paths copy the two are identical and
   * only the partial-coverage runs branch on it. */
  int replace=r->blendmode==6;
  if(surf==0 && r->classic && !r->interp && !flipx && !flipy && r->app_phase_y &&
     W==sw*2 && H==sh*2 && fabs(sx0d)<0.001 && fabs(sy0d)<0.001 &&
     fabs(swd-sw)<0.001 && fabs(shd-sh)<0.001 &&
     alpha>=1.0 && (blend&0xFFFFFF)==0xFFFFFF && (r->blendmode==0||replace) &&
     !shader_alpha_test_requires_filter(r)){
    gml_render_maybe_prepare_draw(r);
    for(int oy=0;oy<H;oy++){
      int ty=y0+oy; if(ty<0||ty>=r->fbh) continue;
      int sy=oy>>1;
      const uint32_t *sample_row=((oy&1)?r->app_phase_y:src)+(size_t)sy*sw;
      uint32_t *dp=r->fb+(size_t)ty*r->fbw;
      for(int ox=0;ox<W;ox++){
        int tx=x0+ox; if(tx<0||tx>=r->fbw) continue;
        int sx=(ox+1)>>1;
        if(sx<sw){ dp[tx]=sample_row[sx]; continue; }
        /* The half-texel phase leaves the far-edge sample one quarter beyond the
         * surface. Treat the missing colour contribution as zero, keep three
         * quarters of the adjacent colour, and preserve coverage. */
        { uint32_t e=sample_row[sw-1];
          unsigned a=e>>24, rr=(e>>16)&255u, gg=(e>>8)&255u, bb=e&255u;
          dp[tx]=(a<<24)|(((rr*3u+2u)>>2)<<16)|(((gg*3u+2u)>>2)<<8)|((bb*3u+2u)>>2); }
      }
    }
    r->fb_opaque_known=1;
    r->fb_all_opaque=1;
    r->fb_all_transparent=0;
    return;
  }
  int src_all_transparent=surface_known_transparent(r,surf);
  if(src_all_transparent && src!=r->fb) return;
  uint32_t *copy=NULL;
  if(src==r->fb) gml_render_maybe_prepare_draw(r);
  if(src==r->fb){
    copy=malloc((size_t)sw*sh*sizeof(uint32_t));
    if(!copy) return;
    memcpy(copy,src,(size_t)sw*sh*sizeof(uint32_t));
    src=copy;
  }
  if(r->interp&&!flipx&&!flipy&&r->target_sp==0&&r->fb==r->base_fb&&
     r->classic_interp_phase[0]&&r->classic_interp_phase[1]&&r->classic_interp_phase[2]){
    draw_surface_interp_phase(r,r->classic_interp_phase[0],src,sw,sh,sx0d,sy0d,swd,shd,
                              dx,dy,W,H,blend,alpha,1,0);
    draw_surface_interp_phase(r,r->classic_interp_phase[1],src,sw,sh,sx0d,sy0d,swd,shd,
                              dx,dy,W,H,blend,alpha,0,1);
    draw_surface_interp_phase(r,r->classic_interp_phase[2],src,sw,sh,sx0d,sy0d,swd,shd,
                              dx,dy,W,H,blend,alpha,1,1);
  }
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  int src_all_opaque=surface_known_opaque(r,surf);
  int sremap=indexed_brightness_active(r)!=NULL || threshold_palette_active(r)!=NULL;
  const struct GmlShaderPal *slut=lut_active(r);
  const struct GmlShaderPal *sgrid=grid_active(r);
  /* The one-to-one surface kernel needs a dedicated four-band branch so
   * opaque copy and channel-mask fast paths do not bypass active mapping. */
  const struct GmlShaderPal *squant=quantise4_active(r);
  GmlGridPixelCache grid_cache={0};
  int alpha_test=shader_alpha_test_requires_filter(r);
  int opaque_alpha_test_passthrough=
    src_all_opaque && !shader_discards_alpha(r,255u);
  if(render_setting(r,"GML_LOG_SHADER") && slut && ++r->lut_shader_log_count<=3){
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] surface draw WITH lut: row=%f pal=%d surf=%d\n",slut->lut_row,r->lut_pal_sprite,surf);
    if(r->lut_shader_log_count==1) for(int ry=0;ry<16;ry++)
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader]   pal row %2d: dark=%06x light=%06x\n",ry,
        sprite_pixel_rgb(r,r->lut_pal_sprite,r->lut_pal_frame,0,ry,0xBAD),
        sprite_pixel_rgb(r,r->lut_pal_sprite,r->lut_pal_frame,1,ry,0xBAD)); }
  if(!flipx && !flipy && W==sw && H==sh &&
     fabs(sx0d) < 0.001 && fabs(sy0d) < 0.001 &&
     fabs(swd - sw) < 0.001 && fabs(shd - sh) < 0.001 &&
     (!alpha_test || opaque_alpha_test_passthrough)){
    int cx0=x0<0?0:x0, cy0=y0<0?0:y0;
    int cx1=x0+W; if(cx1>r->fbw) cx1=r->fbw;
    int cy1=y0+H; if(cy1>r->fbh) cy1=r->fbh;
    int cw=cx1-cx0, ch=cy1-cy0;
    if(cw>0 && ch>0){
      int sx_start=cx0-x0, sy_start=cy0-y0;
      int white=((blend & 0xFFFFFF) == 0xFFFFFF);
      if(src_all_opaque && (r->blendmode==0||replace) && alpha>=1.0 && white &&
         !sremap && !slut && !sgrid && !squant &&
         r->color_write_mask==0x0F &&
         src!=r->fb && cx0==0 && cy0==0 && cw==r->fbw && ch==r->fbh &&
         sx_start==0 && sy_start==0 && sw==r->fbw && sh==r->fbh){
        gml_render_maybe_prepare_opaque_rect(r,cx0,cy0,cx1,cy1);
        memcpy(r->fb,src,(size_t)r->fbw*(size_t)r->fbh*sizeof(uint32_t));
        r->fb_opaque_known=1;
        r->fb_all_opaque=1;
        r->fb_all_transparent=0;
        free(copy);
        return;
      }
      gml_render_maybe_prepare_draw(r);
      if(r->color_write_mask!=0x0F && (r->blendmode==0||replace) && alpha>=1.0 &&
         white && !sremap && !slut && !sgrid && !squant){
        /* Channel-masked surface copy. This is the common mask-construction idiom: draw an
         * opaque color/shape first, disable alpha writes, then copy scene RGB through it. */
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){
            uint32_t sv=sp[xx], sa=sv>>24;
            if(!sa) continue;
            uint32_t old=dp[xx], out;
            if(!r->alphablend || sa==255) out=sv;
            else {
              unsigned ia=255-sa;
              unsigned rr=(((sv>>16)&255)*sa+((old>>16)&255)*ia)/255;
              unsigned gg=(((sv>>8)&255)*sa+((old>>8)&255)*ia)/255;
              unsigned bb=((sv&255)*sa+(old&255)*ia)/255;
              unsigned da=old>>24;
              unsigned aa=sa+(da*ia)/255; if(aa>255) aa=255;
              out=(aa<<24)|(rr<<16)|(gg<<8)|bb;
            }
            dp[xx]=color_write_merge(r,old,out);
          }
        }
        r->fb_opaque_known=0;
        free(copy);
        return;
      } else if(r->blendmode==3){
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){
            uint32_t sv=sp[xx],old=dp[xx];
            if(sremap) sv=mapped_texture_pixel(r,sv);
            else if(slut) sv=lut_map_px(r,slut,sv);
            else if(sgrid) sv=grid_map_px_cached(r,sgrid,sv,&grid_cache);
            int sr=((sv>>16)&255)*bR/255,sg=((sv>>8)&255)*bG/255,sb=(sv&255)*bB/255;
            dp[xx]=color_write_merge(r,old,blend_multiply_pixel(r,old,sr,sg,sb));
          }
        }
      } else if(r->blendmode==4){
        if(r->target_sp>0){
          r->fb_opaque_known=0;
          r->fb_all_opaque=0;
        }
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){
            uint32_t sv=sp[xx],source_alpha=sv>>24,old=dp[xx];
            if(!source_alpha) continue;
            if(sremap) sv=mapped_texture_pixel(r,sv);
            else if(slut) sv=lut_map_px(r,slut,sv);
            else if(sgrid) sv=grid_map_px_cached(r,sgrid,sv,&grid_cache);
            int sr=((sv>>16)&255)*bR/255,sg=((sv>>8)&255)*bG/255,sb=(sv&255)*bB/255;
            source_alpha=(unsigned)lround(source_alpha*alpha);
            dp[xx]=color_write_merge(r,old,
              blend_max_preset_pixel(r,old,sr,sg,sb,source_alpha));
          }
        }
      } else if(r->blendmode==1 || r->blendmode==2){
        /* bm_add and fixed-function bm_subtract surface composites. The latter applies
         * inverse source alpha to offscreen coverage; fully opaque mask pixels still punch a
         * complete hole, while partial coverage follows the documented blend factor. */
        int to_surface = r->target_sp>0;
        int addm = r->blendmode==1;
        if(to_surface && !addm){
          r->fb_opaque_known=0;
          r->fb_all_opaque=0;
        }
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){
            uint32_t sv=sp[xx]; uint32_t sa8=sv>>24;
            if(!sa8) continue;
            double sa=(sa8/255.0)*alpha;
            int source_r=((sv>>16)&0xFF)*bR/255;
            int source_g=((sv>>8)&0xFF)*bG/255;
            int source_b=(sv&0xFF)*bB/255;
            int sr=(int)(source_r*sa), sg=(int)(source_g*sa), sb=(int)(source_b*sa);
            int dr=(dp[xx]>>16)&0xFF, dg=(dp[xx]>>8)&0xFF, db=dp[xx]&0xFF;
            uint32_t dc=dp[xx]>>24;
            int orr,og,ob; uint32_t oc;
            if(addm){ orr=dr+sr; og=dg+sg; ob=db+sb; oc=dc; if(orr>255)orr=255; if(og>255)og=255; if(ob>255)ob=255;
              if(to_surface){ uint32_t ac=dc+(uint32_t)(sa8*alpha); oc=ac>255?255:ac; } else oc=0xFF; }
            else {
              orr=(int)gml_blend_inv_source_u8((unsigned)dr,(unsigned)source_r);
              og=(int)gml_blend_inv_source_u8((unsigned)dg,(unsigned)source_g);
              ob=(int)gml_blend_inv_source_u8((unsigned)db,(unsigned)source_b);
              if(to_surface){
                unsigned source_alpha=(unsigned)lround(sa8*alpha);
                if(source_alpha>255u) source_alpha=255u;
                oc=gml_blend_inv_source_u8(dc,source_alpha);
              } else oc=0xFF;
            }
            dp[xx]=(oc<<24)|((uint32_t)orr<<16)|((uint32_t)og<<8)|(uint32_t)ob;
          }
        }
      } else if((!r->alphablend || alpha>=1.0) && white && sremap){
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){ if(r->alphablend && !(sp[xx]>>24)) continue;
            dp[xx]=mapped_texture_pixel(r,sp[xx]); }
        }
      } else if((!r->alphablend || alpha>=1.0) && white && squant){
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){ if(!(sp[xx]>>24)) continue;
            dp[xx]=quantise4_map_px(squant,sp[xx]); }
        }
      } else if((!r->alphablend || alpha>=1.0) && white && slut){
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){ if(!(sp[xx]>>24)) continue; dp[xx]=lut_map_px(r,slut,sp[xx]); }
        }
      } else if((!r->alphablend || alpha>=1.0) && white && sgrid){
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){ if(!(sp[xx]>>24)) continue;
            dp[xx]=grid_map_px_cached(r,sgrid,sp[xx],&grid_cache); }
        }
      } else if((!r->alphablend || alpha>=1.0) && white){
        /* Surface pixels carry coverage in the high byte. When alpha is zero after a transparent
         * clear, skip uncovered pixels
         * instead of stamping opaque black over the world. */
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          if(src_all_opaque){
            memcpy(dp,sp,(size_t)cw*sizeof(uint32_t));
            continue;
          }
          if(row_all_transparent32(sp,cw)) continue;
          if(row_all_opaque32(sp,cw)){
            memcpy(dp,sp,(size_t)cw*sizeof(uint32_t));
            continue;
          }
          for(int xx=0; xx<cw; ){
            uint32_t sa8=sp[xx]>>24;
            int run=1;
            while(xx+run<cw && (sp[xx+run]>>24)==sa8) run++;
            if(sa8==255){
              memcpy(dp+xx,sp+xx,(size_t)run*sizeof(uint32_t));
            } else if(sa8){
              if(replace){
                /* Mode 6 copies each source texel, including partial coverage, without
                 * destination blending. Zero-coverage runs retain the existing skip. */
                memcpy(dp+xx,sp+xx,(size_t)run*sizeof(uint32_t));
              } else if(r->classic && r->app_surface==src){
                /* The classic automatic present models an alphaless backbuffer: whatever
                 * coverage arithmetic our application surface accumulated, its colour reaches
                 * the screen untouched. Content cannot name surface 0 in that generation, so
                 * this composite is always the engine's own. */
                for(int k=0;k<run;k++) dp[xx+k]=sp[xx+k]|0xFF000000u;
              } else if(r->win && anygm_policy_uses_first_generation_studio(r->win) &&
                 r->app_surface==src){
                uint32_t inverse=255u-sa8;
                for(int k=0;k<run;k++){
                  uint32_t source=sp[xx+k],destination=dp[xx+k];
                  uint32_t sr=(source>>16)&255u,sg=(source>>8)&255u,sb=source&255u;
                  uint32_t dr=(destination>>16)&255u,dg=(destination>>8)&255u,db=destination&255u;
                  dp[xx+k]=0xFF000000u|
                    (((sr*sa8+dr*inverse+127u)/255u)<<16)|
                    (((sg*sa8+dg*inverse+127u)/255u)<<8)|
                    ((sb*sa8+db*inverse+127u)/255u);
                }
              } else {
                uint32_t af=(uint32_t)((sa8*256u)/255u);
                if(af) blend_fast8_src_run(dp+xx,sp+xx,run,af,blend_fast8_rounds(r));
              }
            }
            xx+=run;
          }
        }
      } else {
        double ia=1.0-alpha;
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){
            uint32_t sv=sp[xx];
            double pa=(sv>>24)/255.0;             /* per-pixel coverage x call alpha */
            if(pa<=0) continue;
            if(sremap) sv=mapped_texture_pixel(r,sv);
            else if(slut) sv=lut_map_px(r,slut,sv);
            else if(sgrid) sv=grid_map_px_cached(r,sgrid,sv,&grid_cache);
            int sr=((sv>>16)&0xFF)*bR/255, sg=((sv>>8)&0xFF)*bG/255, sb=(sv&0xFF)*bB/255;
            double ea=alpha*pa; double eia=1.0-ea; (void)ia;
            if(replace) dp[xx]=(sv&0xFF000000u)|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
            else if((!r->alphablend || ea>=1.0)) dp[xx]=0xFF000000u|(sr<<16)|(sg<<8)|sb;
            else {
              uint32_t dv=dp[xx];
              int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
              dp[xx]=0xFF000000u|((int)(sr*ea+dr*eia+0.5)<<16)|((int)(sg*ea+dg*eia+0.5)<<8)|(int)(sb*ea+db*eia+0.5);
            }
          }
        }
      }
      if((r->blendmode==0||replace) && alpha>=1.0 && src_all_opaque && rect_covers_target(r,cx0,cy0,cx1,cy1)){
        r->fb_opaque_known=1;
        r->fb_all_opaque=1;
        r->fb_all_transparent=0;
      }
    }
    free(copy);
    return;
  }
  if(!flipx && !flipy &&
     fabs(sx0d) < 0.001 && fabs(sy0d) < 0.001 &&
     fabs(swd - sw) < 0.001 && fabs(shd - sh) < 0.001 &&
     (r->blendmode==0 || (replace && src_all_opaque)) && alpha>=1.0 &&
     ((blend & 0xFFFFFF) == 0xFFFFFF) && !sremap && !slut && !sgrid &&
     (!alpha_test || opaque_alpha_test_passthrough)){
    /* Record it rather than write it, while the full-target fill in front of it is still deferred:
     * the two are one operation as far as the target is concerned. The recording lives with its
     * own owner, outside this function's composition kernels. */
    if(render_record_deferred_underlay(r,src,sw,sh,x0,y0,W,H,src_all_opaque)){
      free(copy);
      return;
    }
    /* A composite that covers the target with opaque pixels hides the fill in front of it exactly
     * as an opaque rectangle does, and gml_render_prepare_opaque_rect already cancels a pending
     * fill for the rectangle case. Taking the same route here stops the renderer flushing a
     * monitor-sized write nothing will ever be seen through. */
    if(src_all_opaque && !r->interp && rect_covers_target(r,x0,y0,x0+W,y0+H))
      gml_render_maybe_prepare_opaque_rect(r,x0,y0,x0+W,y0+H);
    else
      gml_render_maybe_prepare_draw(r);
    if(draw_scaled_full_surface_normal(r,src,sw,sh,x0,y0,W,H,src_all_opaque)){
      free(copy);
      return;
    }
  }
  gml_render_maybe_prepare_draw(r);
  /* Bilinear magnification when the game asked for interpolation (texture_set_interpolation(true)):
   * GM/GL sample 4 texels at the output pixel centre. Only the upscale case takes this; filtered
   * downscale keeps the box average, while non-interpolated draws use exact point sampling below.
   * This is what makes a full-screen compositor's soft "old TV" bloom render. */
  if(r->interp && !flipx && !flipy && W>sw && H>sh && r->blendmode==0 &&
     !sremap && !slut){
    /* per-column tap indices + weight are constant across rows: precompute once (hoists the div/floor
     * out of the inner loop) and run the taps in float. */
    int *cxa=malloc((size_t)W*sizeof(int)), *cxb=malloc((size_t)W*sizeof(int));
    float *cfx=malloc((size_t)W*sizeof(float));
    if(cxa&&cxb&&cfx){
      for(int px=0; px<W; px++){
        double fsx=sx0d+(px+0.5)*swd/W-0.5; int a=(int)floor(fsx); cfx[px]=(float)(fsx-a);
        int b=a+1; if(a<0)a=0; else if(a>sw-1)a=sw-1; if(b<0)b=0; else if(b>sw-1)b=sw-1; cxa[px]=a; cxb[px]=b;
      }
      SurfBiCtx ctx={ r, src, cxa, cxb, cfx, sw,sh,W,H,x0,y0, sy0d,shd,0.5,-0.5,
        (float)alpha, bR/255.0f,bG/255.0f,bB/255.0f, !r->alphablend };
      gml_run_row_bands(r,H,surf_bi_band,&ctx);
    }
    free(cxa); free(cxb); free(cfx); free(copy); return;
  }
  for(int py=0; py<H; py++){ int ty_=y0+py; if(ty_<0||ty_>=r->fbh) continue;
    int dpy=flipy?(H-1-py):py;
    int sy0=(int)floor(sy0d + (dpy*shd)/H);
    int sy1=!r->interp ? sy0+1 : (int)floor(sy0d + ((dpy+1)*shd)/H);
    if(sy1<=sy0) sy1=sy0+1;
    if(sy0<0) sy0=0;
    if(sy1>sh) sy1=sh;
    for(int px=0; px<W; px++){ int tx_=x0+px; if(tx_<0||tx_>=r->fbw) continue;
      int dpx=flipx?(W-1-px):px;
      int sx0=(int)floor(sx0d + (dpx*swd)/W);
      int sx1=!r->interp ? sx0+1 : (int)floor(sx0d + ((dpx+1)*swd)/W);
      if(sx1<=sx0) sx1=sx0+1;
      if(sx0<0) sx0=0;
      if(sx1>sw) sx1=sw;
      int R=0,G=0,B=0,A=0,n=0;
      for(int sy=sy0;sy<sy1;sy++) for(int sx=sx0;sx<sx1;sx++){
        uint32_t s=src[(size_t)sy*sw+sx]; R+=(s>>16)&0xFF; G+=(s>>8)&0xFF; B+=s&0xFF; A+=s>>24; n++; }
      if(!n) continue;
      if(shader_discards_alpha_value(r,A/(double)n)) continue;
      double pa=(A/(double)n)/255.0; if(pa<=0) continue;   /* box-averaged coverage */
      uint32_t av=((uint32_t)(R/n)<<16)|((uint32_t)(G/n)<<8)|(uint32_t)(B/n);
      if(sremap) av=mapped_texture_pixel(r,av);
      else if(slut) av=lut_map_px(r,slut,av);
      int sr=((av>>16)&0xFF)*bR/255, sg=((av>>8)&0xFF)*bG/255, sb=(av&0xFF)*bB/255;
      uint32_t *dp=&r->fb[(size_t)ty_*r->fbw+tx_];
      double ea=alpha*pa;
      if(r->blendmode==3){ uint32_t old=*dp; *dp=color_write_merge(r,old,blend_multiply_pixel(r,old,sr,sg,sb)); }
      else if(r->blendmode==4){
        uint32_t old=*dp;
        unsigned source_alpha=(unsigned)lround((A/(double)n)*alpha);
        *dp=color_write_merge(r,old,
          blend_max_preset_pixel(r,old,sr,sg,sb,source_alpha));
      }
      else if(replace || (r->classic && src==r->app_surface)){
        /* (one, zero): the sampled fragment replaces the destination, its coverage carried
         * rather than blended by. The classic automatic present takes the same route because
         * the backbuffer it models has no alpha channel to blend with. */
        uint32_t oa=replace?(uint32_t)lround((A/(double)n)):255u;
        *dp=(oa<<24)|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
      }
      else if((!r->alphablend && pa>=1.0) || ea>=1.0){ *dp=0xFF000000u|(sr<<16)|(sg<<8)|sb; }
      else { int dr=(*dp>>16)&0xFF, dg=(*dp>>8)&0xFF, db=*dp&0xFF;
        *dp=0xFF000000u|((int)(sr*ea+dr*(1-ea))<<16)|((int)(sg*ea+dg*(1-ea))<<8)|(int)(sb*ea+db*(1-ea)); }
    }
  }
  free(copy);
}
/* A screen-stage compositor can choose either logical application-surface coordinates or explicit
 * window-pixel coordinates. The latter is observable when surface 0 covers exactly the current
 * target extent. Those dimensions are already physical: applying the logical-to-window GUI
 * transform again magnifies the image by the presentation scale and clips the trailing region. */
static int surface_draw_targets_screen_raster(const GmlRender *r,int surf,
                                              double width,double height){
  return r && surf==0 && gml_render_gui_transform_active(r) &&
         r->target_sp==0 && r->fbw>0 && r->fbh>0 &&
         gml_render_gui_logical_width(r)<=(double)r->fbw+0.001 &&
         gml_render_gui_logical_height(r)<=(double)r->fbh+0.001 &&
         fabs(fabs(width)-(double)r->fbw)<0.001 &&
         fabs(fabs(height)-(double)r->fbh)<0.001;
}

/* First-generation GUI composition samples the application-surface quad at destination pixel
 * centres, both with fractional GUI transforms and with content-owned window presentation.
 * Rounding the rectangle or projecting from its leading edge changes samples at non-integer
 * scales. Restrict this path to the owned application surface; ordinary runtime surfaces
 * retain their established sprite raster rules. */
static int draw_first_generation_gui_app_surface(GmlRender *r,int surf,
                                                  const uint32_t *src,int sw,int sh,
                                                  double dx,double dy,double dw,double dh,
                                                  uint32_t blend,double alpha){
  if(!r || !src || !r->fb || surf!=0 || !r->win ||
     !anygm_policy_uses_first_generation_studio(r->win) ||
     !gml_render_gui_transform_active(r) || r->target_sp!=0 || r->target_id>=0 ||
     r->interp || r->blendmode!=0 || r->color_write_mask!=0x0F ||
     alpha<1.0 || (blend&0xFFFFFFu)!=0xFFFFFFu ||
     !surface_known_opaque(r,surf) || src!=r->app_surface || src==r->fb ||
     dw<=0.0 || dh<=0.0) return 0;
  int transformed_gui_quad=
    fabs(dw-(double)sw*r->gui_scale_x)<=0.001 &&
    fabs(dh-(double)sh*r->gui_scale_y)<=0.001 &&
    (fabs(dw-lround(dw))>=0.001 || fabs(dh-lround(dh))>=0.001);
  int content_owned_presentation=!r->app_draw_enable;
  if(!transformed_gui_quad && !content_owned_presentation) return 0;
  dx-=r->cam_x;
  dy-=r->cam_y;
  int x0=(int)ceil(dx-0.5), y0=(int)ceil(dy-0.5);
  int x1=(int)ceil(dx+dw-0.5), y1=(int)ceil(dy+dh-0.5);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  if(x0>=x1 || y0>=y1) return 1;
  /* This is the frame's last operation, and nothing already in the target can survive it. Record it
   * rather than write it: if anything draws afterwards the record is written through the one kernel
   * its owner holds and the frame is exactly what it always was, and if nothing does, the operation
   * is still available for somewhere other than the processor to perform. The recording and the
   * writing live with that owner, not here, so this function's body stays what it was. */
  render_present_first_generation(r,surf,src,sw,sh,x0,y0,x1,y1,dx,dy,dw,dh);
  return 1;
}

/* draw_surface_stretched[_ext]: blit a runtime surface into the current target, box-averaged. */
/* A surface drawn through a program this renderer does not execute, somewhere other than the
 * frame's terminal presentation: the host runs the program over the surface at the destination's
 * size, and the result is composed here as a surface of that size, with the blend, alpha and
 * flips the draw asked for. Returns 0 when the host could not, and the draw proceeds unshaded. */
/* Run the content's active program over a destination through the host's executor and compose the
 * result at the current target with the given blend, alpha and flips. `source` is the picture the
 * program samples (its region names the part shown); a procedural program ignores it. `dx,dy` are
 * in target pixels — the caller resolved any camera offset — and the composition adds the camera
 * back before draw_surface_region takes it off again. Returns 0 when the host cannot. */
static int shade_source_to_target(GmlRender *r,const uint32_t *source,int sw,int sh,
                                   double rx,double ry,double rw,double rh,uint32_t identity,
                                   double dx,double dy,double dw,double dh,
                                   uint32_t blend,double alpha){
  enum { SHADED_MAX_EXTENT=4096, SHADED_MAX_PIXELS=16u<<20 };
  GmlRenderShaderRequest request;
  int width=(int)lround(fabs(dw)),height=(int)lround(fabs(dh));
  size_t pixels;
  int saved;
  if(!source || sw<=0 || sh<=0) return 0;
  if(width<=0 || height<=0 || width>SHADED_MAX_EXTENT || height>SHADED_MAX_EXTENT) return 0;
  pixels=(size_t)width*(size_t)height;
  if(pixels>SHADED_MAX_PIXELS) return 0;
  if(pixels>r->shaded_plane_capacity){
    uint32_t *grown=(uint32_t*)realloc(r->shaded_plane,pixels*sizeof *grown);
    if(!grown) return 0;
    r->shaded_plane=grown;
    r->shaded_plane_capacity=pixels;
  }
  memset(&request,0,sizeof request);
  request.shader=r->active_shader;
  request.source=source;
  request.source_width=sw;
  request.source_height=sh;
  request.source_pitch=sw;
  request.region_x=(int)lround(rx);
  request.region_y=(int)lround(ry);
  request.region_width=(int)lround(rw);
  request.region_height=(int)lround(rh);
  if(request.region_x<0 || request.region_y<0 ||
     request.region_width<=0 || request.region_height<=0 ||
     request.region_x+request.region_width>sw || request.region_y+request.region_height>sh)
    return 0;
  request.source_identity=identity;
  request.serial=++r->shaded_requests;
  request.width=width;
  request.height=height;
  /* Where this draw lands on the current render target, so a fragment reading its own place gets
   * the answer it would get drawing there. */
  request.target_width=r->fbw;
  request.target_height=r->fbh;
  request.dest_x=(int)lround(dx-r->cam_x);
  request.dest_y=(int)lround(dy-r->cam_y);
  request.linear=r->interp?1:0;
  request.output=r->shaded_plane;
  if(!r->shader_executor(r->shader_executor_context,&request)) return 0;
  r->shaded_plane_borrowed=NULL;
  r->shaded_plane_width=width;
  r->shaded_plane_height=height;
  r->shaded_draws++;
  saved=r->active_shader;
  r->active_shader=-1;
  draw_surface_region(r,GML_RENDER_SHADED_SURFACE,0,0,width,height,dx,dy,
                      dw<0?-(double)width:(double)width,dh<0?-(double)height:(double)height,
                      blend,alpha);
  r->active_shader=saved;
  return 1;
}

static int draw_surface_through_program(GmlRender *r,int surf,const uint32_t *spx,int sw,int sh,
                                        double rx,double ry,double rw,double rh,
                                        double dx,double dy,double dw,double dh,
                                        uint32_t blend,double alpha){
  return shade_source_to_target(r,spx,sw,sh,rx,ry,rw,rh,
                                (uint32_t)(surf<0?0x7FFFFFFF:surf),dx,dy,dw,dh,blend,alpha);
}

static void draw_surface_stretched_impl(GmlRender *r,int surf,double dx,double dy,
                                        double dw,double dh,uint32_t blend,double alpha,
                                        int allow_software3d){
  int explicit_target_raster=surface_draw_targets_screen_raster(r,surf,dw,dh);
  if(!explicit_target_raster){
    gml_render_draw_map_point(r,&dx,&dy);
    gml_render_draw_map_scale(r,&dw,&dh);
  }
  const struct GmlShaderPal *sdual=dual_active(r);
  if(render_setting(r,"GML_LOG_SHADER") && r && r->active_shader>=0 && r->stretched_shader_log_count++<8){
    
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] f%ld stretched active=%d surface=%d\n",
      r->frame,r->active_shader,surf);
  }
  int d3w=gml_surface_width(r,surf),d3h=gml_surface_height(r,surf);
  if(allow_software3d && !sdual && !mapped_texture_active(r) &&
     d3w>0&&d3h>0&&
     gml_d3_draw_surface_part_2d(r,surf,0,0,d3w,d3h,dx,dy,dw/d3w,dh/d3h,blend,alpha)) return;
  int sw=0, sh=0; uint32_t *spx=surface_pixels(r,surf,&sw,&sh); if(!spx) return;
  /* A full-width presentation compositor may retain an integer-scaled native-aspect destination
   * even though its application surface has been widened. Drawing the wide surface into that
   * narrower rectangle squeezes the world horizontally. For a near-full-height application-surface
   * composite, preserve the source aspect using the compositor's chosen vertical scale, then center
   * it in the target. Small previews and ordinary surface draws are intentionally left alone. */
  if(r && r->aspect_fullwidth && surf==0 && r->target_sp==0 &&
     sw==r->aspect_wide_w && sh==r->aspect_wide_h && sw>0 && sh>0 &&
     dw>0.0 && dh>0.0 && r->fbw>0 && r->fbh>0 &&
     dh >= (double)r->fbh*0.75 && dw/dh + 0.01 < (double)sw/(double)sh){
    double scale=dh/(double)sh;
    double want_w=(double)sw*scale;
    if(want_w <= (double)r->fbw+0.5){
      dw=want_w;
      dx=((double)r->fbw-dw)*0.5+r->cam_x;
    } else {
      scale=(double)r->fbw/(double)sw;
      dw=(double)r->fbw;
      dh=(double)sh*scale;
      dx=r->cam_x;
      dy=((double)r->fbh-dh)*0.5+r->cam_y;
    }
  }
  /* Interpolated surface draws with default application-surface blitting disabled
   * signal self-composition. Sticky; the host reads this next frame to supersample that pass. */
  if(r && !r->app_draw_enable && r->interp) r->composites_app=1;
  if(sdual){ draw_surface_dual_sample(r,sdual,surf,0,0,sw,sh,dx,dy,dw,dh,blend,alpha); return; }
  /* A surface presented through a program this renderer does not execute: the frame's last
   * operation, recorded for the host's graphics context when one is adopted, and otherwise drawn
   * plain below exactly as before. The shape is the content-owned presentation's: base target,
   * opaque, unblended, covering the target or sitting on a fill that does. */
  if(r->active_shader>=0 && gml_render_shader_content_candidate(r,r->active_shader) &&
     r->presentation_deferral_enabled && r->target_sp==0 && r->target_id<0 && r->fb==r->base_fb &&
     spx!=r->fb && alpha>=1.0 && (blend&0xFFFFFFu)==0xFFFFFFu && r->blendmode==0 &&
     r->color_write_mask==0x0F && dw>0.0 && dh>0.0){
    double ldx=dx-r->cam_x,ldy=dy-r->cam_y;
    int x0=(int)ceil(ldx-0.5),y0=(int)ceil(ldy-0.5);
    int x1=(int)ceil(ldx+dw-0.5),y1=(int)ceil(ldy+dh-0.5);
    if(x0<0) x0=0;
    if(y0<0) y0=0;
    if(x1>r->fbw) x1=r->fbw;
    if(y1>r->fbh) y1=r->fbh;
    if(x0<x1 && y0<y1 &&
       render_present_content_shader(r,surf,spx,sw,sh,x0,y0,x1,y1,ldx,ldy,dw,dh,r->active_shader))
      return;
  }
  if(r->active_shader>=0 && r->shader_executor && spx!=r->fb &&
     gml_render_shader_content_candidate(r,r->active_shader) &&
     draw_surface_through_program(r,surf,spx,sw,sh,0,0,sw,sh,dx,dy,dw,dh,blend,alpha)) return;
  if(draw_first_generation_gui_app_surface(r,surf,spx,sw,sh,dx,dy,dw,dh,blend,alpha)) return;
  if(r->surface_draw_logging < 0) r->surface_draw_logging = render_setting(r,"GML_LOG_SURF_DRAW") != NULL;
  const char *log_surf_frame = r->surface_draw_logging ? render_setting(r,"GML_LOG_SURF_DRAW_FRAME") : NULL;
  
  if(r->surface_draw_logging && (!log_surf_frame || !*log_surf_frame ||
                       r->frame == atol(log_surf_frame))){
    if(r->surface_draw_log_count++<80){
      int nz=0, sx0=-1, sy0=-1, minx=sw, miny=sh, maxx=-1, maxy=-1; uint32_t sv0=0;
      for(int i=0;i<sw*sh;i++) if(spx[i]>>24){
        if(nz==0){ sx0=i%sw; sy0=i/sw; sv0=spx[i]; }
        int px=i%sw, py=i/sw;
        if(px<minx) minx=px;
        if(px>maxx) maxx=px;
        if(py<miny) miny=py;
        if(py>maxy) maxy=py;
        nz++;
      }
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[surfdraw] target=%d surf=%d src=%dx%d nz=%d dst=(%.0f,%.0f %.0fx%.0f) blend=%06X alpha=%.2f bm=%d alphablend=%d into=%p %dx%d base=%p %dx%d\n",
              r?r->target_id:-999,surf,sw,sh,nz,dx,dy,dw,dh,blend&0xFFFFFF,alpha,r?r->blendmode:-1,r?r->alphablend:-1,
              (const void *)(r?r->fb:NULL),r?r->fbw:0,r?r->fbh:0,(const void *)(r?r->base_fb:NULL),r?r->base_fbw:0,r?r->base_fbh:0);
      if(nz>0) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[surfdraw]   first=(%d,%d) argb=%08X bbox=(%d,%d)-(%d,%d)\n",sx0,sy0,sv0,minx,miny,maxx,maxy);
      /* The figures above count alpha. Count nonzero colour separately so an
       * opaque black surface does not appear to hold coloured pixels. */
      {
        int lit=0, lx0=sw, ly0=sh, lx1=-1, ly1=-1;
        for(int i=0;i<sw*sh;i++) if(spx[i]&0x00FFFFFFu){
          int px=i%sw, py=i/sw;
          if(px<lx0) lx0=px;
          if(py<ly0) ly0=py;
          if(px>lx1) lx1=px;
          if(py>ly1) ly1=py;
          lit++;
        }
        anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
          "[surfdraw]   lit=%d of %d colour-bbox=(%d,%d)-(%d,%d)\n",lit,sw*sh,lx0,ly0,lx1,ly1);
      }
    }
  }
  int prof=rprof_enabled();
  double t0=prof?rprof_now():0.0;
  draw_surface_region(r,surf,0,0,sw,sh,dx,dy,dw,dh,blend,alpha);
  /* Log destination coverage after the draw to distinguish a skipped draw from one that wrote no colored pixels. */
  if(r && r->surface_draw_logging>0 && r->fb && r->fbw>0 && r->fbh>0){
    int dlit=0; for(int i=0;i<r->fbw*r->fbh;i++) if(r->fb[i]&0x00FFFFFFu) dlit++;
    anygm_host_logf(r->win?r->win->host:NULL,ANYGM_LOG_DEBUG,
      "[surfdraw]   destination lit=%d of %d after the draw\n",dlit,r->fbw*r->fbh);
  }
  if(prof) rprof_add("surface",r,NULL,(rprof_now()-t0)*1000.0,(unsigned long long)llround(fabs(dw*dh)));
}
int gml_render_shade_target_rect(GmlRender *r,int x1,int y1,int x2,int y2,uint32_t colour,double alpha){
  uint32_t source;
  if(!r || r->active_shader<0 || !r->shader_executor ||
     !gml_render_shader_content_candidate(r,r->active_shader)) return 0;
  if(x1>x2){ int t=x1; x1=x2; x2=t; }
  if(y1>y2){ int t=y1; y1=y2; y2=t; }
  if(x2<=x1 || y2<=y1) return 0;
  /* A one-texel source of the primitive's colour: a program that samples gm_BaseTexture reads the
   * flat colour a rectangle carries, and a procedural one ignores it. */
  source=gml_render_backend_color_to_xrgb(colour)|0xFF000000u;
  return shade_source_to_target(r,&source,1,1,0,0,1,1,0xFFFFFFFFu,
                                (double)x1+r->cam_x,(double)y1+r->cam_y,
                                (double)(x2-x1),(double)(y2-y1),0xFFFFFFu,alpha);
}

/* The program's answer for one atlas rectangle, evaluated on the device once and kept. A sprite
 * frame and a font glyph are the same thing here — a rectangle of a texture page — so both reuse
 * one answer, which is what turns a device round trip per draw into one per distinct rectangle. */
const uint32_t *gml_render_shaded_atlas_rect(GmlRender *r,int atlas,int sx,int sy,int w,int h){
  int shader,spare=-1;
  const uint32_t *px=NULL;
  uint64_t fingerprint;
  uint32_t oldest=0xFFFFFFFFu;
  GmlRenderShaderRequest request;
  if(!r || r->active_shader<0 || !r->shader_executor || w<=0 || h<=0) return NULL;
  if(!gml_render_shader_content_candidate(r,r->active_shader)) return NULL;
  /* A program whose answer moves with the draw cannot be kept for a rectangle. */
  if(r->shader_pal && r->shader_pal[r->active_shader].position_dependent) return NULL;
  shader=r->active_shader;
  fingerprint=gml_render_shader_uniform_fingerprint(r,shader);
  for(int i=0;i<GML_SHADED_FRAME_CACHE;i++){
    if(r->shaded_frame[i].px && r->shaded_frame[i].atlas==atlas && r->shaded_frame[i].sx==sx &&
       r->shaded_frame[i].sy==sy && r->shaded_frame[i].w==w && r->shaded_frame[i].h==h &&
       r->shaded_frame[i].shader==shader && r->shaded_frame[i].fingerprint==fingerprint){
      r->shaded_frame[i].last_used=++r->shaded_frame_clock;
      return r->shaded_frame[i].px;
    }
  }
  if(!gml_render_atlas_rect_plane(r,atlas,sx,sy,w,h,&px) || !px) return NULL;
  for(int i=0;i<GML_SHADED_FRAME_CACHE;i++){
    if(!r->shaded_frame[i].px){ spare=i; break; }
    if(r->shaded_frame[i].last_used<oldest){ oldest=r->shaded_frame[i].last_used; spare=i; }
  }
  if(spare<0) return NULL;
  if(r->shaded_frame[spare].w*r->shaded_frame[spare].h!=w*h || !r->shaded_frame[spare].px){
    uint32_t *grown=(uint32_t*)realloc(r->shaded_frame[spare].px,(size_t)w*h*sizeof *grown);
    if(!grown) return NULL;
    r->shaded_frame[spare].px=grown;
  }
  memset(&request,0,sizeof request);
  request.shader=shader;
  request.source=px;
  request.source_width=w;
  request.source_height=h;
  request.source_pitch=w;
  request.region_width=w;
  request.region_height=h;
  request.source_identity=((uint32_t)atlas<<20)^((uint32_t)sx<<10)^(uint32_t)sy;
  request.serial=++r->shaded_requests;
  request.width=w;
  request.height=h;
  request.output=r->shaded_frame[spare].px;
  if(!r->shader_executor(r->shader_executor_context,&request)) return NULL;
  r->shaded_frame[spare].atlas=atlas;
  r->shaded_frame[spare].sx=sx;
  r->shaded_frame[spare].sy=sy;
  r->shaded_frame[spare].shader=shader;
  r->shaded_frame[spare].fingerprint=fingerprint;
  r->shaded_frame[spare].w=w;
  r->shaded_frame[spare].h=h;
  r->shaded_frame[spare].last_used=++r->shaded_frame_clock;
  return r->shaded_frame[spare].px;
}

/* Compose an already-shaded plane at the target, with the draw's own blend, alpha and flips. */
int gml_render_compose_shaded_plane(GmlRender *r,const uint32_t *plane,int w,int h,
                                    double rx,double ry,double rw,double rh,
                                    double dx,double dy,double dw,double dh,
                                    uint32_t blend,double alpha){
  int saved;
  if(!r || !plane || w<=0 || h<=0) return 0;
  if(rx<0){ rw+=rx; rx=0; }
  if(ry<0){ rh+=ry; ry=0; }
  if(rw<=0 || rh<=0 || rx>=w || ry>=h) return 0;
  if(rx+rw>w) rw=w-rx;
  if(ry+rh>h) rh=h-ry;
  saved=r->active_shader;
  r->active_shader=-1;
  r->shaded_plane_borrowed=plane;
  r->shaded_plane_width=w;
  r->shaded_plane_height=h;
  draw_surface_region(r,GML_RENDER_SHADED_SURFACE,rx,ry,rw,rh,dx,dy,dw,dh,blend,alpha);
  r->shaded_plane_borrowed=NULL;
  r->active_shader=saved;
  r->shaded_draws++;
  return 1;
}

int gml_render_sprite_frame_rect(GmlRender *r,int sprite,int frame,int *atlas,int *sx,int *sy,
                                 int *w,int *h){
  return gml_render_sprite_frame_rect_full(r,sprite,frame,atlas,sx,sy,w,h,NULL,NULL);
}

/* Draw one atlas rectangle through the content's program at a destination. A program whose answer
 * is a property of the texels is evaluated once for the rectangle and kept; one whose answer moves
 * with the draw is evaluated where it lands, which costs a device round trip per draw and is the
 * only way it can be right. Returns 0 when nothing can run it. */
int gml_render_shade_atlas_rect_at(GmlRender *r,int atlas,int sx,int sy,int w,int h,
                                   double dx,double dy,double dw,double dh,
                                   uint32_t blend,double alpha){
  if(!r || r->active_shader<0 || !r->shader_executor || w<=0 || h<=0) return 0;
  if(!gml_render_shader_content_candidate(r,r->active_shader)) return 0;
  if(r->shader_pal && r->shader_pal[r->active_shader].position_dependent){
    const uint32_t *px=NULL;
    if(!gml_render_atlas_rect_plane(r,atlas,sx,sy,w,h,&px) || !px) return 0;
    return shade_source_to_target(r,px,w,h,0,0,w,h,
                                  ((uint32_t)atlas<<20)^((uint32_t)sx<<10)^(uint32_t)sy,
                                  dx,dy,dw,dh,blend,alpha);
  }
  {
    const uint32_t *shaded=gml_render_shaded_atlas_rect(r,atlas,sx,sy,w,h);
    if(!shaded) return 0;
    return gml_render_compose_shaded_plane(r,shaded,w,h,0,0,w,h,dx,dy,dw,dh,blend,alpha);
  }
}

/* Compose an already-shaded plane as a rotated sprite. The ordinary sprite blit already has the
 * pivot, the filtering and the edge rules, and it reads the byte order a decoded page uses, so the
 * plane is handed over in that order rather than a second rotation being written here. */
int gml_render_compose_shaded_rotated(GmlRender *r,GmlSprite *owner,const uint32_t *plane,
                                      int w,int h,double x,double y,double xs,double ys,
                                      double rot,int origin_x,int origin_y,
                                      uint32_t blend,double alpha){
  size_t count;
  if(!r || !plane || w<=0 || h<=0) return 0;
  count=(size_t)w*(size_t)h;
  if(count>SIZE_MAX/4u) return 0;
  if(count*4u>r->shaded_bytes_capacity){
    uint8_t *grown=(uint8_t*)realloc(r->shaded_bytes,count*4u);
    if(!grown) return 0;
    r->shaded_bytes=grown;
    r->shaded_bytes_capacity=count*4u;
  }
  for(size_t i=0;i<count;i++){
    uint32_t px=plane[i];
    r->shaded_bytes[i*4u+0]=(uint8_t)(px>>16);
    r->shaded_bytes[i*4u+1]=(uint8_t)(px>>8);
    r->shaded_bytes[i*4u+2]=(uint8_t)px;
    r->shaded_bytes[i*4u+3]=(uint8_t)(px>>24);
  }
  {
    int saved=r->active_shader;
    r->active_shader=-1;
    blit_rgba_sprite(r,owner,r->shaded_bytes,w,h,x,y,xs,ys,rot,origin_x,origin_y,blend,alpha,1,
                     NULL,NULL,0);
    r->active_shader=saved;
  }
  r->shaded_draws++;
  return 1;
}

/* The sprite frame's own texture-page rectangle, or a refusal for a runtime-built sprite. */
int gml_render_sprite_frame_rect_full(GmlRender *r,int sprite,int frame,int *atlas,int *sx,int *sy,
                                      int *w,int *h,int *tx,int *ty){
  GmlSprite *s;
  int ti;
  if(!r || sprite<0 || sprite>=r->n_spr) return 0;
  s=&r->spr[sprite];
  if(s->runtime_rgba || !s->frame || s->n_frames<=0) return 0;
  if(frame<0 || frame>=s->n_frames) frame=0;
  ti=s->frame[frame];
  if(ti<0 || ti>=r->n_tpag) return 0;
  {
    GmlTpag *t=&r->tpag[ti];
    if(t->atlas<0 || t->atlas>=r->n_atlas || t->sw<=0 || t->sh<=0) return 0;
    *atlas=t->atlas; *sx=t->sx; *sy=t->sy; *w=t->sw; *h=t->sh;
    if(tx) *tx=t->tx;
    if(ty) *ty=t->ty;
    return 1;
  }
}

int gml_render_shade_target_sprite_part(GmlRender *r,int sprite,int frame,
                                        double rx,double ry,double rw,double rh,
                                        double dx,double dy,double dw,double dh,
                                        uint32_t blend,double alpha){
  int atlas=0,sx=0,sy=0,w=0,h=0;
  const uint32_t *shaded;
  if(!r || r->active_shader<0 || !r->shader_executor ||
     !gml_render_shader_content_candidate(r,r->active_shader)) return 0;
  /* A sprite whose frame is a texture-page rectangle shares the cache with every other rectangle;
   * one built at runtime has no page, and keeps the per-frame path below. */
  {
    /* A stored frame is cropped: the region a draw names is in the sprite's own space, and the
     * page holds only the part that carried colour, placed at the crop offset. Translating by that
     * offset — and clipping the destination to the part the page actually has — is what makes a
     * region of a cropped frame land where the plain blit puts it. */
    int tx=0,ty=0;
    if(gml_render_sprite_frame_rect_full(r,sprite,frame,&atlas,&sx,&sy,&w,&h,&tx,&ty)){
      double scale_x=rw>0?dw/rw:0.0, scale_y=rh>0?dh/rh:0.0;
      double lx0=rx>tx?rx:tx, ly0=ry>ty?ry:ty;
      double lx1=rx+rw<tx+w?rx+rw:tx+w, ly1=ry+rh<ty+h?ry+rh:ty+h;
      if(lx1<=lx0 || ly1<=ly0) return 0;
      dx+=(lx0-rx)*scale_x;
      dy+=(ly0-ry)*scale_y;
      dw=(lx1-lx0)*scale_x;
      dh=(ly1-ly0)*scale_y;
      rx=lx0-tx; ry=ly0-ty; rw=lx1-lx0; rh=ly1-ly0;
    }
  }
  if(gml_render_sprite_frame_rect(r,sprite,frame,&atlas,&sx,&sy,&w,&h)){
    if(r->shader_pal && r->shader_pal[r->active_shader].position_dependent){
      const uint32_t *px=NULL;
      if(!gml_render_atlas_rect_plane(r,atlas,sx,sy,w,h,&px) || !px) return 0;
      if(rx<0){ rw+=rx; rx=0; }
      if(ry<0){ rh+=ry; ry=0; }
      if(rw<=0 || rh<=0 || rx>=w || ry>=h) return 0;
      if(rx+rw>w) rw=w-rx;
      if(ry+rh>h) rh=h-ry;
      return shade_source_to_target(r,px,w,h,rx,ry,rw,rh,
                                    ((uint32_t)atlas<<20)^((uint32_t)sx<<10)^(uint32_t)sy,
                                    dx,dy,dw,dh,blend,alpha);
    }
    shaded=gml_render_shaded_atlas_rect(r,atlas,sx,sy,w,h);
    if(!shaded) return 0;
    return gml_render_compose_shaded_plane(r,shaded,w,h,rx,ry,rw,rh,dx,dy,dw,dh,blend,alpha);
  }
  {
    const uint32_t *px=NULL;
    if(!gml_render_sprite_frame_plane(r,sprite,frame,&w,&h,&px) || !px || w<=0 || h<=0) return 0;
    if(rx<0){ rw+=rx; rx=0; }
    if(ry<0){ rh+=ry; ry=0; }
    if(rw<=0 || rh<=0 || rx>=w || ry>=h) return 0;
    if(rx+rw>w) rw=w-rx;
    if(ry+rh>h) rh=h-ry;
    return shade_source_to_target(r,px,w,h,rx,ry,rw,rh,
                                  ((uint32_t)sprite<<10)|((uint32_t)frame&0x3FFu),
                                  dx,dy,dw,dh,blend,alpha);
  }
}

int gml_render_shade_target_sprite(GmlRender *r,int sprite,int frame,
                                    double dx,double dy,double dw,double dh,
                                    uint32_t blend,double alpha){
  int w=0,h=0;
  const uint32_t *px=NULL;
  if(!r || r->active_shader<0) return 0;
  if(!gml_render_sprite_frame_plane(r,sprite,frame,&w,&h,&px) || !px || w<=0 || h<=0) return 0;
  return gml_render_shade_target_sprite_part(r,sprite,frame,0,0,w,h,dx,dy,dw,dh,blend,alpha);
}

void gml_draw_surface_stretched(GmlRender *r,int surf,double dx,double dy,
                                double dw,double dh,uint32_t blend,double alpha){
  /* A surface drawn onto the base canvas, rather than into another surface, is content compositing
   * its own screen. A bare refresh means only presentation, and a newly created transparent scratch
   * surface writes no pixels. An authored surface can still count after a transparent clear. */
  int slot=surface_slot(surf);
  if(r && r->target_id<0 && alpha>0.0 && slot>=0 &&
     (!surface_known_transparent(r,surf) ||
      (r->content_authored_surfaces&(UINT64_C(1)<<slot))))
    r->content_composited_screen=1;
  draw_surface_stretched_impl(r,surf,dx,dy,dw,dh,blend,alpha,1);
}
int gml_render_backend_surface_stretched(GmlRender *r,int surf,double dx,double dy,
                                         double dw,double dh,uint32_t blend,double alpha){
  if(!r || (surf!=0 && !gml_surface_exists(r,surf))) return 0;
  draw_surface_stretched_impl(r,surf,dx,dy,dw,dh,blend,alpha,0);
  return 1;
}

/* draw_surface_ext rotates around its (x,y) origin, unlike a stretched surface call. Keep this
 * portable software path independent of shaders/GPU APIs; it also provides the fixed-function
 * multiply blend used by surface light masks. */
void gml_draw_surface_ext(GmlRender *r,int surf,double x,double y,
                          double xs,double ys,double rot,uint32_t blend,double alpha){
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  if(fabs(rr)<0.001 || fabs(rr-360.0)<0.001){
    int sw=gml_surface_width(r,surf),sh=gml_surface_height(r,surf);
    if(sw>0&&sh>0) gml_draw_surface_stretched(r,surf,x,y,sw*xs,sh*ys,blend,alpha);
    return;
  }
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  int sw=0,sh=0; uint32_t *src=surface_pixels(r,surf,&sw,&sh);
  if(!r||!r->fb||!src||sw<=0||sh<=0||xs==0.0||ys==0.0||alpha<=0.0) return;
  if(alpha>1.0) alpha=1.0;
  uint32_t *copy=NULL;
  if(src==r->fb){
    gml_render_maybe_prepare_draw(r);
    copy=malloc((size_t)sw*sh*sizeof(*copy));
    if(!copy) return;
    memcpy(copy,src,(size_t)sw*sh*sizeof(*copy)); src=copy;
  }
  double c,sn; render_rotation_sincos(rr,&c,&sn);
  double ax=x-r->cam_x,ay=y-r->cam_y;
  render_modern_cardinal_anchor(r,rr,xs,ys,c,sn,&ax,&ay);
  double local[4][2]={{0,0},{sw*xs,0},{sw*xs,sh*ys},{0,sh*ys}};
  double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
  for(int i=0;i<4;i++){
    double px=ax+local[i][0]*c+local[i][1]*sn;
    double py=ay-local[i][0]*sn+local[i][1]*c;
    if(px<minx) minx=px;
    if(px>maxx) maxx=px;
    if(py<miny) miny=py;
    if(py>maxy) maxy=py;
  }
  int x0=(int)floor(minx)-1,y0=(int)floor(miny)-1;
  int x1=(int)ceil(maxx)+1,y1=(int)ceil(maxy)+1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  if(x1<=x0||y1<=y0){ free(copy); return; }
  gml_render_maybe_prepare_draw(r);
  int bR=blend&255,bG=(blend>>8)&255,bB=(blend>>16)&255;
  int sremap=indexed_brightness_active(r)!=NULL || threshold_palette_active(r)!=NULL;
  const struct GmlShaderPal *slut=lut_active(r),*sgrid=grid_active(r);
  /* A four-colour quantiser is bound for a whole-surface pass, not for sprite draws. */
  const struct GmlShaderPal *squant=quantise4_active(r);
  for(int py=y0;py<y1;py++) for(int px=x0;px<x1;px++){
    double rx=px+0.5-ax,ry=py+0.5-ay;
    double u=(rx*c-ry*sn)/xs,v=(rx*sn+ry*c)/ys;
    if(u<0.0||v<0.0||u>=(double)sw||v>=(double)sh) continue;
    uint32_t sv;
    if(r->interp){
      double fx=u-0.5,fy=v-0.5; int ua=(int)floor(fx),va=(int)floor(fy);
      double tx=fx-ua,ty=fy-va; int ub=ua+1,vb=va+1;
      if(ua<0)ua=0; else if(ua>=sw)ua=sw-1; if(ub<0)ub=0; else if(ub>=sw)ub=sw-1;
      if(va<0)va=0; else if(va>=sh)va=sh-1; if(vb<0)vb=0; else if(vb>=sh)vb=sh-1;
      uint32_t p[4]={src[(size_t)va*sw+ua],src[(size_t)va*sw+ub],
                     src[(size_t)vb*sw+ua],src[(size_t)vb*sw+ub]};
      double w[4]={(1-tx)*(1-ty),tx*(1-ty),(1-tx)*ty,tx*ty};
      int cr=0,cg=0,cb=0,ca=0;
      for(int k=0;k<4;k++){ cr+=(int)(((p[k]>>16)&255)*w[k]); cg+=(int)(((p[k]>>8)&255)*w[k]);
        cb+=(int)((p[k]&255)*w[k]); ca+=(int)((p[k]>>24)*w[k]); }
      sv=((uint32_t)ca<<24)|((uint32_t)cr<<16)|((uint32_t)cg<<8)|(uint32_t)cb;
    } else sv=src[(size_t)(int)floor(v)*sw+(int)floor(u)];
    if(sremap) sv=mapped_texture_pixel(r,sv);
    else if(squant) sv=quantise4_map_px(squant,sv);
    else if(slut) sv=lut_map_px(r,slut,sv); else if(sgrid) sv=grid_map_px(r,sgrid,sv);
    int sr=((sv>>16)&255)*bR/255,sg=((sv>>8)&255)*bG/255,sb=(sv&255)*bB/255;
    uint32_t *dp=&r->fb[(size_t)py*r->fbw+px],old=*dp,out;
    double sa=((sv>>24)/255.0)*alpha;
    if(r->blendmode==3) out=blend_multiply_pixel(r,old,sr,sg,sb);
    else if(r->blendmode==4){
      unsigned source_alpha=(unsigned)lround((double)(sv>>24)*alpha);
      out=blend_max_preset_pixel(r,old,sr,sg,sb,source_alpha);
    }
    else if(r->blendmode==1||r->blendmode==2){
      int dr=(old>>16)&255,dg=(old>>8)&255,db=old&255;
      int nr,ng,nb;
      if(r->blendmode==1){
        nr=dr+(int)(sr*sa); ng=dg+(int)(sg*sa); nb=db+(int)(sb*sa);
        if(nr>255) nr=255;
        if(ng>255) ng=255;
        if(nb>255) nb=255;
      } else {
        nr=(int)gml_blend_inv_source_u8((unsigned)dr,(unsigned)sr);
        ng=(int)gml_blend_inv_source_u8((unsigned)dg,(unsigned)sg);
        nb=(int)gml_blend_inv_source_u8((unsigned)db,(unsigned)sb);
      }
      out=0xFF000000u|((uint32_t)nr<<16)|((uint32_t)ng<<8)|(uint32_t)nb;
    } else if(!r->alphablend||sa>=1.0) out=0xFF000000u|((uint32_t)sr<<16)|((uint32_t)sg<<8)|(uint32_t)sb;
    else if(sa<=0.0) continue;
    else {
      int dr=(old>>16)&255,dg=(old>>8)&255,db=old&255;
      out=0xFF000000u|((uint32_t)(sr*sa+dr*(1-sa)+0.5)<<16)|
          ((uint32_t)(sg*sa+dg*(1-sa)+0.5)<<8)|(uint32_t)(sb*sa+db*(1-sa)+0.5);
    }
    *dp=color_write_merge(r,old,out);
  }
  r->fb_opaque_known=0;
  free(copy);
}
void gml_draw_surface_part_ext(GmlRender *r, int surf, double sx, double sy, double sw, double sh,
                               double dx, double dy, double xs, double ys, uint32_t blend, double alpha){
  int explicit_target_raster=surface_draw_targets_screen_raster(r,surf,sw*xs,sh*ys);
  if(!explicit_target_raster){
    gml_render_draw_map_point(r,&dx,&dy);
    gml_render_draw_map_scale(r,&xs,&ys);
  }
  const struct GmlShaderPal *sdual=dual_active(r);
  if(!sdual && !mapped_texture_active(r) &&
     gml_d3_draw_surface_part_2d(r,surf,sx,sy,sw,sh,dx,dy,xs,ys,blend,alpha)) return;
  if(r && !r->app_draw_enable && r->interp) r->composites_app=1;
  if(sdual){ draw_surface_dual_sample(r,sdual,surf,sx,sy,sw,sh,dx,dy,sw*xs,sh*ys,blend,alpha); return; }
  /* A region of a surface drawn through a program this renderer does not execute: the host shades
   * that region at the destination's size and the answer is composed here, exactly as for a whole
   * surface. */
  if(r->active_shader>=0 && r->shader_executor &&
     gml_render_shader_content_candidate(r,r->active_shader)){
    int psw=0,psh=0;
    uint32_t *ppx=surface_pixels(r,surf,&psw,&psh);
    if(ppx && ppx!=r->fb && psw>0 && psh>0 &&
       draw_surface_through_program(r,surf,ppx,psw,psh,sx,sy,sw,sh,dx,dy,sw*xs,sh*ys,blend,alpha))
      return;
  }
  int prof=rprof_enabled();
  double t0=prof?rprof_now():0.0;
  draw_surface_region(r,surf,sx,sy,sw,sh,dx,dy,sw*xs,sh*ys,blend,alpha);
  if(prof) rprof_add("surface",r,NULL,(rprof_now()-t0)*1000.0,(unsigned long long)llround(fabs(sw*xs*sh*ys)));
}
