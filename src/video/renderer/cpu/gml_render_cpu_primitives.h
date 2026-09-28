/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_PRIMITIVES_H
#define GML_RENDER_CPU_PRIMITIVES_H

#include "gml_render.h"

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
