/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The direct OpenGL / OpenGL ES backend. The only file in the tree that names a graphics API.
 *
 * It owns an instance dispatch table resolved through the host's callback, one program, one vertex
 * array, a small set of source-texture mirrors keyed by resource identity and generation, and the
 * integer axis-map textures that make nearest selection exact. It creates no window, opens no
 * library, and keeps no process-global state: a frontend running two cores must not have one
 * instance calling into the other's context. */
#include "gml_gpu_opengl_internal.h"
#include "gml_gpu_opengl_shaders.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gml_gpu_opengl_record_error(char *error,size_t capacity,const char *format,...){
  va_list arguments;
  if(!error || capacity==0) return;
  va_start(arguments,format);
  vsnprintf(error,capacity,format,arguments);
  va_end(arguments);
}

GmlGpuBackend *gml_gpu_opengl_create(void){
  GmlGpuBackend *backend=(GmlGpuBackend*)calloc(1,sizeof *backend);
  if(!backend) return NULL;
  {
    /* The source arrives as XRGB words uploaded as raw bytes, so which component the sampler calls
     * red depends on this machine, not on the graphics API. Decide it once, here, and let the
     * fragment shader be built for the answer. */
    const uint32_t probe=0x01020304u;
    unsigned char bytes[4];
    memcpy(bytes,&probe,sizeof bytes);
    backend->little_endian=bytes[0]==0x04u;
  }
  return backend;
}

/* Forgetting is not deleting. When a context disappears without notice its objects are already
 * gone, and naming them against a different context would delete something that belongs to
 * somebody else. */
void gml_gpu_opengl_forget(GmlGpuBackend *backend){
  if(!backend) return;
  backend->loaded=0;
  backend->program=0;
  backend->vertex_array=0;
  memset(&backend->gl,0,sizeof backend->gl);
  for(size_t index=0;index<GML_PLAN_MAX_IMAGES;index++){
    memset(&backend->sources[index],0,sizeof backend->sources[index]);
  }
  for(size_t index=0;index<GL_MAP_SLOTS;index++){
    memset(&backend->maps[index],0,sizeof backend->maps[index]);
  }
  memset(backend->content,0,sizeof backend->content);
  backend->content_buffer=0;
  memset(backend->readback_slots,0,sizeof backend->readback_slots);
  backend->readback_framebuffer=0;
  backend->readback_texture=0;
  backend->readback_attached=0;
  backend->readback_width=backend->readback_height=0;
}

static void delete_objects(GmlGpuBackend *backend){
  if(!backend->loaded) return;
  for(size_t index=0;index<GML_PLAN_MAX_IMAGES;index++)
    if(backend->sources[index].name)
      backend->gl.DeleteTextures(1,&backend->sources[index].name);
  for(size_t index=0;index<GL_MAP_SLOTS;index++)
    if(backend->maps[index].name)
      backend->gl.DeleteTextures(1,&backend->maps[index].name);
  if(backend->program) backend->gl.DeleteProgram(backend->program);
  for(size_t index=0;index<GL_CONTENT_PROGRAMS;index++)
    if(backend->content[index].program) backend->gl.DeleteProgram(backend->content[index].program);
  if(backend->content_buffer && backend->gl.DeleteBuffers)
    backend->gl.DeleteBuffers(1,&backend->content_buffer);
  for(size_t index=0;index<GL_READBACK_SLOTS;index++)
    if(backend->readback_slots[index].buffer[0] && backend->gl.DeleteBuffers)
      backend->gl.DeleteBuffers(2,backend->readback_slots[index].buffer);
  if(backend->readback_framebuffer && backend->gl.DeleteFramebuffers)
    backend->gl.DeleteFramebuffers(1,&backend->readback_framebuffer);
  if(backend->readback_texture) backend->gl.DeleteTextures(1,&backend->readback_texture);
  if(backend->vertex_array && backend->gl.DeleteVertexArrays)
    backend->gl.DeleteVertexArrays(1,&backend->vertex_array);
}

void gml_gpu_opengl_destroy(GmlGpuBackend *backend,int context_is_current){
  if(!backend) return;
  if(context_is_current) delete_objects(backend);
  free(backend->map_scratch);
  free(backend->readback_scratch);
  backend->readback_scratch=NULL;
  backend->readback_scratch_capacity=0;
  free(backend);
}

/* Every entry point this backend calls. A name that the host cannot resolve is named exactly and
 * rejects initialization, because the alternative is a null call at the first frame. */
#define GL_MANDATORY_ENTRY_POINTS(X) \
  X(GetError,"glGetError") \
  X(GetString,"glGetString") \
  X(Enable,"glEnable") \
  X(Disable,"glDisable") \
  X(Viewport,"glViewport") \
  X(Scissor,"glScissor") \
  X(ClearColor,"glClearColor") \
  X(Clear,"glClear") \
  X(ColorMask,"glColorMask") \
  X(DepthMask,"glDepthMask") \
  X(BindFramebuffer,"glBindFramebuffer") \
  X(GenTextures,"glGenTextures") \
  X(DeleteTextures,"glDeleteTextures") \
  X(BindTexture,"glBindTexture") \
  X(ActiveTexture,"glActiveTexture") \
  X(TexParameteri,"glTexParameteri") \
  X(TexImage2D,"glTexImage2D") \
  X(TexSubImage2D,"glTexSubImage2D") \
  X(PixelStorei,"glPixelStorei") \
  X(CreateShader,"glCreateShader") \
  X(ShaderSource,"glShaderSource") \
  X(CompileShader,"glCompileShader") \
  X(GetShaderiv,"glGetShaderiv") \
  X(GetShaderInfoLog,"glGetShaderInfoLog") \
  X(DeleteShader,"glDeleteShader") \
  X(CreateProgram,"glCreateProgram") \
  X(AttachShader,"glAttachShader") \
  X(LinkProgram,"glLinkProgram") \
  X(GetProgramiv,"glGetProgramiv") \
  X(GetProgramInfoLog,"glGetProgramInfoLog") \
  X(UseProgram,"glUseProgram") \
  X(DeleteProgram,"glDeleteProgram") \
  X(GetUniformLocation,"glGetUniformLocation") \
  X(Uniform1i,"glUniform1i") \
  X(GenVertexArrays,"glGenVertexArrays") \
  X(BindVertexArray,"glBindVertexArray") \
  X(DeleteVertexArrays,"glDeleteVertexArrays") \
  X(DrawArrays,"glDrawArrays") \
  X(GetAttribLocation,"glGetAttribLocation") \
  X(VertexAttribPointer,"glVertexAttribPointer") \
  X(EnableVertexAttribArray,"glEnableVertexAttribArray") \
  X(DisableVertexAttribArray,"glDisableVertexAttribArray") \
  X(GenBuffers,"glGenBuffers") \
  X(BindBuffer,"glBindBuffer") \
  X(BufferData,"glBufferData") \
  X(DeleteBuffers,"glDeleteBuffers") \
  X(Uniform1fv,"glUniform1fv") \
  X(Uniform2fv,"glUniform2fv") \
  X(Uniform3fv,"glUniform3fv") \
  X(Uniform4fv,"glUniform4fv") \
  X(Uniform1iv,"glUniform1iv") \
  X(Uniform2iv,"glUniform2iv") \
  X(Uniform3iv,"glUniform3iv") \
  X(Uniform4iv,"glUniform4iv") \
  X(UniformMatrix4fv,"glUniformMatrix4fv") \
  X(UniformMatrix3fv,"glUniformMatrix3fv") \
  X(UniformMatrix2fv,"glUniformMatrix2fv") \
  X(GetActiveUniform,"glGetActiveUniform") \
  X(GenFramebuffers,"glGenFramebuffers") \
  X(DeleteFramebuffers,"glDeleteFramebuffers") \
  X(FramebufferTexture2D,"glFramebufferTexture2D") \
  X(CheckFramebufferStatus,"glCheckFramebufferStatus") \
  X(ReadPixels,"glReadPixels") \
  X(MapBufferRange,"glMapBufferRange") \
  X(UnmapBuffer,"glUnmapBuffer")

static int load_entry_points(GmlGpuBackend *backend,const GmlGpuContext *context,
                             char *error,size_t error_capacity){
  GmlGpuProc proc;
#define GL_LOAD_ONE(field,name) \
  proc=context->get_proc_address(context->userdata,name); \
  if(!proc){ gml_gpu_opengl_record_error(error,error_capacity,"missing graphics entry point: %s",name); return 0; } \
  memcpy(&backend->gl.field,&proc,sizeof proc);
  GL_MANDATORY_ENTRY_POINTS(GL_LOAD_ONE)
#undef GL_LOAD_ONE
  return 1;
}


int gml_gpu_opengl_reset(GmlGpuBackend *backend,const GmlGpuContext *context,
                     char *error,size_t error_capacity){
  if(!backend || !context) return 0;
  gml_gpu_opengl_forget(backend);
  backend->api=context->api;
  if(!load_entry_points(backend,context,error,error_capacity)) return 0;
  backend->loaded=1;
  /* Drain whatever the frontend left behind so that a later check reports this backend's own
   * failure rather than somebody else's. */
  while(backend->gl.GetError()!=GL_NO_ERROR){}
  if(!gml_gpu_opengl_build_program(backend,error,error_capacity)){ backend->loaded=0; return 0; }
  backend->gl.GenVertexArrays(1,&backend->vertex_array);
  if(!backend->vertex_array){
    gml_gpu_opengl_record_error(error,error_capacity,"graphics vertex array could not be created");
    backend->gl.DeleteProgram(backend->program);
    backend->program=0;
    backend->loaded=0;
    return 0;
  }
  backend->max_texture_size=0;
  return 1;
}

/* AnyGM addresses rows from the top; the framebuffer's own origin is at the bottom. The rectangle
 * conversion happens here and nowhere else, and the shader converts the fragment's row with the
 * same target height, so one asymmetric fixture proves both. */
void gml_gpu_opengl_set_rect(GmlGpuBackend *backend,const GmlPlanRect *rect,uint32_t target_height){
  GLint bottom=(GLint)target_height-(rect->y+(GLint)rect->height);
  backend->gl.Viewport(rect->x,bottom,(GLsizei)rect->width,(GLsizei)rect->height);
  backend->gl.Scissor(rect->x,bottom,(GLsizei)rect->width,(GLsizei)rect->height);
}

static GmlPlanAxis identity_axis(uint32_t extent){
  GmlPlanAxis axis;
  memset(&axis,0,sizeof axis);
  axis.rule=GML_PLAN_AXIS_PIXEL_CENTRE;
  axis.source_extent=extent;
  axis.destination_extent=extent;
  axis.destination_first=0;
  axis.origin=0.0;
  axis.extent=(double)extent;
  return axis;
}

/* Bind an off-screen target of the plan's extent, creating or resizing it as needed. */
static int bind_readback_target(GmlGpuBackend *backend,uint32_t width,uint32_t height,
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
static int read_back_target(GmlGpuBackend *backend,GmlRenderPlan *plan,char *error,size_t error_capacity){
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
static int read_back_target_pipelined(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                      char *error,size_t error_capacity){
  int rx,ry; uint32_t rw,rh;
  size_t bytes;
  GlReadbackSlot *slot;
  readback_extent(plan,&rx,&ry,&rw,&rh);
  bytes=(size_t)rw*(size_t)rh*4u;
  if(!backend->gl.MapBufferRange || !backend->gl.UnmapBuffer)
    return read_back_target(backend,plan,error,error_capacity);
  slot=readback_slot(backend,plan->shader.identity,rw,rh);
  if(!slot) return read_back_target(backend,plan,error,error_capacity);
  slot->last_used=++backend->use_clock;
  backend->gl.PixelStorei(GL_PACK_ALIGNMENT,1);
  if(!slot->primed){
    /* Nothing to take yet: read once the waiting way, so the first frame is right. */
    if(!read_back_target(backend,plan,error,error_capacity)) return 0;
  } else {
    const uint8_t *mapped;
    backend->gl.BindBuffer(GL_PIXEL_PACK_BUFFER,slot->buffer[slot->cursor^1]);
    mapped=(const uint8_t*)backend->gl.MapBufferRange(GL_PIXEL_PACK_BUFFER,0,(GLsizeiptr)bytes,
                                                      GL_MAP_READ_BIT);
    if(!mapped){
      backend->gl.BindBuffer(GL_PIXEL_PACK_BUFFER,0);
      return read_back_target(backend,plan,error,error_capacity);
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

int gml_gpu_opengl_execute(GmlGpuBackend *backend,const GmlGpuContext *context,
                       GmlRenderPlan *plan,GmlGpuCounters *counters,
                       char *error,size_t error_capacity){
  uintptr_t framebuffer;
  int failed=0;
  if(!backend || !backend->loaded || !context || !plan) return 0;
  framebuffer=context->get_current_framebuffer(context->userdata);
  if(plan->target==GML_PLAN_TARGET_READBACK){
    if(!bind_readback_target(backend,plan->target_width,plan->target_height,error,error_capacity))
      return 0;
  } else {
    /* Handle zero is a valid target: some frontends hand over the default framebuffer. */
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);
  }

  /* The frontend and this core share one context, so every piece of state a pass depends on is set
   * rather than assumed, and nothing is left enabled that the frontend did not enable itself. */
  backend->gl.Disable(GL_BLEND);
  backend->gl.Disable(GL_DEPTH_TEST);
  backend->gl.Disable(GL_STENCIL_TEST);
  backend->gl.Disable(GL_CULL_FACE);
  backend->gl.Disable(GL_DITHER);
  /* Exact 8-bit output: an sRGB-encoding write would change every channel value. Desktop OpenGL
   * exposes the switch; OpenGL ES 3.0 has no default-framebuffer sRGB enable to turn off. */
  if(backend->api==GML_GPU_API_OPENGL_CORE) backend->gl.Disable(GL_FRAMEBUFFER_SRGB);
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  backend->gl.DepthMask(GL_FALSE);
  backend->gl.Enable(GL_SCISSOR_TEST);
  backend->gl.BindVertexArray(backend->vertex_array);
  backend->gl.UseProgram(backend->program);
  backend->gl.Uniform1i(backend->u_source,0);
  backend->gl.Uniform1i(backend->u_map_x,1);
  backend->gl.Uniform1i(backend->u_map_y,2);
  backend->gl.Uniform1i(backend->u_target_height,(GLint)plan->target_height);

  for(uint32_t index=0;index<plan->operation_count;index++){
    const GmlPlanOp *op=&plan->operations[index];
    if(op->opcode==GML_PLAN_OP_SHADER_DRAW){
      if(!gml_gpu_opengl_execute_shader_draw(backend,plan,op,counters,error,error_capacity)){ failed=1; break; }
      continue;
    }
    if(op->opcode==GML_PLAN_OP_CLEAR_XRGB){
      gml_gpu_opengl_set_rect(backend,&op->destination,plan->target_height);
      /* Only fully black is executed here. Zero and one convert to a byte exactly on every
       * implementation; an arbitrary float colour does not, and the plan is refused rather than
       * accepting a value that two drivers would round differently. */
      backend->gl.ClearColor(0.0f,0.0f,0.0f,1.0f);
      backend->gl.Clear(GL_COLOR_BUFFER_BIT);
      continue;
    }
    {
      const GmlPlanImage *image=&plan->images[op->source];
      GmlPlanAxis axis_x=op->axis_x;
      GmlPlanAxis axis_y=op->axis_y;
      GLuint source_name=0,map_x=0,map_y=0;
      if(op->opcode==GML_PLAN_OP_PRESENT_CPU_FRAME){
        axis_x=identity_axis(image->width);
        axis_y=identity_axis(image->height);
      }
      if(!gml_gpu_opengl_upload_source(backend,image,counters,&source_name,op->linear?1:0)){
        gml_gpu_opengl_record_error(error,error_capacity,"graphics source upload failed");
        failed=1;
        break;
      }
      if(!gml_gpu_opengl_upload_axis(backend,index*2u,&axis_x,op->destination.width,counters,&map_x) ||
         !gml_gpu_opengl_upload_axis(backend,index*2u+1u,&axis_y,op->destination.height,counters,&map_y)){
        gml_gpu_opengl_record_error(error,error_capacity,"graphics axis map upload failed");
        failed=1;
        break;
      }
      gml_gpu_opengl_set_rect(backend,&op->destination,plan->target_height);
      backend->gl.Uniform1i(backend->u_dest_x,op->destination.x);
      backend->gl.Uniform1i(backend->u_dest_y,op->destination.y);
      backend->gl.ActiveTexture(GL_TEXTURE0);
      backend->gl.BindTexture(GL_TEXTURE_2D,source_name);
      backend->gl.ActiveTexture(GL_TEXTURE1);
      backend->gl.BindTexture(GL_TEXTURE_2D,map_x);
      backend->gl.ActiveTexture(GL_TEXTURE2);
      backend->gl.BindTexture(GL_TEXTURE_2D,map_y);
      backend->gl.DrawArrays(GL_TRIANGLES,0,3);
      if(counters) counters->draw_calls++;
    }
  }

  backend->gl.ActiveTexture(GL_TEXTURE2);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE1);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.UseProgram(0);
  backend->gl.BindVertexArray(0);
  backend->gl.Disable(GL_SCISSOR_TEST);
  {
    GLenum status=backend->gl.GetError();
    if(status!=GL_NO_ERROR){
      gml_gpu_opengl_record_error(error,error_capacity,"graphics execution reported error 0x%04X",
                   (unsigned)status);
      while(backend->gl.GetError()!=GL_NO_ERROR){}
      failed=1;
    }
  }
  /* A pass that stopped part way has written something the plan does not describe. Report the
   * failure so the caller replays the complete pass, which overwrites it. */
  if(!failed && plan->target==GML_PLAN_TARGET_READBACK){
    if(!(backend->readback_pipelined
           ?read_back_target_pipelined(backend,plan,error,error_capacity)
           :read_back_target(backend,plan,error,error_capacity))) failed=1;
    /* The frontend's own framebuffer is what a pass leaves bound. */
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);
  }
  return failed?0:1;
}
