/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Qualified scene clear and flat primitive spans use the same resolved GL region fill. */
#include "gml_gpu_opengl_internal.h"
#include <math.h>
#include <string.h>

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
  if(image->alpha_tiles_full) return;
  uint32_t tile_width=(image->width+31u)/32u,tile_height=(image->height+31u)/32u;
  uint32_t first=(uint32_t)left/tile_width,last=((uint32_t)right-1u)/tile_width;
  uint32_t mask=UINT32_MAX<<first;
  if(last<31u) mask&=(1u<<(last+1u))-1u;
  for(uint32_t row=(uint32_t)top/tile_height;row<=((uint32_t)bottom-1u)/tile_height;row++)
    image->alpha_tiles[row]|=mask;
  if(left==0.0 && top==0.0 && right==image->width && bottom==image->height)
    image->alpha_tiles_full=1;
}

static int fill_region(GmlGpuBackend *backend,GlSceneImage *target,
                       const GmlRenderRect *region,uint32_t colour){
  GLfloat alpha=target->pixel_format==GML_RENDER_PIXEL_XRGB8888
    ?1.0f:(GLfloat)((colour>>24)&255u)/255.0f;
  int complete=region->x==0 && region->y==0 && region->width==target->width &&
    region->height==target->height;
  int nonzero=alpha!=0.0f && (complete || (target->valid && target->alpha_nonzero));
  int outcome=0,ok;
  if(!complete){
    /* A clipped clear can invoke an unprepared driver shader. Reuse the ordinary unblended
     * colour kernel and placed rectangle instead of delegating that draw to the driver. */
    outcome=gml_gpu_opengl_fill_region_quad(backend,target,region,colour);
  }
  if(outcome) ok=outcome>0;
  else{
    const GLfloat channels[4]={(GLfloat)((colour>>16)&255u)/255.0f,
      (GLfloat)((colour>>8)&255u)/255.0f,(GLfloat)(colour&255u)/255.0f,alpha};
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
    backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    GLint bottom=(GLint)target->height-(region->y+(GLint)region->height);
    backend->gl.Viewport(region->x,bottom,(GLsizei)region->width,(GLsizei)region->height);
    backend->gl.Scissor(region->x,bottom,(GLsizei)region->width,(GLsizei)region->height);
    backend->gl.ClearColor(channels[0],channels[1],channels[2],channels[3]);
    if(complete && backend->gl.InvalidateFramebuffer && backend->gl.ClearBufferfv){
      /* Every channel is replaced. Let a tiled device discard its obsolete attachment and
       * clear the whole storage, while earlier ordered consumers retain their own reads. */
      const GLenum attachment=GL_COLOR_ATTACHMENT0;
      backend->gl.InvalidateFramebuffer(GL_FRAMEBUFFER,1,&attachment);
      backend->gl.Disable(GL_SCISSOR_TEST);
      backend->gl.ClearBufferfv(GL_COLOR,0,channels);
      backend->gl.Enable(GL_SCISSOR_TEST);
    }else backend->gl.Clear(GL_COLOR_BUFFER_BIT);
    ok=backend->gl.GetError()==GL_NO_ERROR;
  }
  if(!ok) target->alpha_bounds_known=0;
  else if(complete){
    target->alpha_bounds_known=1;
    target->alpha_tiles_full=alpha!=0.0f;
    memset(target->alpha_tiles,alpha==0.0f?0:255,sizeof target->alpha_tiles);
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
  target->alpha_nonzero=ok && nonzero;
  return ok;
}

int gml_render_gpu_opengl_clear(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
                                GlSceneImage *target){
  return fill_region(backend,target,&op->target.region,op->payload.clear_colour);
}

int gml_gpu_opengl_scene_clear_depth(GmlGpuBackend *backend,
                                     const GmlRenderTargetRef *target,double depth){
  if(!backend || !target || !isfinite(depth) || !backend->loaded ||
     !backend->gl.ClearBufferfv || !backend->gl.GenRenderbuffers ||
     !backend->gl.BindRenderbuffer || !backend->gl.RenderbufferStorage ||
     !backend->gl.FramebufferRenderbuffer || !backend->gl.DeleteRenderbuffers) return 0;
  GLint maximum=0,write_mask=0,scissor=0,old_renderbuffer=0;
  backend->gl.GetIntegerv(GL_MAX_RENDERBUFFER_SIZE,&maximum);
  if(maximum<=0 || target->width>(uint32_t)maximum || target->height>(uint32_t)maximum)
    return 0;
  GlSceneImage *image=gml_gpu_opengl_scene_target(backend,target);
  if(!image) return 0;
  backend->gl.GetIntegerv(GL_RENDERBUFFER_BINDING,&old_renderbuffer);
  if(!image->depth_renderbuffer){
    GLuint attachment=0;
    backend->gl.GenRenderbuffers(1,&attachment);
    if(!attachment) return 0;
    backend->gl.BindRenderbuffer(GL_RENDERBUFFER,attachment);
    backend->gl.RenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,
                                    (GLsizei)target->width,(GLsizei)target->height);
    backend->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,
                                        GL_RENDERBUFFER,attachment);
    int complete=backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    if(backend->gl.GetError()!=GL_NO_ERROR || !complete){
      backend->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,0);
      backend->gl.DeleteRenderbuffers(1,&attachment);
      backend->gl.BindRenderbuffer(GL_RENDERBUFFER,(GLuint)old_renderbuffer);
      return 0;
    }
    image->depth_renderbuffer=attachment;
  }
  backend->gl.GetIntegerv(GL_DEPTH_WRITEMASK,&write_mask);
  backend->gl.GetIntegerv(GL_SCISSOR_TEST,&scissor);
  backend->gl.Disable(GL_SCISSOR_TEST);
  backend->gl.DepthMask(GL_TRUE);
  GLfloat value=(GLfloat)fmax(0,fmin(1,depth));
  backend->gl.ClearBufferfv(GL_DEPTH,0,&value);
  int succeeded=backend->gl.GetError()==GL_NO_ERROR;
  backend->gl.DepthMask(write_mask?GL_TRUE:GL_FALSE);
  if(scissor) backend->gl.Enable(GL_SCISSOR_TEST);
  backend->gl.BindRenderbuffer(GL_RENDERBUFFER,(GLuint)old_renderbuffer);
  return succeeded && backend->gl.GetError()==GL_NO_ERROR;
}

int gml_render_gpu_opengl_draw_primitive(GmlGpuBackend *backend,
                                         const GmlRenderDrawOp *op,GlSceneImage *target){
  return fill_region(backend,target,&op->payload.primitive.region,
                     op->payload.primitive.colour);
}
