/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SURFACES_INTERNAL_H
#define GML_RENDER_SURFACES_INTERNAL_H

#include "gml_render.h"
#include "gml_render_resources.h"

/* Stable bounded surface ID to pool slot; used only at coarse API boundaries. */
int surface_slot(int id);
int gml_render_surface_draw_targets_screen_raster(const GmlRender *render, int surface,
                                                   double width, double height);

/* Optional scene residency on the existing surface identity. CPU-only surfaces pay no journal
 * allocation. A returned owner is borrowed and expires at resize, free, load, or renderer free. */
GmlRenderResource *gml_render_surface_resource_ensure(GmlRender *render,int surface);
void gml_render_surface_resource_dispose(GmlRender *render,int surface);
GmlRenderResourceExecution gml_render_surface_scene_submit(
  GmlRender *render,int surface,const GmlRenderDrawBatch *batch,
  const GmlRenderResourceSource *sources,uint32_t source_count);

#endif
