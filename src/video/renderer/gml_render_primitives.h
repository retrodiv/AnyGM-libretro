/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Typed fixed-function primitive operations. Language argument decoding stays in builtins. */
#ifndef GML_RENDER_PRIMITIVES_H
#define GML_RENDER_PRIMITIVES_H

#include <stdint.h>

typedef struct GmlRender GmlRender;

void gml_render_primitive_point(GmlRender *render,
                                int x,int y,uint32_t color);

void gml_render_primitive_rectangle(GmlRender *render,
                                    int x1,int y1,int x2,int y2,
                                    uint32_t color,int outline);
void gml_render_primitive_rectangle_color(GmlRender *render,
                                          int x1,int y1,int x2,int y2,
                                          uint32_t color1,uint32_t color2,
                                          uint32_t color3,uint32_t color4,
                                          int outline);
void gml_render_primitive_line(GmlRender *render,
                               int x1,int y1,int x2,int y2,
                               uint32_t color,int width);
void gml_render_primitive_line_color(GmlRender *render,
                                     int x1,int y1,int x2,int y2,
                                     uint32_t color1,uint32_t color2,
                                     int width);
void gml_render_primitive_line_color_subpixel(GmlRender *render,
                                              double x1,double y1,
                                              double x2,double y2,
                                              uint32_t color1,uint32_t color2,
                                              int width);
void gml_render_primitive_circle(GmlRender *render,
                                 int center_x,int center_y,int radius_x,int radius_y,
                                 uint32_t color,int outline);
void gml_render_primitive_circle_subpixel(GmlRender *render,
                                          double center_x,double center_y,
                                          double radius_x,double radius_y,
                                          uint32_t color,int outline);

/* Filled circles apply a one-unit logical centre bias before view mapping and retain continuous
 * radii until the covered pixel set is quantized. */
#define GML_RENDER_CIRCLE_CENTER_BIAS 1.0

void gml_render_circle_geometry(const GmlRender *render,
                                double x,double y,double radius,
                                double *center_x,double *center_y,
                                double *radius_x,double *radius_y);
void gml_render_primitive_circle_color(GmlRender *render,
                                       int center_x,int center_y,int radius_x,int radius_y,
                                       uint32_t inner,uint32_t outer,int outline);
void gml_render_primitive_circle_color_subpixel(GmlRender *render,
                                                double center_x,double center_y,
                                                double radius_x,double radius_y,
                                                uint32_t inner,uint32_t outer,int outline);
void gml_render_primitive_triangle_alpha(GmlRender *render,
                                         double x1,double y1,double x2,double y2,
                                         double x3,double y3,uint32_t color,
                                         double alpha,int outline);
void gml_draw_layer_color_fill(GmlRender *render,uint32_t color,double alpha);

#endif
