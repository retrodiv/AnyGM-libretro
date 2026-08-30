/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The fake graphics driver. See graphics_driver_fixture.h for why it exists and what it can and
 * cannot prove. */
#include "graphics_driver_fixture.h"

#include "gml_gpu_gl_api.h"

#include <stdio.h>
#include <stdlib.h>
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
  unsigned shader_stage[256];
  /* The uniforms the last linked program declares, read from the text it was linked from. */
  struct { char name[48]; GLenum type; GLint size; } uniform[64];
  int uniform_declared;
  char shader_source[2][4096];
  GLint strip_draw_calls;
  int no_attributes;
  int attributes_enabled;
  GLuint bound_buffer;
  size_t buffer_bytes;
  unsigned float_uniforms;
  unsigned matrix_uniforms;
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
static GLuint fake_CreateShader(GLenum stage){
  GLuint name=g_fake.next_name++;
  note_call();
  if(name<256) g_fake.shader_stage[name]=stage;
  return name;
}
/* The text is kept, so a test can read what the backend handed the driver: which is the only
 * way to check a rewrite without restating it. */
static void fake_ShaderSource(GLuint s,GLsizei n,const GLchar *const *source,const GLint *length){
  int fragment=s<256 && g_fake.shader_stage[s]==GL_FRAGMENT_SHADER;
  (void)length; note_call();
  g_fake.shader_source[fragment][0]='\0';
  if(n>0 && source && source[0]) snprintf(g_fake.shader_source[fragment],sizeof g_fake.shader_source[fragment],"%s",source[0]);
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
/* Linking reads the uniform declarations out of both stages, so the backend's query for the
 * program's active uniforms answers with what the text declares: name, type and array size. */
static GLenum fake_uniform_type(const char *word){
  static const struct { const char *word; GLenum type; } types[]={
    {"float",GL_FLOAT},{"vec2",GL_FLOAT_VEC2},{"vec3",GL_FLOAT_VEC3},{"vec4",GL_FLOAT_VEC4},
    {"int",GL_INT},{"ivec2",GL_INT_VEC2},{"ivec3",GL_INT_VEC3},{"ivec4",GL_INT_VEC4},
    {"bool",GL_BOOL},{"mat2",GL_FLOAT_MAT2},{"mat3",GL_FLOAT_MAT3},{"mat4",GL_FLOAT_MAT4},
    {"sampler2D",GL_SAMPLER_2D},
  };
  for(size_t index=0;index<sizeof types/sizeof types[0];index++)
    if(!strcmp(word,types[index].word)) return types[index].type;
  return 0;
}
static void fake_declare_uniforms(const char *text){
  const char *cursor=text;
  while((cursor=strstr(cursor,"uniform "))!=NULL){
    char qualifier[32]="",type_word[32]="",name[64]="";
    const char *p=cursor+8;
    int consumed=0;
    cursor=p;
    if(sscanf(p,"%31s %63[^; \t\n]%n",type_word,name,&consumed)<2) continue;
    if(!fake_uniform_type(type_word)){
      /* A precision qualifier before the type. */
      snprintf(qualifier,sizeof qualifier,"%s",type_word);
      if(sscanf(p,"%31s %31s %63[^; \t\n]%n",qualifier,type_word,name,&consumed)<3) continue;
    }
    if(g_fake.uniform_declared<64){
      char *bracket=strchr(name,'[');
      GLint size=1;
      if(bracket){ size=atoi(bracket+1); *bracket='\0'; }
      snprintf(g_fake.uniform[g_fake.uniform_declared].name,48,"%s%s",name,bracket?"[0]":"");
      g_fake.uniform[g_fake.uniform_declared].type=fake_uniform_type(type_word);
      g_fake.uniform[g_fake.uniform_declared].size=size;
      g_fake.uniform_declared++;
    }
  }
}
static void fake_LinkProgram(GLuint p){
  (void)p; note_call();
  g_fake.uniform_declared=0;
  fake_declare_uniforms(g_fake.shader_source[0]);
  fake_declare_uniforms(g_fake.shader_source[1]);
}
static void fake_GetActiveUniform(GLuint p,GLuint index,GLsizei buffer_size,GLsizei *length,
                                  GLint *size,GLenum *type,GLchar *name){
  (void)p; note_call();
  if(buffer_size>0) name[0]='\0';
  if((int)index<g_fake.uniform_declared){
    snprintf(name,(size_t)buffer_size,"%s",g_fake.uniform[index].name);
    *size=g_fake.uniform[index].size;
    *type=g_fake.uniform[index].type;
  } else { *size=0; *type=0; }
  if(length) *length=(GLsizei)strlen(name);
}
static void fake_UniformMatrix3fv(GLint l,GLsizei n,GLboolean t,const GLfloat *v){
  (void)l;(void)t;(void)v; note_call(); g_fake.matrix_uniforms+=(unsigned)n;
}
static void fake_UniformMatrix2fv(GLint l,GLsizei n,GLboolean t,const GLfloat *v){
  (void)l;(void)t;(void)v; note_call(); g_fake.matrix_uniforms+=(unsigned)n;
}
static void fake_GetProgramiv(GLuint p,GLenum name,GLint *out){
  (void)p; note_call();
  if(name==GL_LINK_STATUS) *out=g_fake.link_fails?GL_FALSE:GL_TRUE;
  else if(name==GL_ACTIVE_UNIFORMS) *out=g_fake.uniform_declared;
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
  if(mode==GL_TRIANGLE_STRIP && first==0 && count==4) g_fake.strip_draw_calls++;
}
/* The entry points only a content program uses. The fixture records what a test can ask about:
 * how many quads were drawn, and which attribute names were resolved. */
static GLint fake_GetAttribLocation(GLuint p,const GLchar *name){
  (void)p; note_call();
  if(g_fake.no_attributes) return -1;
  if(!strcmp(name,"in_Position")) return 0;
  if(!strcmp(name,"in_Colour")) return 1;
  if(!strcmp(name,"in_TextureCoord")) return 2;
  return -1;
}
static void fake_VertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,
                                     GLsizei stride,const void *pointer){
  (void)index;(void)size;(void)type;(void)normalized;(void)stride;(void)pointer; note_call();
}
static void fake_EnableVertexAttribArray(GLuint index){ (void)index; note_call(); g_fake.attributes_enabled++; }
static void fake_DisableVertexAttribArray(GLuint index){ (void)index; note_call(); g_fake.attributes_enabled--; }
static void fake_GenBuffers(GLsizei n,GLuint *names){
  note_call();
  for(GLsizei index=0;index<n;index++) names[index]=g_fake.next_name++;
}
static void fake_BindBuffer(GLenum target,GLuint name){ (void)target; note_call(); g_fake.bound_buffer=name; }
static void fake_BufferData(GLenum target,GLsizeiptr size,const void *data,GLenum usage){
  (void)target;(void)data;(void)usage; note_call(); g_fake.buffer_bytes=(size_t)size;
}
static void fake_DeleteBuffers(GLsizei n,const GLuint *names){ (void)names; note_call(); g_fake.deletes_issued+=n; }
static void fake_Uniform1fv(GLint l,GLsizei n,const GLfloat *v){ (void)l;(void)n;(void)v; note_call(); g_fake.float_uniforms++; }
static void fake_Uniform2fv(GLint l,GLsizei n,const GLfloat *v){ (void)l;(void)n;(void)v; note_call(); g_fake.float_uniforms++; }
static void fake_Uniform3fv(GLint l,GLsizei n,const GLfloat *v){ (void)l;(void)n;(void)v; note_call(); g_fake.float_uniforms++; }
static void fake_Uniform4fv(GLint l,GLsizei n,const GLfloat *v){ (void)l;(void)n;(void)v; note_call(); g_fake.float_uniforms++; }
static void fake_Uniform1iv(GLint l,GLsizei n,const GLint *v){
  (void)n; note_call();
  if(l>=0 && l<8) g_fake.uniforms[l]=v[0];
}
static void fake_Uniform2iv(GLint l,GLsizei n,const GLint *v){ (void)l;(void)n;(void)v; note_call(); }
static void fake_Uniform3iv(GLint l,GLsizei n,const GLint *v){ (void)l;(void)n;(void)v; note_call(); }
static void fake_Uniform4iv(GLint l,GLsizei n,const GLint *v){ (void)l;(void)n;(void)v; note_call(); }
static void fake_UniformMatrix4fv(GLint l,GLsizei n,GLboolean t,const GLfloat *v){
  (void)l;(void)t;(void)v; note_call(); g_fake.matrix_uniforms+=(unsigned)n;
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
    {"glGetAttribLocation",(void*)fake_GetAttribLocation},
    {"glVertexAttribPointer",(void*)fake_VertexAttribPointer},
    {"glEnableVertexAttribArray",(void*)fake_EnableVertexAttribArray},
    {"glDisableVertexAttribArray",(void*)fake_DisableVertexAttribArray},
    {"glGenBuffers",(void*)fake_GenBuffers},
    {"glBindBuffer",(void*)fake_BindBuffer},
    {"glBufferData",(void*)fake_BufferData},
    {"glDeleteBuffers",(void*)fake_DeleteBuffers},
    {"glUniform1fv",(void*)fake_Uniform1fv},
    {"glUniform2fv",(void*)fake_Uniform2fv},
    {"glUniform3fv",(void*)fake_Uniform3fv},
    {"glUniform4fv",(void*)fake_Uniform4fv},
    {"glUniform1iv",(void*)fake_Uniform1iv},
    {"glUniform2iv",(void*)fake_Uniform2iv},
    {"glUniform3iv",(void*)fake_Uniform3iv},
    {"glUniform4iv",(void*)fake_Uniform4iv},
    {"glUniformMatrix4fv",(void*)fake_UniformMatrix4fv},
    {"glUniformMatrix3fv",(void*)fake_UniformMatrix3fv},
    {"glUniformMatrix2fv",(void*)fake_UniformMatrix2fv},
    {"glGetActiveUniform",(void*)fake_GetActiveUniform},
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
int anygm_test_graphics_quad_draw_calls(void){ return g_fake.strip_draw_calls; }
const char *anygm_test_graphics_shader_source(int fragment){ return g_fake.shader_source[fragment?1:0]; }
void anygm_test_graphics_withhold_attributes(int withhold){ g_fake.no_attributes=withhold; }
int anygm_test_graphics_attributes_enabled(void){ return g_fake.attributes_enabled; }
unsigned anygm_test_graphics_bound_buffer(void){ return g_fake.bound_buffer; }
unsigned anygm_test_graphics_float_uniforms(void){ return g_fake.float_uniforms; }
unsigned anygm_test_graphics_matrix_uniforms(void){ return g_fake.matrix_uniforms; }
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
