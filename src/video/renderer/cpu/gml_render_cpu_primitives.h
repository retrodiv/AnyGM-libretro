/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_PRIMITIVES_H
#define GML_RENDER_CPU_PRIMITIVES_H

#include "gml_render.h"

typedef void (*GmlRenderPrimitiveSpan)(void *context,int x0,int x1,int y);
typedef void (*GmlRenderPrimitiveBox)(void *context,int x1,int y1,int x2,int y2,
                                      uint32_t color);
int gml_render_line_integer_boxes(GmlRender *render,int x1,int y1,int x2,int y2,
                                  uint32_t color1,uint32_t color2,int width,
                                  GmlRenderPrimitiveBox emit,void *context);
int gml_render_line_subpixel_boxes(GmlRender *render,double x1,double y1,
                                   double x2,double y2,uint32_t color1,
                                   uint32_t color2,int width,
                                   GmlRenderPrimitiveBox emit,void *context);
int gml_render_rectangle_outline_pixels(GmlRender *render,int x1,int y1,int x2,int y2,
                                        uint32_t color,GmlRenderPrimitiveBox emit,
                                        void *context);
int gml_render_circle_outline_segments(GmlRender *render,double cx,double cy,
                                       double rx,double ry,uint32_t color,int coloured,
                                       GmlRenderPrimitiveBox emit,void *context);
void gml_render_cpu_line_color_subpixel(GmlRender *render,double x1,double y1,
                                        double x2,double y2,uint32_t color1,
                                        uint32_t color2,int width);
int gml_render_circle_filled_spans(GmlRender *render,double cx,double cy,
                                   double rx,double ry,GmlRenderPrimitiveSpan emit,
                                   void *context);
int gml_render_triangle_opaque_spans(GmlRender *render,
                                     double x1,double y1,double x2,double y2,
                                     double x3,double y3,double alpha,int outline,
                                     GmlRenderPrimitiveSpan emit,void *context);
void gml_render_cpu_circle(GmlRender *render,double cx,double cy,double rx,double ry,
                           uint32_t color,int outline);
void gml_render_cpu_circle_color(GmlRender *render,double cx,double cy,
                                 double rx,double ry,uint32_t inner,
                                 uint32_t outer,int outline);
void gml_render_cpu_triangle_alpha(GmlRender *render,double x1,double y1,
                                   double x2,double y2,double x3,double y3,
                                   uint32_t color,double alpha,int outline);

void gml_render_cpu_point(GmlRender *render,int x,int y,uint32_t color);
void gml_render_cpu_line(GmlRender *render,int x1,int y1,int x2,int y2,
                         uint32_t color,int width);
void gml_render_cpu_line_color(GmlRender *render,int x1,int y1,int x2,int y2,
                               uint32_t color1,uint32_t color2,int width);
void gml_render_cpu_rectangle(GmlRender *render,int x1,int y1,int x2,int y2,
                              uint32_t color,int outline);
void gml_render_cpu_rectangle_color(GmlRender *render,int x1,int y1,int x2,int y2,
                                    uint32_t color1,uint32_t color2,
                                    uint32_t color3,uint32_t color4,int outline);

#endif
