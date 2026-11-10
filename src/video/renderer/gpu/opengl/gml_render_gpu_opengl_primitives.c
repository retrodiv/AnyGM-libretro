/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Qualified scene clear and flat primitive spans use the same resolved GL region fill. */
#include "gml_gpu_opengl_internal.h"
#include <math.h>

void gml_gpu_opengl_alpha_cover(GlSceneImage *image,double left,double top,
                               double right,double bottom){
  if(!image->alpha_bounds_known) return;
  if(!isfinite(left) || !isfinite(top) || !isfinite(right) || !isfinite(bottom)){
    image->alpha_bounds_known=0;return;
  }
  left=fmax(0.0,fmin(image->width,floor(left)));
  top=fmax(0.0,fmin(image->height,floor(top)));
  right=fmax(0.0,fmin(image->width,ceil(right)));
  bottom=fmax(0.0,fmin(image->height,ceil(bottom)));
  if(left>=right || top>=bottom) return;
  if(image->alpha_bounds[0]>left) image->alpha_bounds[0]=(int32_t)left;
  if(image->alpha_bounds[1]>top) image->alpha_bounds[1]=(int32_t)top;
  if(image->alpha_bounds[2]<right) image->alpha_bounds[2]=(int32_t)right;
  if(image->alpha_bounds[3]<bottom) image->alpha_bounds[3]=(int32_t)bottom;
}

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
  int ok=backend->gl.GetError()==GL_NO_ERROR;
  int complete=region->x==0 && region->y==0 && region->width==target->width &&
    region->height==target->height;
  if(!ok) target->alpha_bounds_known=0;
  else if(complete){
    target->alpha_bounds_known=1;
    target->alpha_bounds[0]=alpha==0.0f?(int32_t)target->width:0;
    target->alpha_bounds[1]=alpha==0.0f?(int32_t)target->height:0;
    target->alpha_bounds[2]=alpha==0.0f?0:(int32_t)target->width;
    target->alpha_bounds[3]=alpha==0.0f?0:(int32_t)target->height;
  }else if(alpha!=0.0f)
    gml_gpu_opengl_alpha_cover(target,region->x,region->y,
      (double)region->x+region->width,(double)region->y+region->height);
  target->alpha_opaque=ok && alpha==1.0f &&
    ((target->valid && target->alpha_opaque) ||
     (region->x==0 && region->y==0 && region->width==target->width &&
      region->height==target->height));
  return ok;
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
