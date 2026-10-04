/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared world and GUI coordinate transforms. */
#include "gml_render_internal.h"
#include <math.h>

int gml_render_view_surface_begin(GmlRender *r,int surface,double cx,double cy){
  if(!gml_surface_set_target(r,surface)) return 0;
  /* A view keeps its room camera, unlike an explicit surface_set_target call. */
  r->projection_cam_x=r->cam_x=cx;
  r->projection_cam_y=r->cam_y=cy;
  gml_d3_sync_render_camera(r);
  return 1;
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
  /* The earlier camera representation uses an integer vertical origin when the owned
   * application-surface projection is scaled. Later bytecode retains a fractional origin. */
  if(r->win && scale_y>1.0 &&
     anygm_policy_has_modern_layer_semantics(r->win) &&
     anygm_policy_uses_first_generation_studio(r->win)){
    r->projection_cam_y=floor(r->projection_cam_y);
    r->cam_y=floor(r->cam_y);
  }
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
