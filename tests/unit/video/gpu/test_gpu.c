/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Lifecycle and call-level contract for the hardware backend, driven by a graphics driver written
 * here rather than by a real one.
 *
 * The split is deliberate. What a fake driver can prove is exactly what a real one makes awkward:
 * that every entry point is resolved through the host callback and a missing one is named and
 * refused, that a context which disappeared is forgotten rather than deleted into its successor,
 * that every piece of shared state a pass depends on is set rather than inherited, and that the
 * rectangle handed to the driver is the flipped one. What it cannot prove is which pixel comes
 * out; restating the shader here would only compare a rule with itself. That question belongs to
 * a separate hardware-host comparison against the software executor. */
#include "anygm_test_runner.h"

#include "gml_gpu.h"
#include "gml_gpu_gl_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"gpu failed: %s\n",label); \
    return 0; \
  } \
}while(0)

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

static void fake_reset(void){
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

typedef struct { const char *name; GmlGpuProc proc; } FakeEntry;

static GmlGpuProc fake_get_proc(void *userdata,const char *name){
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
      GmlGpuProc proc;
      memcpy(&proc,&table[index].proc,sizeof proc);
      return proc;
    }
  return NULL;
}

static uintptr_t fake_get_framebuffer(void *userdata){
  (void)userdata;
  g_fake.framebuffer_queries++;
  return 7u;
}

static GmlGpuContext fake_context(GmlGpuApi api){
  GmlGpuContext context;
  memset(&context,0,sizeof context);
  context.api=api;
  context.version_major=3;
  context.version_minor=api==GML_GPU_API_OPENGLES3?0u:3u;
  context.get_proc_address=fake_get_proc;
  context.get_current_framebuffer=fake_get_framebuffer;
  return context;
}

/* A plan that magnifies a small opaque source into a larger target with black margins: the shape
 * the host-canvas pass produces. */
static int build_canvas_plan(GmlRenderPlan *plan,const uint32_t *pixels,
                             uint32_t source_width,uint32_t source_height,
                             uint32_t target_width,uint32_t target_height,
                             int32_t canvas_x,int32_t canvas_y,
                             uint32_t canvas_width,uint32_t canvas_height){
  GmlPlanImage image;
  GmlPlanRect whole={0,0,0,0};
  GmlPlanRect canvas;
  GmlPlanAxis axis_x,axis_y;
  uint32_t index;
  whole.width=target_width;
  whole.height=target_height;
  memset(&image,0,sizeof image);
  image.image_class=GML_PLAN_IMAGE_COMPLETED_FRAME;
  image.identity=1u;
  image.content_generation=1u;
  image.pixel_generation=1u;
  image.width=source_width;
  image.height=source_height;
  image.pitch_pixels=source_width;
  image.pixel_format=GML_PLAN_PIXEL_XRGB8888;
  image.opaque=1u;
  image.cpu_pixels=pixels;
  memset(&axis_x,0,sizeof axis_x);
  memset(&axis_y,0,sizeof axis_y);
  axis_x.rule=GML_PLAN_AXIS_ACCUMULATOR;
  axis_x.source_extent=source_width;
  axis_x.destination_extent=canvas_width;
  axis_y.rule=GML_PLAN_AXIS_ACCUMULATOR;
  axis_y.source_extent=source_height;
  axis_y.destination_extent=canvas_height;
  canvas.x=canvas_x;
  canvas.y=canvas_y;
  canvas.width=canvas_width;
  canvas.height=canvas_height;
  gml_render_plan_reset(plan,GML_PLAN_TARGET_HOST_FRAMEBUFFER,target_width,target_height);
  if(!gml_render_plan_add_clear(plan,whole,0u)) return 0;
  index=gml_render_plan_add_image(plan,&image);
  if(index==GML_PLAN_NO_IMAGE) return 0;
  return gml_render_plan_add_blit_nearest(plan,index,canvas,axis_x,axis_y,0u);
}

static int lifecycle_reset_case(void){
  GmlGpu *gpu;
  fake_reset();
  gpu=gml_gpu_create();
  REQUIRE(gpu!=NULL,"the subsystem is created without a context");
  REQUIRE(!gml_gpu_context_active(gpu),"creation adopts no context");
  REQUIRE(gml_gpu_context_reset(gpu,&(GmlGpuContext){0})==0,"an empty context is refused");
  {
    GmlGpuContext context=fake_context(GML_GPU_API_OPENGL_CORE);
    REQUIRE(gml_gpu_context_reset(gpu,&context),"a desktop context is adopted");
    REQUIRE(gml_gpu_context_active(gpu),"the adopted context is active");
  }
  {
    /* A reset with no destroy before it: the previous context is already gone, so nothing may be
     * deleted into the new one. */
    GmlGpuContext context=fake_context(GML_GPU_API_OPENGLES3);
    int before=g_fake.deletes_issued;
    REQUIRE(gml_gpu_context_reset(gpu,&context),"an embedded context is adopted");
    REQUIRE(g_fake.deletes_issued==before,"a reset without a destroy deletes nothing");
  }
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.context_resets==2,"both resets are counted");
  }
  gml_gpu_destroy(gpu,1);
  REQUIRE(g_fake.deletes_issued>0,"a controlled destroy deletes while the context is current");
  return 1;
}

static int lifecycle_loss_case(void){
  GmlGpu *gpu;
  GmlGpuContext context;
  fake_reset();
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  gml_gpu_context_lost(gpu);
  REQUIRE(!gml_gpu_context_active(gpu),"a lost context is no longer active");
  REQUIRE(g_fake.deletes_issued==0,"a lost context is forgotten, not deleted");
  /* Anything reaching the driver from here would be a call into a context that is gone. */
  g_fake.allow_calls=0;
  gml_gpu_destroy(gpu,1);
  REQUIRE(g_fake.calls_after_forget==0,"destroying after a loss issues no graphics call");
  return 1;
}

static int lifecycle_destroy_without_context_case(void){
  GmlGpu *gpu;
  GmlGpuContext context;
  fake_reset();
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  g_fake.allow_calls=0;
  gml_gpu_destroy(gpu,0);
  REQUIRE(g_fake.calls_after_forget==0,"destroying without a current context issues no call");
  return 1;
}

static int missing_entry_point_case(void){
  GmlGpu *gpu;
  GmlGpuContext context;
  fake_reset();
  g_fake.withhold="glTexSubImage2D";
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(!gml_gpu_context_reset(gpu,&context),"a missing entry point rejects the context");
  REQUIRE(!gml_gpu_context_active(gpu),"a rejected context is not active");
  {
    const char *error=gml_gpu_last_error(gpu);
    REQUIRE(error!=NULL,"a rejected context leaves a reason");
    REQUIRE(strstr(error,"glTexSubImage2D")!=NULL,"the reason names the exact missing entry point");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int program_failure_case(void){
  GmlGpu *gpu;
  GmlGpuContext context;
  fake_reset();
  g_fake.link_fails=1;
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGLES3);
  REQUIRE(!gml_gpu_context_reset(gpu,&context),"a program that will not link rejects the context");
  {
    const char *error=gml_gpu_last_error(gpu);
    REQUIRE(error && strstr(error,"link")!=NULL,"the reason names the failing stage");
  }
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.program_failures==1,"the failure is counted once");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int execute_state_and_geometry_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=3,TARGET_WIDTH=40,TARGET_HEIGHT=30,
         CANVAS_X=4,CANVAS_Y=6,CANVAS_WIDTH=32,CANVAS_HEIGHT=18 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  fake_reset();
  for(size_t index=0;index<SOURCE_WIDTH*SOURCE_HEIGHT;index++)
    source[index]=0xFF000000u|(uint32_t)(index*7u+3u);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");

  /* Hostile prior state: the frontend shares this context and may leave anything enabled. Every
   * piece of state the pass depends on has to be set rather than inherited. */
  g_fake.blend_enabled=1;
  g_fake.depth_enabled=1;
  g_fake.stencil_enabled=1;
  g_fake.cull_enabled=1;
  g_fake.dither_enabled=1;
  g_fake.color_mask_all=0;
  g_fake.depth_mask=1;

  REQUIRE(build_canvas_plan(&plan,source,SOURCE_WIDTH,SOURCE_HEIGHT,
                            TARGET_WIDTH,TARGET_HEIGHT,
                            CANVAS_X,CANVAS_Y,CANVAS_WIDTH,CANVAS_HEIGHT),"plan built");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"the plan executes");

  REQUIRE(g_fake.framebuffer_queries==1,"the current framebuffer is queried for the frame");
  REQUIRE(!g_fake.blend_enabled,"blending is disabled");
  REQUIRE(!g_fake.depth_enabled,"depth testing is disabled");
  REQUIRE(!g_fake.stencil_enabled,"stencil testing is disabled");
  REQUIRE(!g_fake.cull_enabled,"face culling is disabled");
  REQUIRE(!g_fake.dither_enabled,"dithering is disabled");
  REQUIRE(g_fake.color_mask_all,"every colour channel is written");
  REQUIRE(!g_fake.depth_mask,"depth writes are off");
  REQUIRE(!g_fake.scissor_enabled,"the scissor test is left disabled for the frontend");
  REQUIRE(g_fake.bound_program==0,"the program is unbound at the end of the pass");
  REQUIRE(g_fake.bound_vertex_array==0,"the vertex array is unbound at the end of the pass");
  REQUIRE(g_fake.clear_calls==1,"the margins are cleared once");
  REQUIRE(g_fake.clear_colour[0]==0.0f && g_fake.clear_colour[1]==0.0f &&
          g_fake.clear_colour[2]==0.0f && g_fake.clear_colour[3]==1.0f,
          "the clear is exactly black and opaque");
  REQUIRE(g_fake.draw_calls==1,"one draw covers the canvas");

  /* The rectangle reaches the driver flipped, because AnyGM addresses rows from the top and the
   * framebuffer's origin is at the bottom. A pass that got this wrong would still look plausible
   * on a symmetric canvas, so the fixture is deliberately off-centre. */
  REQUIRE(g_fake.viewport[0]==CANVAS_X,"viewport x is the canvas origin");
  REQUIRE(g_fake.viewport[1]==TARGET_HEIGHT-(CANVAS_Y+CANVAS_HEIGHT),
          "viewport y is measured from the bottom");
  REQUIRE(g_fake.viewport[2]==CANVAS_WIDTH && g_fake.viewport[3]==CANVAS_HEIGHT,
          "the viewport is the canvas extent");
  REQUIRE(g_fake.scissor[0]==g_fake.viewport[0] && g_fake.scissor[1]==g_fake.viewport[1] &&
          g_fake.scissor[2]==g_fake.viewport[2] && g_fake.scissor[3]==g_fake.viewport[3],
          "the scissor matches the viewport");
  REQUIRE(g_fake.unpack_row_length==0,"the source pitch override is restored");
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int upload_and_reuse_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=3,TARGET=16 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  uint32_t first_uploads=0;
  fake_reset();
  for(size_t index=0;index<SOURCE_WIDTH*SOURCE_HEIGHT;index++)
    source[index]=0xFF000000u|(uint32_t)(index*11u+5u);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGLES3);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  REQUIRE(build_canvas_plan(&plan,source,SOURCE_WIDTH,SOURCE_HEIGHT,TARGET,TARGET,
                            0,0,TARGET,TARGET),"plan built");
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"first execution");

  /* The source arrives as raw XRGB words: the driver must receive exactly those bytes, unswizzled
   * and unpadded, because the shader is what puts the components in order. */
  {
    GLuint name=0;
    for(GLuint index=1;index<FAKE_MAX_TEXTURES;index++)
      if(g_fake.textures[index].created &&
         g_fake.textures[index].internal_format==(GLint)GL_RGBA8) name=index;
    REQUIRE(name!=0,"the source became a normalized eight-bit texture");
    REQUIRE(g_fake.textures[name].width==SOURCE_WIDTH &&
            g_fake.textures[name].height==SOURCE_HEIGHT,"the source keeps its extent");
    REQUIRE(g_fake.textures[name].byte_count==sizeof source,"the whole source was uploaded");
    REQUIRE(memcmp(g_fake.textures[name].bytes,source,sizeof source)==0,
            "the uploaded bytes are the source words unchanged");
    first_uploads=g_fake.textures[name].upload_count;
  }
  /* The axis maps are integer textures, and their contents are the exact indices the software
   * executor selects. */
  {
    GLuint name=0;
    for(GLuint index=1;index<FAKE_MAX_TEXTURES;index++)
      if(g_fake.textures[index].created &&
         g_fake.textures[index].internal_format==(GLint)GL_R16UI) { name=index; break; }
    REQUIRE(name!=0,"the axis map became an unsigned integer texture");
    REQUIRE(g_fake.textures[name].height==1,"an axis map is one row");
    REQUIRE(g_fake.textures[name].width==TARGET,"an axis map covers the destination extent");
    {
      const uint16_t *map=(const uint16_t*)g_fake.textures[name].bytes;
      for(uint32_t index=0;index<TARGET;index++){
        uint32_t expected=(uint32_t)(((uint64_t)SOURCE_WIDTH*(2u*(uint64_t)index+1u))/
                                     (2u*(uint64_t)TARGET));
        REQUIRE(map[index]==expected,"the axis map holds the exact source index");
      }
    }
  }
  /* An unchanged generation must not upload again: a monitor-sized re-upload every frame is the
   * cost this path exists to remove. */
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"second execution");
  {
    GLuint name=0;
    for(GLuint index=1;index<FAKE_MAX_TEXTURES;index++)
      if(g_fake.textures[index].created &&
         g_fake.textures[index].internal_format==(GLint)GL_RGBA8) name=index;
    REQUIRE(g_fake.textures[name].upload_count==first_uploads,
            "an unchanged source is not uploaded again");
  }
  /* A changed generation must. */
  plan.images[0].pixel_generation=2u;
  REQUIRE(gml_gpu_execute_plan(gpu,&plan),"third execution");
  {
    GLuint name=0;
    for(GLuint index=1;index<FAKE_MAX_TEXTURES;index++)
      if(g_fake.textures[index].created &&
         g_fake.textures[index].internal_format==(GLint)GL_RGBA8) name=index;
    REQUIRE(g_fake.textures[name].upload_count>first_uploads,
            "a changed generation uploads again");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int reduction_falls_back_case(void){
  enum { SOURCE=32,TARGET=8 };
  uint32_t source[SOURCE*SOURCE];
  GmlGpu *gpu;
  GmlGpuContext context;
  GmlRenderPlan plan;
  GmlPlanImage image;
  GmlPlanRect whole={0,0,TARGET,TARGET};
  uint32_t index;
  fake_reset();
  memset(source,0,sizeof source);
  gpu=gml_gpu_create();
  context=fake_context(GML_GPU_API_OPENGL_CORE);
  REQUIRE(gml_gpu_context_reset(gpu,&context),"context adopted");
  memset(&image,0,sizeof image);
  image.image_class=GML_PLAN_IMAGE_COMPLETED_FRAME;
  image.identity=1u;
  image.width=SOURCE;
  image.height=SOURCE;
  image.pitch_pixels=SOURCE;
  image.pixel_format=GML_PLAN_PIXEL_XRGB8888;
  image.cpu_pixels=source;
  gml_render_plan_reset(&plan,GML_PLAN_TARGET_HOST_FRAMEBUFFER,TARGET,TARGET);
  index=gml_render_plan_add_image(&plan,&image);
  REQUIRE(gml_render_plan_add_blit_box(&plan,index,whole,0u),"reduction recorded");
  {
    int draws_before=g_fake.draw_calls;
    REQUIRE(!gml_gpu_execute_plan(gpu,&plan),"the reduction is refused");
    REQUIRE(g_fake.draw_calls==draws_before,"a refused pass publishes no partial work");
    REQUIRE(plan.fallback_reason==GML_PLAN_FALLBACK_BOX_REDUCTION,"the refusal names itself");
  }
  {
    GmlGpuCounters counters;
    gml_gpu_counters(gpu,&counters);
    REQUIRE(counters.passes_replayed==1,"the software replay is counted");
    REQUIRE(counters.fallbacks[GML_PLAN_FALLBACK_BOX_REDUCTION]==1,"the reason is counted");
    REQUIRE(counters.passes_accepted==0,"nothing was accepted");
  }
  gml_gpu_destroy(gpu,1);
  return 1;
}

static int no_context_falls_back_case(void){
  enum { SOURCE=4,TARGET=8 };
  uint32_t source[SOURCE*SOURCE];
  GmlGpu *gpu;
  GmlRenderPlan plan;
  fake_reset();
  memset(source,0,sizeof source);
  gpu=gml_gpu_create();
  REQUIRE(build_canvas_plan(&plan,source,SOURCE,SOURCE,TARGET,TARGET,0,0,TARGET,TARGET),
          "plan built");
  REQUIRE(!gml_gpu_execute_plan(gpu,&plan),"a plan without a context is refused");
  REQUIRE(plan.fallback_reason==GML_PLAN_FALLBACK_CONTEXT_UNAVAILABLE,"the refusal names itself");
  REQUIRE(g_fake.draw_calls==0,"nothing reached a driver");
  gml_gpu_destroy(gpu,0);
  return 1;
}

int main(int argc,char **argv){
  const char *filter=NULL;
  for(int index=1;index<argc;++index){
    if(strcmp(argv[index],"--case")) continue;
    if(index+1>=argc){
      fprintf(stderr,"--case requires a filter\n");
      return EXIT_FAILURE;
    }
    filter=argv[++index];
  }
  static const AnygmTestCase lifecycle_cases[]={
    {"reset",lifecycle_reset_case},
    {"context_loss",lifecycle_loss_case},
    {"destroy_without_context",lifecycle_destroy_without_context_case},
    {"missing_entry_point",missing_entry_point_case},
    {"program_failure",program_failure_case},
  };
  static const AnygmTestCase execution_cases[]={
    {"state_and_geometry",execute_state_and_geometry_case},
    {"upload_and_reuse",upload_and_reuse_case},
    {"reduction_falls_back",reduction_falls_back_case},
    {"no_context_falls_back",no_context_falls_back_case},
  };
  const AnygmTestGroup groups[]={
    {"lifecycle",lifecycle_cases,sizeof lifecycle_cases/sizeof lifecycle_cases[0]},
    {"execution",execution_cases,sizeof execution_cases/sizeof execution_cases[0]},
  };
  AnygmTestResult result;
  anygm_test_run_groups(groups,sizeof groups/sizeof groups[0],filter,&result);
  printf("gpu cases: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
