/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_SURFACES_H
#define GML_RENDER_CPU_SURFACES_H

#include "gml_render.h"

/* Coarse CPU surface composition after shared request preparation. */
void gml_render_cpu_draw_surface_stretched_impl(GmlRender *render, int surface,
                                                 double x, double y, double width, double height,
                                                 uint32_t blend, double alpha, int allow_software3d);
void gml_render_cpu_draw_surface_part_mapped(GmlRender *render, int surface,
                                              double sx, double sy, double sw, double sh,
                                              double x, double y, double xs, double ys,
                                              uint32_t blend, double alpha);

#endif
