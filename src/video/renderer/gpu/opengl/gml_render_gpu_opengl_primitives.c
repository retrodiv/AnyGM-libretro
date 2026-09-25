/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Qualified scene clear, using the target's resolved region rather than current GML state. */
#include "gml_gpu_opengl_internal.h"

int gml_render_gpu_opengl_clear(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
                                GlSceneImage *target){
  uint32_t colour=op->payload.clear_colour;
  const GmlRenderRect *region=&op->target.region;
  GLfloat alpha=target->pixel_format==GML_RENDER_PIXEL_XRGB8888
    ?1.0f:(GLfloat)((colour>>24)&255u)/255.0f;
  GLint bottom=(GLint)target->height-(region->y+(GLint)region->height);
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  backend->gl.Viewport(region->x,bottom,(GLsizei)region->width,(GLsizei)region->height);
  backend->gl.Scissor(region->x,bottom,(GLsizei)region->width,(GLsizei)region->height);
  backend->gl.ClearColor((GLfloat)((colour>>16)&255u)/255.0f,
                         (GLfloat)((colour>>8)&255u)/255.0f,
                         (GLfloat)(colour&255u)/255.0f,alpha);
  backend->gl.Clear(GL_COLOR_BUFFER_BIT);
  return backend->gl.GetError()==GL_NO_ERROR;
}
