/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared background source, transform, and room-order preparation. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_cpu_blit.h"
#include "anygm_host.h"

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
