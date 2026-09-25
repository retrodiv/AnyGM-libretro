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
  GML_RENDER_RESOURCE_INVALID=5
} GmlRenderResourceAuthority;

typedef enum GmlRenderResourceExecution {
  GML_RENDER_RESOURCE_EXEC_FAILED=0,
  GML_RENDER_RESOURCE_EXEC_DEVICE=1,
  GML_RENDER_RESOURCE_EXEC_CPU=2
} GmlRenderResourceExecution;

typedef struct GmlRenderResourceLimits {
  size_t journal_bytes;
  size_t retained_mutable_bytes;
} GmlRenderResourceLimits;

/* A source version is immutable for the lifetime of one submission. If borrowed_immutable is
 * true, bytes must remain stable until the journal is materialised or the resource is destroyed;
 * authored immutable atlas pages can meet that contract without duplication. Mutable versions
 * are copied once per distinct ref before any device write and charged to the retained budget. */
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
  uint32_t journal_operations,retained_versions;
  uint32_t authority;
} GmlRenderJournalMetrics;

GmlRenderResource *gml_render_resource_create(GmlRenderImageRef image,uint32_t width,
                                               uint32_t height,uint32_t pixel_format,
                                               uint32_t *canonical_pixels,
                                               GmlRenderResourceLimits limits);
void gml_render_resource_destroy(GmlRenderResource *resource);
GmlRenderImageRef gml_render_resource_image_ref(const GmlRenderResource *resource);
int gml_render_resource_source_view(GmlRenderResource *resource,
                                    GmlRenderResourceSource *out_source);

/* Submission reserves replay storage before issuing device work. Reconstructible rejected or
 * partially failed batches replay once on CPU. A non-reconstructible DEVICE_CHECKPOINT result is
 * read back before return; a device failure before that checkpoint returns FAILED and retains
 * only the previous valid CPU representation. */
GmlRenderResourceExecution gml_render_resource_submit(
  GmlRenderResource *resource,const GmlRenderDeviceServices *device,
  const GmlRenderDrawBatch *batch,const GmlRenderResourceSource *sources,
  uint32_t source_count);

/* CPU access is complete without a current graphics context. A write acquisition materialises
 * first; the caller then commits a new logical version after changing the borrowed pixel plane. */
int gml_render_resource_acquire_cpu_read(GmlRenderResource *resource,
                                          const uint32_t **out_pixels);
int gml_render_resource_acquire_cpu_write(GmlRenderResource *resource,
                                           uint32_t **out_pixels);
int gml_render_resource_commit_cpu_write(GmlRenderResource *resource);
void gml_render_resource_context_lost(GmlRenderResource *resource);
void gml_render_resource_journal_metrics(const GmlRenderResource *resource,
                                          GmlRenderJournalMetrics *out_metrics);

#endif
