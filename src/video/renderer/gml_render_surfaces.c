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

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
uint32_t *surface_pixels(GmlRender *r, int id, int *w, int *h){
  if(id==0){
    if(!r->app_surface) return NULL;
    /* the app surface has its own dims (the view render); the current target may be the larger
     * presentation canvas — using target dims here read the view buffer with the wrong stride */
    if(w) *w=r->app_w?r->app_w:(r->base_fbw?r->base_fbw:r->fbw);
    if(h) *h=r->app_h?r->app_h:(r->base_fbh?r->base_fbh:r->fbh);
    return r->app_surface;
  }
  int i=surface_slot(id);
  if(i<0 || !r->surface[i].live || !r->surface[i].px) return NULL;
  if(w) *w=r->surface[i].w;
  if(h) *h=r->surface[i].h;
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
void gml_surface_resize(GmlRender *r, int id, int w, int h){
  if(!r || w<=0 || h<=0 || w>4096 || h>4096) return;
  if(id==0){
    /* Studio 1.x exposes surface 0 but its legacy presentation path does not turn
     * surface_resize(application_surface, ...) into a persistent, separately allocated target.
     * Doing so shrinks its fixed-width compositors into one corner of a forced-aspect frame.
     * The independently resizable application surface is a modern-format behavior. */
    if(!r->win || !anygm_policy_has_modern_layer_semantics(r->win)) return;
    if(render_setting(r,"GML_LOG_SURF")) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[surf] resize application_surface %dx%d (was %dx%d)\n",w,h,r->app_w,r->app_h);
    if((size_t)w > SIZE_MAX/(size_t)h || (size_t)w*(size_t)h > SIZE_MAX/sizeof(uint32_t)) return;
    uint32_t *old=r->app_surface;
    int oldw=r->app_w, oldh=r->app_h;
    uint32_t *px=calloc((size_t)w*(size_t)h,sizeof(uint32_t));
    if(!px) return;
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
  int si=surface_slot(id);
  { if(si>=0) r->surface[si].dirty=1; }   /* about to be drawn into */
  r->target_stack[r->target_sp++]=(typeof(r->target_stack[0])){
    r->fb,r->fbw,r->fbh,r->cam_x,r->cam_y,r->projection_cam_x,r->projection_cam_y,
    r->target_id,r->fb_opaque_known,r->fb_all_opaque,r->fb_all_transparent,
    r->pending_underlay,r->underlay_x,r->underlay_y,r->underlay_w,r->underlay_h,
    r->pending_fill,r->pending_fill_color
  };
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
  return 1;
}
void gml_surface_reset_target(GmlRender *r){
  if(r && surface_slot(r->target_id)>=0){
    gml_render_flush_pending_underlay(r);
    gml_render_flush_pending_fill(r);
    surface_store_target_coverage(r);
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
          uint32_t d=*dp,ia=255-a;
          int sr=(s>>16)&0xFF,sg=(s>>8)&0xFF,sb=s&0xFF;
          int dr=(d>>16)&0xFF,dg=(d>>8)&0xFF,db=d&0xFF;
          *dp=0xFF000000u|((uint32_t)((sr*a+dr*ia)/255)<<16)|
              ((uint32_t)((sg*a+dg*ia)/255)<<8)|(uint32_t)((sb*a+db*ia)/255);
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

  int *xspan = (int*)malloc((size_t)W * 2u * sizeof(int));
  if(!xspan) return 0;
  int *xs0 = xspan, *xs1 = xspan + W;
  for(int px=0; px<W; px++){
    int sx0, sx1;
    if(W>=sw){
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

  for(int py=py0; py<py1; py++){
    int sy0, sy1;
    if(H>=sh){
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
            *dp = 0xFF000000u | ((int)(sr * pa + dr * (1.0 - pa)) << 16) |
                  ((int)(sg * pa + dg * (1.0 - pa)) << 8) |
                  (int)(sb * pa + db * (1.0 - pa));
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
  const struct GmlShaderPal *spal=pal_active(r);
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
      if(spal) sampled=pal_map_px(spal,sampled);
      else if(slut) sampled=lut_map_px(r,slut,sampled);
      else if(sgrid) sampled=grid_map_px(r,sgrid,sampled);
      sr=((sampled>>16)&0xFF)*bR/255;
      sg=((sampled>>8)&0xFF)*bG/255;
      sb=(sampled&0xFF)*bB/255;
      uint32_t *dp=&drow[px];
      double sa=(aa/255.0)*alpha;
      if(r->blendmode==1||r->blendmode==2){
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
  if(surf==0 && r->classic && !r->interp && !flipx && !flipy && r->app_phase_y &&
     W==sw*2 && H==sh*2 && fabs(sx0d)<0.001 && fabs(sy0d)<0.001 &&
     fabs(swd-sw)<0.001 && fabs(shd-sh)<0.001 &&
     alpha>=1.0 && (blend&0xFFFFFF)==0xFFFFFF && r->blendmode==0 &&
     !shader_alpha_test_active(r)){
    gml_render_maybe_prepare_draw(r);
    for(int oy=0;oy<H;oy++){
      int ty=y0+oy; if(ty<0||ty>=r->fbh) continue;
      int sy=oy>>1;
      const uint32_t *sample_row=((oy&1)?r->app_phase_y:src)+(size_t)sy*sw;
      uint32_t *dp=r->fb+(size_t)ty*r->fbw;
      for(int ox=0;ox<W;ox++){
        int tx=x0+ox; if(tx<0||tx>=r->fbw) continue;
        int sx=(ox+1)>>1;
        /* The half-step at the right edge lands exactly one texel past the surface.  Classic
         * texture repeat wraps that sample to the first column; clamping it to the last column
         * leaves a one-pixel seam at exact 2x. */
        if(sx>=sw) sx=0;
        dp[tx]=sample_row[sx];
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
  const struct GmlShaderPal *spal=pal_active(r);
  const struct GmlShaderPal *slut=lut_active(r);
  const struct GmlShaderPal *sgrid=grid_active(r);
  GmlGridPixelCache grid_cache={0};
  int alpha_test=shader_alpha_test_active(r);
  if(render_setting(r,"GML_LOG_SHADER") && slut && ++r->lut_shader_log_count<=3){
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] surface draw WITH lut: row=%f pal=%d surf=%d\n",slut->lut_row,r->lut_pal_sprite,surf);
    if(r->lut_shader_log_count==1) for(int ry=0;ry<16;ry++)
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader]   pal row %2d: dark=%06x light=%06x\n",ry,
        sprite_pixel_rgb(r,r->lut_pal_sprite,r->lut_pal_frame,0,ry,0xBAD),
        sprite_pixel_rgb(r,r->lut_pal_sprite,r->lut_pal_frame,1,ry,0xBAD)); }
  if(!flipx && !flipy && W==sw && H==sh &&
     fabs(sx0d) < 0.001 && fabs(sy0d) < 0.001 &&
     fabs(swd - sw) < 0.001 && fabs(shd - sh) < 0.001 && !alpha_test){
    int cx0=x0<0?0:x0, cy0=y0<0?0:y0;
    int cx1=x0+W; if(cx1>r->fbw) cx1=r->fbw;
    int cy1=y0+H; if(cy1>r->fbh) cy1=r->fbh;
    int cw=cx1-cx0, ch=cy1-cy0;
    if(cw>0 && ch>0){
      int sx_start=cx0-x0, sy_start=cy0-y0;
      int white=((blend & 0xFFFFFF) == 0xFFFFFF);
      if(src_all_opaque && r->blendmode==0 && alpha>=1.0 && white && !spal && !slut && !sgrid &&
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
      if(r->color_write_mask!=0x0F && r->blendmode==0 && alpha>=1.0 && white && !spal && !slut && !sgrid){
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
            if(spal) sv=pal_map_px(spal,sv);
            else if(slut) sv=lut_map_px(r,slut,sv);
            else if(sgrid) sv=grid_map_px_cached(r,sgrid,sv,&grid_cache);
            int sr=((sv>>16)&255)*bR/255,sg=((sv>>8)&255)*bG/255,sb=(sv&255)*bB/255;
            dp[xx]=color_write_merge(r,old,blend_multiply_pixel(r,old,sr,sg,sb));
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
      } else if((!r->alphablend || alpha>=1.0) && white && spal){
        for(int yy=0; yy<ch; yy++){
          const uint32_t *sp=src+(size_t)(sy_start+yy)*sw+sx_start;
          uint32_t *dp=r->fb+(size_t)(cy0+yy)*r->fbw+cx0;
          for(int xx=0; xx<cw; xx++){ if(!(sp[xx]>>24)) continue; dp[xx]=pal_map_px(spal,sp[xx]); }
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
              uint32_t af=(uint32_t)((sa8*256u)/255u);
              if(af) blend_fast8_src_run(dp+xx,sp+xx,run,af);
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
            if(spal) sv=pal_map_px(spal,sv);
            else if(slut) sv=lut_map_px(r,slut,sv);
            else if(sgrid) sv=grid_map_px_cached(r,sgrid,sv,&grid_cache);
            int sr=((sv>>16)&0xFF)*bR/255, sg=((sv>>8)&0xFF)*bG/255, sb=(sv&0xFF)*bB/255;
            double ea=alpha*pa; double eia=1.0-ea; (void)ia;
            if((!r->alphablend || ea>=1.0)) dp[xx]=0xFF000000u|(sr<<16)|(sg<<8)|sb;
            else {
              uint32_t dv=dp[xx];
              int dr=(dv>>16)&0xFF, dg=(dv>>8)&0xFF, db=dv&0xFF;
              dp[xx]=0xFF000000u|((int)(sr*ea+dr*eia+0.5)<<16)|((int)(sg*ea+dg*eia+0.5)<<8)|(int)(sb*ea+db*eia+0.5);
            }
          }
        }
      }
      if(r->blendmode==0 && alpha>=1.0 && src_all_opaque && rect_covers_target(r,cx0,cy0,cx1,cy1)){
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
     r->blendmode==0 && alpha>=1.0 &&
     ((blend & 0xFFFFFF) == 0xFFFFFF) && !spal && !slut && !sgrid && !alpha_test){
    gml_render_maybe_prepare_draw(r);
    if(draw_scaled_full_surface_normal(r,src,sw,sh,x0,y0,W,H,src_all_opaque)){
      free(copy);
      return;
    }
  }
  gml_render_maybe_prepare_draw(r);
  /* Bilinear magnification when the game asked for interpolation (texture_set_interpolation(true)):
   * GM/GL sample 4 texels at the output pixel centre. Only the upscale case takes this — downscale
   * keeps the box average, and non-interpolated pixel-art draws keep exact nearest. This is what makes
   * a full-screen compositor's soft "old TV" bloom (a 0.7-alpha stretched surface pass) render. */
  if(r->interp && !flipx && !flipy && W>sw && H>sh && r->blendmode==0 && !spal && !slut){
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
    int sy0=(int)floor(sy0d + (dpy*shd)/H), sy1=(int)floor(sy0d + ((dpy+1)*shd)/H);
    if(sy1<=sy0) sy1=sy0+1;
    if(sy0<0) sy0=0;
    if(sy1>sh) sy1=sh;
    for(int px=0; px<W; px++){ int tx_=x0+px; if(tx_<0||tx_>=r->fbw) continue;
      int dpx=flipx?(W-1-px):px;
      int sx0=(int)floor(sx0d + (dpx*swd)/W), sx1=(int)floor(sx0d + ((dpx+1)*swd)/W);
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
      if(spal) av=pal_map_px(spal,av);
      else if(slut) av=lut_map_px(r,slut,av);
      int sr=((av>>16)&0xFF)*bR/255, sg=((av>>8)&0xFF)*bG/255, sb=(av&0xFF)*bB/255;
      uint32_t *dp=&r->fb[(size_t)ty_*r->fbw+tx_];
      double ea=alpha*pa;
      if(r->blendmode==3){ uint32_t old=*dp; *dp=color_write_merge(r,old,blend_multiply_pixel(r,old,sr,sg,sb)); }
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
         fabs(fabs(width)-(double)r->fbw)<0.001 &&
         fabs(fabs(height)-(double)r->fbh)<0.001;
}

/* draw_surface_stretched[_ext]: blit a runtime surface into the current target, box-averaged. */
static void draw_surface_stretched_impl(GmlRender *r,int surf,double dx,double dy,
                                        double dw,double dh,uint32_t blend,double alpha,
                                        int allow_software3d){
  int explicit_target_raster=surface_draw_targets_screen_raster(r,surf,dw,dh);
  if(!explicit_target_raster){
    gml_render_gui_map_point(r,&dx,&dy);
    gml_render_gui_map_scale(r,&dw,&dh);
  }
  const struct GmlShaderPal *sdual=dual_active(r);
  const struct GmlShaderPal *shsv=hsv_scan_active(r);
  if(render_setting(r,"GML_LOG_SHADER") && r && r->active_shader>=0 && r->stretched_shader_log_count++<8){
    
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[shader] f%ld stretched active=%d embedded=%d hsv=%d surface=%d\n",
      r->frame,r->active_shader,r->crt_shader_enable,shsv!=NULL,surf);
  }
  int d3w=gml_surface_width(r,surf),d3h=gml_surface_height(r,surf);
  if(allow_software3d && !sdual && !shsv && d3w>0&&d3h>0&&
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
  if(shsv){ draw_surface_hsv_scan(r,shsv,surf,0,0,sw,sh,dx,dy,dw,dh,blend,alpha); return; }
  { const struct GmlShaderPal *sampled=sampled_crt_active(r);
    if(sampled){ draw_surface_sampled_crt(r,sampled,surf,0,0,sw,sh,dx,dy,dw,dh,blend,alpha); return; } }
  { const struct GmlShaderPal *scrt=crt_active(r);
    if(scrt){ draw_surface_crt(r,scrt,surf,0,0,sw,sh,dx,dy,dw,dh,blend,alpha); return; } }
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
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[surfdraw] target=%d surf=%d src=%dx%d nz=%d dst=(%.0f,%.0f %.0fx%.0f) blend=%06X alpha=%.2f bm=%d alphablend=%d\n",
              r?r->target_id:-999,surf,sw,sh,nz,dx,dy,dw,dh,blend&0xFFFFFF,alpha,r?r->blendmode:-1,r?r->alphablend:-1);
      if(nz>0) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[surfdraw]   first=(%d,%d) argb=%08X bbox=(%d,%d)-(%d,%d)\n",sx0,sy0,sv0,minx,miny,maxx,maxy);
    }
  }
  int prof=rprof_enabled();
  double t0=prof?rprof_now():0.0;
  draw_surface_region(r,surf,0,0,sw,sh,dx,dy,dw,dh,blend,alpha);
  if(prof) rprof_add("surface",r,NULL,(rprof_now()-t0)*1000.0,(unsigned long long)llround(fabs(dw*dh)));
}
void gml_draw_surface_stretched(GmlRender *r,int surf,double dx,double dy,
                                double dw,double dh,uint32_t blend,double alpha){
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
  gml_render_gui_map_point(r,&x,&y);
  gml_render_gui_map_scale(r,&xs,&ys);
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
  const struct GmlShaderPal *spal=pal_active(r),*slut=lut_active(r),*sgrid=grid_active(r);
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
    if(spal) sv=pal_map_px(spal,sv); else if(slut) sv=lut_map_px(r,slut,sv); else if(sgrid) sv=grid_map_px(r,sgrid,sv);
    int sr=((sv>>16)&255)*bR/255,sg=((sv>>8)&255)*bG/255,sb=(sv&255)*bB/255;
    uint32_t *dp=&r->fb[(size_t)py*r->fbw+px],old=*dp,out;
    double sa=((sv>>24)/255.0)*alpha;
    if(r->blendmode==3) out=blend_multiply_pixel(r,old,sr,sg,sb);
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
    gml_render_gui_map_point(r,&dx,&dy);
    gml_render_gui_map_scale(r,&xs,&ys);
  }
  const struct GmlShaderPal *sdual=dual_active(r);
  const struct GmlShaderPal *shsv=hsv_scan_active(r);
  if(!sdual && !shsv && gml_d3_draw_surface_part_2d(r,surf,sx,sy,sw,sh,dx,dy,xs,ys,blend,alpha)) return;
  if(r && !r->app_draw_enable && r->interp) r->composites_app=1;
  if(sdual){ draw_surface_dual_sample(r,sdual,surf,sx,sy,sw,sh,dx,dy,sw*xs,sh*ys,blend,alpha); return; }
  if(shsv){ draw_surface_hsv_scan(r,shsv,surf,sx,sy,sw,sh,dx,dy,sw*xs,sh*ys,blend,alpha); return; }
  { const struct GmlShaderPal *sampled=sampled_crt_active(r);
    if(sampled){ draw_surface_sampled_crt(r,sampled,surf,sx,sy,sw,sh,dx,dy,sw*xs,sh*ys,blend,alpha); return; } }
  { const struct GmlShaderPal *scrt=crt_active(r);
    if(scrt){ draw_surface_crt(r,scrt,surf,sx,sy,sw,sh,dx,dy,sw*xs,sh*ys,blend,alpha); return; } }
  int prof=rprof_enabled();
  double t0=prof?rprof_now():0.0;
  draw_surface_region(r,surf,sx,sy,sw,sh,dx,dy,sw*xs,sh*ys,blend,alpha);
  if(prof) rprof_add("surface",r,NULL,(rprof_now()-t0)*1000.0,(unsigned long long)llround(fabs(sw*xs*sh*ys)));
}
