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
  gml_gpu_opengl_scene_forget(backend);
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
  backend->completed_framebuffer=backend->completed_texture=0;
  backend->completed_width=backend->completed_height=0;
  backend->completed_generation=0;
  backend->completed_valid=backend->completed_attached=0;
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
  if(backend->completed_framebuffer)
    backend->gl.DeleteFramebuffers(1,&backend->completed_framebuffer);
  if(backend->completed_texture) backend->gl.DeleteTextures(1,&backend->completed_texture);
  if(backend->vertex_array && backend->gl.DeleteVertexArrays)
    backend->gl.DeleteVertexArrays(1,&backend->vertex_array);
}

void gml_gpu_opengl_destroy(GmlGpuBackend *backend,int context_is_current){
  if(!backend) return;
  gml_gpu_opengl_scene_destroy(backend,context_is_current);
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
  gml_gpu_opengl_scene_reset(backend,context);
  return 1;
}
