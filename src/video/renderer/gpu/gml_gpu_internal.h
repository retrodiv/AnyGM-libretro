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

GmlGpuBackend *gml_gpu_opengl_create(void);
/* Deletes graphics objects only while the context is still current. */
void gml_gpu_opengl_destroy(GmlGpuBackend *backend,int context_is_current);
/* The context went away without notice: drop every handle without issuing a call. */
void gml_gpu_opengl_forget(GmlGpuBackend *backend);
void gml_gpu_opengl_scene_work_counters(const GmlGpuBackend *backend,GmlGpuCounters *counters);
int gml_gpu_opengl_reset(GmlGpuBackend *backend,const GmlGpuContext *context,
                     char *error,size_t error_capacity);
/* Executes the plan. A refusal that is the content program's - it did not compile or link - is
 * reported in the plan's fallback reason as well as the return value, so the caller can retire the
 * program rather than retry it every frame. */
/* Take the pipelined read-back: a pass reads what the previous pass of the same program left and
 * starts its own without waiting. The answer is one frame old, which is what removes the stall. */
void gml_gpu_opengl_set_readback_pipelined(GmlGpuBackend *backend,int pipelined);
int gml_gpu_opengl_present_emergency_frame(GmlGpuBackend *backend,const GmlGpuContext *context,
                                          const uint32_t *pixels,uint32_t width,uint32_t height);
int gml_gpu_opengl_execute(GmlGpuBackend *backend,const GmlGpuContext *context,
                       GmlRenderPlan *plan,GmlGpuCounters *counters,
                       char *error,size_t error_capacity);
int gml_gpu_opengl_capture_completed_frame(GmlGpuBackend *backend,uintptr_t framebuffer,
                                            uint32_t width,uint32_t height,uint64_t generation);
int gml_gpu_opengl_capture_delivered_frame(GmlGpuBackend *backend,uintptr_t framebuffer,
                                            uint32_t width,uint32_t height);
int gml_render_gpu_opengl_repeat_delivered_frame(GmlGpuBackend *backend,uintptr_t framebuffer,
  uint32_t width,uint32_t height,GmlGpuCounters *counters,char *error,size_t error_capacity);
int gml_gpu_opengl_capture_device_presentation(GmlGpuBackend *backend,uintptr_t framebuffer,
                                                uint32_t width,uint32_t height,
                                                uint64_t generation);
int gml_gpu_opengl_capture_scene_image(GmlGpuBackend *backend,GmlRenderImageRef image,
                                        uint32_t width,uint32_t height,uint64_t generation);
int gml_gpu_opengl_read_completed_frame(GmlGpuBackend *backend,uintptr_t framebuffer,
                                         uint32_t *pixels,uint32_t pitch_pixels,
                                         uint32_t width,uint32_t height,uint64_t generation,
                                         GmlGpuCounters *counters);
int gml_render_gpu_opengl_present_transform(GmlGpuBackend *backend,uintptr_t framebuffer,
  uint32_t width,uint32_t height,uint64_t generation,int dx,int dy,unsigned rotation,int scratch,
  GmlGpuCounters *counters,char *error,size_t error_capacity);
int gml_gpu_opengl_scene_capabilities(const GmlGpuBackend *backend,GmlRenderDeviceCaps *caps);
int gml_gpu_opengl_scene_prepare(GmlGpuBackend *backend,uint32_t width,uint32_t height,
                                 char *error,size_t error_capacity);
int gml_gpu_opengl_prepare_presentation(GmlGpuBackend *backend,const GmlGpuContext *context,
                                        GmlGpuCounters *counters,char *error,size_t error_capacity);
int gml_gpu_opengl_scene_upload(GmlGpuBackend *backend,const GmlRenderImageUpload *transfer,
                                int *did_upload);
int gml_gpu_opengl_scene_upload_source_region(GmlGpuBackend *backend,
  const GmlRenderImageUpload *full,GmlRenderRect region,uint64_t *uploaded_bytes);
/* Grants the next offered frame its completion band and returns the completion uploads and
 * bytes defined since the previous call. */
void gml_gpu_opengl_scene_begin_frame(GmlGpuBackend *backend,uint64_t *completion_uploads,
                                      uint64_t *completion_bytes);
int gml_gpu_opengl_scene_readback(GmlGpuBackend *backend,const GmlRenderImageReadback *transfer);
int gml_gpu_opengl_scene_has_rgb(GmlGpuBackend *backend,GmlRenderImageRef image,int *has_rgb);
int gml_gpu_opengl_scene_alpha_nonzero(GmlGpuBackend *backend,GmlRenderImageRef image);
int gml_gpu_opengl_scene_clear_depth(GmlGpuBackend *backend,
                                     const GmlRenderTargetRef *target,double depth);
int gml_gpu_opengl_depth_upload(GmlGpuBackend *backend,const GmlRenderDepthUpload *transfer);
int gml_gpu_opengl_depth_readback(GmlGpuBackend *backend,const GmlRenderDepthReadback *transfer);
void gml_gpu_opengl_scene_retire(GmlGpuBackend *backend,GmlRenderImageRef image);
int gml_gpu_opengl_scene_flush(GmlGpuBackend *backend);
int gml_gpu_opengl_scene_wait(GmlGpuBackend *backend);
GmlRenderExecutionResult gml_gpu_opengl_scene_submit(GmlGpuBackend *backend,
                                                       const GmlRenderDrawBatch *batch,
                                                       uintptr_t frontend_framebuffer,
                                                       char *error,size_t error_capacity);
int gml_gpu_opengl_content_program_failed(const GmlGpuBackend *backend,
                                           const GmlPlanShader *shader);

#endif
