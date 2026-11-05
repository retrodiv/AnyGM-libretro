/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_H
#define GML_GPU_H

#include <stddef.h>
#include <stdint.h>

#include "gml_render_plan.h"
#include "gml_render_device.h"

/* The GPU subsystem boundary. Everything here is API-neutral: no graphics API type and no
 * frontend-adapter type appears, and the only way into a graphics API is the entry-point callback
 * the host supplied.
 *
 * Context handles and dispatch are never serialized. Immutable sources are rebuilt after loss;
 * GPU-owned persistent/completed pixels are observed before controlled destruction. Unexpected
 * loss is a separate mutable-resource policy, not permission to publish stale CPU pixels. */

typedef struct GmlGpu GmlGpu;

typedef uint32_t GmlGpuApi;
enum {
  GML_GPU_API_OPENGL_CORE=1,
  GML_GPU_API_OPENGLES3=2
};

typedef void (*GmlGpuProc)(void);
typedef GmlGpuProc (*GmlGpuGetProcFn)(void *userdata,const char *name);
typedef uintptr_t (*GmlGpuGetFramebufferFn)(void *userdata);
typedef uint32_t (*GmlGpuIsCurrentFn)(void *userdata);

typedef struct GmlGpuContext {
  GmlGpuApi api;
  uint32_t version_major;
  uint32_t version_minor;
  void *userdata;
  GmlGpuGetProcFn get_proc_address;
  GmlGpuGetFramebufferFn get_current_framebuffer;
  GmlGpuIsCurrentFn is_current;
} GmlGpuContext;

/* Opt-in, content-neutral counters. A normal build pays nothing for these beyond the increments
 * themselves, which happen at pass granularity. */
typedef struct GmlGpuCounters {
  uint32_t context_resets;
  uint32_t context_destroys;
  uint32_t context_losses;
  uint32_t program_failures;
  uint32_t frames_offered;
  uint32_t passes_accepted;
  uint32_t passes_replayed;
  uint32_t cpu_upload_frames;
  uint32_t draw_calls;
  uint32_t full_uploads;
  uint64_t uploaded_bytes;
  uint32_t fallbacks[GML_PLAN_FALLBACK_COUNT];
  /* Resident scene counters are separate from the older presentation-plan transport counters.
   * An operation is accepted only after its entire batch was accepted. */
  uint64_t scene_batches_offered,scene_batches_accepted,scene_batches_refused;
  uint64_t scene_partial_prefix;
  uint64_t scene_operations[GML_RENDER_OPERATION_COUNT];
  uint64_t scene_families[GML_RENDER_SCENE_FAMILY_COUNT];
  uint64_t scene_uploads,scene_uploaded_bytes,scene_readbacks,scene_readback_bytes;
  uint64_t scene_readback_masks,scene_readback_mask_bytes;
  uint64_t scene_rgb_queries,scene_rgb_query_refusals;
  uint64_t completed_frame_copies,completed_frame_copy_bytes;
  uint64_t completed_frame_reads,completed_frame_read_bytes;
  /* Successful backend work, cumulative across context loss and reset. */
  uint64_t scene_target_attachments,scene_target_reuses,scene_white_unit_clears;
} GmlGpuCounters;

/* Creation allocates CPU metadata only and issues no graphics call, so an engine may own a GmlGpu
 * before any context exists and after one is lost. */
GmlGpu *gml_gpu_create(void);
/* Deletes objects in the host context only while it is still current; otherwise forgets them. */
void gml_gpu_destroy(GmlGpu *gpu,int context_is_current);

/* Adopt a context, or a recreated one. A reset with no destroy before it means the previous
 * context is already gone: previous handles are forgotten, never deleted against the new one. */
int gml_gpu_context_reset(GmlGpu *gpu,const GmlGpuContext *context);
/* The context went away without notice. Forget every handle without issuing a call. */
void gml_gpu_context_lost(GmlGpu *gpu);
int gml_gpu_context_active(const GmlGpu *gpu);
/* Whether the host installed an access witness; this metadata query does not invoke it. */
int gml_gpu_context_has_access_proof(const GmlGpu *gpu);
/* Host proof of owning-thread access, not merely an adopted dispatch table. */
int gml_gpu_context_is_current(const GmlGpu *gpu);

/* Prepare fixed scene programs, geometry and destination-copy storage at a load boundary.
 * Requires current context access; private terminal/composition draws warm the driver without reading
 * pixels or publishing an image. Repeated calls reuse resources; failure retains lazy retry. */
int gml_gpu_prepare_scene(GmlGpu *gpu,uint32_t width,uint32_t height);

/* Copies an API-neutral scene service table. Capabilities state the currently qualified scene
 * variants; unsupported submissions are rejected before a device write. */
void gml_gpu_device_services(GmlGpu *gpu,GmlRenderDeviceServices *out_services);

/* Execute a complete plan against the host's current framebuffer. Returns zero without publishing
 * partial work when any part of the plan cannot be executed exactly, so the caller can replay the
 * whole pass in software. The plan's fallback reason names why. */
int gml_gpu_execute_plan(GmlGpu *gpu,GmlRenderPlan *plan);

/* Preserve a completed host target in a separate device image before the frontend reuses it.
 * Both calls require the host's owning-context proof. Capture performs no CPU read; observation
 * publishes top-origin ARGB words only after a successful full read of the matching generation.
 * Other readback plans cannot overwrite this image. Context loss forgets it without a call. */
int gml_gpu_capture_completed_frame(GmlGpu *gpu,uint32_t width,uint32_t height,
                                   uint64_t generation);
int gml_gpu_read_completed_frame(GmlGpu *gpu,uint32_t *pixels,uint32_t pitch_pixels,
                                uint32_t width,uint32_t height,uint64_t generation);
/* Redisplay the unchanged completed image, or the immediately preceding device-only
 * presentation plan, with an integer host offset and opaque black vacated bands.
 * The latter is single-use scratch, never state authority. */
int gml_gpu_present_shifted_frame(GmlGpu *gpu,uint32_t width,uint32_t height,
                                  uint64_t generation,int dx,int dy,int scratch);

void gml_gpu_counters(const GmlGpu *gpu,GmlGpuCounters *counters);
/* The most recent lifecycle failure, as a bounded, content-neutral string, or NULL. */
/* Take the pipelined read-back for shaded draws inside a frame: correct, one frame late, and it
 * does not stall the device. */
void gml_gpu_set_readback_pipelined(GmlGpu *gpu,int pipelined);
const char *gml_gpu_last_error(const GmlGpu *gpu);

#endif
