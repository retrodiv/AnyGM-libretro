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
#include "gml_gpu_internal.h"
#include "gml_gpu_gl_api.h"
#include "gml_gpu_gl_shaders.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  GL_MAP_SLOTS=GML_PLAN_MAX_OPERATIONS*2,
  GL_INFO_LOG_LIMIT=512
};

typedef struct GlSourceTexture {
  GLuint name;
  uint32_t image_class;
  uint32_t identity;
  uint32_t content_generation;
  uint32_t pixel_generation;
  uint32_t width;
  uint32_t height;
  uint32_t last_used;
  int uploaded;
} GlSourceTexture;

/* An axis map is a pure function of the axis record, so the record is also its cache key. Keeping
 * the key instead of the contents makes the comparison a handful of scalars rather than several
 * kilobytes, and it cannot disagree with the map it describes. */
typedef struct GlAxisMap {
  GLuint name;
  uint32_t capacity;
  uint32_t count;
  uint32_t rule;
  uint32_t source_extent;
  uint32_t destination_extent;
  int32_t destination_first;
  double origin;
  double extent;
  int valid;
} GlAxisMap;

struct GmlGpuBackend {
  GmlGlApi gl;
  int loaded;
  GmlGpuApi api;
  int little_endian;
  GLint max_texture_size;
  GLuint program;
  GLuint vertex_array;
  GLint u_source;
  GLint u_map_x;
  GLint u_map_y;
  GLint u_dest_x;
  GLint u_dest_y;
  GLint u_target_height;
  GlSourceTexture sources[GML_PLAN_MAX_IMAGES];
  GlAxisMap maps[GL_MAP_SLOTS];
  uint16_t *map_scratch;
  uint32_t map_scratch_capacity;
  uint32_t use_clock;
};

static void record_error(char *error,size_t capacity,const char *format,...);

static void record_error(char *error,size_t capacity,const char *format,...){
  va_list arguments;
  if(!error || capacity==0) return;
  va_start(arguments,format);
  vsnprintf(error,capacity,format,arguments);
  va_end(arguments);
}

GmlGpuBackend *gml_gpu_gl_create(void){
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
void gml_gpu_gl_forget(GmlGpuBackend *backend){
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
  if(backend->vertex_array && backend->gl.DeleteVertexArrays)
    backend->gl.DeleteVertexArrays(1,&backend->vertex_array);
}

void gml_gpu_gl_destroy(GmlGpuBackend *backend,int context_is_current){
  if(!backend) return;
  if(context_is_current) delete_objects(backend);
  free(backend->map_scratch);
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
  X(DrawArrays,"glDrawArrays")

static int load_entry_points(GmlGpuBackend *backend,const GmlGpuContext *context,
                             char *error,size_t error_capacity){
  GmlGpuProc proc;
#define GL_LOAD_ONE(field,name) \
  proc=context->get_proc_address(context->userdata,name); \
  if(!proc){ record_error(error,error_capacity,"missing graphics entry point: %s",name); return 0; } \
  memcpy(&backend->gl.field,&proc,sizeof proc);
  GL_MANDATORY_ENTRY_POINTS(GL_LOAD_ONE)
#undef GL_LOAD_ONE
  return 1;
}

static GLuint compile_stage(GmlGpuBackend *backend,GLenum stage,const char *source,
                            char *error,size_t error_capacity){
  GLuint shader=backend->gl.CreateShader(stage);
  GLint status=GL_FALSE;
  if(!shader){
    record_error(error,error_capacity,"graphics shader object could not be created");
    return 0;
  }
  backend->gl.ShaderSource(shader,1,&source,NULL);
  backend->gl.CompileShader(shader);
  backend->gl.GetShaderiv(shader,GL_COMPILE_STATUS,&status);
  if(status!=GL_TRUE){
    char log[GL_INFO_LOG_LIMIT];
    GLsizei written=0;
    log[0]='\0';
    backend->gl.GetShaderInfoLog(shader,(GLsizei)sizeof log,&written,log);
    log[sizeof log-1]='\0';
    record_error(error,error_capacity,"graphics %s shader failed: %s",
                 stage==GL_VERTEX_SHADER?"vertex":"fragment",log);
    backend->gl.DeleteShader(shader);
    return 0;
  }
  return shader;
}

static int build_program(GmlGpuBackend *backend,char *error,size_t error_capacity){
  char vertex_source[512];
  char fragment_source[1536];
  const char *prefix=backend->api==GML_GPU_API_OPENGLES3
    ?GML_GPU_GL_PREFIX_ES:GML_GPU_GL_PREFIX_DESKTOP;
  GLuint vertex,fragment;
  GLint status=GL_FALSE;
  snprintf(vertex_source,sizeof vertex_source,"%s%s",prefix,GML_GPU_GL_VERTEX_BODY);
  snprintf(fragment_source,sizeof fragment_source,"%s%s%s%s",prefix,
           GML_GPU_GL_FRAGMENT_BODY_HEAD,
           backend->little_endian?GML_GPU_GL_FRAGMENT_SWIZZLE_LITTLE
                                 :GML_GPU_GL_FRAGMENT_SWIZZLE_BIG,
           GML_GPU_GL_FRAGMENT_BODY_TAIL);
  vertex=compile_stage(backend,GL_VERTEX_SHADER,vertex_source,error,error_capacity);
  if(!vertex) return 0;
  fragment=compile_stage(backend,GL_FRAGMENT_SHADER,fragment_source,error,error_capacity);
  if(!fragment){ backend->gl.DeleteShader(vertex); return 0; }
  backend->program=backend->gl.CreateProgram();
  if(!backend->program){
    record_error(error,error_capacity,"graphics program object could not be created");
    backend->gl.DeleteShader(vertex);
    backend->gl.DeleteShader(fragment);
    return 0;
  }
  backend->gl.AttachShader(backend->program,vertex);
  backend->gl.AttachShader(backend->program,fragment);
  backend->gl.LinkProgram(backend->program);
  backend->gl.GetProgramiv(backend->program,GL_LINK_STATUS,&status);
  backend->gl.DeleteShader(vertex);
  backend->gl.DeleteShader(fragment);
  if(status!=GL_TRUE){
    char log[GL_INFO_LOG_LIMIT];
    GLsizei written=0;
    log[0]='\0';
    backend->gl.GetProgramInfoLog(backend->program,(GLsizei)sizeof log,&written,log);
    log[sizeof log-1]='\0';
    record_error(error,error_capacity,"graphics program failed to link: %s",log);
    backend->gl.DeleteProgram(backend->program);
    backend->program=0;
    return 0;
  }
  backend->u_source=backend->gl.GetUniformLocation(backend->program,"u_source");
  backend->u_map_x=backend->gl.GetUniformLocation(backend->program,"u_map_x");
  backend->u_map_y=backend->gl.GetUniformLocation(backend->program,"u_map_y");
  backend->u_dest_x=backend->gl.GetUniformLocation(backend->program,"u_dest_x");
  backend->u_dest_y=backend->gl.GetUniformLocation(backend->program,"u_dest_y");
  backend->u_target_height=backend->gl.GetUniformLocation(backend->program,"u_target_height");
  if(backend->u_source<0 || backend->u_map_x<0 || backend->u_map_y<0 ||
     backend->u_dest_x<0 || backend->u_dest_y<0 || backend->u_target_height<0){
    record_error(error,error_capacity,"graphics program is missing a required uniform");
    backend->gl.DeleteProgram(backend->program);
    backend->program=0;
    return 0;
  }
  return 1;
}

int gml_gpu_gl_reset(GmlGpuBackend *backend,const GmlGpuContext *context,
                     char *error,size_t error_capacity){
  if(!backend || !context) return 0;
  gml_gpu_gl_forget(backend);
  backend->api=context->api;
  if(!load_entry_points(backend,context,error,error_capacity)) return 0;
  backend->loaded=1;
  /* Drain whatever the frontend left behind so that a later check reports this backend's own
   * failure rather than somebody else's. */
  while(backend->gl.GetError()!=GL_NO_ERROR){}
  if(!build_program(backend,error,error_capacity)){ backend->loaded=0; return 0; }
  backend->gl.GenVertexArrays(1,&backend->vertex_array);
  if(!backend->vertex_array){
    record_error(error,error_capacity,"graphics vertex array could not be created");
    backend->gl.DeleteProgram(backend->program);
    backend->program=0;
    backend->loaded=0;
    return 0;
  }
  backend->max_texture_size=0;
  return 1;
}

static GlSourceTexture *acquire_source(GmlGpuBackend *backend,const GmlPlanImage *image){
  GlSourceTexture *slot=NULL;
  uint32_t oldest=0xFFFFFFFFu;
  for(size_t index=0;index<GML_PLAN_MAX_IMAGES;index++){
    GlSourceTexture *candidate=&backend->sources[index];
    if(candidate->name && candidate->image_class==image->image_class &&
       candidate->identity==image->identity) return candidate;
  }
  for(size_t index=0;index<GML_PLAN_MAX_IMAGES;index++){
    GlSourceTexture *candidate=&backend->sources[index];
    if(!candidate->name){ slot=candidate; break; }
    if(candidate->last_used<oldest){ oldest=candidate->last_used; slot=candidate; }
  }
  if(!slot) return NULL;
  if(!slot->name){
    backend->gl.GenTextures(1,&slot->name);
    if(!slot->name) return NULL;
  }
  slot->image_class=image->image_class;
  slot->identity=image->identity;
  slot->uploaded=0;
  slot->width=0;
  slot->height=0;
  return slot;
}

static int upload_source(GmlGpuBackend *backend,const GmlPlanImage *image,
                         GmlGpuCounters *counters,GLuint *out_name){
  GlSourceTexture *slot=acquire_source(backend,image);
  int recreate;
  if(!slot) return 0;
  slot->last_used=++backend->use_clock;
  recreate=!slot->uploaded || slot->width!=image->width || slot->height!=image->height;
  if(!recreate && slot->content_generation==image->content_generation &&
     slot->pixel_generation==image->pixel_generation){
    *out_name=slot->name;
    return 1;
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,slot->name);
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,4);
  backend->gl.PixelStorei(GL_UNPACK_ROW_LENGTH,(GLint)image->pitch_pixels);
  if(recreate){
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)image->width,
                           (GLsizei)image->height,0,GL_RGBA,GL_UNSIGNED_BYTE,
                           image->cpu_pixels);
    /* Nearest with clamped edges and exactly one level: a sampler that could reach a mip level or
     * an edge outside the image would fetch a texel the software executor never selects. */
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
    slot->width=image->width;
    slot->height=image->height;
    slot->uploaded=1;
  } else {
    backend->gl.TexSubImage2D(GL_TEXTURE_2D,0,0,0,(GLsizei)image->width,
                              (GLsizei)image->height,GL_RGBA,GL_UNSIGNED_BYTE,
                              image->cpu_pixels);
  }
  backend->gl.PixelStorei(GL_UNPACK_ROW_LENGTH,0);
  slot->content_generation=image->content_generation;
  slot->pixel_generation=image->pixel_generation;
  if(counters){
    counters->full_uploads++;
    counters->uploaded_bytes+=(uint64_t)image->width*image->height*4u;
  }
  *out_name=slot->name;
  return 1;
}

static int axis_matches(const GlAxisMap *slot,const GmlPlanAxis *axis,uint32_t count){
  return slot->valid && slot->count==count && slot->rule==axis->rule &&
         slot->source_extent==axis->source_extent &&
         slot->destination_extent==axis->destination_extent &&
         slot->destination_first==axis->destination_first &&
         slot->origin==axis->origin && slot->extent==axis->extent;
}

static int upload_axis(GmlGpuBackend *backend,uint32_t slot_index,const GmlPlanAxis *axis,
                       uint32_t count,GmlGpuCounters *counters,GLuint *out_name){
  GlAxisMap *slot;
  if(slot_index>=GL_MAP_SLOTS || count==0u) return 0;
  slot=&backend->maps[slot_index];
  if(axis_matches(slot,axis,count) && slot->name){ *out_name=slot->name; return 1; }
  if(backend->map_scratch_capacity<count){
    uint16_t *grown=(uint16_t*)realloc(backend->map_scratch,(size_t)count*sizeof *grown);
    if(!grown) return 0;
    backend->map_scratch=grown;
    backend->map_scratch_capacity=count;
  }
  if(!gml_render_plan_axis_map(axis,backend->map_scratch,count)) return 0;
  if(!slot->name){
    backend->gl.GenTextures(1,&slot->name);
    if(!slot->name) return 0;
    slot->capacity=0;
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,slot->name);
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,2);
  backend->gl.PixelStorei(GL_UNPACK_ROW_LENGTH,0);
  if(slot->capacity!=count){
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,(GLint)GL_R16UI,(GLsizei)count,1,0,
                           GL_RED_INTEGER,GL_UNSIGNED_SHORT,backend->map_scratch);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
    slot->capacity=count;
  } else {
    backend->gl.TexSubImage2D(GL_TEXTURE_2D,0,0,0,(GLsizei)count,1,
                              GL_RED_INTEGER,GL_UNSIGNED_SHORT,backend->map_scratch);
  }
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,4);
  slot->count=count;
  slot->rule=axis->rule;
  slot->source_extent=axis->source_extent;
  slot->destination_extent=axis->destination_extent;
  slot->destination_first=axis->destination_first;
  slot->origin=axis->origin;
  slot->extent=axis->extent;
  slot->valid=1;
  if(counters) counters->uploaded_bytes+=(uint64_t)count*2u;
  *out_name=slot->name;
  return 1;
}

/* AnyGM addresses rows from the top; the framebuffer's own origin is at the bottom. The rectangle
 * conversion happens here and nowhere else, and the shader converts the fragment's row with the
 * same target height, so one asymmetric fixture proves both. */
static void set_rect(GmlGpuBackend *backend,const GmlPlanRect *rect,uint32_t target_height){
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

int gml_gpu_gl_execute(GmlGpuBackend *backend,const GmlGpuContext *context,
                       const GmlRenderPlan *plan,GmlGpuCounters *counters,
                       char *error,size_t error_capacity){
  uintptr_t framebuffer;
  int failed=0;
  if(!backend || !backend->loaded || !context || !plan) return 0;
  framebuffer=context->get_current_framebuffer(context->userdata);
  /* Handle zero is a valid target: some frontends hand over the default framebuffer. */
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)framebuffer);

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
    if(op->opcode==GML_PLAN_OP_CLEAR_XRGB){
      set_rect(backend,&op->destination,plan->target_height);
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
      if(!upload_source(backend,image,counters,&source_name)){
        record_error(error,error_capacity,"graphics source upload failed");
        failed=1;
        break;
      }
      if(!upload_axis(backend,index*2u,&axis_x,op->destination.width,counters,&map_x) ||
         !upload_axis(backend,index*2u+1u,&axis_y,op->destination.height,counters,&map_y)){
        record_error(error,error_capacity,"graphics axis map upload failed");
        failed=1;
        break;
      }
      set_rect(backend,&op->destination,plan->target_height);
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
      record_error(error,error_capacity,"graphics execution reported error 0x%04X",
                   (unsigned)status);
      while(backend->gl.GetError()!=GL_NO_ERROR){}
      failed=1;
    }
  }
  /* A pass that stopped part way has written something the plan does not describe. Report the
   * failure so the caller replays the complete pass, which overwrites it. */
  return failed?0:1;
}
