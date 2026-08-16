/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_H
#define GML_GPU_H

#include <stddef.h>
#include <stdint.h>

#include "gml_render_plan.h"

/* The GPU subsystem boundary. Everything here is API-neutral: no graphics API type and no
 * frontend-adapter type appears, and the only way into a graphics API is the entry-point callback
 * the host supplied.
 *
 * A GmlGpu is a derived cache with a lifecycle, never state. Nothing it owns is serialized under
 * any schema, and losing all of it costs a rebuild rather than a wrong frame. */

typedef struct GmlGpu GmlGpu;

typedef uint32_t GmlGpuApi;
enum {
  GML_GPU_API_OPENGL_CORE=1,
  GML_GPU_API_OPENGLES3=2
};

typedef void (*GmlGpuProc)(void);
typedef GmlGpuProc (*GmlGpuGetProcFn)(void *userdata,const char *name);
typedef uintptr_t (*GmlGpuGetFramebufferFn)(void *userdata);

typedef struct GmlGpuContext {
  GmlGpuApi api;
  uint32_t version_major;
  uint32_t version_minor;
  void *userdata;
  GmlGpuGetProcFn get_proc_address;
  GmlGpuGetFramebufferFn get_current_framebuffer;
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

/* Execute a complete plan against the host's current framebuffer. Returns zero without publishing
 * partial work when any part of the plan cannot be executed exactly, so the caller can replay the
 * whole pass in software. The plan's fallback reason names why. */
int gml_gpu_execute_plan(GmlGpu *gpu,GmlRenderPlan *plan);

void gml_gpu_counters(const GmlGpu *gpu,GmlGpuCounters *counters);
/* The most recent lifecycle failure, as a bounded, content-neutral string, or NULL. */
const char *gml_gpu_last_error(const GmlGpu *gpu);

#endif
