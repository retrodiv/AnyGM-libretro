/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Qualified scene clear and flat primitive spans use the same resolved GL region fill. */
#include "gml_gpu_opengl_internal.h"

static int fill_region(GmlGpuBackend *backend,GlSceneImage *target,
                       const GmlRenderRect *region,uint32_t colour){
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

int gml_render_gpu_opengl_clear(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
                                GlSceneImage *target){
  return fill_region(backend,target,&op->target.region,op->payload.clear_colour);
}

int gml_render_gpu_opengl_draw_primitive(GmlGpuBackend *backend,
                                         const GmlRenderDrawOp *op,GlSceneImage *target){
  return fill_region(backend,target,&op->payload.primitive.region,
                     op->payload.primitive.colour);
}
