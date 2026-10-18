/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_RESOURCES_H
#define GML_RENDER_RESOURCES_H

#include "gml_render_device.h"

/* One metadata owner attached to the existing canonical pixel plane. It never owns or replaces
 * GML-visible surface identity or storage. The caller keeps pixels alive until destroy. */
typedef struct GmlRenderResource GmlRenderResource;

typedef enum GmlRenderResourceAuthority {
  GML_RENDER_RESOURCE_CPU_CURRENT=1,
  GML_RENDER_RESOURCE_BOTH_CURRENT=2,
  GML_RENDER_RESOURCE_GPU_REPLAYABLE=3,
  GML_RENDER_RESOURCE_GPU_PENDING_CHECKPOINT=4,
  GML_RENDER_RESOURCE_INVALID=5,
  /* Actual device pixels, without a replay recipe. Failed observation is retryable while
   * the owning namespace survives; a genuinely lost namespace invalidates this image. */
  GML_RENDER_RESOURCE_GPU_RESIDENT=6
} GmlRenderResourceAuthority;

typedef enum GmlRenderResourceExecution {
  GML_RENDER_RESOURCE_EXEC_FAILED=0,
  GML_RENDER_RESOURCE_EXEC_DEVICE=1,
  GML_RENDER_RESOURCE_EXEC_CPU=2
} GmlRenderResourceExecution;

typedef struct GmlRenderResourceLimits {
  size_t journal_bytes;
  size_t retained_mutable_bytes;
  /* Separate per-resource scratch budget; zero selects the default. Only SCREEN/application
   * owners retain checkpoint staging, so arbitrary numbered surfaces do not multiply caches. */
  size_t checkpoint_staging_bytes;
} GmlRenderResourceLimits;

/* A source version is immutable for the lifetime of one submission. If borrowed_immutable is
 * true, bytes must remain stable until the journal is materialised or the resource is destroyed;
 * authored immutable atlas pages can meet that contract without duplication. Mutable versions
 * are copied once per distinct ref before any device write and charged to the retained budget.
 * A byte-free resident source rooted at an exact full clear instead pins its immutable replay
 * recipe, charged to the journal budget, without observing or duplicating completed pixels. */
typedef struct GmlRenderResourceSource {
  GmlRenderImageRef image;
  uint32_t width,height,pixel_format,stride_bytes;
  size_t byte_capacity;
  const void *bytes;
  int borrowed_immutable;
  /* Optional current resource backing this CPU view. A matching GPU version can be sampled
   * directly; reconstruction still pins a separate immutable copy of mutable pixels. */
  const GmlRenderResource *resident_resource;
} GmlRenderResourceSource;

typedef struct GmlRenderJournalMetrics {
  uint64_t logical_version,cpu_version,gpu_version;
  uint64_t cpu_replays,device_checkpoints,consolidations,cpu_fallbacks;
  size_t journal_bytes,retained_mutable_bytes;
  /* Pinned recipes are included in journal_bytes. A reusable current-version snapshot has
   * its own bounded metadata/source storage; report that cache without hiding shared costs. */
  size_t cached_replay_bytes;
  uint32_t retained_replay_depth;
  /* Disposable host scratch, never canonical pixels or serialized state. Allocation count
   * includes transient checkpoint buffers, not source pins or CPU replay candidates. */
  size_t checkpoint_staging_bytes;
  uint64_t checkpoint_allocations;
  uint32_t journal_operations,retained_versions;
  uint32_t authority;
} GmlRenderJournalMetrics;

GmlRenderResource *gml_render_resource_create(GmlRenderImageRef image,uint32_t width,
                                               uint32_t height,uint32_t pixel_format,
                                               uint32_t *canonical_pixels,
                                               GmlRenderResourceLimits limits);
void gml_render_resource_destroy(GmlRenderResource *resource);
struct GmlRender;
/* Borrow the renderer only for transient replay-completion accounting. Rebind
 * after moving that owner; retained source recipes never capture this pointer. */
void gml_render_resource_bind_dispatch(GmlRenderResource *resource,struct GmlRender *render);
/* Commit staging storage only for a byte-free, current resident device image. The new host
 * shadow is deliberately stale; invalidate incremental checkpoint baselines on rebinding. */
int gml_render_resource_rebind_resident_pixels(GmlRenderResource *resource,uint32_t *pixels);
GmlRenderImageRef gml_render_resource_image_ref(const GmlRenderResource *resource);
int gml_render_resource_source_view(GmlRenderResource *resource,
                                    GmlRenderResourceSource *out_source);
/* Borrow only an already committed CPU-current version. Never replay, checkpoint or observe
 * the device, and never expose an open CPU write lease as a committed source version. */
int gml_render_resource_current_cpu_source_view(
  GmlRenderResource *resource,GmlRenderResourceSource *out_source);
/* A current version on this device may use a byte-free view without observing CPU pixels.
 * Exact replacement may retain a bounded complete-clear source recipe; sources without that
 * proof still require a checkpoint. No completed pixel buffer is allocated to retain a recipe. */
int gml_render_resource_device_source_view(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  GmlRenderResourceSource *out_source);
/* Make a committed CPU source resident once,without observing device pixels or
 * advancing its version. Later reads of that version use the byte-free view. */
int gml_render_resource_prepare_device_source(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  GmlRenderResourceSource *out_source);

/* Legacy submission reserves replay storage before issuing device work. Reconstructible rejected or
 * partially failed batches replay once on CPU. A non-reconstructible DEVICE_CHECKPOINT result is
 * read back before return; a device failure before that checkpoint returns FAILED and retains
 * only the previous valid CPU representation. A witnessed on-demand device instead retains its
 * actual pixels without replay storage or an automatic read. Explicit observation seals them. */
GmlRenderResourceExecution gml_render_resource_submit(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  const GmlRenderDrawBatch *batch,const GmlRenderResourceSource *sources,
  uint32_t source_count);
/* Consecutive device operations may share a pending observation span. Once device-only work
 * enters the span, exact suffix operations also retain device authority. CPU/state observation
 * or a host-required seal materialises canonical pixels. An on-demand host may keep the span
 * across frames while the device and its observation services remain alive. */
GmlRenderResourceExecution gml_render_resource_submit_deferred(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  const GmlRenderDrawBatch *batch,const GmlRenderResourceSource *sources,
  uint32_t source_count);
int gml_render_resource_checkpoint(GmlRenderResource *resource,
                                    const GmlRenderDeviceServices *device);
/* Observe the actual resident device pixels, even when a CPU replay recipe exists.
 * The caller must lend the owning current context through device; no stored service
 * is used when it is absent. Failed observation does not discard a replayable image
 * or publish partially read pixels. Already current CPU pixels need no device call. */
int gml_render_resource_observe_device(GmlRenderResource *resource,
                                        const GmlRenderDeviceServices *device);

/* CPU access materialises first: legacy recipes need no graphics context, while device-only
 * pixels need their owning current context. The caller commits a new logical version after
 * changing the borrowed pixel plane. Failed resident observation never publishes stale pixels. */
int gml_render_resource_acquire_cpu_read(GmlRenderResource *resource,
                                          const uint32_t **out_pixels);
int gml_render_resource_acquire_cpu_write(GmlRenderResource *resource,
                                           uint32_t **out_pixels);
int gml_render_resource_commit_cpu_write(GmlRenderResource *resource);
void gml_render_resource_context_lost(GmlRenderResource *resource);
void gml_render_resource_journal_metrics(const GmlRenderResource *resource,
                                          GmlRenderJournalMetrics *out_metrics);

#endif
