/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_BLIT_H
#define GML_RENDER_CPU_BLIT_H

#include "gml_render.h"

typedef struct GmlTpag GmlTpag;
typedef struct GmlSprite GmlSprite;
typedef struct GmlAtlas GmlAtlas;

/* Existing CPU skeletal and raster closures reached after shared sprite preparation. */
int gml_render_cpu_draw_spine_sprite(GmlRender *render, GmlSprite *sprite,
                                     double x, double y, double xscale, double yscale,
                                     double rotation, uint32_t blend, double alpha);
void gml_render_cpu_blit_sprite_ext_resolved(GmlRender *render, GmlSprite *sprite_record,
                                             int sprite, int subimage, double x, double y,
                                             double xscale, double yscale, double rotation,
                                             uint32_t blend, double alpha);
void gml_render_cpu_blit_sprite_stretched_resolved(GmlRender *render, GmlSprite *sprite_record,
                                                    int frame, double x, double y,
                                                    double width, double height,
                                                    uint32_t blend, double alpha);
/* Shared execution of the preserved bilinear band. All scratch is allocated before writes;
 * failure returns zero without modifying the borrowed target. */
int gml_render_cpu_bilinear_sprite_stretch(GmlRender *render,
                                           const GmlAtlas *atlas,const GmlTpag *page,
                                           int x0,int y0,int width,int height,
                                           int source_width,int source_height,
                                           int red,int green,int blue,double alpha);
void gml_render_cpu_blit_sprite_pos_resolved(GmlRender *render, int sprite, int subimage,
                                              const double x[4], const double y[4], double alpha);

/* Existing coarse sprite-region raster entries; sources remain borrowed. */
void gml_render_cpu_blit_rgba_region(GmlRender *r, const uint8_t *source, int width, int height,
                                     double sx, double sy, double sw, double sh,
                                     double dx, double dy, double dw, double dh,
                                     uint32_t blend, double alpha);
void gml_render_cpu_blit_tpag_part(GmlRender *r, GmlTpag *page,
                                   double sx, double sy, double sw, double sh,
                                   double dx, double dy, double xs, double ys,
                                   uint32_t blend, double alpha);
void gml_render_cpu_blit_sprite_general_rotated(GmlRender *r, GmlSprite *sprite,
                                                 int subimg, double sx, double sy,
                                                 double sw, double sh, double x,
                                                 double y, double xs, double ys,
                                                 double rotation, uint32_t colour,
                                                 double alpha);
void gml_render_cpu_blit_sprite_general_coloured(GmlRender *r, GmlSprite *sprite,
                                                  int subimg, double sx, double sy,
                                                  double sw, double sh, double x,
                                                  double y, double xs, double ys,
                                                  double rotation,
                                                  const uint32_t corner_colour[4],
                                                  double alpha);

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
