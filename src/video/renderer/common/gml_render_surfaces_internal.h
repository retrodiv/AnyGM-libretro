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
/* Register a current plain SCREEN for an independent full-target replacement.
 * This does not admit later drawing or change language-visible draw state. */
int gml_render_clear_target_prepare(GmlRender *render);
int gml_render_resident_sample_clear(GmlRender *render,uint32_t colour);
int gml_render_interpolated_resources_observe(GmlRender *render);
void gml_render_interpolated_resources_dispose(GmlRender *render);
/* Bind only an explicit nearest root plane. The caller must observe and retire it
 * before moving its borrowed allocation. No engine admission is implied. */
int gml_render_classic_phase_resource_bind(GmlRender *render);
int gml_render_classic_phase_resource_observe(GmlRender *render);
int gml_render_classic_phase_cpu_barrier(GmlRender *render);
void gml_render_classic_phase_resource_dispose(GmlRender *render);
/* A second target is available only inside atomic whole-draw preparation. */
int gml_render_classic_phase_scene_select(GmlRender *render,int vertical);
/* Replace both explicitly owned root planes without reading their old pixels.
 * Preparation of a partial draw must not use this whole-target boundary. */
int gml_render_resident_classic_phase_clear(GmlRender *render,uint32_t colour);
/* Replace the separately owned application image, preserving the active draw target.
 * Only an on-demand scene device may leave its canonical CPU allocation stale. */
int gml_render_resident_owned_application_clear(GmlRender *render,uint32_t colour);
/* Raw public copies preserve all four channels and ignore drawing state.
 * A full source uses full_source rather than caller-supplied region extents. */
int gml_render_resident_surface_copy(GmlRender *render,int destination,int x,int y,
  int source,int source_x,int source_y,int width,int height,int full_source);
/* Map the public tiled surface once and stage its complete ordered cell run. */
int gml_render_resident_surface_tiled(GmlRender *render,int surface,double x,double y,
  double scale_x,double scale_y,uint32_t colour,double alpha);
/* Compose the already owned nearest root pair without observing its pixels. */
int gml_render_resident_classic_phase_present(GmlRender *render,
  double x,double y,double width,double height,uint32_t colour,double alpha);
/* Attempt the host-owned pending application composite before a SCREEN scene draw. */
int gml_render_compose_resident_underlay(GmlRender *render);
void gml_render_base_resource_dispose(GmlRender *render);
void gml_render_retained_base_dispose(GmlRender *render);
void gml_render_view_base_dispose(GmlRender *render);
GmlRenderResource *gml_render_base_resource_lookup(const GmlRender *render,
  const uint32_t *pixels,int *width,int *height);
/* Return -1 rather than evicting an unobserved third physical canvas. */
int gml_render_base_resource_select(GmlRender *render,uint32_t *pixels);
int gml_render_base_resource_resize(GmlRender *render,int width,int height);
int gml_render_base_history_observe(GmlRender *render);
/* Raw mask destinations are caller-owned even when renderer state is replaced. */
int gml_render_masked_bases_observe(GmlRender *render);
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
/* One immutable draw may resolve to several disjoint quads. Reserve and append the whole
 * group before accepting it, so a refusal cannot leave a partially blended draw queued. */
int gml_render_application_scene_enqueue_quads(GmlRender *render,
  const GmlRenderDrawOp *operations,uint32_t count,
  const GmlRenderDrawStateSnapshot *state,const GmlRenderResourceSource *source);
/* Stage a complete immutable multi-cell draw without delivering any prefix.
 * A capacity/state refusal leaves the original queue and coverage unchanged. */
int gml_render_application_scene_prepare_begin(GmlRender *render);
int gml_render_application_scene_prepare_finish(GmlRender *render,int accepted);
int gml_render_application_scene_flush(GmlRender *render);
int gml_render_application_scene_enqueue_primitive(GmlRender *render,
                                                   const GmlRenderDrawOp *operation);
/* Validate a complete ordered source-free draw before appending any of its regions. */
int gml_render_application_scene_enqueue_primitives(GmlRender *render,
  const GmlRenderDrawOp *operations,uint32_t count);
int gml_render_application_scene_enqueue_textured_primitives(GmlRender *render,
  const GmlRenderDrawOp *operations,uint32_t count,const GmlRenderResourceSource *source);
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
