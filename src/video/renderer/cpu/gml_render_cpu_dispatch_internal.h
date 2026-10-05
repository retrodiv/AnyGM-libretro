/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Header-local software coverage metadata, not another drawing implementation. */
#ifndef GML_RENDER_CPU_DISPATCH_INTERNAL_H
#define GML_RENDER_CPU_DISPATCH_INTERNAL_H

/* Only renderer implementation owners include this after their storage header. */
#ifndef GML_RENDER_INTERNAL_H
#error "CPU dispatch helpers require renderer-owner storage"
#endif
#include "gml_render_cpu_backend.h"
#include <limits.h>
#include <stdatomic.h>

/* Stack-local values group one operation's spans and pixels without storing a
 * stack address in the renderer. Workers mark renderer/queue-owned coverage only. */
typedef GmlRenderCpuBackendDispatchScope GmlRenderDispatchCpuScope;

typedef struct {
  unsigned depth;
  unsigned family,reason;
  int written;
} GmlRenderDispatchCpuSuspension;

/* A pending composition is a distinct draw, not another span of the draw that
 * caused its flush. Only the owner uses this around preparation, before row
 * workers start; all values stay on the stack and no stack pointer is retained. */
static inline void gml_render_dispatch_cpu_scope_suspend(GmlRender *render,
                                               GmlRenderDispatchCpuSuspension *saved){
  saved->depth=render?render->dispatch_cpu_depth:0u;
  saved->family=saved->depth?render->dispatch_cpu_family:GML_RENDER_SCENE_UNCLASSIFIED;
  saved->reason=saved->depth?render->dispatch_cpu_reason:GML_RENDER_DISPATCH_UNQUALIFIED;
  saved->written=saved->depth
    ?atomic_load_explicit(&render->dispatch_cpu_written,memory_order_relaxed):0;
  if(saved->depth) render->dispatch_cpu_depth=0u;
}

static inline void gml_render_dispatch_cpu_scope_resume(GmlRender *render,
                                         const GmlRenderDispatchCpuSuspension *saved){
  if(!saved->depth) return;
  if(render->dispatch_cpu_depth)
    gml_render_dispatch_cpu_record(render,GML_RENDER_SCENE_UNCLASSIFIED,
                                   GML_RENDER_DISPATCH_UNQUALIFIED,1u);
  render->dispatch_cpu_depth=saved->depth;
  render->dispatch_cpu_family=saved->family;
  render->dispatch_cpu_reason=saved->reason;
  atomic_store_explicit(&render->dispatch_cpu_written,saved->written,memory_order_relaxed);
}

static inline void gml_render_dispatch_cpu_scope_begin(GmlRender *render,
                                                       GmlRenderDispatchCpuScope *scope,
                                                       unsigned family){
  scope->reason=GML_RENDER_DISPATCH_UNQUALIFIED;
  scope->active=render && render->dispatch_metrics.enabled;
  if(!scope->active) return;
  if(render->dispatch_cpu_depth==UINT_MAX){
    gml_render_dispatch_cpu_record(render,GML_RENDER_SCENE_UNCLASSIFIED,
                                   GML_RENDER_DISPATCH_UNQUALIFIED,1u);
    scope->active=0;
    return;
  }
  scope->outer=render->dispatch_cpu_depth++==0u;
  scope->family=family;
  scope->reason=gml_render_dispatch_cpu_reason(render);
  scope->exception=GML_RENDER_DISPATCH_EXCEPTION_COUNT;
  if(scope->outer){
    render->dispatch_cpu_family=family;
    render->dispatch_cpu_reason=scope->reason;
    atomic_init(&render->dispatch_cpu_written,0);
  }
}

static inline void gml_render_dispatch_cpu_scope_end(GmlRender *render,
                                                     GmlRenderDispatchCpuScope *scope){
  if(!scope->active) return;
  if(!render->dispatch_cpu_depth){
    gml_render_dispatch_cpu_record(render,GML_RENDER_SCENE_UNCLASSIFIED,
                                   GML_RENDER_DISPATCH_UNQUALIFIED,1u);
    return;
  }
  render->dispatch_cpu_depth--;
  if(!scope->outer || !atomic_load_explicit(&render->dispatch_cpu_written,memory_order_relaxed))
    return;
  if(scope->exception<GML_RENDER_DISPATCH_EXCEPTION_COUNT)
    gml_render_dispatch_cpu_exception_record(render,scope->family,scope->exception,1u);
  else gml_render_dispatch_cpu_record(render,scope->family,scope->reason,1u);
}

/* Queue workers borrow their operation's owned flag only after the queue stops
 * growing. Immediate workers borrow the renderer-owned active scope flag. */
static inline atomic_int *gml_render_dispatch_cpu_scope_coverage(GmlRender *render){
  return render && render->dispatch_cpu_depth?&render->dispatch_cpu_written:NULL;
}

static inline void gml_render_dispatch_cpu_coverage_write(atomic_int *written){
  if(written && !atomic_load_explicit(written,memory_order_relaxed))
    atomic_store_explicit(written,1,memory_order_relaxed);
}

/* No new cross-unit dispatch inside a pixel loop. Disabled scopes do no work. */
static inline void gml_render_dispatch_cpu_scope_write(GmlRender *render){
  gml_render_dispatch_cpu_coverage_write(gml_render_dispatch_cpu_scope_coverage(render));
}

#endif
