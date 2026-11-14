/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_DEVICE_H
#define GML_RENDER_DEVICE_H

#include "gml_render_draw.h"

/* Volatile raw depth belongs to the same physical colour image and context.
 * Its epoch/version are independent of colour versions. Epochs increase when
 * a depth domain is reset; versions increase within an epoch. Transfer words
 * are opaque binary32 encodings, never colour channels or normalized depth. */
typedef struct GmlRenderDepthPlaneRef {
  GmlRenderImageRef colour;
  uint32_t width,height;
  uint64_t epoch,version;
} GmlRenderDepthPlaneRef;

typedef struct GmlRenderDepthUpload {
  GmlRenderDepthPlaneRef plane;
  const void *bytes;
  size_t byte_capacity;
  uint32_t stride_bytes;
} GmlRenderDepthUpload;

typedef struct GmlRenderDepthReadback {
  /* Colour identity/incarnation associates storage; its content version may
   * advance independently. Depth epoch/version must still match exactly. */
  GmlRenderDepthPlaneRef plane;
  void *bytes;
  size_t byte_capacity;
  uint32_t stride_bytes;
} GmlRenderDepthReadback;

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
  /* Optional exact RGB-zero query of this current image; transfers only the boolean result. */
  int (*has_rgb)(void *context,GmlRenderImageRef image,int *has_rgb);
  /* Optional side-effect-free ownership proof for on-demand pixel observation. A positive
   * answer keeps successful work resident without retaining CPU replay operations or sources.
   * The host must support observation at CPU/state barriers and controlled destruction. */
  int (*observe_on_demand)(void *context);
  /* Optional immutable-source preparation. full describes every source texel and borrows bytes
   * only during this call. Success makes the requested region available at that exact version;
   * it does not publish uninitialised texels elsewhere. Unknown consumers still use upload. */
  int (*upload_source_region)(void *context,const GmlRenderImageUpload *full,
                              GmlRenderRect region);
  /* Ordered side-plane operation; it never advances the colour image version. */
  int (*clear_depth)(void *context,const GmlRenderTargetRef *target,double depth);
  /* Complete top-origin planes only. Upload publishes after all device work
   * succeeds, leaving a previous plane readable after refusal. Readback writes
   * the caller's bytes only after a complete successful read. Neither changes
   * colour authority. Resize, retirement and context loss invalidate the plane. */
  int (*upload_depth)(void *context,const GmlRenderDepthUpload *transfer);
  int (*readback_depth)(void *context,const GmlRenderDepthReadback *transfer);
} GmlRenderDeviceServices;

#endif
