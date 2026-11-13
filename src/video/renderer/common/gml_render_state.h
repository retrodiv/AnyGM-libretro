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

typedef struct GmlRenderFontCheckpoint GmlRenderFontCheckpoint;
/* In-memory rollback protection, not a second wire format. No VFS reads are needed to restore it. */
int gml_render_font_checkpoint_create(const GmlRender *render,GmlRenderFontCheckpoint **out);
int gml_render_font_checkpoint_restore(GmlRender *render,GmlRenderFontCheckpoint *checkpoint);
void gml_render_font_checkpoint_free(GmlRenderFontCheckpoint *checkpoint);

/* A nonzero omission requires caller-proven disposable storage. Engine snapshots
 * pass zero; content-visible view targets must retain their actual pixels. */
size_t gml_render_state_size(GmlRender *render,int derived_view_surface);
/* Metadata-only upper bound for the existing payload; no pixel observer or compression. */
size_t gml_render_state_capacity(GmlRender *render,int derived_view_surface);
/* Additional worst-case payload for a surface's pixels; its fixed slot header already exists. */
size_t gml_render_surface_state_capacity(int width,int height);
int gml_render_state_save(GmlRender *render,int derived_view_surface,
                          void *data,size_t length,size_t *written);
int gml_render_state_load(GmlRender *render,const void *data,size_t length,size_t *used);
int gml_render_state_profile_metrics(const GmlRender *render,
                                     GmlRenderStateProfileMetrics *metrics);

#endif
