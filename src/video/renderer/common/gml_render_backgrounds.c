/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared background source, transform, and room-order preparation. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_cpu_blit.h"
#include "anygm_host.h"

#include <math.h>
#include <stdlib.h>

void gml_draw_background(GmlRender *r, int bg, double x, double y){
  gml_render_draw_map_point(r,&x,&y);
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  if(gml_d3_draw_background_2d(r,bg,x,y,1,1,0xFFFFFF,r->alpha)) return;
  GmlTpag *t=&r->tpag[ti];
  gml_render_cpu_blit_background_phase(r,t, x - r->cam_x + t->tx, y - r->cam_y + t->ty, 1,1, 0xFFFFFF, r->alpha);
}

void gml_draw_background_stretched(GmlRender *r, int bg, double x, double y, double w, double h, uint32_t color, double alpha){
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  int bw=t->bw?t->bw:t->sw, bh=t->bh?t->bh:t->sh;
  if(bw<=0 || bh<=0) return;
  gml_draw_background_part_ext(r,bg,0,0,bw,bh,x,y,w/bw,h/bh,color,alpha);
}

void gml_draw_background_part_ext(GmlRender *r, int bg, double sx, double sy, double sw, double sh,
                                  double x, double y, double xs, double ys, uint32_t color, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  if(gml_d3_draw_background_part_2d(r,bg,sx,sy,sw,sh,x,y,xs,ys,color,alpha)) return;
  gml_render_cpu_blit_tpag_part_background(r,&r->tpag[ti],sx,sy,sw,sh,x-r->cam_x,y-r->cam_y,xs,ys,color,alpha);
}

/* draw_background[_tiled]_ext: as above + xscale/yscale + blend colour + alpha. */
void gml_draw_background_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                            uint32_t color, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  if(gml_d3_draw_background_2d(r,bg,x,y,xs,ys,color,alpha)) return;
  GmlTpag *t=&r->tpag[ti];
  gml_render_cpu_blit_background_phase(r,t, x - r->cam_x + t->tx*xs, y - r->cam_y + t->ty*ys, xs,ys, color, alpha);
}

/* draw a room's background layers (from the room Background pointer-list).
 * want_fg: 0 = backgrounds (behind instances), 1 = foregrounds (in front). */
void gml_draw_room_backgrounds(GmlRender *r, uint32_t bg_ptr, int want_fg){
  if(!bg_ptr) return;
  const uint8_t *d=r->win->data; uint32_t cnt=u32(d,bg_ptr);
  for(uint32_t i=0;i<cnt;i++){
    uint32_t p=u32(d,bg_ptr+4+i*4);
    int enabled=(int)u32(d,p), fg=(int)u32(d,p+4), bgdef=(int)u32(d,p+8);
    int x=(int)u32(d,p+12), y=(int)u32(d,p+16), tilex=(int)u32(d,p+20), tiley=(int)u32(d,p+24);
    if(!enabled || fg!=want_fg) continue;
    if(bgdef<0||bgdef>=r->n_bg) continue;
    int ti=r->bg[bgdef].tpag; if(ti<0||ti>=r->n_tpag) continue;
    GmlTpag *t=&r->tpag[ti]; int bw=t->sw, bh=t->sh; if(bw<=0||bh<=0) continue;
    if(render_setting(r,"GML_LOG_BG"))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[bg-room] layer=%u def=%d tpag=%d src=%d,%d %dx%d dst=%d,%d base=%dx%d atlas=%d pos=(%d,%d) tiled=(%d,%d) fg=%d\n",
        i,bgdef,ti,t->sx,t->sy,t->sw,t->sh,t->tx,t->ty,t->bw,t->bh,t->atlas,
        x,y,tilex,tiley,fg);
    int x0=x, y0=y;
    if(tilex){ x0 = x % bw; if(x0>0) x0-=bw; }
    if(tiley){ y0 = y % bh; if(y0>0) y0-=bh; }
    for(int yy=y0; yy < (tiley? r->fbh : y0+1); yy+=bh){
      for(int xx=x0; xx < (tilex? r->fbw : x0+1); xx+=bw){
        gml_render_cpu_blit_background_phase(r,t, xx, yy, 1,1, 0xFFFFFF, 1);
      }
      if(!tilex) {}
    }
  }
}

/* draw_background_tiled: repeat the background image along the layer's tiled axes, anchored at (x,y).
 * htiled/vtiled come from the room layer (background_htiled[]/vtiled[]): a layer that only tiles
 * horizontally (e.g. a ground strip) must draw a single row, not fill the screen vertically. */
static void do_bg_tiled_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                            uint32_t color, double alpha, int htiled, int vtiled){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(bg<0||bg>=r->n_bg) return;
  { const char*sb=render_setting(r,"GML_SKIP_BG"); if(sb&&atoi(sb)==bg) return; }
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  double logical_w=(t->bw?t->bw:t->sw)*xs;
  double logical_h=(t->bh?t->bh:t->sh)*ys;
  if(logical_w<=0 || logical_h<=0) return;
  if(render_setting(r,"GML_LOG_BG"))
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[bg-tiled] def=%d tpag=%d src=%d,%d %dx%d dst=%d,%d base=%dx%d atlas=%d pos=(%.3f,%.3f) scale=(%.3f,%.3f) tiled=(%d,%d) alpha=%.3f\n",
      bg,ti,t->sx,t->sy,t->sw,t->sh,t->tx,t->ty,t->bw,t->bh,t->atlas,
      x,y,xs,ys,htiled,vtiled,alpha);
  if(r->classic){
    /* Keep the anchor where the content put it and let the blit apply its nearest-pixel snapping.
     * Truncating here would make the layer follow a different rule from other draws: a parallax
     * position derived from a view that oscillates by a fraction of a pixel can sit on an integer
     * boundary and flip a whole pixel while the world remains still. The tiling period is
     * unchanged, so every repeat keeps one phase. */
    double anchor_x=x-r->cam_x, anchor_y=y-r->cam_y;
    double x0,xend,y0,yend;
    if(htiled){ x0=fmod(anchor_x,logical_w); if(x0>0) x0-=logical_w; xend=r->fbw; }
    else { x0=anchor_x; xend=anchor_x+1; }
    if(vtiled){ y0=fmod(anchor_y,logical_h); if(y0>0) y0-=logical_h; yend=r->fbh; }
    else { y0=anchor_y; yend=anchor_y+1; }
    if(render_setting(r,"GML_LOG_BG"))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,
        "[bg-anchor] def=%d cam=(%.3f,%.3f) anchor=(%.3f,%.3f) start=(%.3f,%.3f)\n",
        bg,r->cam_x,r->cam_y,anchor_x,anchor_y,x0,y0);
    if(gml_d3_is_active(r)){
      for(double yy=y0; yy<yend; yy+=logical_h) for(double xx=x0; xx<xend; xx+=logical_w)
        gml_d3_draw_background_2d(r,bg,xx+r->cam_x,yy+r->cam_y,xs,ys,color,alpha);
      return;
    }
    for(double yy=y0; yy<yend; yy+=logical_h)
      for(double xx=x0; xx<xend; xx+=logical_w){
        if(vtiled && logical_h<r->fbh)
          gml_render_cpu_blit_with_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,color,alpha,2,0);
        else
          gml_render_cpu_blit_background_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,color,alpha);
    }
    return;
  }
  /* Repeat the complete logical background cell. Texture pages omit transparent margins, but
   * those margins remain part of both the tiling period and its authored anchor. */
  double anchor_x=floor(x-r->cam_x),anchor_y=floor(y-r->cam_y);
  double x0,xend,y0,yend;
  if(htiled){ x0=fmod(anchor_x,logical_w); if(x0>0) x0-=logical_w; xend=r->fbw; }
  else { x0=anchor_x; xend=anchor_x+1; }
  if(vtiled){ y0=fmod(anchor_y,logical_h); if(y0>0) y0-=logical_h; yend=r->fbh; }
  else { y0=anchor_y; yend=anchor_y+1; }
  if(gml_d3_is_active(r)){
    for(double yy=y0; yy<yend; yy+=logical_h)
      for(double xx=x0; xx<xend; xx+=logical_w)
        gml_d3_draw_background_2d(r,bg,xx+r->cam_x,yy+r->cam_y,xs,ys,color,alpha);
    return;
  }
  for(double yy=y0; yy<yend; yy+=logical_h)
    for(double xx=x0; xx<xend; xx+=logical_w)
      gml_render_cpu_blit_background_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,color,alpha);
}
/* Paint a background layer immediately in the order requested by GML. */
static void bg_emit(GmlRender *r, int bgdef, double x, double y, double xs, double ys,
                    uint32_t color, double alpha, int htiled, int vtiled){
  do_bg_tiled_ext(r,bgdef,x,y,xs,ys,color,alpha,htiled,vtiled);
}
void gml_draw_background_tiled(GmlRender *r, int bg, double x, double y, int htiled, int vtiled){
  bg_emit(r,bg,x,y,1,1,0xFFFFFF,r?r->alpha:1,htiled,vtiled);
}
void gml_draw_background_tiled_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                                  uint32_t color, double alpha, int htiled, int vtiled){
  bg_emit(r,bg,x,y,xs,ys,color,alpha,htiled,vtiled);
}
