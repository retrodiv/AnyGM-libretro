/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared world and GUI coordinate transforms. */
#include "gml_render_internal.h"
#include "gml_render_surfaces_internal.h"
#include <math.h>
#include <limits.h>

int gml_render_view_surface_begin(GmlRender *r,int surface,double cx,double cy){
  if(!gml_surface_set_target(r,surface)) return 0;
  /* A view keeps its room camera, unlike an explicit surface_set_target call. */
  r->projection_cam_x=r->cam_x=cx;
  r->projection_cam_y=r->cam_y=cy;
  gml_d3_sync_render_camera(r);
  return 1;
}

void gml_render_world_set_view_rotation(GmlRender *r,double angle,double width,double height){
  if(!r) return;
  r->view_rotation=0.0;r->view_cosine=1.0;r->view_sine=0.0;
  if(!isfinite(angle) || !isfinite(width) || !isfinite(height) || width<=0 || height<=0)
    return;
  angle=remainder(angle,360.0);
  double radians=angle*(M_PI/180.0);
  r->view_rotation=angle;
  r->view_cosine=cos(radians);r->view_sine=sin(radians);
  r->view_centre_x=width*.5*(r->world_transform_active?r->world_scale_x:1.0);
  r->view_centre_y=height*.5*(r->world_transform_active?r->world_scale_y:1.0);
  r->view_target_id=r->target_id;r->view_target_depth=r->target_sp;
}
int gml_render_view_rotation_active(const GmlRender *r){
  return r && r->view_rotation!=0.0 && !r->gui_pass_active &&
    r->target_id==r->view_target_id && r->target_sp==r->view_target_depth;
}
void gml_render_view_rotate_point(const GmlRender *r,double *x,double *y){
  if(!x || !y || !gml_render_view_rotation_active(r)) return;
  double sx=r->world_transform_active?r->world_scale_x:1.0;
  double sy=r->world_transform_active?r->world_scale_y:1.0;
  double dx=(*x-r->view_centre_x)/sx,dy=(*y-r->view_centre_y)/sy;
  *x=r->view_centre_x+(r->view_cosine*dx-r->view_sine*dy)*sx;
  *y=r->view_centre_y+(r->view_sine*dx+r->view_cosine*dy)*sy;
}

int gml_render_view_visible_bounds(const GmlRender *r,double *x,double *y,
                                    double *width,double *height){
  if(!gml_render_view_rotation_active(r) || r->fbw<=0 || r->fbh<=0) return 0;
  double sx=r->world_transform_active?r->world_scale_x:1.0;
  double sy=r->world_transform_active?r->world_scale_y:1.0;
  if(!isfinite(sx) || !isfinite(sy) || sx<=0.0 || sy<=0.0) return 0;
  double min_x=INFINITY,min_y=INFINITY,max_x=-INFINITY,max_y=-INFINITY;
  for(unsigned corner=0;corner<4u;corner++){
    double px=(corner&1u)?r->fbw:0.0,py=(corner&2u)?r->fbh:0.0;
    double dx=(px-r->view_centre_x)/sx,dy=(py-r->view_centre_y)/sy;
    px=r->view_centre_x+(r->view_cosine*dx+r->view_sine*dy)*sx;
    py=r->view_centre_y+(-r->view_sine*dx+r->view_cosine*dy)*sy;
    if(!isfinite(px) || !isfinite(py)) return 0;
    min_x=fmin(min_x,px);min_y=fmin(min_y,py);
    max_x=fmax(max_x,px);max_y=fmax(max_y,py);
  }
  if(x) *x=min_x;
  if(y) *y=min_y;
  if(width) *width=max_x-min_x;
  if(height) *height=max_y-min_y;
  return 1;
}

int gml_render_view_tile_grid(const GmlRender *r,double x,double y,
                             double width,double height,GmlRenderViewTileGrid *grid){
  if(!grid || !gml_render_view_rotation_active(r) || r->fbw<=0 || r->fbh<=0 ||
     !isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) ||
     width<=0.0 || height<=0.0) return 0;
  double min_x,min_y,visible_width,visible_height;
  if(!gml_render_view_visible_bounds(r,&min_x,&min_y,&visible_width,&visible_height)) return 0;
  double max_x=min_x+visible_width,max_y=min_y+visible_height;
  /* Reduce distant phases before indexing. Integer counters cannot stall when
   * a floating-point increment is smaller than the original supplied anchor. */
  double phase_x=fmod(x,width),phase_y=fmod(y,height);
  double first_x=phase_x+floor((min_x-phase_x)/width)*width;
  double first_y=phase_y+floor((min_y-phase_y)/height)*height;
  double columns=ceil((max_x-first_x)/width)+1.0;
  double rows=ceil((max_y-first_y)/height)+1.0;
  if(!isfinite(first_x) || !isfinite(first_y) || !isfinite(columns) || !isfinite(rows) ||
     columns<1.0 || rows<1.0 || columns>INT_MAX || rows>INT_MAX) return 0;
  /* The trailing anchors also cover negative scales, whose cells extend left/up. */
  *grid=(GmlRenderViewTileGrid){first_x,first_y,width,height,(int64_t)columns,(int64_t)rows};
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
GmlRenderShaderGeometry gml_render_shader_geometry(const GmlRender *r,double x,double y,
                                                   double width,double height,int mapped){
  GmlRenderShaderGeometry geometry={x,y,width,height,0u};
  if(mapped && render_gui_transform_active_local(r)){
    geometry.origin_x=(x-r->gui_maximise_xoffset)/r->gui_scale_x;
    geometry.origin_y=(y-r->gui_maximise_yoffset)/r->gui_scale_y;
    geometry.extent_x=width/r->gui_scale_x;
    geometry.extent_y=height/r->gui_scale_y;
  } else if(mapped && render_world_transform_active_local(r)){
    geometry.origin_x=x/r->world_scale_x;
    geometry.origin_y=y/r->world_scale_y;
    geometry.extent_x=width/r->world_scale_x;
    geometry.extent_y=height/r->world_scale_y;
  }
  geometry.valid=isfinite(geometry.origin_x) && isfinite(geometry.origin_y) &&
    isfinite(geometry.extent_x) && isfinite(geometry.extent_y) &&
    geometry.extent_x!=0.0 && geometry.extent_y!=0.0;
  return geometry;
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

int gml_render_resident_view_atlas_quad(GmlRender *r,const GmlTpag *page,
  double sx,double sy,double sw,double sh,const double x[4],const double y[4],
  uint32_t colour,double alpha,GmlRenderSceneFamily family){
  const GmlRenderDeviceServices *device=gml_render_device(r);
  GmlRenderResourceSource source={0};
  if(!r || !page ||
     (family!=GML_RENDER_SCENE_BACKGROUND && family!=GML_RENDER_SCENE_TILE && family!=GML_RENDER_SCENE_TEXT) ||
     !r->win || !device || !r->resident_application_pass ||
     !gml_render_scene_target_matches(r) || r->pending_presentation ||
     r->classic_phase_scene_target || r->fog_flat || r->active_shader>=0 ||
     r->alpha_test_enable || !r->alphablend || r->blendmode ||
     r->blend_equation!=1 || r->blend_equation_alpha!=1 || r->color_write_mask>15u ||
     r->rotated_batch_count || r->rotated_batch_building || alpha<=0.0 ||
     page->atlas<0 || page->atlas>=r->n_atlas || page->sx<0 || page->sy<0 ||
     !gml_render_atlas_scene_source_view(r,page->atlas,&source) ||
     (!source.borrowed_immutable &&
      !(family==GML_RENDER_SCENE_TEXT && r->atlas[page->atlas].runtime_font_page &&
        !source.resident_resource && device->context && device->observe_on_demand &&
        device->observe_on_demand(device->context))) ||
     source.width>4096u || source.height>4096u ||
     (uint64_t)page->sx+page->sw>source.width ||
     (uint64_t)page->sy+page->sh>source.height) return 0;
  if(r->pending_underlay && !gml_render_compose_resident_underlay(r)) return 0;
  gml_render_flush_pending_fill(r);
  GmlRenderResource *target=gml_render_scene_target_resource_ensure(r);
  if(!target || r->pending_fill) return 0;
  GmlRenderImageRef image=gml_render_application_scene_next_image_ref(r,target);
  if(image.content_version==UINT64_MAX) return 0;
  GmlRenderDrawOp op={.opcode=GML_RENDER_OP_TEXTURED_QUAD,
    .scene_family=family,.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT,
    .source_count=1u,.sources=&source.image};
  op.target=(GmlRenderTargetRef){image,1u,(uint32_t)r->fbw,(uint32_t)r->fbh,
    GML_RENDER_PIXEL_ARGB8888,{0,0,(uint32_t)r->fbw,(uint32_t)r->fbh}};
  op.target.image.content_version++;
  GmlRenderQuad *q=&op.payload.quad;
  q->shape=GML_RENDER_QUAD_GENERAL;q->filtered_draw_alpha=alpha;
  q->sampling=(GmlRenderSamplingPolicy){r->interp && !(family==GML_RENDER_SCENE_TILE && r->classic)
      ?GML_RENDER_FILTER_HARDWARE_BILINEAR:GML_RENDER_FILTER_NEAREST,
    GML_RENDER_EDGE_CLAMP_SUBIMAGE,{page->sx,page->sy,(uint32_t)page->sw,(uint32_t)page->sh},
    family==GML_RENDER_SCENE_TILE?GML_RENDER_QUANTISE_VIEW_TILE:
      family==GML_RENDER_SCENE_TEXT?GML_RENDER_QUANTISE_VIEW_TEXT:GML_RENDER_QUANTISE_VIEW_BACKGROUND,0u};
  uint32_t tint=((uint32_t)lround(alpha*255.0)<<24)|
    ((colour&255u)<<16)|(colour&0xFF00u)|((colour>>16)&255u);
  for(unsigned i=0;i<4u;i++){
    q->x[i]=x[i];q->y[i]=y[i];q->corner_colour[i]=tint;
    q->u[i]=(page->sx+sx-page->tx+((i==1u || i==2u)?sw:0.0))/source.width;
    q->v[i]=(page->sy+sy-page->ty+(i>=2u?sh:0.0))/source.height;
  }
  GmlRenderDrawStateSnapshot state={.colour=0xFFFFFFFFu,.alpha=255u,
    .write_mask=r->color_write_mask,.sampler_count=1u,
    .rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA,
    .rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA,
    .alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA,
    .alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA,
    .rgb_equation=GML_RENDER_BLEND_ADD,.alpha_equation=GML_RENDER_BLEND_ADD};
  if(!gml_render_target_preserves_alpha(r)){
    state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  }
  state.samplers[0]=q->sampling;
  return (family==GML_RENDER_SCENE_TEXT
      ?gml_render_view_text_quad_valid(&op,&state,source.width,source.height)
      :family==GML_RENDER_SCENE_TILE
      ?gml_render_view_tile_quad_valid(&op,&state,source.width,source.height)
      :gml_render_view_background_quad_valid(&op,&state,source.width,source.height)) &&
    gml_render_application_scene_enqueue(r,&op,&state,&source);
}
