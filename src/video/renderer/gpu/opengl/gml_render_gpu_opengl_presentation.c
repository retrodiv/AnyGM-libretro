/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing OpenGL rectangle conversion, off-screen target and readback. */
#include "gml_gpu_opengl_internal.h"

#include <stdlib.h>
#include <string.h>

/* AnyGM addresses rows from the top; the framebuffer's own origin is at the bottom. The rectangle
 * conversion happens here and nowhere else, and the shader converts the fragment's row with the
 * same target height, so one asymmetric fixture proves both. */
void gml_gpu_opengl_set_rect(GmlGpuBackend *backend,const GmlPlanRect *rect,uint32_t target_height){
  GLint bottom=(GLint)target_height-(rect->y+(GLint)rect->height);
  backend->gl.Viewport(rect->x,bottom,(GLsizei)rect->width,(GLsizei)rect->height);
  backend->gl.Scissor(rect->x,bottom,(GLsizei)rect->width,(GLsizei)rect->height);
}

/* Bind an off-screen target of the plan's extent, creating or resizing it as needed. */
int gml_gpu_opengl_bind_readback_target(GmlGpuBackend *backend,uint32_t width,uint32_t height,
                                char *error,size_t error_capacity){
  if(!backend->readback_framebuffer){
    backend->gl.GenFramebuffers(1,&backend->readback_framebuffer);
    if(!backend->readback_framebuffer){
      gml_gpu_opengl_record_error(error,error_capacity,"graphics read-back target could not be created");
      return 0;
    }
  }
  if(!backend->readback_texture || backend->readback_width!=width || backend->readback_height!=height){
    if(!backend->readback_texture) backend->gl.GenTextures(1,&backend->readback_texture);
    if(!backend->readback_texture){
      gml_gpu_opengl_record_error(error,error_capacity,"graphics read-back texture could not be created");
      return 0;
    }
    backend->gl.ActiveTexture(GL_TEXTURE0);
    backend->gl.BindTexture(GL_TEXTURE_2D,backend->readback_texture);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)width,(GLsizei)height,0,GL_RGBA,
                           GL_UNSIGNED_BYTE,NULL);
    backend->gl.BindTexture(GL_TEXTURE_2D,0);
    backend->readback_width=width;
    backend->readback_height=height;
  }
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,backend->readback_framebuffer);
  /* The attachment persists on the framebuffer object; it is re-made, and the completeness
   * checked, only when the texture behind it changed. Both calls can be slow on some drivers. */
  if(backend->readback_attached!=backend->readback_texture){
    backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                     backend->readback_texture,0);
    if(backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){
      gml_gpu_opengl_record_error(error,error_capacity,"graphics read-back target is incomplete");
      return 0;
    }
    backend->readback_attached=backend->readback_texture;
  }
  return 1;
}

/* Read the off-screen target into the plan's plane. The device hands rows from the bottom and
 * channels in memory order R,G,B,A; the plane wants rows from the top and the runtime's word. */
/* Rows arrive from the bottom with channels in memory order R,G,B,A; the plane wants rows from the
 * top in the runtime's word. One place does that conversion. */
/* The rectangle the plan reads back, defaulting to the whole target. */
static void readback_extent(const GmlRenderPlan *plan,int *x,int *y,uint32_t *w,uint32_t *h){
  if(plan->readback_rect.width && plan->readback_rect.height){
    *x=plan->readback_rect.x; *y=plan->readback_rect.y;
    *w=plan->readback_rect.width; *h=plan->readback_rect.height;
    return;
  }
  *x=0; *y=0; *w=plan->target_width; *h=plan->target_height;
}

static void readback_convert(GmlRenderPlan *plan,const uint8_t *source_bytes){
  int rx,ry; uint32_t rw,rh;
  readback_extent(plan,&rx,&ry,&rw,&rh);
  (void)rx; (void)ry;
  for(uint32_t row=0;row<rh;row++){
    const uint8_t *source=source_bytes+(size_t)(rh-1u-row)*(size_t)rw*4u;
    uint32_t *target=plan->readback_pixels+(size_t)row*plan->readback_pitch_pixels;
    for(uint32_t column=0;column<rw;column++){
      const uint8_t *px=source+(size_t)column*4u;
      target[column]=((uint32_t)px[3]<<24)|((uint32_t)px[0]<<16)|((uint32_t)px[1]<<8)|(uint32_t)px[2];
    }
  }
}

static int readback_scratch_ready(GmlGpuBackend *backend,size_t needed,char *error,size_t error_capacity){
  if(needed<=backend->readback_scratch_capacity) return 1;
  {
    uint8_t *grown=(uint8_t*)realloc(backend->readback_scratch,needed);
    if(!grown){
      gml_gpu_opengl_record_error(error,error_capacity,"graphics read-back scratch could not be allocated");
      return 0;
    }
    backend->readback_scratch=grown;
    backend->readback_scratch_capacity=needed;
  }
  return 1;
}

/* Read the off-screen target now, waiting for the device. */
int gml_gpu_opengl_read_back_target(GmlGpuBackend *backend,GmlRenderPlan *plan,char *error,size_t error_capacity){
  int rx,ry; uint32_t rw,rh;
  size_t needed;
  readback_extent(plan,&rx,&ry,&rw,&rh);
  needed=(size_t)rw*(size_t)rh*4u;
  if(!readback_scratch_ready(backend,needed,error,error_capacity)) return 0;
  backend->gl.PixelStorei(GL_PACK_ALIGNMENT,1);
  /* The device addresses rows from the bottom, so the rectangle's own origin is measured there. */
  backend->gl.ReadPixels(rx,(GLint)(plan->target_height-(uint32_t)ry-rh),(GLsizei)rw,(GLsizei)rh,
                         GL_RGBA,GL_UNSIGNED_BYTE,backend->readback_scratch);
  readback_convert(plan,backend->readback_scratch);
  return 1;
}

void gml_gpu_opengl_set_readback_pipelined(GmlGpuBackend *backend,int pipelined){
  if(backend) backend->readback_pipelined=pipelined?1:0;
}

static GlReadbackSlot *readback_slot(GmlGpuBackend *backend,uint32_t identity,
                                     uint32_t width,uint32_t height){
  GlReadbackSlot *spare=NULL;
  uint32_t oldest=0xFFFFFFFFu;
  for(size_t index=0;index<GL_READBACK_SLOTS;index++){
    GlReadbackSlot *slot=&backend->readback_slots[index];
    if(slot->buffer[0] && slot->identity==identity && slot->width==width && slot->height==height)
      return slot;
  }
  for(size_t index=0;index<GL_READBACK_SLOTS;index++){
    GlReadbackSlot *slot=&backend->readback_slots[index];
    if(!slot->buffer[0]){ spare=slot; break; }
    if(slot->last_used<oldest){ oldest=slot->last_used; spare=slot; }
  }
  if(!spare) return NULL;
  if(spare->buffer[0]) backend->gl.DeleteBuffers(2,spare->buffer);
  memset(spare,0,sizeof *spare);
  backend->gl.GenBuffers(2,spare->buffer);
  if(!spare->buffer[0] || !spare->buffer[1]) return NULL;
  spare->identity=identity;
  spare->width=width;
  spare->height=height;
  for(int which=0;which<2;which++){
    backend->gl.BindBuffer(GL_PIXEL_PACK_BUFFER,spare->buffer[which]);
    backend->gl.BufferData(GL_PIXEL_PACK_BUFFER,
                           (GLsizeiptr)((size_t)width*height*4u),NULL,GL_STREAM_READ);
  }
  backend->gl.BindBuffer(GL_PIXEL_PACK_BUFFER,0);
  return spare;
}

/* Read the off-screen target without waiting: take what the previous pass of this program left,
 * and start this one. The answer is one frame old, which is what buys the frame time back. */
int gml_gpu_opengl_read_back_target_pipelined(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                      char *error,size_t error_capacity){
  int rx,ry; uint32_t rw,rh;
  size_t bytes;
  GlReadbackSlot *slot;
  readback_extent(plan,&rx,&ry,&rw,&rh);
  bytes=(size_t)rw*(size_t)rh*4u;
  if(!backend->gl.MapBufferRange || !backend->gl.UnmapBuffer)
    return gml_gpu_opengl_read_back_target(backend,plan,error,error_capacity);
  slot=readback_slot(backend,plan->shader.identity,rw,rh);
  if(!slot) return gml_gpu_opengl_read_back_target(backend,plan,error,error_capacity);
  slot->last_used=++backend->use_clock;
  backend->gl.PixelStorei(GL_PACK_ALIGNMENT,1);
  if(!slot->primed){
    /* Nothing to take yet: read once the waiting way, so the first frame is right. */
    if(!gml_gpu_opengl_read_back_target(backend,plan,error,error_capacity)) return 0;
  } else {
    const uint8_t *mapped;
    backend->gl.BindBuffer(GL_PIXEL_PACK_BUFFER,slot->buffer[slot->cursor^1]);
    mapped=(const uint8_t*)backend->gl.MapBufferRange(GL_PIXEL_PACK_BUFFER,0,(GLsizeiptr)bytes,
                                                      GL_MAP_READ_BIT);
    if(!mapped){
      backend->gl.BindBuffer(GL_PIXEL_PACK_BUFFER,0);
      return gml_gpu_opengl_read_back_target(backend,plan,error,error_capacity);
    }
    readback_convert(plan,mapped);
    backend->gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
  }
  /* Start this pass's read; nothing waits on it. */
  backend->gl.BindBuffer(GL_PIXEL_PACK_BUFFER,slot->buffer[slot->cursor]);
  backend->gl.ReadPixels(rx,(GLint)(plan->target_height-(uint32_t)ry-rh),(GLsizei)rw,(GLsizei)rh,
                         GL_RGBA,GL_UNSIGNED_BYTE,(void*)0);
  backend->gl.BindBuffer(GL_PIXEL_PACK_BUFFER,0);
  slot->cursor^=1;
  slot->primed=1;
  return 1;
}
