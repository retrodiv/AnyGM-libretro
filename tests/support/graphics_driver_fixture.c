/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The fake graphics driver. See graphics_driver_fixture.h for why it exists and what it can and
 * cannot prove. */
#include "graphics_driver_fixture.h"

#include "gml_gpu_gl_api.h"

#include <stdio.h>
#include <string.h>

enum { FAKE_MAX_TEXTURES=32,FAKE_MAX_CALLS=4096,FAKE_MAX_PIXELS=1u<<16 };

typedef struct FakeTexture {
  int created;
  int deleted;
  GLint internal_format;
  GLsizei width;
  GLsizei height;
  uint32_t upload_count;
  unsigned char bytes[FAKE_MAX_PIXELS*4];
  size_t byte_count;
} FakeTexture;

typedef struct FakeDriver {
  /* Which entry point the loader is not allowed to resolve, so that a missing one can be tested
   * exactly rather than by removing code. */
  const char *withhold;
  int deletes_issued;
  int calls_after_forget;
  int allow_calls;
  GLuint next_name;
  FakeTexture textures[FAKE_MAX_TEXTURES];
  GLuint bound_texture[3];
  GLuint active_unit;
  GLuint framebuffer;
  uint32_t framebuffer_queries;
  GLuint program;
  GLuint vertex_array;
  GLuint bound_program;
  GLuint bound_vertex_array;
  int blend_enabled;
  int depth_enabled;
  int stencil_enabled;
  int cull_enabled;
  int dither_enabled;
  int scissor_enabled;
  int color_mask_all;
  int depth_mask;
  GLint viewport[4];
  GLint scissor[4];
  GLint clear_calls;
  GLfloat clear_colour[4];
  GLint draw_calls;
  GLint unpack_row_length;
  GLint unpack_alignment;
  GLint uniforms[8];
  int uniform_count;
  int compile_fails;
  int link_fails;
} FakeDriver;

static FakeDriver g_fake;

void anygm_test_graphics_reset(void){
  memset(&g_fake,0,sizeof g_fake);
  g_fake.next_name=1;
  g_fake.allow_calls=1;
  g_fake.framebuffer=7;
}

static void note_call(void){
  if(!g_fake.allow_calls) g_fake.calls_after_forget++;
}

static GLenum fake_GetError(void){ note_call(); return GL_NO_ERROR; }
static const GLubyte *fake_GetString(GLenum name){ (void)name; note_call(); return (const GLubyte*)"fake"; }
static void fake_Enable(GLenum cap){
  note_call();
  switch(cap){
    case GL_BLEND: g_fake.blend_enabled=1; break;
    case GL_DEPTH_TEST: g_fake.depth_enabled=1; break;
    case GL_STENCIL_TEST: g_fake.stencil_enabled=1; break;
    case GL_CULL_FACE: g_fake.cull_enabled=1; break;
    case GL_DITHER: g_fake.dither_enabled=1; break;
    case GL_SCISSOR_TEST: g_fake.scissor_enabled=1; break;
    default: break;
  }
}
static void fake_Disable(GLenum cap){
  note_call();
  switch(cap){
    case GL_BLEND: g_fake.blend_enabled=0; break;
    case GL_DEPTH_TEST: g_fake.depth_enabled=0; break;
    case GL_STENCIL_TEST: g_fake.stencil_enabled=0; break;
    case GL_CULL_FACE: g_fake.cull_enabled=0; break;
    case GL_DITHER: g_fake.dither_enabled=0; break;
    case GL_SCISSOR_TEST: g_fake.scissor_enabled=0; break;
    default: break;
  }
}
static void fake_Viewport(GLint x,GLint y,GLsizei w,GLsizei h){
  note_call();
  g_fake.viewport[0]=x; g_fake.viewport[1]=y; g_fake.viewport[2]=w; g_fake.viewport[3]=h;
}
static void fake_Scissor(GLint x,GLint y,GLsizei w,GLsizei h){
  note_call();
  g_fake.scissor[0]=x; g_fake.scissor[1]=y; g_fake.scissor[2]=w; g_fake.scissor[3]=h;
}
static void fake_ClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){
  note_call();
  g_fake.clear_colour[0]=r; g_fake.clear_colour[1]=g;
  g_fake.clear_colour[2]=b; g_fake.clear_colour[3]=a;
}
static void fake_Clear(GLbitfield mask){ (void)mask; note_call(); g_fake.clear_calls++; }
static void fake_ColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a){
  note_call();
  g_fake.color_mask_all=r&&g&&b&&a;
}
static void fake_DepthMask(GLboolean flag){ note_call(); g_fake.depth_mask=flag; }
static void fake_BindFramebuffer(GLenum target,GLuint name){
  (void)target; note_call(); g_fake.framebuffer=name;
}
static void fake_GenTextures(GLsizei n,GLuint *names){
  note_call();
  for(GLsizei index=0;index<n;index++){
    GLuint name=g_fake.next_name++;
    names[index]=name;
    if(name<FAKE_MAX_TEXTURES) g_fake.textures[name].created=1;
  }
}
static void fake_DeleteTextures(GLsizei n,const GLuint *names){
  note_call();
  g_fake.deletes_issued+=n;
  for(GLsizei index=0;index<n;index++)
    if(names[index]<FAKE_MAX_TEXTURES) g_fake.textures[names[index]].deleted=1;
}
static void fake_BindTexture(GLenum target,GLuint name){
  (void)target; note_call();
  if(g_fake.active_unit<3) g_fake.bound_texture[g_fake.active_unit]=name;
}
static void fake_ActiveTexture(GLenum unit){ note_call(); g_fake.active_unit=unit-GL_TEXTURE0; }
static void fake_TexParameteri(GLenum t,GLenum p,GLint v){ (void)t;(void)p;(void)v; note_call(); }
static void fake_TexImage2D(GLenum target,GLint level,GLint internal_format,GLsizei width,
                            GLsizei height,GLint border,GLenum format,GLenum type,
                            const void *pixels){
  GLuint name=g_fake.active_unit<3?g_fake.bound_texture[g_fake.active_unit]:0;
  (void)target; (void)level; (void)border; (void)format;
  note_call();
  if(name<FAKE_MAX_TEXTURES){
    FakeTexture *texture=&g_fake.textures[name];
    size_t stride=type==GL_UNSIGNED_SHORT?2u:4u;
    size_t bytes=(size_t)width*(size_t)height*stride;
    texture->internal_format=internal_format;
    texture->width=width;
    texture->height=height;
    texture->upload_count++;
    texture->byte_count=bytes<sizeof texture->bytes?bytes:sizeof texture->bytes;
    if(pixels) memcpy(texture->bytes,pixels,texture->byte_count);
  }
}
static void fake_TexSubImage2D(GLenum target,GLint level,GLint x,GLint y,GLsizei width,
                               GLsizei height,GLenum format,GLenum type,const void *pixels){
  GLuint name=g_fake.active_unit<3?g_fake.bound_texture[g_fake.active_unit]:0;
  (void)target; (void)level; (void)x; (void)y; (void)format;
  note_call();
  if(name<FAKE_MAX_TEXTURES){
    FakeTexture *texture=&g_fake.textures[name];
    size_t stride=type==GL_UNSIGNED_SHORT?2u:4u;
    size_t bytes=(size_t)width*(size_t)height*stride;
    texture->upload_count++;
    texture->byte_count=bytes<sizeof texture->bytes?bytes:sizeof texture->bytes;
    if(pixels) memcpy(texture->bytes,pixels,texture->byte_count);
  }
}
static void fake_PixelStorei(GLenum name,GLint value){
  note_call();
  if(name==GL_UNPACK_ROW_LENGTH) g_fake.unpack_row_length=value;
  if(name==GL_UNPACK_ALIGNMENT) g_fake.unpack_alignment=value;
}
static GLuint fake_CreateShader(GLenum stage){ (void)stage; note_call(); return g_fake.next_name++; }
static void fake_ShaderSource(GLuint s,GLsizei n,const GLchar *const *source,const GLint *length){
  (void)s;(void)n;(void)source;(void)length; note_call();
}
static void fake_CompileShader(GLuint s){ (void)s; note_call(); }
static void fake_GetShaderiv(GLuint s,GLenum name,GLint *out){
  (void)s; note_call();
  if(name==GL_COMPILE_STATUS) *out=g_fake.compile_fails?GL_FALSE:GL_TRUE;
  else *out=0;
}
static void fake_GetShaderInfoLog(GLuint s,GLsizei size,GLsizei *written,GLchar *log){
  (void)s; note_call();
  snprintf(log,(size_t)size,"fake compile diagnostic");
  if(written) *written=(GLsizei)strlen(log);
}
static void fake_DeleteShader(GLuint s){ (void)s; note_call(); }
static GLuint fake_CreateProgram(void){
  note_call();
  g_fake.program=g_fake.next_name++;
  return g_fake.program;
}
static void fake_AttachShader(GLuint p,GLuint s){ (void)p;(void)s; note_call(); }
static void fake_LinkProgram(GLuint p){ (void)p; note_call(); }
static void fake_GetProgramiv(GLuint p,GLenum name,GLint *out){
  (void)p; note_call();
  if(name==GL_LINK_STATUS) *out=g_fake.link_fails?GL_FALSE:GL_TRUE;
  else *out=0;
}
static void fake_GetProgramInfoLog(GLuint p,GLsizei size,GLsizei *written,GLchar *log){
  (void)p; note_call();
  snprintf(log,(size_t)size,"fake link diagnostic");
  if(written) *written=(GLsizei)strlen(log);
}
static void fake_UseProgram(GLuint p){ note_call(); g_fake.bound_program=p; }
static void fake_DeleteProgram(GLuint p){ (void)p; note_call(); g_fake.deletes_issued++; }
static GLint fake_GetUniformLocation(GLuint p,const GLchar *name){
  (void)p; (void)name; note_call();
  return g_fake.uniform_count++;
}
static void fake_Uniform1i(GLint location,GLint value){
  note_call();
  if(location>=0 && location<8) g_fake.uniforms[location]=value;
}
static void fake_GenVertexArrays(GLsizei n,GLuint *names){
  note_call();
  for(GLsizei index=0;index<n;index++) names[index]=g_fake.next_name++;
  if(n>0) g_fake.vertex_array=names[0];
}
static void fake_BindVertexArray(GLuint name){ note_call(); g_fake.bound_vertex_array=name; }
static void fake_DeleteVertexArrays(GLsizei n,const GLuint *names){
  (void)names; note_call(); g_fake.deletes_issued+=n;
}
static void fake_DrawArrays(GLenum mode,GLint first,GLsizei count){
  note_call();
  if(mode==GL_TRIANGLES && first==0 && count==3) g_fake.draw_calls++;
}

void (*anygm_test_graphics_proc(void *userdata,const char *name))(void){
  static const struct { const char *name; void *proc; } table[]={
    {"glGetError",(void*)fake_GetError},
    {"glGetString",(void*)fake_GetString},
    {"glEnable",(void*)fake_Enable},
    {"glDisable",(void*)fake_Disable},
    {"glViewport",(void*)fake_Viewport},
    {"glScissor",(void*)fake_Scissor},
    {"glClearColor",(void*)fake_ClearColor},
    {"glClear",(void*)fake_Clear},
    {"glColorMask",(void*)fake_ColorMask},
    {"glDepthMask",(void*)fake_DepthMask},
    {"glBindFramebuffer",(void*)fake_BindFramebuffer},
    {"glGenTextures",(void*)fake_GenTextures},
    {"glDeleteTextures",(void*)fake_DeleteTextures},
    {"glBindTexture",(void*)fake_BindTexture},
    {"glActiveTexture",(void*)fake_ActiveTexture},
    {"glTexParameteri",(void*)fake_TexParameteri},
    {"glTexImage2D",(void*)fake_TexImage2D},
    {"glTexSubImage2D",(void*)fake_TexSubImage2D},
    {"glPixelStorei",(void*)fake_PixelStorei},
    {"glCreateShader",(void*)fake_CreateShader},
    {"glShaderSource",(void*)fake_ShaderSource},
    {"glCompileShader",(void*)fake_CompileShader},
    {"glGetShaderiv",(void*)fake_GetShaderiv},
    {"glGetShaderInfoLog",(void*)fake_GetShaderInfoLog},
    {"glDeleteShader",(void*)fake_DeleteShader},
    {"glCreateProgram",(void*)fake_CreateProgram},
    {"glAttachShader",(void*)fake_AttachShader},
    {"glLinkProgram",(void*)fake_LinkProgram},
    {"glGetProgramiv",(void*)fake_GetProgramiv},
    {"glGetProgramInfoLog",(void*)fake_GetProgramInfoLog},
    {"glUseProgram",(void*)fake_UseProgram},
    {"glDeleteProgram",(void*)fake_DeleteProgram},
    {"glGetUniformLocation",(void*)fake_GetUniformLocation},
    {"glUniform1i",(void*)fake_Uniform1i},
    {"glGenVertexArrays",(void*)fake_GenVertexArrays},
    {"glBindVertexArray",(void*)fake_BindVertexArray},
    {"glDeleteVertexArrays",(void*)fake_DeleteVertexArrays},
    {"glDrawArrays",(void*)fake_DrawArrays},
    {NULL,NULL}
  };
  (void)userdata;
  if(g_fake.withhold && !strcmp(g_fake.withhold,name)) return NULL;
  for(size_t index=0;table[index].name;index++)
    if(!strcmp(table[index].name,name)){
      void (*proc)(void)=NULL;
      memcpy(&proc,&table[index].proc,sizeof proc);
      return proc;
    }
  return NULL;
}

uintptr_t anygm_test_graphics_framebuffer(void *userdata){
  (void)userdata;
  g_fake.framebuffer_queries++;
  return 7u;
}


void anygm_test_graphics_withhold(const char *name){ g_fake.withhold=name; }
void anygm_test_graphics_fail_compile(int failing){ g_fake.compile_fails=failing; }
void anygm_test_graphics_fail_link(int failing){ g_fake.link_fails=failing; }
void anygm_test_graphics_forbid_calls(void){ g_fake.allow_calls=0; }
int anygm_test_graphics_deletes(void){ return g_fake.deletes_issued; }
int anygm_test_graphics_calls_after_forget(void){ return g_fake.calls_after_forget; }
int anygm_test_graphics_draw_calls(void){ return g_fake.draw_calls; }
int anygm_test_graphics_clear_calls(void){ return g_fake.clear_calls; }
unsigned anygm_test_graphics_framebuffer_queries(void){ return g_fake.framebuffer_queries; }
int anygm_test_graphics_blend_enabled(void){ return g_fake.blend_enabled; }
int anygm_test_graphics_depth_enabled(void){ return g_fake.depth_enabled; }
int anygm_test_graphics_stencil_enabled(void){ return g_fake.stencil_enabled; }
int anygm_test_graphics_cull_enabled(void){ return g_fake.cull_enabled; }
int anygm_test_graphics_dither_enabled(void){ return g_fake.dither_enabled; }
int anygm_test_graphics_scissor_enabled(void){ return g_fake.scissor_enabled; }
int anygm_test_graphics_color_mask_all(void){ return g_fake.color_mask_all; }
int anygm_test_graphics_depth_mask(void){ return g_fake.depth_mask; }
unsigned anygm_test_graphics_bound_program(void){ return g_fake.bound_program; }
unsigned anygm_test_graphics_bound_vertex_array(void){ return g_fake.bound_vertex_array; }
int anygm_test_graphics_unpack_row_length(void){ return g_fake.unpack_row_length; }

void anygm_test_graphics_viewport(int *x,int *y,int *width,int *height){
  if(x) *x=g_fake.viewport[0];
  if(y) *y=g_fake.viewport[1];
  if(width) *width=g_fake.viewport[2];
  if(height) *height=g_fake.viewport[3];
}

void anygm_test_graphics_scissor(int *x,int *y,int *width,int *height){
  if(x) *x=g_fake.scissor[0];
  if(y) *y=g_fake.scissor[1];
  if(width) *width=g_fake.scissor[2];
  if(height) *height=g_fake.scissor[3];
}

void anygm_test_graphics_clear_colour(float *red,float *green,float *blue,float *alpha){
  if(red) *red=g_fake.clear_colour[0];
  if(green) *green=g_fake.clear_colour[1];
  if(blue) *blue=g_fake.clear_colour[2];
  if(alpha) *alpha=g_fake.clear_colour[3];
}

/* Exactly the states a pass is required to set for itself. */
void anygm_test_graphics_disturb(void){
  g_fake.blend_enabled=1;
  g_fake.depth_enabled=1;
  g_fake.stencil_enabled=1;
  g_fake.cull_enabled=1;
  g_fake.dither_enabled=1;
  g_fake.color_mask_all=0;
  g_fake.depth_mask=1;
}

unsigned anygm_test_graphics_texture_with_format(int internal_format){
  unsigned found=0;
  for(unsigned index=1;index<FAKE_MAX_TEXTURES;index++)
    if(g_fake.textures[index].created &&
       g_fake.textures[index].internal_format==(GLint)internal_format) found=index;
  return found;
}

unsigned anygm_test_graphics_first_texture_with_format(int internal_format){
  for(unsigned index=1;index<FAKE_MAX_TEXTURES;index++)
    if(g_fake.textures[index].created &&
       g_fake.textures[index].internal_format==(GLint)internal_format) return index;
  return 0;
}

int anygm_test_graphics_texture_width(unsigned name){
  return name<FAKE_MAX_TEXTURES?(int)g_fake.textures[name].width:0;
}

int anygm_test_graphics_texture_height(unsigned name){
  return name<FAKE_MAX_TEXTURES?(int)g_fake.textures[name].height:0;
}

unsigned anygm_test_graphics_texture_uploads(unsigned name){
  return name<FAKE_MAX_TEXTURES?g_fake.textures[name].upload_count:0u;
}

const unsigned char *anygm_test_graphics_texture_bytes(unsigned name,size_t *count){
  if(name>=FAKE_MAX_TEXTURES){ if(count) *count=0; return NULL; }
  if(count) *count=g_fake.textures[name].byte_count;
  return g_fake.textures[name].bytes;
}
