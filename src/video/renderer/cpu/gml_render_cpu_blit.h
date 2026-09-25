/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_BLIT_H
#define GML_RENDER_CPU_BLIT_H

#include "gml_render.h"

typedef struct GmlTpag GmlTpag;

/* One resolved background/tile source emitted into the active CPU target. */
void gml_render_cpu_blit_background_phase(GmlRender *r, GmlTpag *t, double dx, double dy,
                                          double xs, double ys, uint32_t blend, double alpha);

/* Preserve classic phase replay for a resolved repeated background cell. */
void gml_render_cpu_blit_with_phase(GmlRender *r, GmlTpag *t, double dx, double dy,
                                    double xs, double ys, uint32_t blend, double alpha,
                                    int phase_mode, int quantize_alpha);

/* Emit a resolved background sub-rectangle through the unchanged CPU part kernel. */
void gml_render_cpu_blit_tpag_part_background(GmlRender *r, GmlTpag *t,
                                              double sx, double sy, double sw, double sh,
                                              double dx, double dy, double xs, double ys,
                                              uint32_t blend, double alpha);

void gml_render_cpu_blit_tpag_part_tile(GmlRender *r, GmlTpag *t,
                                        double sx, double sy, double sw, double sh,
                                        double dx, double dy, double xs, double ys,
                                        uint32_t blend, double alpha);
int gml_render_cpu_try_exact_rotated_background_tile(
    GmlRender *r, GmlTpag *page, double sx, double sy, double sw, double sh,
    double x, double y, double xs, double ys, int mirror, int flip,
    uint32_t color, double alpha);
void gml_render_cpu_blit_rotated_background_tile(GmlRender *r, GmlTpag *view,
                                                  double draw_x, double draw_y,
                                                  double tile_xscale, double tile_yscale,
                                                  double degrees, uint32_t color, double alpha);

#endif
