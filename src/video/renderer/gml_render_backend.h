/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Non-reentrant renderer leaf services for software raster backends. */
#ifndef GML_RENDER_BACKEND_H
#define GML_RENDER_BACKEND_H

#include "gml_render.h"

typedef struct {
  uint8_t red[256],green[256],blue[256];
  uint32_t surface_alpha;
} GmlRenderBackendFlatBlend;

enum {
  GML_RENDER_BACKEND_PIXELS_NONE=0,
  GML_RENDER_BACKEND_PIXELS_RGBA,
  GML_RENDER_BACKEND_PIXELS_XRGB
};
/* Borrowed non-reentrant texture view. Pixel storage remains renderer-owned
 * and is valid only while the referenced resource and renderer stay alive. */
typedef struct {
  int pixel_kind, runtime;
  int logical_width, logical_height, origin_x, origin_y;
  int trim_x, trim_y;
  int width, height, stride, source_x, source_y;
  int full_width, full_height;
  int resource_index, page_index, atlas_index;
  const uint8_t *rgba;
  const uint32_t *xrgb;
} GmlRenderBackendTextureView;

/* Borrowed non-reentrant view of the current draw target and policy. Writable
 * pixel planes remain renderer-owned and are valid only until the next
 * renderer operation. Callers keep this view outside per-pixel loops. */
typedef struct {
  const struct AnygmHostServices *host;
  uint32_t *pixels;
  uint32_t *interpolation_pixels[3];
  long frame;
  double camera_x, camera_y, alpha;
  double coordinate_scale_x, coordinate_scale_y;
  int width, height;
  int interpolate, alpha_blend, blend_mode, circle_precision;
} GmlRenderBackendDrawView;

uint32_t gml_render_backend_color_to_xrgb(uint32_t color);
uint32_t gml_render_backend_lerp_xrgb(uint32_t first,uint32_t second,
                                      int numerator,int denominator);
void gml_render_backend_fill_xrgb(uint32_t *pixels,int count,uint32_t color);
void gml_render_backend_flat_blend_init(GmlRenderBackendFlatBlend *blend,
                                        uint32_t color,double alpha);
void gml_render_backend_flat_blend_run(GmlRender *render,uint32_t *pixels,
                                       int count,const GmlRenderBackendFlatBlend *blend);
int gml_render_backend_flat_blend_uses_float_alpha(double alpha);
void gml_render_backend_draw_xrgb_alpha(GmlRender *render,uint32_t *pixels,
                                        int count,uint32_t color,double alpha);
void gml_render_backend_draw_pixel_alpha(GmlRender *render,int x,int y,
                                         uint32_t color,double alpha);
void gml_render_backend_draw_pixel(GmlRender *render,int x,int y,uint32_t color);
int gml_render_backend_texture_view(GmlRender *render,int handle,int full_atlas,
                                    GmlRenderBackendTextureView *view);
int gml_render_backend_atlas_view(GmlRender *render,int atlas,
                                  GmlRenderBackendTextureView *view);
GmlSoftware3D *gml_render_backend_software3d(GmlRender *render);
int gml_render_backend_draw_view(GmlRender *render,GmlRenderBackendDrawView *view);
void gml_render_backend_prepare_draw(GmlRender *render);
int gml_render_backend_prepare_draw_view(GmlRender *render,
                                         GmlRenderBackendDrawView *view);
void gml_render_backend_sync_camera(GmlRender *render,double translation_x,
                                    double translation_y);
void gml_render_backend_draw_map_point(const GmlRender *render,double *x,double *y);
int gml_render_backend_surface_stretched(GmlRender *render,int surface,
                                         double x,double y,double width,double height,
                                         uint32_t blend,double alpha);

#define GML_TEX_SPR_TAG  0x54000000u
#define GML_TEX_SURF_TAG 0x55000000u
#define GML_TEX_BG_TAG   0x56000000u
#define GML_TEX_KIND_MASK 0xFF000000u


#endif
