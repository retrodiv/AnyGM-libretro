/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The fake graphics driver. See graphics_driver_fixture.h for why it exists and what it can and
 * cannot prove. */
#include "graphics_driver_fixture.h"

#include "gml_gpu_opengl_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FAKE_MAX_TEXTURES=32,FAKE_MAX_CALLS=4096,FAKE_MAX_PIXELS=1u<<16,
       FAKE_TEXTURE_UNITS=4 };

typedef struct FakeTexture {
  int created;
  int deleted;
  GLint internal_format;
  GLint min_filter;
  GLsizei width;
  GLsizei height;
  uint32_t upload_count;
  uint32_t data_upload_count;
  unsigned char bytes[FAKE_MAX_PIXELS*4];
  size_t byte_count;
} FakeTexture;

typedef struct FakeDriver {
  /* Which entry point the loader is not allowed to resolve, so that a missing one can be tested
   * exactly rather than by removing code. */
  const char *withhold;
  const char *renderer;
  int deletes_issued;
  int calls_after_forget;
  int allow_calls;
  GLuint next_name;
  FakeTexture textures[FAKE_MAX_TEXTURES];
  GLuint bound_texture[FAKE_TEXTURE_UNITS];
  GLuint scene_sampler_texture[FAKE_TEXTURE_UNITS];
  GLuint upload_error_texture;
  GLuint quad_texture[3];
  GLint quad_filter[3];
  GLuint active_unit;
  GLuint framebuffer,last_draw_framebuffer;
  uint32_t framebuffer_queries,framebuffer_attachments;
  GLuint program;
  GLuint vertex_array;
  GLuint bound_program;
  GLuint box_program;
  GLint box_location,box_destination[2];
  GLuint bound_vertex_array;
  int blend_enabled;
  unsigned blend_configurations;
  unsigned scene_copies;
  unsigned copy_error_at,last_copy_framebuffer,last_read_framebuffer;
  uint64_t scene_copied_pixels;
  int depth_enabled;
  int stencil_enabled;
  int cull_enabled;
  int dither_enabled;
  int scissor_enabled;
  int color_mask_all;
  unsigned color_mask_bits;
  int depth_mask;
  GLint viewport[4];
  GLint scissor[4];
  GLint clear_calls;
  unsigned invalidates, invalidate_attachment, invalidate_framebuffer, invalidate_prior_clears;
  unsigned version_major,version_minor;
  unsigned full_attachment_clears;
  GLfloat clear_colour[4];
  GLint draw_calls;
  GLint scene_draw_calls;
  unsigned scene_mask_draws[16];
  unsigned scene_draw_error_at,terminal_draw_error_at,flush_calls,finish_calls;
  struct { GLint location; GLuint program; } font_channel_locations[64];
  unsigned font_channel_location_count,font_channel_draws[3],font_channel_errors;
  GLint font_channel;
  GLint scene_vertices;
  GLuint scene_texture;
  GLint scene_filter;
  struct { GLint location,value; GLuint program; } sampler_locations[64];
  unsigned sampler_location_count;
  GLint scene_sampler;
  GLenum blend_factors[4];
  unsigned shader_stage[256];
  /* The uniforms the last linked program declares, read from the text it was linked from. */
  /* The declaration parser admits 63 name bytes, plus an array's [0] suffix. */
  struct { char name[68]; GLenum type; GLint size; } uniform[64];
  int uniform_declared;
  char shader_source[2][65536];
  GLint strip_draw_calls;
  int no_attributes;
  int attributes_enabled;
  GLuint bound_buffer;
  size_t buffer_bytes;
  size_t buffer_capacity[FAKE_MAX_CALLS],buffer_high_water[FAKE_MAX_CALLS];
  unsigned char *buffer_store[FAKE_MAX_CALLS];
  GLuint instance_buffer;
  size_t instance_offset,instance_size;
  size_t attribute_offset[16];
  GLsizei attribute_stride[16];
  GLuint attribute_buffer[16],attribute_divisor[16];
  GLint attribute_components[16];
  AnygmTestGraphicsStream stream;
  unsigned stream_map_mode,stream_mapped;
  unsigned char *stream_map_store;
  size_t stream_map_capacity,stream_map_offset,stream_map_length;
  float buffer_data[64];
  unsigned float_uniforms;
  unsigned matrix_uniforms;
  float content_matrices[5*16];
  int content_matrices_valid;
  GLuint attached_texture;
  unsigned read_pixels;
  uint8_t readback_alpha;
  int checkpoint_mask;
  unsigned readback_error_at,pending_error;
  unsigned async_read_pixels;
  GLuint bound_pack_buffer;
  AnygmTestGraphicsUnpack unpack;
  unsigned char *unpack_store;
  unsigned unpack_failure;
  int unpack_mapped;
  unsigned char pack_store[2][64*64*4];
  unsigned pack_slot_for[8];
  unsigned pack_fill;
  GLint unpack_row_length;
  GLint unpack_alignment;
  GLint uniforms[8];
  int uniform_count;
  int compile_fails;
  const char *compile_fail_contains;
  int link_fails;
  int texture_unit_limit;
  int active_uniform_override;
  unsigned compile_calls;
  unsigned active_query,rgb_query_result,rgb_queries;
} FakeDriver;

static FakeDriver g_fake;

void anygm_test_graphics_reset(void){
  free(g_fake.unpack_store);
  free(g_fake.stream_map_store);
  for(unsigned index=0;index<FAKE_MAX_CALLS;index++) free(g_fake.buffer_store[index]);
  memset(&g_fake,0,sizeof g_fake);
  g_fake.next_name=1;
  g_fake.allow_calls=1;
  g_fake.framebuffer=7;
  g_fake.texture_unit_limit=8;
  g_fake.active_uniform_override=-1;
  g_fake.rgb_query_result=1u;
  g_fake.readback_alpha=255u;
  g_fake.checkpoint_mask=-1;
  g_fake.renderer="fake";
}
void anygm_test_graphics_stream_mapping(unsigned mode){ g_fake.stream_map_mode=mode; }
void anygm_test_graphics_fail_unpack(unsigned phase){ g_fake.unpack_failure=phase; }
AnygmTestGraphicsUnpack anygm_test_graphics_unpack(void){ return g_fake.unpack; }
void anygm_test_graphics_renderer(const char *renderer){ g_fake.renderer=renderer; }
void anygm_test_graphics_readback_alpha(unsigned alpha){ g_fake.readback_alpha=(uint8_t)alpha; }
void anygm_test_graphics_checkpoint_mask(unsigned bits){ g_fake.checkpoint_mask=(int)(bits&63u); }
void anygm_test_graphics_fail_readback_at(unsigned ordinal){ g_fake.readback_error_at=ordinal; }
void anygm_test_graphics_fail_all_readbacks(int failing){
  g_fake.readback_error_at=failing?UINT32_MAX:0u;
}
void anygm_test_graphics_fail_copy_at(unsigned ordinal){ g_fake.copy_error_at=ordinal; }
unsigned anygm_test_graphics_last_read_framebuffer(void){ return g_fake.last_read_framebuffer; }
unsigned anygm_test_graphics_last_copy_framebuffer(void){ return g_fake.last_copy_framebuffer; }
void anygm_test_graphics_fail_scene_draw_at(unsigned ordinal){ g_fake.scene_draw_error_at=ordinal; }
void anygm_test_graphics_fail_terminal_draw_at(unsigned ordinal){ g_fake.terminal_draw_error_at=ordinal; }
void anygm_test_graphics_fail_texture_upload(unsigned name){ g_fake.upload_error_texture=name; }
void anygm_test_graphics_rgb_query_result(unsigned result){ g_fake.rgb_query_result=result; }
void anygm_test_graphics_active_query(unsigned name){ g_fake.active_query=name; }
unsigned anygm_test_graphics_rgb_queries(void){ return g_fake.rgb_queries; }

int anygm_test_graphics_reuse_name(unsigned name){
  if(!name || name>=FAKE_MAX_TEXTURES || name<g_fake.next_name) return 0;
  g_fake.next_name=name;
  return 1;
}

static void note_call(void){
  if(!g_fake.allow_calls) g_fake.calls_after_forget++;
}

static GLenum fake_GetError(void){
  unsigned error=g_fake.pending_error;
  note_call();g_fake.pending_error=0;
  return error;
}
static const GLubyte *fake_GetString(GLenum name){
  note_call();
  return (const GLubyte*)(name==GL_RENDERER?g_fake.renderer:"fake");
}
static void fake_GetIntegerv(GLenum name,GLint *value){
  note_call();
  if(value) *value=name==GL_MAX_TEXTURE_SIZE?4096:
                   name==GL_MAX_TEXTURE_IMAGE_UNITS?g_fake.texture_unit_limit:
                   name==GL_MAJOR_VERSION?(GLint)g_fake.version_major:
                   name==GL_MINOR_VERSION?(GLint)g_fake.version_minor:0;
}
static void fake_Flush(void){ note_call(); g_fake.flush_calls++; }
static void fake_Finish(void){ note_call();g_fake.finish_calls++; }
static void fake_PolygonMode(GLenum face,GLenum mode){ (void)face;(void)mode; note_call(); }
static void fake_BlendFuncSeparate(GLenum a,GLenum b,GLenum c,GLenum d){
  note_call();
  g_fake.blend_factors[0]=a;g_fake.blend_factors[1]=b;
  g_fake.blend_factors[2]=c;g_fake.blend_factors[3]=d;
  if(a==GL_SRC_ALPHA && b==GL_ONE_MINUS_SRC_ALPHA &&
     c==GL_ONE && d==GL_ONE_MINUS_SRC_ALPHA) g_fake.blend_configurations++;
}
static void fake_BlendEquationSeparate(GLenum a,GLenum b){ (void)a;(void)b; note_call(); }
static void fake_CopyTexSubImage2D(GLenum a,GLint b,GLint c,GLint d,GLint e,GLint f,
                                    GLsizei g,GLsizei h){
  (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;
  note_call();
  g_fake.scene_copies++;
  g_fake.scene_copied_pixels+=(uint64_t)(unsigned)g*(unsigned)h;
  g_fake.last_copy_framebuffer=g_fake.framebuffer;
  if(g_fake.scene_copies==g_fake.copy_error_at) g_fake.pending_error=0x0502u;
}
static void fake_BlitFramebuffer(GLint sx0,GLint sy0,GLint sx1,GLint sy1,
                                  GLint dx0,GLint dy0,GLint dx1,GLint dy1,
                                  GLbitfield mask,GLenum filter){
  (void)sx0;(void)sy0;(void)sx1;(void)sy1;(void)mask;(void)filter;
  note_call();
  g_fake.scene_copies++;
  g_fake.scene_copied_pixels+=(uint64_t)(unsigned)abs(dx1-dx0)*(unsigned)abs(dy1-dy0);
}
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
static void fake_InvalidateFramebuffer(GLenum target,GLsizei count,const GLenum *attachments){
  note_call();
  if(target!=GL_FRAMEBUFFER || count!=1 || !attachments){g_fake.pending_error=0x0502u;return;}
  g_fake.invalidates++;g_fake.invalidate_attachment=attachments[0];
  g_fake.invalidate_framebuffer=g_fake.framebuffer;
  g_fake.invalidate_prior_clears=(unsigned)g_fake.clear_calls;
}
static void fake_ClearBufferfv(GLenum buffer,GLint drawbuffer,const GLfloat *value){
  note_call();
  if(buffer!=GL_COLOR || drawbuffer!=0 || !value || g_fake.scissor_enabled){
    g_fake.pending_error=0x0502u;return;
  }
  g_fake.full_attachment_clears++;g_fake.clear_calls++;
  memcpy(g_fake.clear_colour,value,sizeof g_fake.clear_colour);
}
static void fake_Clear(GLbitfield mask){ (void)mask; note_call(); g_fake.clear_calls++; }
static void fake_ColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a){
  note_call();
  g_fake.color_mask_all=r&&g&&b&&a;
  g_fake.color_mask_bits=(r?1u:0u)|(g?2u:0u)|(b?4u:0u)|(a?8u:0u);
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
  if(g_fake.active_unit<FAKE_TEXTURE_UNITS) g_fake.bound_texture[g_fake.active_unit]=name;
}
static void fake_ActiveTexture(GLenum unit){ note_call(); g_fake.active_unit=unit-GL_TEXTURE0; }
static void fake_TexParameteri(GLenum t,GLenum p,GLint v){
  (void)t; note_call();
  GLuint name=g_fake.active_unit<FAKE_TEXTURE_UNITS?g_fake.bound_texture[g_fake.active_unit]:0;
  if(name<FAKE_MAX_TEXTURES && p==GL_TEXTURE_MIN_FILTER) g_fake.textures[name].min_filter=v;
}
static void fake_TexImage2D(GLenum target,GLint level,GLint internal_format,GLsizei width,
                            GLsizei height,GLint border,GLenum format,GLenum type,
                            const void *pixels){
  GLuint name=g_fake.active_unit<FAKE_TEXTURE_UNITS?g_fake.bound_texture[g_fake.active_unit]:0;
  (void)target; (void)level; (void)border; (void)format;
  note_call();
  if(pixels && name && name==g_fake.upload_error_texture){
    g_fake.pending_error=0x0505u; /* GL_OUT_OF_MEMORY leaves the old texture unmodified. */
    return;
  }
  if(name<FAKE_MAX_TEXTURES){
    FakeTexture *texture=&g_fake.textures[name];
    size_t stride=type==GL_UNSIGNED_SHORT?2u:4u;
    size_t bytes=(size_t)width*(size_t)height*stride;
    texture->internal_format=internal_format;
    texture->width=width;
    texture->height=height;
    texture->upload_count++;
    if(pixels) texture->data_upload_count++;
    texture->byte_count=bytes<sizeof texture->bytes?bytes:sizeof texture->bytes;
    if(pixels) memcpy(texture->bytes,pixels,texture->byte_count);
  }
}
static void fake_TexSubImage2D(GLenum target,GLint level,GLint x,GLint y,GLsizei width,
                               GLsizei height,GLenum format,GLenum type,const void *pixels){
  GLuint name=g_fake.active_unit<FAKE_TEXTURE_UNITS?g_fake.bound_texture[g_fake.active_unit]:0;
  (void)target; (void)level; (void)x; (void)y; (void)format;
  note_call();
  if(g_fake.unpack.bound){
    size_t bytes=(size_t)width*(size_t)height*(type==GL_UNSIGNED_SHORT?2u:4u);
    size_t offset=(size_t)pixels;
    if(g_fake.unpack_mapped || offset>g_fake.unpack.capacity ||
       bytes>g_fake.unpack.capacity-offset){
      g_fake.unpack.errors++;
      g_fake.pending_error=0x0502u;
      return;
    }
    pixels=g_fake.unpack_store+offset;
    g_fake.unpack.uploads++;
  }
  if(pixels && name && name==g_fake.upload_error_texture){
    g_fake.pending_error=0x0505u;
    return;
  }
  if(name<FAKE_MAX_TEXTURES){
    FakeTexture *texture=&g_fake.textures[name];
    size_t stride=type==GL_UNSIGNED_SHORT?2u:4u;
    size_t bytes=(size_t)width*(size_t)height*stride;
    texture->upload_count++;
    if(pixels) texture->data_upload_count++;
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
static void fake_CompileShader(GLuint s){ (void)s; note_call(); g_fake.compile_calls++; }
static void fake_GetShaderiv(GLuint s,GLenum name,GLint *out){
  int fragment=s<256 && g_fake.shader_stage[s]==GL_FRAGMENT_SHADER;
  note_call();
  if(name==GL_COMPILE_STATUS)
    *out=g_fake.compile_fails ||
      (fragment && g_fake.compile_fail_contains &&
       strstr(g_fake.shader_source[1],g_fake.compile_fail_contains))?GL_FALSE:GL_TRUE;
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
      snprintf(g_fake.uniform[g_fake.uniform_declared].name,
               sizeof g_fake.uniform[0].name,"%s%s",name,bracket?"[0]":"");
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
static void fake_GenFramebuffers(GLsizei n,GLuint *names){
  note_call();
  for(GLsizei index=0;index<n;index++) names[index]=g_fake.next_name++;
}
static void fake_DeleteFramebuffers(GLsizei n,const GLuint *names){ (void)names; note_call(); g_fake.deletes_issued+=n; }
static void fake_FramebufferTexture2D(GLenum target,GLenum attachment,GLenum textarget,GLuint texture,GLint level){
  (void)target;(void)attachment;(void)textarget;(void)level; note_call(); g_fake.attached_texture=texture; g_fake.framebuffer_attachments++;
}
static GLenum fake_CheckFramebufferStatus(GLenum target){ (void)target; note_call(); return GL_FRAMEBUFFER_COMPLETE; }
/* Rows from the bottom, bytes R,G,B,A: the red byte carries the device row and the green byte the
 * column, so a reader that forgets to flip or reorder is caught. */
/* A pack buffer bound means the read is asynchronous: the pattern goes into the fixture's own
 * storage for that buffer and is handed back when the backend maps it, which is what lets a test
 * see that a pass took the previous pass's answer. */
static void fake_BufferSubData(GLenum target,GLintptr offset,GLsizeiptr size,const void *data);
static void *fake_MapBufferRange(GLenum target,GLintptr offset,GLsizeiptr length,GLbitfield access){
  (void)target;(void)offset;(void)length;(void)access; note_call();
  if(target==GL_ARRAY_BUFFER){
    unsigned buffer=g_fake.bound_buffer;
    g_fake.stream.maps++;
    if(!g_fake.stream_map_mode) return NULL;
    if(!buffer || buffer>=FAKE_MAX_CALLS || offset<0 || length<=0 ||
       (size_t)offset<g_fake.buffer_high_water[buffer] ||
       (size_t)offset>g_fake.buffer_capacity[buffer] ||
       (size_t)length>g_fake.buffer_capacity[buffer]-(size_t)offset ||
       access!=(GL_MAP_WRITE_BIT|GL_MAP_UNSYNCHRONIZED_BIT) || g_fake.stream_mapped){
      g_fake.stream.errors++;
      return NULL;
    }
    if((size_t)length+4u>g_fake.stream_map_capacity){
      unsigned char *grown=realloc(g_fake.stream_map_store,(size_t)length+4u);
      if(!grown) return NULL;
      g_fake.stream_map_store=grown;g_fake.stream_map_capacity=(size_t)length+4u;
    }
    g_fake.stream_map_offset=(size_t)offset;g_fake.stream_map_length=(size_t)length;
    g_fake.stream_mapped=1u;
    memset(g_fake.stream_map_store,0xCD,(size_t)length+4u);
    return g_fake.stream_map_store+(g_fake.stream_map_mode==3u?4u:0u);
  }
  if(target==GL_PIXEL_UNPACK_BUFFER){
    g_fake.unpack.maps++;
    g_fake.unpack.access=access;
    if(g_fake.unpack_failure==2u){ g_fake.pending_error=0x0505u;return NULL; }
    if(!g_fake.unpack.bound || offset!=0 || length<=0 ||
       (size_t)length>g_fake.unpack.capacity || g_fake.unpack_mapped ||
       access!=(GL_MAP_WRITE_BIT|GL_MAP_UNSYNCHRONIZED_BIT)){
      g_fake.unpack.errors++;
      return NULL;
    }
    g_fake.unpack_mapped=1;
    return g_fake.unpack_store;
  }
  return g_fake.pack_store[g_fake.bound_pack_buffer&1u];
}
static GLboolean fake_UnmapBuffer(GLenum target){
  note_call();
  if(target==GL_ARRAY_BUFFER){
    g_fake.stream.unmaps++;
    if(!g_fake.stream_mapped){g_fake.stream.errors++;return GL_FALSE;}
    g_fake.stream_mapped=0u;
    if(g_fake.stream_map_mode==2u) return GL_FALSE;
    fake_BufferSubData(target,(GLintptr)g_fake.stream_map_offset,
      (GLsizeiptr)g_fake.stream_map_length,
      g_fake.stream_map_store+(g_fake.stream_map_mode==3u?4u:0u));
    return GL_TRUE;
  }
  if(target==GL_PIXEL_UNPACK_BUFFER){
    g_fake.unpack.unmaps++;
    if(!g_fake.unpack_mapped) g_fake.unpack.errors++;
    g_fake.unpack_mapped=0;
    if(g_fake.unpack_failure==3u) return GL_FALSE;
  }
  return GL_TRUE;
}
static void fake_ReadPixels(GLint x,GLint y,GLsizei width,GLsizei height,GLenum format,GLenum type,void *pixels){
  uint8_t *out=(uint8_t*)pixels;
  (void)x;(void)y;(void)format;(void)type; note_call();
  g_fake.last_read_framebuffer=g_fake.framebuffer;
  if(g_fake.bound_pack_buffer){
    /* Asynchronous: fill the fixture's storage for the bound buffer, hand nothing back now. */
    uint8_t *store=g_fake.pack_store[g_fake.bound_pack_buffer&1u];
    size_t bytes=(size_t)width*(size_t)height*4u;
    if(bytes>sizeof g_fake.pack_store[0]) bytes=sizeof g_fake.pack_store[0];
    g_fake.async_read_pixels++;
    g_fake.pack_fill++;
    for(size_t i=0;i+3<bytes;i+=4){
      store[i]=(uint8_t)(g_fake.pack_fill); store[i+1]=(uint8_t)(i/4); store[i+2]=0x33; store[i+3]=0xFF;
    }
    return;
  }
  g_fake.read_pixels++;
  if(g_fake.read_pixels==g_fake.readback_error_at || g_fake.readback_error_at==UINT32_MAX)
    g_fake.pending_error=0x0502u;
  for(GLsizei row=0;row<height;row++)
    for(GLsizei column=0;column<width;column++){
      uint8_t *px=out+((size_t)row*(size_t)width+(size_t)column)*4u;
      px[0]=(uint8_t)row; px[1]=(uint8_t)column; px[2]=0x33; px[3]=g_fake.readback_alpha;
      if(width==3 && height==2 && g_fake.checkpoint_mask>=0){
        px[0]=(g_fake.checkpoint_mask&(1<<(row*3+column)))?255u:0u;
        px[1]=px[2]=0u;px[3]=255u;
      }
    }
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
  else if(name==GL_ACTIVE_UNIFORMS) *out=g_fake.active_uniform_override>=0
    ?g_fake.active_uniform_override:g_fake.uniform_declared;
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
  note_call();
  if(!strcmp(name,"u_box_destination")){
    g_fake.box_program=p;g_fake.box_location=g_fake.uniform_count;
  }
  if((!strcmp(name,"u_filter") || !strcmp(name,"u_selected_filter")) &&
     g_fake.sampler_location_count<64u){
    unsigned index=g_fake.sampler_location_count++;
    g_fake.sampler_locations[index].location=g_fake.uniform_count;
    g_fake.sampler_locations[index].program=p;
    g_fake.sampler_locations[index].value=-1;
  }
  if(!strcmp(name,"u_font_channel") && g_fake.font_channel_location_count<64u){
    unsigned index=g_fake.font_channel_location_count++;
    g_fake.font_channel_locations[index].location=g_fake.uniform_count;
    g_fake.font_channel_locations[index].program=p;
  }
  return g_fake.uniform_count++;
}
static void fake_Uniform1i(GLint location,GLint value){
  note_call();
  for(unsigned index=0;index<g_fake.sampler_location_count;index++)
    if(g_fake.sampler_locations[index].location==location &&
       g_fake.sampler_locations[index].program==g_fake.bound_program)
      g_fake.sampler_locations[index].value=value;
  if(location>=0 && location<8) g_fake.uniforms[location]=value;
  for(unsigned index=0;index<g_fake.font_channel_location_count;index++)
    if(g_fake.font_channel_locations[index].location==location &&
       g_fake.font_channel_locations[index].program==g_fake.bound_program)
      g_fake.font_channel=value;
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
  g_fake.last_draw_framebuffer=g_fake.framebuffer;
  if(mode==GL_TRIANGLES && first==0 && count==3){
    g_fake.draw_calls++;
    if((unsigned)g_fake.draw_calls==g_fake.terminal_draw_error_at)
      g_fake.pending_error=0x0502u;
  }
  if(mode==GL_TRIANGLES && first==0 && count>=6 && count%6==0){
    g_fake.scene_draw_calls++;
    g_fake.scene_mask_draws[g_fake.color_mask_bits&15u]++;
    g_fake.scene_vertices+=count;
    g_fake.scene_texture=g_fake.bound_texture[0];
    memcpy(g_fake.scene_sampler_texture,g_fake.bound_texture,sizeof g_fake.bound_texture);
    g_fake.scene_filter=g_fake.scene_texture<FAKE_MAX_TEXTURES
      ?g_fake.textures[g_fake.scene_texture].min_filter:0;
    g_fake.scene_sampler=-1;
    for(unsigned index=0;index<g_fake.sampler_location_count;index++)
      if(g_fake.sampler_locations[index].program==g_fake.bound_program)
        g_fake.scene_sampler=g_fake.sampler_locations[index].value;
    int font_program=0;
    for(unsigned index=0;index<g_fake.font_channel_location_count;index++)
      if(g_fake.font_channel_locations[index].program==g_fake.bound_program) font_program=1;
    if(font_program){
      int channel=g_fake.font_channel;
      unsigned mask=channel==0?1u:channel==1?2u:12u;
      if(channel>=0 && channel<3 && g_fake.color_mask_bits==mask)
        g_fake.font_channel_draws[channel]++;
      else g_fake.font_channel_errors++;
    }
    if((unsigned)g_fake.scene_draw_calls==g_fake.scene_draw_error_at)
      g_fake.pending_error=0x0502u; /* GL_INVALID_OPERATION after a possibly partial write. */
  }
  if(mode==GL_TRIANGLE_STRIP && first==0 && count==4){
    g_fake.strip_draw_calls++;
    for(unsigned unit=0;unit<3;unit++){
      GLuint name=g_fake.bound_texture[unit];
      g_fake.quad_texture[unit]=name;
      g_fake.quad_filter[unit]=name<FAKE_MAX_TEXTURES?g_fake.textures[name].min_filter:0;
    }
  }
}
static void fake_DrawArraysInstanced(GLenum mode,GLint first,GLsizei count,
                                     GLsizei instance_count){
  if(mode==GL_TRIANGLES && first==0 && count==6 && instance_count>0){
    size_t offset=g_fake.attribute_offset[0],bytes=(size_t)instance_count*52u*sizeof(float);
    GLuint buffer=g_fake.bound_buffer;
    for(unsigned index=0;index<13u;index++){
      if(g_fake.attribute_offset[index]!=offset+index*4u*sizeof(float) ||
         g_fake.attribute_stride[index]!=52*(GLsizei)sizeof(float) ||
         g_fake.attribute_components[index]!=4 || g_fake.attribute_divisor[index]!=1u ||
         g_fake.attribute_buffer[index]!=g_fake.bound_buffer) g_fake.stream.errors++;
    }
    if(!buffer || buffer>=FAKE_MAX_CALLS || !g_fake.buffer_store[buffer] ||
       offset>g_fake.buffer_high_water[buffer] || bytes>g_fake.buffer_high_water[buffer]-offset)
      g_fake.stream.errors++;
    g_fake.instance_buffer=buffer;
    g_fake.instance_offset=offset;
    g_fake.instance_size=bytes;
    fake_DrawArrays(mode,first,count*instance_count);
    return;
  }
  note_call();
}
/* The entry points only a content program uses. The fixture records what a test can ask about:
 * how many quads were drawn, and which attribute names were resolved. */
static GLint fake_GetAttribLocation(GLuint p,const GLchar *name){
  (void)p; note_call();
  if(g_fake.no_attributes) return -1;
  if(!strcmp(name,"in_Position")) return 0;
  if(!strcmp(name,"in_Colour")) return 1;
  if(!strcmp(name,"in_TextureCoord")) return 2;
  if(!strcmp(name,"in_Position01")) return 0;
  if(!strcmp(name,"in_Position23")) return 1;
  if(!strcmp(name,"in_TextureCoord01")) return 2;
  if(!strcmp(name,"in_TextureCoord23")) return 3;
  if(!strcmp(name,"in_Colour0")) return 4;
  if(!strcmp(name,"in_Colour1")) return 5;
  if(!strcmp(name,"in_Colour2")) return 6;
  if(!strcmp(name,"in_Colour3")) return 7;
  if(!strcmp(name,"in_Clamp")) return 8;
  if(!strcmp(name,"in_Map")) return 9;
  if(!strcmp(name,"in_StepMarginActive")) return 10;
  if(!strcmp(name,"in_Step")) return 3;
  if(!strcmp(name,"in_Margin")) return 4;
  if(!strcmp(name,"in_SpanActive")) return 5;
  if(!strcmp(name,"in_SpanX")) return 11;
  if(!strcmp(name,"in_SpanY")) return 12;
  return -1;
}
static void fake_VertexAttribPointer(GLuint index,GLint size,GLenum type,GLboolean normalized,
                                     GLsizei stride,const void *pointer){
  (void)type;(void)normalized; note_call();
  if(index<16u){
    g_fake.attribute_offset[index]=(size_t)pointer;
    g_fake.attribute_stride[index]=stride;
    g_fake.attribute_components[index]=size;
    g_fake.attribute_buffer[index]=g_fake.bound_buffer;
  }
}
static void fake_EnableVertexAttribArray(GLuint index){ (void)index; note_call(); g_fake.attributes_enabled++; }
static void fake_DisableVertexAttribArray(GLuint index){ (void)index; note_call(); g_fake.attributes_enabled--; }
static void fake_VertexAttribDivisor(GLuint index,GLuint divisor){
  note_call();
  if(index<16u) g_fake.attribute_divisor[index]=divisor;
}
static void fake_GenBuffers(GLsizei n,GLuint *names){
  note_call();
  for(GLsizei index=0;index<n;index++) names[index]=g_fake.next_name++;
}
static void fake_BindBuffer(GLenum target,GLuint name){
  note_call();
  if(target==GL_PIXEL_PACK_BUFFER){ g_fake.bound_pack_buffer=name; return; }
  if(target==GL_PIXEL_UNPACK_BUFFER){ g_fake.unpack.bound=name; return; }
  g_fake.bound_buffer=name;
}
static void fake_BufferData(GLenum target,GLsizeiptr size,const void *data,GLenum usage){
  size_t floats=(size_t)size/sizeof(float);
  (void)target;(void)usage; note_call();
  if(target==GL_PIXEL_UNPACK_BUFFER){
    g_fake.unpack.allocations++;
    if(g_fake.unpack_failure==1u){ g_fake.pending_error=0x0505u;return; }
    if(!g_fake.unpack.bound || data || size<=0 || (size_t)size>16u*1024u*1024u ||
       usage!=GL_STREAM_DRAW || g_fake.unpack_mapped){
      g_fake.unpack.errors++;
      g_fake.pending_error=0x0502u;
      return;
    }
    unsigned char *grown=(unsigned char*)realloc(g_fake.unpack_store,(size_t)size);
    if(!grown){ g_fake.pending_error=0x0505u;return; }
    g_fake.unpack_store=grown;
    g_fake.unpack.capacity=(size_t)size;
    g_fake.unpack.buffer=g_fake.unpack.bound;
    memset(grown,0xCD,(size_t)size);
    return;
  }
  g_fake.buffer_bytes=(size_t)size;
  if(target==GL_ARRAY_BUFFER && g_fake.bound_buffer<FAKE_MAX_CALLS){
    unsigned char *store=size>0?(unsigned char*)realloc(g_fake.buffer_store[g_fake.bound_buffer],(size_t)size):NULL;
    if(size>0 && !store){g_fake.pending_error=0x0505u;return;}
    if(size<=0) free(g_fake.buffer_store[g_fake.bound_buffer]);
    g_fake.buffer_store[g_fake.bound_buffer]=store;
    if(data && size>0) memcpy(store,data,(size_t)size);
    g_fake.buffer_capacity[g_fake.bound_buffer]=(size_t)size;
    g_fake.buffer_high_water[g_fake.bound_buffer]=0;
    if(!data){
      g_fake.stream.allocations++;
      g_fake.stream.capacity=(size_t)size;
    }
  }
  if(floats>64) floats=64;
  if(data) memcpy(g_fake.buffer_data,data,floats*sizeof(float));
}
static void fake_BufferSubData(GLenum target,GLintptr offset,GLsizeiptr size,const void *data){
  size_t floats=size>0?(size_t)size/sizeof(float):0;
  GLuint buffer=g_fake.bound_buffer;
  note_call();
  if(target!=GL_ARRAY_BUFFER || !buffer || buffer>=FAKE_MAX_CALLS ||
     offset<0 || size<0 || !data ||
     (size_t)offset<g_fake.buffer_high_water[buffer] ||
     (size_t)offset>g_fake.buffer_capacity[buffer] ||
     (size_t)size>g_fake.buffer_capacity[buffer]-(size_t)offset){
    g_fake.stream.errors++;
    return;
  }
  g_fake.buffer_high_water[buffer]=(size_t)offset+(size_t)size;
  if(!g_fake.stream.updates) g_fake.stream.packet_hash=UINT64_C(14695981039346656037);
  for(size_t byte=0;byte<(size_t)size;byte++){
    g_fake.stream.packet_hash^=((const unsigned char*)data)[byte];
    g_fake.stream.packet_hash*=UINT64_C(1099511628211);
  }
  if(g_fake.buffer_store[buffer]) memcpy(g_fake.buffer_store[buffer]+offset,data,(size_t)size);
  g_fake.stream.updates++;
  g_fake.stream.uploaded_bytes+=(size_t)size;
  g_fake.stream.offset=(size_t)offset;
  g_fake.stream.size=(size_t)size;
  g_fake.buffer_bytes=(size_t)size;
  if(floats>64u) floats=64u;
  memcpy(g_fake.buffer_data,data,floats*sizeof(float));
}
static void fake_DeleteBuffers(GLsizei n,const GLuint *names){
  note_call(); g_fake.deletes_issued+=n;
  for(GLsizei index=0;index<n;index++) if(names[index]<FAKE_MAX_CALLS){
    free(g_fake.buffer_store[names[index]]);
    g_fake.buffer_store[names[index]]=NULL;
    g_fake.buffer_capacity[names[index]]=g_fake.buffer_high_water[names[index]]=0;
  }
  for(GLsizei index=0;index<n;index++) if(names[index]==g_fake.unpack.buffer){
    g_fake.unpack.deletes++;
    free(g_fake.unpack_store);
    g_fake.unpack_store=NULL;
    g_fake.unpack.capacity=0;
    g_fake.unpack.buffer=0;
  }
}
static void fake_GenQueries(GLsizei count,GLuint *names){
  note_call();
  for(GLsizei index=0;index<count;index++) names[index]=g_fake.next_name++;
}
static void fake_DeleteQueries(GLsizei count,const GLuint *names){
  (void)names; note_call(); g_fake.deletes_issued+=count;
}
static void fake_BeginQuery(GLenum target,GLuint name){
  (void)target; note_call(); g_fake.active_query=name; g_fake.rgb_queries++;
}
static void fake_EndQuery(GLenum target){ (void)target; note_call(); g_fake.active_query=0u; }
static void fake_GetQueryiv(GLenum target,GLenum pname,GLint *value){
  (void)target; (void)pname; note_call(); *value=(GLint)g_fake.active_query;
}
static void fake_GetQueryObjectuiv(GLuint name,GLenum pname,GLuint *value){
  (void)name; (void)pname; note_call(); *value=g_fake.rgb_query_result;
}
static void fake_Uniform1fv(GLint l,GLsizei n,const GLfloat *v){ (void)l;(void)n;(void)v; note_call(); g_fake.float_uniforms++; }
static void fake_Uniform2fv(GLint l,GLsizei n,const GLfloat *v){ (void)l;(void)n;(void)v; note_call(); g_fake.float_uniforms++; }
static void fake_Uniform3fv(GLint l,GLsizei n,const GLfloat *v){ (void)l;(void)n;(void)v; note_call(); g_fake.float_uniforms++; }
static void fake_Uniform4fv(GLint l,GLsizei n,const GLfloat *v){ (void)l;(void)n;(void)v; note_call(); g_fake.float_uniforms++; }
static void fake_Uniform1iv(GLint l,GLsizei n,const GLint *v){
  (void)n; note_call();
  if(l>=0 && l<8) g_fake.uniforms[l]=v[0];
}
static void fake_Uniform2iv(GLint l,GLsizei n,const GLint *v){
  note_call();
  if(v && n==1 && l==g_fake.box_location && g_fake.bound_program==g_fake.box_program){
    g_fake.box_destination[0]=v[0];g_fake.box_destination[1]=v[1];
  }
}
static void fake_Uniform3iv(GLint l,GLsizei n,const GLint *v){ (void)l;(void)n;(void)v; note_call(); }
static void fake_Uniform4iv(GLint l,GLsizei n,const GLint *v){ (void)l;(void)n;(void)v; note_call(); }
static void fake_UniformMatrix4fv(GLint l,GLsizei n,GLboolean t,const GLfloat *v){
  (void)l;(void)t; note_call(); g_fake.matrix_uniforms+=(unsigned)n;
  if(n==5 && v){
    memcpy(g_fake.content_matrices,v,sizeof g_fake.content_matrices);
    g_fake.content_matrices_valid=1;
  }
}

void (*anygm_test_graphics_proc(void *userdata,const char *name))(void){
  static const struct { const char *name; void *proc; } table[]={
    {"glGetError",(void*)fake_GetError},
    {"glGetString",(void*)fake_GetString},
    {"glGetIntegerv",(void*)fake_GetIntegerv},
    {"glPolygonMode",(void*)fake_PolygonMode},
    {"glGenQueries",(void*)fake_GenQueries},
    {"glDeleteQueries",(void*)fake_DeleteQueries},
    {"glBeginQuery",(void*)fake_BeginQuery},
    {"glEndQuery",(void*)fake_EndQuery},
    {"glGetQueryiv",(void*)fake_GetQueryiv},
    {"glGetQueryObjectuiv",(void*)fake_GetQueryObjectuiv},
    {"glFlush",(void*)fake_Flush},
    {"glFinish",(void*)fake_Finish},
    {"glBlendFuncSeparate",(void*)fake_BlendFuncSeparate},
    {"glBlendEquationSeparate",(void*)fake_BlendEquationSeparate},
    {"glCopyTexSubImage2D",(void*)fake_CopyTexSubImage2D},
    {"glBlitFramebuffer",(void*)fake_BlitFramebuffer},
    {"glEnable",(void*)fake_Enable},
    {"glDisable",(void*)fake_Disable},
    {"glViewport",(void*)fake_Viewport},
    {"glScissor",(void*)fake_Scissor},
    {"glClearColor",(void*)fake_ClearColor},
    {"glClear",(void*)fake_Clear},
    {"glClearBufferfv",(void*)fake_ClearBufferfv},
    {"glInvalidateFramebuffer",(void*)fake_InvalidateFramebuffer},
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
    {"glDrawArraysInstanced",(void*)fake_DrawArraysInstanced},
    {"glGetAttribLocation",(void*)fake_GetAttribLocation},
    {"glVertexAttribPointer",(void*)fake_VertexAttribPointer},
    {"glEnableVertexAttribArray",(void*)fake_EnableVertexAttribArray},
    {"glDisableVertexAttribArray",(void*)fake_DisableVertexAttribArray},
    {"glVertexAttribDivisor",(void*)fake_VertexAttribDivisor},
    {"glGenBuffers",(void*)fake_GenBuffers},
    {"glBindBuffer",(void*)fake_BindBuffer},
    {"glBufferData",(void*)fake_BufferData},
    {"glBufferSubData",(void*)fake_BufferSubData},
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
    {"glGenFramebuffers",(void*)fake_GenFramebuffers},
    {"glDeleteFramebuffers",(void*)fake_DeleteFramebuffers},
    {"glFramebufferTexture2D",(void*)fake_FramebufferTexture2D},
    {"glCheckFramebufferStatus",(void*)fake_CheckFramebufferStatus},
    {"glReadPixels",(void*)fake_ReadPixels},
    {"glMapBufferRange",(void*)fake_MapBufferRange},
    {"glUnmapBuffer",(void*)fake_UnmapBuffer},
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
void anygm_test_graphics_fail_compile_containing(const char *text){
  g_fake.compile_fail_contains=text;
}
void anygm_test_graphics_fail_link(int failing){ g_fake.link_fails=failing; }
void anygm_test_graphics_texture_unit_limit(int units){ g_fake.texture_unit_limit=units; }
void anygm_test_graphics_active_uniform_limit(int count){ g_fake.active_uniform_override=count; }
void anygm_test_graphics_forbid_calls(void){ g_fake.allow_calls=0; }
void anygm_test_graphics_allow_calls(void){ g_fake.allow_calls=1; }
int anygm_test_graphics_deletes(void){ return g_fake.deletes_issued; }
int anygm_test_graphics_calls_after_forget(void){ return g_fake.calls_after_forget; }
int anygm_test_graphics_draw_calls(void){ return g_fake.draw_calls; }
int anygm_test_graphics_scene_draw_calls(void){ return g_fake.scene_draw_calls; }
int anygm_test_graphics_scene_vertices(void){ return g_fake.scene_vertices; }
unsigned anygm_test_graphics_scene_texture(void){ return g_fake.scene_texture; }
unsigned anygm_test_graphics_scene_sampler_texture(unsigned unit){
  return unit<FAKE_TEXTURE_UNITS?g_fake.scene_sampler_texture[unit]:0u;
}
int anygm_test_graphics_quad_draw_calls(void){ return g_fake.strip_draw_calls; }
unsigned anygm_test_graphics_compile_calls(void){ return g_fake.compile_calls; }
unsigned anygm_test_graphics_flush_calls(void){ return g_fake.flush_calls; }
unsigned anygm_test_graphics_font_channel_draws(unsigned channel){
  return channel<3u?g_fake.font_channel_draws[channel]:0u;
}
unsigned anygm_test_graphics_font_channel_errors(void){ return g_fake.font_channel_errors; }
unsigned anygm_test_graphics_quad_texture(unsigned unit){ return unit<3?g_fake.quad_texture[unit]:0; }
int anygm_test_graphics_quad_filter(unsigned unit){ return unit<3?g_fake.quad_filter[unit]:0; }
const char *anygm_test_graphics_shader_source(int fragment){ return g_fake.shader_source[fragment?1:0]; }
void anygm_test_graphics_withhold_attributes(int withhold){ g_fake.no_attributes=withhold; }
int anygm_test_graphics_attributes_enabled(void){ return g_fake.attributes_enabled; }
unsigned anygm_test_graphics_bound_buffer(void){ return g_fake.bound_buffer; }
size_t anygm_test_graphics_buffer_bytes(void){ return g_fake.buffer_bytes; }
AnygmTestGraphicsStream anygm_test_graphics_stream(void){ return g_fake.stream; }
size_t anygm_test_graphics_instance_bytes(void *bytes,size_t capacity){
  GLuint buffer=g_fake.instance_buffer;
  if(!bytes || capacity<g_fake.instance_size || !buffer || buffer>=FAKE_MAX_CALLS ||
     !g_fake.buffer_store[buffer] || g_fake.instance_offset>g_fake.buffer_high_water[buffer] ||
     g_fake.instance_size>g_fake.buffer_high_water[buffer]-g_fake.instance_offset) return 0;
  memcpy(bytes,g_fake.buffer_store[buffer]+g_fake.instance_offset,g_fake.instance_size);
  return g_fake.instance_size;
}
unsigned anygm_test_graphics_float_uniforms(void){ return g_fake.float_uniforms; }
unsigned anygm_test_graphics_matrix_uniforms(void){ return g_fake.matrix_uniforms; }
unsigned anygm_test_graphics_read_pixels(void){ return g_fake.read_pixels; }
unsigned anygm_test_graphics_async_read_pixels(void){ return g_fake.async_read_pixels; }
int anygm_test_graphics_quad_colour(int corner,float *rgba){
  if(corner<0 || corner>3 || g_fake.buffer_bytes<(size_t)(4*9*sizeof(float))) return 0;
  for(int i=0;i<4;i++) rgba[i]=g_fake.buffer_data[corner*9+3+i];
  return 1;
}
int anygm_test_graphics_quad_position(int corner,float *xyz){
  if(!xyz || corner<0 || corner>3 ||
     g_fake.buffer_bytes<(size_t)(4*9*sizeof(float))) return 0;
  for(int i=0;i<3;i++) xyz[i]=g_fake.buffer_data[corner*9+i];
  return 1;
}
int anygm_test_graphics_content_matrix(unsigned index,float *matrix){
  if(index>=5 || !matrix || !g_fake.content_matrices_valid) return 0;
  memcpy(matrix,g_fake.content_matrices+index*16,16*sizeof(float));
  return 1;
}
int anygm_test_graphics_quad_texcoord(int corner,float *u,float *v){
  /* Nine floats a vertex: position, colour, texture coordinate. */
  if(corner<0 || corner>3 || g_fake.buffer_bytes<(size_t)(4*9*sizeof(float))) return 0;
  if(u) *u=g_fake.buffer_data[corner*9+7];
  if(v) *v=g_fake.buffer_data[corner*9+8];
  return 1;
}
unsigned anygm_test_graphics_attached_texture(void){ return g_fake.attached_texture; }
int anygm_test_graphics_clear_calls(void){ return g_fake.clear_calls; }
unsigned anygm_test_graphics_framebuffer_queries(void){ return g_fake.framebuffer_queries; }
unsigned anygm_test_graphics_bound_framebuffer(void){ return g_fake.framebuffer; }
unsigned anygm_test_graphics_finish_calls(void){ return g_fake.finish_calls; }
unsigned anygm_test_graphics_last_draw_framebuffer(void){ return g_fake.last_draw_framebuffer; }
int anygm_test_graphics_blend_enabled(void){ return g_fake.blend_enabled; }
int anygm_test_graphics_scene_filter(void){ return g_fake.scene_filter; }
int anygm_test_graphics_scene_sampler(void){ return g_fake.scene_sampler; }
unsigned anygm_test_graphics_scene_mask_draws(unsigned mask){
  return mask<16u?g_fake.scene_mask_draws[mask]:0u;
}
void anygm_test_graphics_blend_factors(unsigned factors[4]){
  for(unsigned index=0;index<4u;index++) factors[index]=g_fake.blend_factors[index];
}
unsigned anygm_test_graphics_blend_configurations(void){ return g_fake.blend_configurations; }
unsigned anygm_test_graphics_scene_copies(void){ return g_fake.scene_copies; }
uint64_t anygm_test_graphics_scene_copied_pixels(void){ return g_fake.scene_copied_pixels; }
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
int anygm_test_graphics_texture_deleted(unsigned name){
  return name<FAKE_MAX_TEXTURES?g_fake.textures[name].deleted:0;
}
unsigned anygm_test_graphics_texture_data_uploads(unsigned name){
  return name<FAKE_MAX_TEXTURES?g_fake.textures[name].data_upload_count:0u;
}

const unsigned char *anygm_test_graphics_texture_bytes(unsigned name,size_t *count){
  if(name>=FAKE_MAX_TEXTURES){ if(count) *count=0; return NULL; }
  if(count) *count=g_fake.textures[name].byte_count;
  return g_fake.textures[name].bytes;
}

unsigned anygm_test_graphics_invalidates(unsigned *attachment,unsigned *framebuffer,unsigned *prior_clears){
  if(attachment) *attachment=g_fake.invalidate_attachment;
  if(framebuffer) *framebuffer=g_fake.invalidate_framebuffer;
  if(prior_clears) *prior_clears=g_fake.invalidate_prior_clears;
  return g_fake.invalidates;
}

void anygm_test_graphics_version(unsigned major,unsigned minor){
  g_fake.version_major=major;g_fake.version_minor=minor;
}

unsigned anygm_test_graphics_full_attachment_clears(void){return g_fake.full_attachment_clears;}

unsigned anygm_test_graphics_framebuffer_attachments(void){ return g_fake.framebuffer_attachments; }

void anygm_test_graphics_box_destination(int values[2]){
  values[0]=g_fake.box_destination[0];values[1]=g_fake.box_destination[1];
}
