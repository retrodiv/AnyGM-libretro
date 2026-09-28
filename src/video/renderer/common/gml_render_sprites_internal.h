/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SPRITES_INTERNAL_H
#define GML_RENDER_SPRITES_INTERNAL_H

#include "gml_render.h"

typedef struct GmlSprite GmlSprite;
typedef struct GmlTpag GmlTpag;

typedef struct {
  double qx[4], qy[4];
  double tx[2][3], ty[2][3], tu[2][3], tv[2][3];
  int x0, x1, y0, y1;
} GmlRenderSpritePosGeometry;

int gml_render_sprite_log_enabled(const GmlRender *render);
/* Submit a qualified atlas crop shared by ordinary sprites, backgrounds, and tiles. */
int gml_render_resident_atlas_page(GmlRender *render, int page_index,
                                  double left, double top, double xscale, double yscale,
                                  GmlRenderSceneFamily family);
int gml_render_resident_filtered_atlas_page(GmlRender *render, int page_index,
                                           double left, double top,
                                           double xscale, double yscale,
                                           GmlRenderSceneFamily family,
                                           uint32_t colour, double alpha,
                                           GmlTpag *part_view, int nearest_scaled);
int gml_render_resident_filtered_atlas_part(GmlRender *render, int page_index,
                                           double source_x, double source_y,
                                           double source_width, double source_height,
                                           double x, double y,
                                           double xscale, double yscale,
                                           uint32_t colour, double alpha,
                                           GmlRenderSceneFamily family);
int gml_render_resident_atlas_crop(GmlRender *render, int page_index,
                                  int crop_x, int crop_y, int crop_width, int crop_height,
                                  double left, double top, double xscale, double yscale,
                                  GmlRenderSceneFamily family);
int gml_render_resident_font_glyph(GmlRender *render, int atlas,
                                   int source_x, int source_y, int width, int height,
                                   double left, double top);
int gml_render_resident_tiled_sprite(GmlRender *render, int sprite, int subimage,
                                     double x, double y, double xscale, double yscale,
                                     uint32_t blend, double alpha);
void gml_render_sprite_tiled_ext_unmasked(GmlRender *render, int sprite, int subimage,
                                           double x, double y, double xscale, double yscale,
                                           uint32_t blend, double alpha);
void gml_render_sprite_ext_unmasked(GmlRender *render, int sprite, int subimage,
                                    double x, double y, double xscale, double yscale,
                                    double rotation, uint32_t blend, double alpha);
int gml_render_sprite_pos_geometry(GmlRender *render, const double x[4], const double y[4],
                                    GmlRenderSpritePosGeometry *geometry);
void gml_render_sprite_nineslice(GmlRender *render, GmlSprite *sprite_record, GmlTpag *page,
                                  double x, double y, int width, int height,
                                  uint32_t blend, double alpha);

#endif
