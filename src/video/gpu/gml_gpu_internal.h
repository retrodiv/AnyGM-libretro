/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_INTERNAL_H
#define GML_GPU_INTERNAL_H

#include "gml_gpu.h"

/* The boundary between the API-neutral GPU facade and the one backend that knows a graphics API.
 * The backend record is opaque here so that no graphics type reaches the facade, and the facade
 * owns everything that is not API-specific: the adopted context, the counters, and the one bounded
 * error string a lifecycle failure leaves behind. */

typedef struct GmlGpuBackend GmlGpuBackend;

struct GmlGpu {
  GmlGpuContext context;
  int context_active;
  GmlGpuBackend *backend;
  GmlGpuCounters counters;
  char error[192];
};

GmlGpuBackend *gml_gpu_gl_create(void);
/* Deletes graphics objects only while the context is still current. */
void gml_gpu_gl_destroy(GmlGpuBackend *backend,int context_is_current);
/* The context went away without notice: drop every handle without issuing a call. */
void gml_gpu_gl_forget(GmlGpuBackend *backend);
int gml_gpu_gl_reset(GmlGpuBackend *backend,const GmlGpuContext *context,
                     char *error,size_t error_capacity);
int gml_gpu_gl_execute(GmlGpuBackend *backend,const GmlGpuContext *context,
                       const GmlRenderPlan *plan,GmlGpuCounters *counters,
                       char *error,size_t error_capacity);

#endif
