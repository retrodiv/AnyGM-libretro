/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared background source, transform, and room-order preparation. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_sprites_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_cpu_blit.h"
#include "anygm_host.h"

#include <math.h>
#include <limits.h>
#include <stdlib.h>

int gml_render_tpag_part_view(const GmlTpag *t,
                          double sx,double sy,double sw,double sh,
                          GmlTpag *view,int *source_x,int *source_y,
                          double *overlap_x0,double *overlap_y0,
                          double *overlap_x1,double *overlap_y1){
  if(!t || !view || sw<=0 || sh<=0) return 0;
  double ix0d=fmax(sx,(double)t->tx), iy0d=fmax(sy,(double)t->ty);
  double ix1d=fmin(sx+sw,(double)(t->tx+t->sw)), iy1d=fmin(sy+sh,(double)(t->ty+t->sh));
  int ix0=(int)floor(ix0d), iy0=(int)floor(iy0d);
  int ix1=(int)ceil(ix1d), iy1=(int)ceil(iy1d);
  if(ix1<=ix0 || iy1<=iy0) return 0;
  /* The continuous overlap, before the outward snap, so callers can keep the selected extent. */
  if(overlap_x0) *overlap_x0=ix0d;
  if(overlap_y0) *overlap_y0=iy0d;
  if(overlap_x1) *overlap_x1=ix1d;
  if(overlap_y1) *overlap_y1=iy1d;
  *view=*t;
  view->sx=t->sx + (ix0 - t->tx);
  view->sy=t->sy + (iy0 - t->ty);
  view->sw=ix1-ix0;
  view->sh=iy1-iy0;
  view->tx=0; view->ty=0;
  /* the copy dies on return, so any alpha scan on it is thrown-away work redone EVERY call
   * (a big tiled background part re-scanned its full sub-rect per draw); and the pointer
   * caches copied from the parent describe the PARENT's geometry, not this sub-rect.
   * Mark it pre-scanned with conservative full-rect bounds and detach the caches. */
  view->alpha_scanned=1;
  view->ax0=0; view->ay0=0; view->ax1=view->sw-1; view->ay1=view->sh-1;
  view->alpha_max=t->alpha_scanned ? t->alpha_max : 255;
  view->alpha_partial=t->alpha_scanned ? t->alpha_partial : 1;
  view->black_scanned=0; view->all_black=0;
  view->alpha_row_min=view->alpha_row_max=NULL;
  view->alpha_qrow_min=view->alpha_qrow_max=NULL; view->alpha_qrow_built=NULL;
  view->alpha_runs=NULL; view->alpha_run_count=0; view->alpha_runs_built=0;
  view->alpha8_cache=NULL;
  view->argb_cache=NULL;
  view->solid_blur_alpha_cache=NULL; view->solid_blur_alpha_shader=-1;
  view->interp_phase_cache[0]=view->interp_phase_cache[1]=view->interp_phase_cache[2]=NULL;
  view->fast8_draw_cache=NULL; view->fast8_draw_cache_valid=0; view->fast8_draw_pending_count=0;
  view->interp_draw_cache=NULL; view->interp_draw_runs=NULL;
  view->interp_draw_cache_bytes=0; view->interp_draw_run_count=0;
  view->interp_draw_cache_valid=0; view->interp_draw_pending_count=0;
  if(source_x) *source_x=ix0;
  if(source_y) *source_y=iy0;
  return 1;
}

static int resident_integer_background_part(GmlRender *r,int page_index,
                                            double sx,double sy,double sw,double sh,
                                            double x,double y,double xs,double ys,
                                            uint32_t color,double alpha,
                                            GmlRenderSceneFamily family){
  if(!r || !r->resident_application_pass || r->interp || gml_d3_is_active(r) ||
     color!=0xFFFFFFu || alpha!=1.0 ||
     !isfinite(sx) || !isfinite(sy) || !isfinite(sw) || !isfinite(sh) ||
     sx<0.0 || sy<0.0 || sx>INT_MAX || sy>INT_MAX ||
     sw<=0.0 || sh<=0.0 || sw>INT_MAX || sh>INT_MAX ||
     sx!=(double)(int)sx || sy!=(double)(int)sy ||
     sw!=(double)(int)sw || sh!=(double)(int)sh) return 0;
  GmlTpag *page=&r->tpag[page_index];
  if(page->tx!=0 || page->ty!=0 || page->bw!=page->sw || page->bh!=page->sh)
    return 0;
  /* Do not erase auxiliary CPU phase-plane work or special draw-state effects.
   * The ordinary resident crop performs its fuller validation below. */
  if(r->classic || r->classic_phase_y || r->gui_pass_active ||
     r->active_shader>=0 || r->alpha_test_enable ||
     r->pending_underlay || r->pending_presentation ||
     r->fb!=r->app_surface_owned || r->fbw!=r->app_w || r->fbh!=r->app_h)
    return 0;
  for(int index=0;index<3;index++)
    if(r->classic_interp_phase[index] || r->app_interp_phase[index]) return 0;
  /* Integer source crops cannot cover pixels beyond their projected full rectangle.
   * At integral unit scale/position the CPU writer's rounded edge is exact, including
   * mirrored tiles; other projections retain a one-pixel rounding guard. An
   * entirely offscreen crop must not drain later visible resident quads. */
  double left=x-r->cam_x, top=y-r->cam_y;
  double right=left+sw*xs, bottom=top+sh*ys;
  if(isfinite(left) && isfinite(top) && isfinite(right) && isfinite(bottom)){
    double x_guard=(floor(left)==left && (xs==1.0 || xs==-1.0))?0.0:1.0;
    double y_guard=(floor(top)==top && (ys==1.0 || ys==-1.0))?0.0:1.0;
    if(fmax(left,right)<=-x_guard || fmin(left,right)>=(double)r->fbw+x_guard ||
       fmax(top,bottom)<=-y_guard || fmin(top,bottom)>=(double)r->fbh+y_guard)
      return 1;
  }
  return gml_render_resident_atlas_crop(r,page_index,(int)sx,(int)sy,(int)sw,(int)sh,
                                       x-r->cam_x,y-r->cam_y,xs,ys,family);
}

int gml_render_resident_filtered_atlas_part(GmlRender *r,int page_index,
                                             double sx,double sy,double sw,double sh,
                                             double x,double y,double xs,double ys,
                                             uint32_t color,double alpha,
                                             GmlRenderSceneFamily family){
  GmlTpag *page,view;
  int source_x=0,source_y=0;
  int trimmed;
  double overlap_x0=0.0,overlap_y0=0.0,overlap_x1=0.0,overlap_y1=0.0;
  double anchor_x,anchor_y,cell_xs,cell_ys;
  if(!r || !r->resident_application_pass || !r->interp ||
     page_index<0 || page_index>=r->n_tpag || gml_d3_is_active(r) ||
     !isfinite(sx) || !isfinite(sy) || !isfinite(sw) || !isfinite(sh) ||
     !isfinite(x) || !isfinite(y) || !isfinite(xs) || !isfinite(ys) ||
     !isfinite(alpha) || sx<0.0 || sy<0.0 || sw<=0.0 || sh<=0.0 ||
     sx>INT_MAX || sy>INT_MAX || sw>INT_MAX || sh>INT_MAX) return 0;
  page=&r->tpag[page_index];
  /* The CPU part view already clips logical trim. Admit a trimmed atlas page
   * only for an integral background selection wholly inside its packed area;
   * fractional/edge-crossing trim and tile-source trim remain separate rules. */
  trimmed=page->tx || page->ty || page->bw!=page->sw || page->bh!=page->sh;
  if(page->tx<0 || page->ty<0 || page->bw<=0 || page->bh<=0 ||
     (int64_t)page->tx+page->sw>page->bw ||
     (int64_t)page->ty+page->sh>page->bh ||
     (trimmed && family!=GML_RENDER_SCENE_BACKGROUND) ||
     sx<(double)page->tx || sy<(double)page->ty ||
     sx+sw>(double)page->tx+page->sw || sy+sh>(double)page->ty+page->sh ||
     !gml_render_tpag_part_view(page,sx,sy,sw,sh,&view,&source_x,&source_y,
                                &overlap_x0,&overlap_y0,&overlap_x1,&overlap_y1)) return 0;
  if(trimmed && (sx!=floor(sx) || sy!=floor(sy) ||
                 sw!=floor(sw) || sh!=floor(sh))) return 0;
  /* Reuse the CPU part writer's one-cell extent and anchor normalization.
   * Multi-cell selections retain outward-snapped source geometry. Tile source
   * fractions have a distinct authored-edge policy and remain on CPU. */
  if((sx!=floor(sx) || sy!=floor(sy) || sw!=floor(sw) || sh!=floor(sh)) &&
     family!=GML_RENDER_SCENE_BACKGROUND) return 0;
  anchor_x=view.sw==1?overlap_x0:(double)source_x;
  anchor_y=view.sh==1?overlap_y0:(double)source_y;
  cell_xs=view.sw==1?(overlap_x1-overlap_x0)*xs:xs;
  cell_ys=view.sh==1?(overlap_y1-overlap_y0)*ys:ys;
  if(cell_xs==0.0 || cell_ys==0.0) return 0;
  /* Keep bw/bh from the CPU's part view: filtered taps at its right/bottom
   * may reach physical atlas neighbours beyond the selected visible crop. */
  view.tw=view.sw;
  view.th=view.sh;
  return gml_render_resident_filtered_atlas_page(
    r,page_index,x+(anchor_x-sx)*xs-r->cam_x,
    y+(anchor_y-sy)*ys-r->cam_y,cell_xs,cell_ys,
    family,color,alpha,&view);
}

void gml_draw_background(GmlRender *r, int bg, double x, double y){
  gml_render_draw_map_point(r,&x,&y);
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  if(r->alpha==1.0 && !r->interp && !gml_d3_is_active(r) &&
     gml_render_resident_atlas_page(r,ti,x-r->cam_x+t->tx,y-r->cam_y+t->ty,1,1,
                                    GML_RENDER_SCENE_BACKGROUND)) return;
  if(r->interp && !gml_d3_is_active(r) &&
     gml_render_resident_filtered_atlas_page(r,ti,x-r->cam_x+t->tx,
                                              y-r->cam_y+t->ty,1,1,
                                              GML_RENDER_SCENE_BACKGROUND,0xFFFFFFu,r->alpha,t)) return;
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  if(gml_d3_draw_background_2d(r,bg,x,y,1,1,0xFFFFFF,r->alpha)) return;
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
  if(resident_integer_background_part(r,ti,sx,sy,sw,sh,x,y,xs,ys,color,alpha,
                                      GML_RENDER_SCENE_BACKGROUND)) return;
  if(gml_render_resident_filtered_atlas_part(r,ti,sx,sy,sw,sh,x,y,xs,ys,color,alpha,
                                      GML_RENDER_SCENE_BACKGROUND)) return;
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  if(gml_d3_draw_background_part_2d(r,bg,sx,sy,sw,sh,x,y,xs,ys,color,alpha)) return;
  gml_render_cpu_blit_tpag_part_background(r,&r->tpag[ti],sx,sy,sw,sh,x-r->cam_x,y-r->cam_y,xs,ys,color,alpha);
}

void gml_draw_background_tile(GmlRender *r,int bg,
                              double sx,double sy,double sw,double sh,
                              double x,double y,double xs,double ys,
                              int mirror,int flip,int rotate,
                              uint32_t color,double alpha){
  if(!rotate){
    if(mirror){ x+=sw*xs; xs=-xs; }
    if(flip){ y+=sh*ys; ys=-ys; }
    gml_render_draw_map_point(r,&x,&y);
    gml_render_draw_map_scale(r,&xs,&ys);
    if(!r || bg<0 || bg>=r->n_bg || sw<=0 || sh<=0 || xs==0 || ys==0) return;
    int ti=r->bg[bg].tpag; if(ti<0 || ti>=r->n_tpag) return;
    if(resident_integer_background_part(r,ti,sx,sy,sw,sh,x,y,xs,ys,color,alpha,
                                        GML_RENDER_SCENE_TILE)) return;
    if(gml_render_resident_filtered_atlas_part(r,ti,sx,sy,sw,sh,x,y,xs,ys,color,alpha,
                                        GML_RENDER_SCENE_TILE)) return;
    if(!gml_render_resident_application_cpu_barrier(r)) return;
    if(gml_d3_draw_background_part_2d(r,bg,sx,sy,sw,sh,x,y,xs,ys,color,alpha)) return;
    gml_render_cpu_blit_tpag_part_tile(r,&r->tpag[ti],sx,sy,sw,sh,
                        x-r->cam_x,y-r->cam_y,xs,ys,color,alpha);
    return;
  }
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(!r || bg<0 || bg>=r->n_bg || sw<=0 || sh<=0 || xs==0 || ys==0) return;
  int ti=r->bg[bg].tpag; if(ti<0 || ti>=r->n_tpag) return;
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  GmlTpag *page=&r->tpag[ti];
  if(gml_render_cpu_try_exact_rotated_background_tile(
       r,page,sx,sy,sw,sh,x,y,xs,ys,mirror,flip,color,alpha)) return;
  GmlTpag view;
  int clipped_source_x=0,clipped_source_y=0;
  if(!gml_render_tpag_part_view(page,sx,sy,sw,sh,
                     &view,&clipped_source_x,&clipped_source_y,
                     NULL,NULL,NULL,NULL)) return;
  view.tx=(int)lround(clipped_source_x-sx);
  view.ty=(int)lround(clipped_source_y-sy);
  view.bw=(int)ceil(sw);
  view.bh=(int)ceil(sh);

  /* Tile transforms permute texture coordinates inside the authored destination rectangle.
   * Compensate the source axes before rotation, including nonuniform target scaling. */
  double tile_xscale=(sh/sw)*ys;
  double tile_yscale=(sw/sh)*xs;
  if(mirror) tile_xscale=-tile_xscale;
  if(flip) tile_yscale=-tile_yscale;
  double cosine=0.0,sine=0.0;
  const double degrees=-90.0;
  render_rotation_sincos(degrees,&cosine,&sine);
  double min_x=1e30,min_y=1e30;
  const double corner_x[4]={0.0,sw,sw,0.0};
  const double corner_y[4]={0.0,0.0,sh,sh};
  for(int i=0;i<4;i++){
    double local_x=corner_x[i]*tile_xscale;
    double local_y=corner_y[i]*tile_yscale;
    double transformed_x=local_x*cosine+local_y*sine;
    double transformed_y=-local_x*sine+local_y*cosine;
    if(transformed_x<min_x) min_x=transformed_x;
    if(transformed_y<min_y) min_y=transformed_y;
  }
  double draw_x=x-min_x,draw_y=y-min_y;
  /* blit_rotated applies the shared half-open cardinal anchor. The tile's public position is the
   * transformed bounding-box corner, so compensate before entering that origin-based kernel. */
  if(tile_xscale*cosine < -1e-12 || tile_yscale*sine < -1e-12) draw_x+=1.0;
  if(-tile_xscale*sine < -1e-12 || tile_yscale*cosine < -1e-12) draw_y+=1.0;
  gml_render_cpu_blit_rotated_background_tile(r,&view,draw_x,draw_y,
                                              tile_xscale,tile_yscale,degrees,color,alpha);
}

/* draw_background[_tiled]_ext: as above + xscale/yscale + blend colour + alpha. */
void gml_draw_background_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                            uint32_t color, double alpha){
  gml_render_draw_map_point(r,&x,&y);
  gml_render_draw_map_scale(r,&xs,&ys);
  if(bg<0||bg>=r->n_bg) return;
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  if((color&0xFFFFFFu)==0xFFFFFFu && alpha==1.0 && !r->interp && !gml_d3_is_active(r) &&
     gml_render_resident_atlas_page(r,ti,x-r->cam_x+t->tx*xs,
                                      y-r->cam_y+t->ty*ys,xs,ys,
                                      GML_RENDER_SCENE_BACKGROUND)) return;
  if(r->interp && !gml_d3_is_active(r) &&
     gml_render_resident_filtered_atlas_page(r,ti,x-r->cam_x+t->tx*xs,
                                              y-r->cam_y+t->ty*ys,xs,ys,
                                              GML_RENDER_SCENE_BACKGROUND,color,alpha,t)) return;
  /* A complete source axis wholly outside the target writes no pixels. Keep a preceding resident
   * pass when an otherwise unsupported mirror is safely beyond either target axis. */
  if(r->resident_application_pass && !gml_d3_is_active(r) &&
     isfinite(x) && isfinite(y) && isfinite(xs) && isfinite(ys)){
    double left=x-r->cam_x,right=left+(double)t->sw*xs;
    double top=y-r->cam_y,bottom=top+(double)t->sh*ys;
    if((!t->tx && t->bw==t->sw && t->tw==t->sw &&
        (fmax(left,right)<=-1.0 || fmin(left,right)>=(double)r->fbw+1.0)) ||
       (!t->ty && t->bh==t->sh && t->th==t->sh &&
        (fmax(top,bottom)<=-1.0 || fmin(top,bottom)>=(double)r->fbh+1.0))) return;
  }
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  if(gml_d3_draw_background_2d(r,bg,x,y,xs,ys,color,alpha)) return;
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
        if(!gml_d3_is_active(r) &&
           gml_render_resident_atlas_page(r,ti,xx+t->tx,yy+t->ty,1,1,
                                          GML_RENDER_SCENE_BACKGROUND)) continue;
        if(!gml_render_resident_application_cpu_barrier(r)) return;
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
    if(!gml_render_resident_application_cpu_barrier(r)) return;
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
    if(!gml_render_resident_application_cpu_barrier(r)) return;
    for(double yy=y0; yy<yend; yy+=logical_h)
      for(double xx=x0; xx<xend; xx+=logical_w)
        gml_d3_draw_background_2d(r,bg,xx+r->cam_x,yy+r->cam_y,xs,ys,color,alpha);
    return;
  }
  for(double yy=y0; yy<yend; yy+=logical_h)
    for(double xx=x0; xx<xend; xx+=logical_w){
      /* The CPU tint uses RGB only; a GML white value may carry an unused high byte. */
      if((color&0xFFFFFFu)==0xFFFFFFu && alpha==1.0 && !r->interp &&
         gml_render_resident_atlas_page(r,ti,xx+t->tx*xs,yy+t->ty*ys,xs,ys,
                                        GML_RENDER_SCENE_BACKGROUND))
        continue;
      if((color&0xFFFFFFu)==0xFFFFFFu && alpha==1.0 && r->interp &&
         gml_render_resident_filtered_atlas_page(r,ti,xx+t->tx*xs,yy+t->ty*ys,xs,ys,
                                                 GML_RENDER_SCENE_BACKGROUND,0xFFFFFFu,1.0,NULL))
        continue;
      if(!gml_render_resident_application_cpu_barrier(r)) return;
      gml_render_cpu_blit_background_phase(r,t,xx+t->tx*xs,yy+t->ty*ys,xs,ys,color,alpha);
    }
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
