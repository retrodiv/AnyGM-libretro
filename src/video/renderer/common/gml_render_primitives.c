/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Shared primitive geometry and the existing rectangle shader-preparation decision. */
#include "gml_render.h"
#include "gml_render_internal.h"
#include "gml_render_cpu_primitives.h"

#include <math.h>

void gml_render_primitive_rectangle(GmlRender *render,
                                    int x1,int y1,int x2,int y2,
                                    uint32_t color,int outline){
  if(!outline && gml_render_shade_target_rect(render,x1,y1,x2,y2,color,render?render->alpha:1.0))
    return;
  gml_render_cpu_rectangle(render,x1,y1,x2,y2,color,outline);
}

void gml_render_primitive_rectangle_color(GmlRender *render,
                                          int x1,int y1,int x2,int y2,
                                          uint32_t color1,uint32_t color2,
                                          uint32_t color3,uint32_t color4,
                                          int outline){
  if(!outline && color1==color2 && color2==color3 && color3==color4 &&
     gml_render_shade_target_rect(render,x1,y1,x2,y2,color1,render?render->alpha:1.0)) return;
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
