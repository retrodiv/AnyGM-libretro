/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_STATE_H
#define GML_RENDER_STATE_H

#include "gml_render.h"

typedef struct {
  size_t surface_bytes;
  size_t inline_runtime_sprite_bytes;
  size_t file_runtime_sprite_bytes;
  int surface_count;
  int runtime_sprite_count;
  int file_runtime_sprite_count;
} GmlRenderStateProfileMetrics;

size_t gml_render_state_size(GmlRender *render,int derived_view_surface);
int gml_render_state_save(GmlRender *render,int derived_view_surface,
                          void *data,size_t length,size_t *written);
int gml_render_state_load(GmlRender *render,const void *data,size_t length,size_t *used);
int gml_render_state_profile_metrics(const GmlRender *render,
                                     GmlRenderStateProfileMetrics *metrics);

#endif
