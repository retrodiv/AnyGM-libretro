/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Non-reentrant renderer leaf services for software raster backends. */
#ifndef GML_RENDER_CPU_BACKEND_H
#define GML_RENDER_CPU_BACKEND_H

#include "gml_render.h"
#include "gml_render_texture.h"

typedef struct {
  uint8_t red[256],green[256],blue[256];
  uint32_t surface_alpha;
} GmlRenderCpuBackendFlatBlend;

enum {
  GML_RENDER_CPU_BACKEND_PIXELS_NONE=0,
  GML_RENDER_CPU_BACKEND_PIXELS_RGBA,
  GML_RENDER_CPU_BACKEND_PIXELS_XRGB
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
} GmlRenderCpuBackendTextureView;

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
} GmlRenderCpuBackendDrawView;

uint32_t gml_render_cpu_backend_color_to_xrgb(uint32_t color);
uint32_t gml_render_cpu_backend_lerp_xrgb(uint32_t first,uint32_t second,
                                      int numerator,int denominator);
void gml_render_cpu_backend_fill_xrgb(uint32_t *pixels,int count,uint32_t color);
void gml_render_cpu_backend_flat_blend_init(GmlRenderCpuBackendFlatBlend *blend,
                                        uint32_t color,double alpha);
void gml_render_cpu_backend_flat_blend_run(GmlRender *render,uint32_t *pixels,
                                       int count,const GmlRenderCpuBackendFlatBlend *blend);
int gml_render_cpu_backend_flat_blend_uses_float_alpha(double alpha);
void gml_render_cpu_backend_draw_xrgb_alpha(GmlRender *render,uint32_t *pixels,
                                        int count,uint32_t color,double alpha);
void gml_render_cpu_backend_draw_pixel_alpha(GmlRender *render,int x,int y,
                                         uint32_t color,double alpha);
void gml_render_cpu_backend_draw_pixel(GmlRender *render,int x,int y,uint32_t color);
int gml_render_cpu_backend_texture_view(GmlRender *render,int handle,int full_atlas,
                                    GmlRenderCpuBackendTextureView *view);
int gml_render_cpu_backend_atlas_view(GmlRender *render,int atlas,
                                  GmlRenderCpuBackendTextureView *view);
GmlSoftware3D *gml_render_cpu_backend_software3d(GmlRender *render);
/* Update the derived full-fog silhouette cache without entering draw preparation. */
void gml_render_cpu_backend_set_flat_fog(GmlRender *render,int enabled,uint32_t colour);
int gml_render_cpu_backend_draw_view(GmlRender *render,GmlRenderCpuBackendDrawView *view);
void gml_render_cpu_backend_prepare_draw(GmlRender *render);
int gml_render_cpu_backend_prepare_draw_view(GmlRender *render,
                                         GmlRenderCpuBackendDrawView *view);
void gml_render_cpu_backend_sync_camera(GmlRender *render,double translation_x,
                                    double translation_y);
void gml_render_cpu_backend_draw_map_point(const GmlRender *render,double *x,double *y);
int gml_render_cpu_backend_surface_stretched(GmlRender *render,int surface,
                                         double x,double y,double width,double height,
                                         uint32_t blend,double alpha);
/* The colour modulation the active vertex program applies to every vertex colour it passes on, as
 * red, green, blue and alpha factors. Answers zero when no such program is set, so callers keep
 * their unmodulated path. Reads shader state and touches no draw target. */
int gml_render_cpu_backend_shader_vertex_colour_blend(const GmlRender *render,double blend[4]);

#endif
