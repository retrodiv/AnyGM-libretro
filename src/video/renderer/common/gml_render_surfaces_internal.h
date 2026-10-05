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
GmlRenderResourceExecution gml_render_surface_scene_submit_deferred_shader(
  GmlRender *render,int surface,const GmlRenderDrawBatch *batch,
  const GmlRenderResourceSource *sources,uint32_t source_count);

/* The application target borrows the renderer's existing canonical allocation; CPU-only
 * sessions never allocate this metadata. Resize and renderer teardown retire it. */
GmlRenderResource *gml_render_application_resource_ensure(GmlRender *render);
void gml_render_application_resource_dispose(GmlRender *render);
GmlRenderResource *gml_render_scene_target_resource_ensure(GmlRender *render);
int gml_render_scene_target_matches(const GmlRender *render);
/* Attempt the host-owned pending application composite before a SCREEN scene draw. */
int gml_render_compose_resident_underlay(GmlRender *render);
void gml_render_base_resource_dispose(GmlRender *render);
GmlRenderResourceExecution gml_render_application_scene_submit(
  GmlRender *render,const GmlRenderDrawBatch *batch,
  const GmlRenderResourceSource *sources,uint32_t source_count);
/* Consecutive immutable-source sprites may queue resolved work. Every CPU observation, target
 * switch, context change, and non-compatible draw drains the queue in order. */
GmlRenderImageRef gml_render_application_scene_next_image_ref(GmlRender *render,
                                                               GmlRenderResource *resource);
int gml_render_application_scene_enqueue(GmlRender *render,const GmlRenderDrawOp *operation,
                                          const GmlRenderDrawStateSnapshot *state,
                                          const GmlRenderResourceSource *source);
int gml_render_application_scene_flush(GmlRender *render);
int gml_render_application_scene_enqueue_primitive(GmlRender *render,
                                                   const GmlRenderDrawOp *operation);
int gml_render_checkpoint_pending_resources(GmlRender *render);
void gml_render_application_scene_queue_dispose(GmlRender *render);
/* Call before a CPU leaf writes an explicitly resident application pass. The pending GPU clear
 * may be submitted first; a caller must not enter its CPU kernel on a false result. */
int gml_render_resident_application_cpu_barrier(GmlRender *render);

/* A scene executor may carry a numbered resident target into the next pass without acquiring
 * CPU pixels. It must materialise that target before any CPU leaf observes or writes it.
 * Ordinary gml_render_begin_retaining_target keeps its established CPU barrier. */
void gml_render_begin_retaining_resident_target(GmlRender *render,uint32_t *base,
                                                int width,int height,double camera_x,
                                                double camera_y);

#endif
