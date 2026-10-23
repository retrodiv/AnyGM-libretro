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
int gml_render_resident_flat_fog_sprite(GmlRender *render,int sprite,int subimage,
  double x,double y,double xscale,double yscale,double rotation,uint32_t colour,double alpha);
/* Explicit paired-root writer. It never enables the engine's phase policy or
 * admits unowned sample planes; callers keep clear/composition/state ownership. */
int gml_render_resident_classic_phase_sprite(GmlRender *render,int sprite,int subimage,
  double x,double y,double xscale,double yscale,double rotation,uint32_t colour,double alpha);
/* Each port names the existing CPU caller's opacity and vertical-span policy.
 * These axis cells borrow immutable atlas bytes; layout and source selection
 * remain with the text/background/tile owner. */
typedef enum {
  GML_RENDER_PHASE_TEXT,
  GML_RENDER_PHASE_BACKGROUND,
  GML_RENDER_PHASE_BACKGROUND_TILED,
  GML_RENDER_PHASE_BACKGROUND_PART,
  GML_RENDER_PHASE_TILE,
  GML_RENDER_PHASE_ROOM_TILE,
  GML_RENDER_PHASE_SPRITE_TEXT
} GmlRenderClassicPhaseTexturePort;
int gml_render_resident_classic_phase_atlas(GmlRender *render,GmlTpag *page,
  double left,double top,double xscale,double yscale,uint32_t colour,double alpha,
  GmlRenderClassicPhaseTexturePort port);
int gml_render_resident_masked_sprite(GmlRender *render, int sprite, int subimage,
                                      double x, double y, double xscale, double yscale,
                                      double rotation, uint32_t colour, double alpha);
/* Submit a qualified atlas crop shared by ordinary sprites, backgrounds, and tiles. */
int gml_render_resident_atlas_page(GmlRender *render, int page_index,
                                  double left, double top, double xscale, double yscale,
                                  GmlRenderSceneFamily family);
int gml_render_resident_integer_background_page(GmlRender *render,int page_index,
                                                double left,double top,double xscale,double yscale,
                                                uint32_t colour,double alpha);
int gml_render_resident_nearest_background_page(GmlRender *render,int page_index,
                                                double left,double top,double xscale,double yscale,
                                                uint32_t colour,double alpha);
int gml_render_resident_selected_background_page(GmlRender *render,GmlTpag *page,
                                                 double left,double top,double xscale,double yscale,
                                                 uint32_t colour,double alpha);
int gml_render_resident_nearest_tile_page(GmlRender *render,GmlTpag *page,
                                         double left,double top,double xscale,double yscale,
                                         uint32_t colour,double alpha);
int gml_render_resident_nearest_quarter_tile_page(GmlRender *render,GmlTpag *page,
  double x,double y,double xscale,double yscale,int exact_unit,int mirror,int flip,
  uint32_t colour,double alpha);
int gml_render_resident_filtered_quarter_tile_page(GmlRender *render,int page_index,GmlTpag *page,
  double x,double y,double xscale,double yscale,uint32_t colour,double alpha);
enum {
  GML_RENDER_FILTERED_ATLAS_RULE=0,
  GML_RENDER_NEAREST_PART_PHASE_RULE=1,
  GML_RENDER_NEAREST_DIRECT_SNAP_RULE=2,
  GML_RENDER_FILTERED_HARDWARE_PART_RULE=3
};
int gml_render_resident_filtered_atlas_page(GmlRender *render, int page_index,
                                           double left, double top,
                                           double xscale, double yscale,
                                           GmlRenderSceneFamily family,
                                           uint32_t colour, double alpha,
                                           GmlTpag *part_view, int sampling_rule);
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
int gml_render_resident_ortho_font_glyph(GmlRender *render, int atlas,
                                         int source_x, int source_y, int width, int height,
                                         double x, double y);
int gml_render_resident_font_glyph(GmlRender *render, int atlas,
                                   int source_x, int source_y, int width, int height,
                                   double left, double top, double xscale, double yscale,
                                   uint32_t colour, double alpha);
int gml_render_resident_sprite_font_glyph(GmlRender *render,int page_index,
  double left,double top,double xscale,double yscale,uint32_t colour,double alpha);
int gml_render_resident_rotated_font_glyph(GmlRender *render,GmlSprite *sprite,GmlTpag *page,
  double x,double y,double xscale,double yscale,double rotation,uint32_t colour,double alpha);
int gml_render_resident_filtered_font_glyph(GmlRender *render, int atlas,
                                            int source_x, int source_y,
                                            int width, int height,
                                            double left, double top,
                                            double xscale, double yscale,
                                            uint32_t colour, double alpha);
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
/* The first CPU triangle owns overlapping coverage. These helpers resolve
 * input coordinates/alpha support only, never destination colour. */
int gml_render_sprite_pos_coordinates(const GmlRenderSpritePosGeometry *geometry,
                                      int x,int y,double *u,double *v);
double gml_render_sprite_pos_source_alpha(const uint8_t *rgba,int stride,
  int ox,int oy,int width,int height,double u,double v,int interpolate);
int gml_render_sprite_pos_axis_sample(GmlRender *render,const GmlTpag *page,
  GmlRenderRect bounds,double left,double top,double xscale,double yscale,
  int x,int y,double alpha,double *u,double *v,double *sample_alpha);
void gml_render_sprite_nineslice(GmlRender *render, GmlSprite *sprite_record, GmlTpag *page,
                                  double x, double y, int width, int height,
                                  uint32_t blend, double alpha);

#endif
