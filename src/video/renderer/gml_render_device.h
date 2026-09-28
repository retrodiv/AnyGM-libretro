/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_DEVICE_H
#define GML_RENDER_DEVICE_H

#include "gml_render_draw.h"

/* Renderer-facing service. Only engine_graphics binds it to a concrete device; shared draw
 * preparation and the software executor never include a GPU-private or graphics-API header. */
typedef struct GmlRenderDeviceServices {
  void *context;
  int (*capabilities)(void *context,GmlRenderDeviceCaps *out_caps);
  GmlRenderExecutionResult (*submit)(void *context,const GmlRenderDrawBatch *batch);
  int (*upload)(void *context,const GmlRenderImageUpload *transfer);
  int (*readback)(void *context,const GmlRenderImageReadback *transfer);
  void (*retire)(void *context,GmlRenderImageRef image);
  int (*flush)(void *context);
  int (*wait)(void *context);
  /* Query a cached compiler refusal for the exact scene program after a failed submission. */
  int (*content_program_failed)(void *context,const struct GmlPlanShader *shader);
} GmlRenderDeviceServices;

#endif
