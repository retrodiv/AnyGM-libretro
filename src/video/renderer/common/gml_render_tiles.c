/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared tile source, room order, and draw preparation. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_sprites_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_render_cpu_blit.h"
#include "gml_render_cpu_backend.h"
#include "anygm_host.h"

#include <stdlib.h>

void gml_draw_tileset_tile(GmlRender *r,int background,uint32_t datum,int frame,
                           double x,double y,uint32_t color,double alpha){
  GmlRenderTilesetLayout layout; GmlRenderTileSource source;
  if(!gml_render_tileset_layout(r,background,0,0,&layout) ||
     !gml_render_tileset_source(r,&layout,datum,frame,&source)) return;
  gml_draw_background_tile(r,background,source.x,source.y,source.width,source.height,
                           x,y,1,1,(datum>>28)&1,(datum>>29)&1,(datum>>30)&1,color,alpha);
}

/* a room's tile layer: each tile blits a sub-rect (src_x,src_y,w,h) of a background/tileset
 * image to a room position, drawn back-to-front by tile depth. The room tile list is a
 * pointer-list (count + pointers), each record: x,y,bgdef,srcx,srcy,w,h,depth,id,sx,sy,color. */
typedef struct { int x,y,def,sx,sy,w,h,depth; } GmlTileRec;
static int cmp_tile_depth(const void *a, const void *b){
  const GmlTileRec *ta=a,*tb=b;
  return ta->depth>tb->depth? -1 : (ta->depth<tb->depth? 1 : 0);   /* high depth first (behind) */
}
/* blit a single tile: a sub-rect (sx,sy,w,h) of background/tileset `def` at room (x,y). */
void gml_draw_tile(GmlRender *r, int def, int sx, int sy, int w, int h, double x, double y){
  gml_render_draw_map_point(r,&x,&y);
  double draw_xscale=1.0,draw_yscale=1.0;
  gml_render_draw_map_scale(r,&draw_xscale,&draw_yscale);
  if(def<0||def>=r->n_bg) return;
  int ti=r->bg[def].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *bt=&r->tpag[ti];
  if(!r->interp && !gml_d3_is_active(r) &&
     bt->tx==0 && bt->ty==0 &&
     bt->bw==bt->sw && bt->bh==bt->sh &&
     gml_render_resident_atlas_crop(r,ti,sx,sy,w,h,
                                   x-r->cam_x,y-r->cam_y,
                                   draw_xscale,draw_yscale,
                                   GML_RENDER_SCENE_TILE)) return;
  if(gml_render_resident_filtered_atlas_part(r,ti,sx,sy,w,h,x,y,
                                            draw_xscale,draw_yscale,0xFFFFFFu,1.0,
                                            GML_RENDER_SCENE_TILE)) return;
  if(!gml_render_resident_application_cpu_barrier(r)) return;
  if(gml_d3_draw_background_part_2d(r,def,sx,sy,w,h,x,y,draw_xscale,draw_yscale,0xFFFFFF,1)) return;
  GmlTpag tt=*bt;
  tt.sx=bt->sx+sx; tt.sy=bt->sy+sy; tt.sw=w; tt.sh=h;
  /* Adjacent tilemap cells are independent GPU quads whose authored edges are contiguous. At a
   * fractional view scale, rounding every cell to one fixed width leaves a one-pixel crack each
   * time the accumulated phase crosses a pixel boundary. Preserve both projected edges for this
   * sub-rectangle; ordinary sprites retain their established extent rule. */
  tt.project_authored_edges=1;
  tt.interp_phase_cache[0]=tt.interp_phase_cache[1]=tt.interp_phase_cache[2]=NULL;
  /* A sub-rectangle borrows atlas pixels but not the page's geometry-dependent
   * caches. Detach the row spans and pixel caches before drawing this view. */
  tt.alpha_scanned=0; tt.ax0=tt.ay0=0; tt.ax1=tt.ay1=-1; tt.alpha_max=0;
  tt.black_scanned=0; tt.all_black=0;
  tt.alpha_row_min=tt.alpha_row_max=NULL;
  tt.alpha_qrow_min=tt.alpha_qrow_max=NULL; tt.alpha_qrow_built=NULL;
  tt.alpha_runs=NULL; tt.alpha_run_count=0; tt.alpha_runs_built=0;
  tt.alpha8_cache=NULL;
  tt.argb_cache=NULL;
  tt.solid_blur_alpha_cache=NULL; tt.solid_blur_alpha_shader=-1;
  tt.fast8_draw_cache=NULL; tt.fast8_draw_cache_valid=0; tt.fast8_draw_pending_count=0;
  tt.interp_draw_cache=NULL; tt.interp_draw_runs=NULL;
  tt.interp_draw_cache_bytes=0; tt.interp_draw_run_count=0;
  tt.interp_draw_cache_valid=0; tt.interp_draw_pending_count=0;

  GmlRenderCpuBackendDispatchScope tile_scope;
  gml_render_cpu_backend_dispatch_begin(r,&tile_scope,GML_RENDER_SCENE_TILE,0);
  gml_render_cpu_blit_background_phase(r,&tt, x - r->cam_x, y - r->cam_y,
                        draw_xscale,draw_yscale,0xFFFFFF,1);
  gml_render_cpu_backend_dispatch_end(r,&tile_scope);
}
void gml_draw_room_tiles(GmlRender *r, uint32_t tile_ptr){
  if(!tile_ptr) return;
  const uint8_t *d=r->win->data; uint32_t cnt=u32(d,tile_ptr);
  if(cnt==0 || cnt>100000) return;
  if(render_setting(r,"GML_LOG_BG")) anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[tile-room] count=%u\n",cnt);
  GmlTileRec *tiles=malloc((size_t)cnt*sizeof(GmlTileRec)); if(!tiles) return; int m=0;
  for(uint32_t i=0;i<cnt;i++){
    uint32_t p=u32(d,tile_ptr+4+i*4);
    GmlTileRec t;
    t.x=(int)u32(d,p);    t.y=(int)u32(d,p+4);  t.def=(int)u32(d,p+8);
    t.sx=(int)u32(d,p+12);t.sy=(int)u32(d,p+16);t.w=(int)u32(d,p+20);
    t.h=(int)u32(d,p+24); t.depth=(int)u32(d,p+28);
    tiles[m++]=t;
    if(render_setting(r,"GML_LOG_BG"))
      anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[tile] index=%u def=%d source=(%d,%d %dx%d) position=(%d,%d) depth=%d\n",
        i,t.def,t.sx,t.sy,t.w,t.h,t.x,t.y,t.depth);
  }
  qsort(tiles,m,sizeof(GmlTileRec),cmp_tile_depth);
  for(int i=0;i<m;i++){
    GmlTileRec *t=&tiles[i];
    if(t->def<0 || t->def>=r->n_bg) continue;
    int ti=r->bg[t->def].tpag; if(ti<0 || ti>=r->n_tpag) continue;
    GmlTpag *bt=&r->tpag[ti];
    if(!r->interp && !gml_d3_is_active(r) && bt->tx==0 && bt->ty==0 &&
       bt->bw==bt->sw && bt->bh==bt->sh &&
       gml_render_resident_atlas_crop(r,ti,t->sx,t->sy,t->w,t->h,
                                     t->x-r->cam_x,t->y-r->cam_y,1,1,
                                     GML_RENDER_SCENE_TILE)) continue;
    if(gml_render_resident_filtered_atlas_part(r,ti,t->sx,t->sy,t->w,t->h,
                                              t->x,t->y,1,1,0xFFFFFFu,1.0,
                                              GML_RENDER_SCENE_TILE)) continue;
    if(!gml_render_resident_application_cpu_barrier(r)) break;
    GmlTpag tt=*bt;                                  /* sub-rect of the tileset image */
    tt.sx=bt->sx + t->sx; tt.sy=bt->sy + t->sy; tt.sw=t->w; tt.sh=t->h;
    tt.interp_phase_cache[0]=tt.interp_phase_cache[1]=tt.interp_phase_cache[2]=NULL;
    /* A sub-rectangle borrows atlas pixels but not the page's geometry-dependent
     * caches. Detach the row spans and pixel caches before drawing this view. */
    tt.alpha_scanned=0; tt.ax0=tt.ay0=0; tt.ax1=tt.ay1=-1; tt.alpha_max=0;
    tt.black_scanned=0; tt.all_black=0;
    tt.alpha_row_min=tt.alpha_row_max=NULL;
    tt.alpha_qrow_min=tt.alpha_qrow_max=NULL; tt.alpha_qrow_built=NULL;
    tt.alpha_runs=NULL; tt.alpha_run_count=0; tt.alpha_runs_built=0;
    tt.alpha8_cache=NULL;
    tt.argb_cache=NULL;
    tt.solid_blur_alpha_cache=NULL; tt.solid_blur_alpha_shader=-1;
    tt.fast8_draw_cache=NULL; tt.fast8_draw_cache_valid=0; tt.fast8_draw_pending_count=0;
    tt.interp_draw_cache=NULL; tt.interp_draw_runs=NULL;
    tt.interp_draw_cache_bytes=0; tt.interp_draw_run_count=0;
    tt.interp_draw_cache_valid=0; tt.interp_draw_pending_count=0;

    GmlRenderCpuBackendDispatchScope tile_scope;
    gml_render_cpu_backend_dispatch_begin(r,&tile_scope,GML_RENDER_SCENE_TILE,0);
    gml_render_cpu_blit_background_phase(r,&tt, t->x - r->cam_x, t->y - r->cam_y, 1,1, 0xFFFFFF, 1);
    gml_render_cpu_backend_dispatch_end(r,&tile_scope);
  }
  free(tiles);
}
