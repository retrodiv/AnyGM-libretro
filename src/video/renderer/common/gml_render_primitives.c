/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared primitive geometry and the existing rectangle shader-preparation decision. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_cpu_primitives.h"
#include "gml_render_cpu_backend.h"
#include "gml_render_surfaces_internal.h"

#include <math.h>
#include <limits.h>

/* Opaque flat coverage has the same half-open pixel region for point-sized and filled
 * rectangles. Keep every non-qualified shape in its established CPU raster body. */
static int resident_flat_rect(GmlRender *render,int x1,int y1,int x2,int y2,
                              uint32_t color){
  GmlRenderResource *resource;
  GmlRenderImageRef image;
  GmlRenderDrawOp operation={0};
  GmlRenderDrawBatch batch;
  GmlRenderResourceExecution result;
  if(!render || !render->resident_application_pass || !gml_render_device(render) ||
     render->fb!=render->app_surface_owned || render->fbw!=render->app_w ||
     render->fbh!=render->app_h || render->fbw<=0 || render->fbh<=0 ||
     render->target_sp || render->target_id!=-1 || render->gui_pass_active ||
     render->world_transform_active || render->classic ||
     render->classic_phase_y || render->classic_interp_phase[0] ||
     render->classic_interp_phase[1] || render->classic_interp_phase[2] ||
     render->active_shader>=0 || render->alpha!=1.0 || render->blendmode ||
     render->blend_equation!=1 || render->blend_equation_alpha!=1 ||
     render->color_write_mask!=15u || render->alpha_test_enable ||
     render->pending_presentation || render->rotated_batch_count ||
     render->rotated_batch_building) return 0;
  if(x1>x2){int swap=x1;x1=x2;x2=swap;}
  if(y1>y2){int swap=y1;y1=y2;y2=swap;}
  if(x2<0 || y2<0 || x1>=render->fbw || y1>=render->fbh) return 0;
  if(x1<0) x1=0;
  if(y1<0) y1=0;
  if(x2>=render->fbw) x2=render->fbw-1;
  if(y2>=render->fbh) y2=render->fbh-1;
  gml_render_maybe_prepare_opaque_rect(render,x1,y1,x2+1,y2+1);
  if(render->pending_fill || render->pending_underlay ||
     !render->resident_application_pass) return 0;
  resource=gml_render_application_resource_ensure(render);
  if(!resource) return 0;
  image=gml_render_application_scene_next_image_ref(render,resource);
  if(image.content_version==UINT64_MAX) return 0;
  operation.opcode=GML_RENDER_OP_COLOURED_PRIMITIVE;
  operation.scene_family=GML_RENDER_SCENE_PRIMITIVE;
  operation.reconstruction_mode=GML_RENDER_REPLAY_EXACT;
  operation.target.image=image;
  operation.target.image.content_version++;
  operation.target.storage_version=1u;
  operation.target.width=(uint32_t)render->fbw;
  operation.target.height=(uint32_t)render->fbh;
  operation.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  operation.target.region=(GmlRenderRect){0,0,(uint32_t)render->fbw,(uint32_t)render->fbh};
  operation.payload.primitive.region=(GmlRenderRect){x1,y1,(uint32_t)(x2-x1+1),
                                                      (uint32_t)(y2-y1+1)};
  operation.payload.primitive.colour=gml_render_cpu_backend_color_to_xrgb(color);
  batch=(GmlRenderDrawBatch){&operation,1u,NULL,0u};
  result=gml_render_application_scene_submit(render,&batch,NULL,0u);
  if(result==GML_RENDER_RESOURCE_EXEC_CPU) render->resident_application_pass=0;
  return result!=GML_RENDER_RESOURCE_EXEC_FAILED;
}

static int resident_axis_line(GmlRender *render,int x1,int y1,int x2,int y2,
                              uint32_t color,int width){
  int64_t left,right,top,bottom;
  if(x1!=x2 && y1!=y2) return 0;
  if(width<1) width=1;
  left=(int64_t)(x1<x2?x1:x2)-width/2;
  right=(int64_t)(x1>x2?x1:x2)-width/2+width-1;
  top=(int64_t)(y1<y2?y1:y2)-width/2;
  bottom=(int64_t)(y1>y2?y1:y2)-width/2+width-1;
  if(left<INT_MIN || right>INT_MAX || top<INT_MIN || bottom>INT_MAX) return 0;
  return resident_flat_rect(render,(int)left,(int)top,(int)right,(int)bottom,color);
}

void gml_render_primitive_point(GmlRender *render,int x,int y,uint32_t color){
  if(resident_flat_rect(render,x,y,x,y,color)) return;
  gml_render_cpu_point(render,x,y,color);
}

void gml_render_primitive_line(GmlRender *render,int x1,int y1,int x2,int y2,
                               uint32_t color,int width){
  if(resident_axis_line(render,x1,y1,x2,y2,color,width)) return;
  gml_render_cpu_line(render,x1,y1,x2,y2,color,width);
}

void gml_render_primitive_line_color(GmlRender *render,int x1,int y1,int x2,int y2,
                                     uint32_t color1,uint32_t color2,int width){
  if(color1==color2 && resident_axis_line(render,x1,y1,x2,y2,color1,width)) return;
  gml_render_cpu_line_color(render,x1,y1,x2,y2,color1,color2,width);
}

void gml_render_primitive_rectangle(GmlRender *render,
                                    int x1,int y1,int x2,int y2,
                                    uint32_t color,int outline){
  if(!outline && gml_render_shade_target_rect(render,x1,y1,x2,y2,color,render?render->alpha:1.0))
    return;
  if(!outline && resident_flat_rect(render,x1,y1,x2,y2,color)) return;
  gml_render_cpu_rectangle(render,x1,y1,x2,y2,color,outline);
}

void gml_render_primitive_rectangle_color(GmlRender *render,
                                          int x1,int y1,int x2,int y2,
                                          uint32_t color1,uint32_t color2,
                                          uint32_t color3,uint32_t color4,
                                          int outline){
  if(!outline && color1==color2 && color2==color3 && color3==color4 &&
     gml_render_shade_target_rect(render,x1,y1,x2,y2,color1,render?render->alpha:1.0)) return;
  if(!outline && color1==color2 && color2==color3 && color3==color4 &&
     resident_flat_rect(render,x1,y1,x2,y2,color1)) return;
  gml_render_cpu_rectangle_color(render,x1,y1,x2,y2,color1,color2,color3,color4,outline);
}

void gml_render_circle_geometry(const GmlRender *render,
                                double x,double y,double radius,
                                double *center_x,double *center_y,
                                double *radius_x,double *radius_y){
  double mapped_x=x+GML_RENDER_CIRCLE_CENTER_BIAS;
  double mapped_y=y+GML_RENDER_CIRCLE_CENTER_BIAS;
  double mapped_rx=radius,mapped_ry=radius;
  GmlRenderTargetMetrics metrics={0};
  gml_render_draw_map_point(render,&mapped_x,&mapped_y);
  gml_render_draw_map_scale(render,&mapped_rx,&mapped_ry);
  (void)gml_render_target_metrics(render,&metrics);
  if(center_x) *center_x=mapped_x-metrics.camera_x;
  if(center_y) *center_y=mapped_y-metrics.camera_y;
  if(radius_x) *radius_x=fabs(mapped_rx);
  if(radius_y) *radius_y=fabs(mapped_ry);
}
