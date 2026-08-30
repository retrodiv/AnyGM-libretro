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

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  GL_MAP_SLOTS=GML_PLAN_MAX_OPERATIONS*2,
  GL_INFO_LOG_LIMIT=512,
  GL_CONTENT_PROGRAMS=16,
  GL_CONTENT_UNIFORMS=64
};

/* One uniform the content's program declares, as the context reports it after linking. A value
 * the content sets is delivered through the setter this type calls for, whatever width the content
 * supplied: sending a vector to a scalar, or a matrix through a vector setter, is an error the
 * context refuses the whole draw over. */
typedef struct GlContentUniform {
  char name[48];
  GLint location;
  GLint size;
  GLenum type;
} GlContentUniform;

/* A content program compiled for this context, keyed by the shader's identity and the text it was
 * compiled from. `failed` retires a program the context refused, so a frame does not pay the
 * refusal again and the runtime can answer that the shader did not compile. */
typedef struct GlContentProgram {
  GLuint program;
  uint32_t identity;
  const char *fragment_key;
  int failed;
  uint32_t last_used;
  GLint a_position,a_colour,a_texcoord;
  GLint u_matrices,u_base_texture,u_alpha_test,u_fog_ps,u_fog_vs;
  GlContentUniform uniforms[GL_CONTENT_UNIFORMS];
  uint32_t uniform_count;
} GlContentProgram;

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
  GlContentProgram content[GL_CONTENT_PROGRAMS];
  GLuint content_buffer;
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
  memset(backend->content,0,sizeof backend->content);
  backend->content_buffer=0;
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
  X(GetActiveUniform,"glGetActiveUniform")

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

static void set_rect(GmlGpuBackend *backend,const GmlPlanRect *rect,uint32_t target_height);

/* ---- The content's own programs ----
 *
 * A Studio payload ships its shaders as GLSL ES 1.00 text, with a desktop GLSL 1.20 rendering
 * beside it. Neither is what a core-profile context accepts, so the text is rewritten, token by
 * token and nothing more: the storage qualifiers and the sampling calls of the older dialect are
 * spelled the newer way, the fragment output is declared, and one helper is added that samples a
 * texture and reorders its channels, because every picture this backend uploads is the runtime's
 * XRGB word layout as raw bytes. The content's own expressions are untouched. */

static int glsl_word_boundary(const char *text,size_t index,size_t length){
  int before=index==0 || !(isalnum((unsigned char)text[index-1]) || text[index-1]=='_');
  size_t end=index+length;
  int after=!(isalnum((unsigned char)text[end]) || text[end]=='_');
  return before && after;
}

/* Append `text` to a growing buffer. */
static int glsl_append(char **buffer,size_t *length,size_t *capacity,const char *text,size_t count){
  if(*length+count+1>*capacity){
    size_t grown=(*capacity?*capacity:1024u);
    char *next;
    while(grown<*length+count+1) grown*=2u;
    next=(char*)realloc(*buffer,grown);
    if(!next) return 0;
    *buffer=next;
    *capacity=grown;
  }
  memcpy(*buffer+*length,text,count);
  *length+=count;
  (*buffer)[*length]='\0';
  return 1;
}

/* Rewrite one source. `desktop` selects the 330 core dialect; otherwise 300 es. `vertex` selects
 * the storage qualifier spellings. Returns a malloc'd string, or NULL on allocation failure. */
static char *glsl_rewrite(const char *source,int vertex,int desktop,int little_endian){
  static const struct { const char *from; const char *to_vertex; const char *to_fragment; } words[]={
    {"attribute","in","in"},
    {"varying","out","in"},
    {"texture2DLod","textureLod","textureLod"},
    {"texture2DProj","textureProj","textureProj"},
    {"texture2D","anygm_sample","anygm_sample"},
    {"textureCube","texture","texture"},
    {"gl_FragColor","gl_FragColor","anygm_fragment"},
  };
  char *out=NULL;
  size_t length=0,capacity=0,i=0,n=strlen(source);
  const char *header=desktop
    ?"#version 330 core\n"
    :"#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n";
  const char *sample=little_endian
    ?"vec4 anygm_sample(sampler2D s,vec2 c){ return texture(s,c).bgra; }\n"
    :"vec4 anygm_sample(sampler2D s,vec2 c){ return texture(s,c).gbar; }\n";
  if(!glsl_append(&out,&length,&capacity,header,strlen(header))) return NULL;
  if(!vertex && !glsl_append(&out,&length,&capacity,"out vec4 anygm_fragment;\n",25)) { free(out); return NULL; }
  if(!glsl_append(&out,&length,&capacity,sample,strlen(sample))){ free(out); return NULL; }
  while(i<n){
    /* Lines the older dialect needs and the newer one rejects or already has. */
    if((i==0 || source[i-1]=='\n') && (!strncmp(source+i,"#version",8) ||
                                       (desktop && !strncmp(source+i,"precision",9)))){
      while(i<n && source[i]!='\n') i++;
      continue;
    }
    if(!strncmp(source+i,"gl_FragData[0]",14) && glsl_word_boundary(source,i,14) && !vertex){
      if(!glsl_append(&out,&length,&capacity,"anygm_fragment",14)){ free(out); return NULL; }
      i+=14;
      continue;
    }
    if(isalpha((unsigned char)source[i]) || source[i]=='_'){
      size_t start=i,matched=0;
      while(i<n && (isalnum((unsigned char)source[i]) || source[i]=='_')) i++;
      for(size_t w=0;w<sizeof words/sizeof words[0];w++){
        size_t wl=strlen(words[w].from);
        if(i-start==wl && !strncmp(source+start,words[w].from,wl)){
          const char *to=vertex?words[w].to_vertex:words[w].to_fragment;
          if(!glsl_append(&out,&length,&capacity,to,strlen(to))){ free(out); return NULL; }
          matched=1;
          break;
        }
      }
      if(!matched && !glsl_append(&out,&length,&capacity,source+start,i-start)){ free(out); return NULL; }
      continue;
    }
    if(!glsl_append(&out,&length,&capacity,source+i,1)){ free(out); return NULL; }
    i++;
  }
  return out;
}

static GlContentProgram *content_slot(GmlGpuBackend *backend,const GmlPlanShader *shader){
  GlContentProgram *slot=NULL;
  uint32_t oldest=0xFFFFFFFFu;
  for(size_t index=0;index<GL_CONTENT_PROGRAMS;index++){
    GlContentProgram *candidate=&backend->content[index];
    if((candidate->program || candidate->failed) && candidate->identity==shader->identity &&
       candidate->fragment_key==shader->fragment_es) return candidate;
  }
  for(size_t index=0;index<GL_CONTENT_PROGRAMS;index++){
    GlContentProgram *candidate=&backend->content[index];
    if(!candidate->program && !candidate->failed){ slot=candidate; break; }
    if(candidate->last_used<oldest){ oldest=candidate->last_used; slot=candidate; }
  }
  if(!slot) return NULL;
  if(slot->program) backend->gl.DeleteProgram(slot->program);
  memset(slot,0,sizeof *slot);
  slot->identity=shader->identity;
  slot->fragment_key=shader->fragment_es;
  return slot;
}

static GLuint link_content(GmlGpuBackend *backend,const char *vertex,const char *fragment,
                           char *error,size_t error_capacity){
  GLuint vs,fs,program;
  GLint status=GL_FALSE;
  vs=compile_stage(backend,GL_VERTEX_SHADER,vertex,error,error_capacity);
  if(!vs) return 0;
  fs=compile_stage(backend,GL_FRAGMENT_SHADER,fragment,error,error_capacity);
  if(!fs){ backend->gl.DeleteShader(vs); return 0; }
  program=backend->gl.CreateProgram();
  if(!program){
    backend->gl.DeleteShader(vs);
    backend->gl.DeleteShader(fs);
    record_error(error,error_capacity,"graphics program object could not be created");
    return 0;
  }
  backend->gl.AttachShader(program,vs);
  backend->gl.AttachShader(program,fs);
  backend->gl.LinkProgram(program);
  backend->gl.GetProgramiv(program,GL_LINK_STATUS,&status);
  backend->gl.DeleteShader(vs);
  backend->gl.DeleteShader(fs);
  if(status!=GL_TRUE){
    char log[GL_INFO_LOG_LIMIT];
    GLsizei written=0;
    log[0]='\0';
    backend->gl.GetProgramInfoLog(program,(GLsizei)sizeof log,&written,log);
    log[sizeof log-1]='\0';
    record_error(error,error_capacity,"content program failed to link: %s",log);
    backend->gl.DeleteProgram(program);
    return 0;
  }
  return program;
}

/* Compile the content's program for this context, or return the cached one. Returns NULL with the
 * slot marked failed when the context refuses it. */
static GlContentProgram *content_program(GmlGpuBackend *backend,const GmlPlanShader *shader,
                                         char *error,size_t error_capacity){
  GlContentProgram *slot=content_slot(backend,shader);
  int desktop=backend->api!=GML_GPU_API_OPENGLES3;
  char *vertex=NULL,*fragment=NULL;
  if(!slot) return NULL;
  slot->last_used=++backend->use_clock;
  if(slot->program) return slot;
  if(slot->failed) return NULL;
  vertex=glsl_rewrite(shader->vertex_es,1,desktop,backend->little_endian);
  fragment=glsl_rewrite(shader->fragment_es,0,desktop,backend->little_endian);
  if(vertex && fragment) slot->program=link_content(backend,vertex,fragment,error,error_capacity);
  free(vertex);
  free(fragment);
  if(!slot->program){
    /* The refusal stands: this context will refuse the same text next frame too. */
    slot->failed=1;
    return NULL;
  }
  slot->a_position=backend->gl.GetAttribLocation(slot->program,"in_Position");
  slot->a_colour=backend->gl.GetAttribLocation(slot->program,"in_Colour");
  slot->a_texcoord=backend->gl.GetAttribLocation(slot->program,"in_TextureCoord");
  slot->u_matrices=backend->gl.GetUniformLocation(slot->program,"gm_Matrices");
  if(slot->u_matrices<0) slot->u_matrices=backend->gl.GetUniformLocation(slot->program,"gm_Matrices[0]");
  slot->u_base_texture=backend->gl.GetUniformLocation(slot->program,"gm_BaseTexture");
  slot->u_alpha_test=backend->gl.GetUniformLocation(slot->program,"gm_AlphaTestEnabled");
  slot->u_fog_ps=backend->gl.GetUniformLocation(slot->program,"gm_PS_FogEnabled");
  slot->u_fog_vs=backend->gl.GetUniformLocation(slot->program,"gm_VS_FogEnabled");
  {
    GLint active=0;
    backend->gl.GetProgramiv(slot->program,GL_ACTIVE_UNIFORMS,&active);
    slot->uniform_count=0;
    for(GLint index=0;index<active && slot->uniform_count<GL_CONTENT_UNIFORMS;index++){
      GlContentUniform *entry=&slot->uniforms[slot->uniform_count];
      GLsizei length=0;
      size_t name_length;
      entry->size=0;
      entry->type=0;
      entry->name[0]='\0';
      backend->gl.GetActiveUniform(slot->program,(GLuint)index,(GLsizei)sizeof entry->name,&length,
                                   &entry->size,&entry->type,entry->name);
      entry->name[sizeof entry->name-1]='\0';
      /* An array is reported by its first element; the content names the array. */
      name_length=strlen(entry->name);
      if(name_length>3 && !strcmp(entry->name+name_length-3,"[0]")) entry->name[name_length-3]='\0';
      if(!entry->name[0]) continue;
      entry->location=backend->gl.GetUniformLocation(slot->program,entry->name);
      if(entry->location<0) continue;
      slot->uniform_count++;
    }
  }
  if(slot->a_position<0){
    record_error(error,error_capacity,"content program declares no in_Position attribute");
    backend->gl.DeleteProgram(slot->program);
    slot->program=0;
    slot->failed=1;
    return NULL;
  }
  return slot;
}

static const GlContentUniform *content_uniform(const GlContentProgram *program,const char *name){
  for(uint32_t index=0;index<program->uniform_count;index++)
    if(!strcmp(program->uniforms[index].name,name)) return &program->uniforms[index];
  return NULL;
}

/* Deliver one value through the setter its declared type calls for. The content's values arrive
 * as up to sixteen numbers; they are regrouped by the type's width, padded with zeros when fewer
 * were given, and bounded by the array size the program declares. */
static void set_content_uniform(GmlGpuBackend *backend,const GlContentUniform *entry,
                                const float *values,uint32_t count){
  float floats[16];
  GLint ints[16];
  uint32_t width,groups,matrix=0,integer=0;
  switch(entry->type){
    case GL_FLOAT: width=1; break;
    case GL_FLOAT_VEC2: width=2; break;
    case GL_FLOAT_VEC3: width=3; break;
    case GL_FLOAT_VEC4: width=4; break;
    case GL_INT: case GL_UNSIGNED_INT: case GL_BOOL: case GL_SAMPLER_2D: width=1; integer=1; break;
    case GL_INT_VEC2: case GL_UNSIGNED_INT_VEC2: case GL_BOOL_VEC2: width=2; integer=1; break;
    case GL_INT_VEC3: case GL_UNSIGNED_INT_VEC3: case GL_BOOL_VEC3: width=3; integer=1; break;
    case GL_INT_VEC4: case GL_UNSIGNED_INT_VEC4: case GL_BOOL_VEC4: width=4; integer=1; break;
    case GL_FLOAT_MAT2: width=4; matrix=2; break;
    case GL_FLOAT_MAT3: width=9; matrix=3; break;
    case GL_FLOAT_MAT4: width=16; matrix=4; break;
    default: return;
  }
  if(count==0) return;
  groups=(count+width-1)/width;
  if(groups<1) groups=1;
  if(entry->size>0 && groups>(uint32_t)entry->size) groups=(uint32_t)entry->size;
  if(groups*width>16) groups=16/width;
  if(groups<1) return;
  memset(floats,0,sizeof floats);
  memcpy(floats,values,(count<16?count:16)*sizeof floats[0]);
  if(integer){
    for(uint32_t index=0;index<16;index++) ints[index]=(GLint)floats[index];
    switch(width){
      case 1: backend->gl.Uniform1iv(entry->location,(GLsizei)groups,ints); break;
      case 2: backend->gl.Uniform2iv(entry->location,(GLsizei)groups,ints); break;
      case 3: backend->gl.Uniform3iv(entry->location,(GLsizei)groups,ints); break;
      default: backend->gl.Uniform4iv(entry->location,(GLsizei)groups,ints); break;
    }
    return;
  }
  if(matrix==2){ backend->gl.UniformMatrix2fv(entry->location,(GLsizei)groups,GL_FALSE,floats); return; }
  if(matrix==3){ backend->gl.UniformMatrix3fv(entry->location,(GLsizei)groups,GL_FALSE,floats); return; }
  if(matrix==4){ backend->gl.UniformMatrix4fv(entry->location,(GLsizei)groups,GL_FALSE,floats); return; }
  switch(width){
    case 1: backend->gl.Uniform1fv(entry->location,(GLsizei)groups,floats); break;
    case 2: backend->gl.Uniform2fv(entry->location,(GLsizei)groups,floats); break;
    case 3: backend->gl.Uniform3fv(entry->location,(GLsizei)groups,floats); break;
    default: backend->gl.Uniform4fv(entry->location,(GLsizei)groups,floats); break;
  }
}

static void set_sampler_filter(GmlGpuBackend *backend,GLuint texture,int linear){
  backend->gl.BindTexture(GL_TEXTURE_2D,texture);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,linear?GL_LINEAR:GL_NEAREST);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear?GL_LINEAR:GL_NEAREST);
}

/* Draw the source through the content's program over the destination rectangle. The quad is the
 * rectangle in its own pixel units; the world-view-projection matrix the vertex program multiplies
 * by maps those units to clip space with the row order the runtime addresses, and the viewport is
 * the rectangle itself. */
static int execute_shader_draw(GmlGpuBackend *backend,GmlRenderPlan *plan,const GmlPlanOp *op,
                               GmlGpuCounters *counters,char *error,size_t error_capacity){
  const GmlPlanShader *shader=&plan->shader;
  const GmlPlanImage *image=&plan->images[op->source];
  GlContentProgram *program;
  GLuint source_name=0;
  GLuint sampler_names[GML_PLAN_MAX_SAMPLERS]={0,0,0,0};
  float matrices[5*16];
  float vertices[4*9];
  const float w=(float)op->destination.width,h=(float)op->destination.height;
  static const GLenum units[GML_PLAN_MAX_SAMPLERS]={GL_TEXTURE1,GL_TEXTURE2,GL_TEXTURE3,GL_TEXTURE4};
  program=content_program(backend,shader,error,error_capacity);
  if(!program){
    plan->fallback_reason=GML_PLAN_FALLBACK_SHADER_FAILURE;
    return 0;
  }
  if(!upload_source(backend,image,counters,&source_name)){
    record_error(error,error_capacity,"graphics source upload failed");
    return 0;
  }
  for(uint32_t index=0;index<shader->sampler_count && index<GML_PLAN_MAX_SAMPLERS;index++){
    uint32_t bound=shader->samplers[index].image;
    if(bound==GML_PLAN_NO_IMAGE || bound>=plan->image_count) continue;
    if(!upload_source(backend,&plan->images[bound],counters,&sampler_names[index])){
      record_error(error,error_capacity,"graphics sampler upload failed");
      return 0;
    }
  }
  set_rect(backend,&op->destination,plan->target_height);
  backend->gl.UseProgram(program->program);
  /* Five matrices in the runtime's order: view, projection, world, world-view,
   * world-view-projection. The last maps the quad to clip space; the others are the identity, and
   * the projection is the same map so a program that multiplies by it alone still lands. */
  memset(matrices,0,sizeof matrices);
  for(int m=0;m<5;m++){ matrices[m*16+0]=1.0f; matrices[m*16+5]=1.0f; matrices[m*16+10]=1.0f; matrices[m*16+15]=1.0f; }
  for(int m=1;m<5;m+=3){
    float *target=matrices+m*16;
    target[0]=2.0f/w; target[5]=-2.0f/h; target[10]=1.0f; target[12]=-1.0f; target[13]=1.0f; target[15]=1.0f;
  }
  if(program->u_matrices>=0) backend->gl.UniformMatrix4fv(program->u_matrices,5,GL_FALSE,matrices);
  if(program->u_base_texture>=0){ GLint zero=0; backend->gl.Uniform1iv(program->u_base_texture,1,&zero); }
  if(program->u_alpha_test>=0){ GLint off=0; backend->gl.Uniform1iv(program->u_alpha_test,1,&off); }
  if(program->u_fog_ps>=0){ GLint off=0; backend->gl.Uniform1iv(program->u_fog_ps,1,&off); }
  if(program->u_fog_vs>=0){ GLint off=0; backend->gl.Uniform1iv(program->u_fog_vs,1,&off); }
  for(uint32_t index=0;index<shader->uniform_count && index<GML_PLAN_MAX_UNIFORMS;index++){
    const GmlPlanUniform *value=&shader->uniforms[index];
    const GlContentUniform *entry=content_uniform(program,value->name);
    if(entry) set_content_uniform(backend,entry,value->value,value->count);
  }
  for(uint32_t index=0;index<shader->sampler_count && index<GML_PLAN_MAX_SAMPLERS;index++){
    const GlContentUniform *entry=content_uniform(program,shader->samplers[index].name);
    GLint unit=(GLint)index+1;
    if(!entry || entry->type!=GL_SAMPLER_2D || !sampler_names[index]) continue;
    backend->gl.Uniform1iv(entry->location,1,&unit);
    backend->gl.ActiveTexture(units[index]);
    set_sampler_filter(backend,sampler_names[index],op->linear?1:0);
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  set_sampler_filter(backend,source_name,op->linear?1:0);
  /* Two triangles as a strip: top-left, top-right, bottom-left, bottom-right, each with a white
   * vertex colour and the texture coordinate of its corner. */
  {
    const float corners[4][2]={{0.0f,0.0f},{w,0.0f},{0.0f,h},{w,h}};
    for(int v=0;v<4;v++){
      float *row=vertices+v*9;
      row[0]=corners[v][0]; row[1]=corners[v][1]; row[2]=0.0f;
      row[3]=1.0f; row[4]=1.0f; row[5]=1.0f; row[6]=1.0f;
      row[7]=corners[v][0]/w; row[8]=corners[v][1]/h;
    }
  }
  if(!backend->content_buffer){
    backend->gl.GenBuffers(1,&backend->content_buffer);
    if(!backend->content_buffer){
      record_error(error,error_capacity,"graphics vertex buffer could not be created");
      return 0;
    }
  }
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,backend->content_buffer);
  backend->gl.BufferData(GL_ARRAY_BUFFER,(GLsizeiptr)sizeof vertices,vertices,GL_STREAM_DRAW);
  backend->gl.EnableVertexAttribArray((GLuint)program->a_position);
  backend->gl.VertexAttribPointer((GLuint)program->a_position,3,GL_FLOAT,GL_FALSE,9*(GLsizei)sizeof(float),(const void*)0);
  if(program->a_colour>=0){
    backend->gl.EnableVertexAttribArray((GLuint)program->a_colour);
    backend->gl.VertexAttribPointer((GLuint)program->a_colour,4,GL_FLOAT,GL_FALSE,9*(GLsizei)sizeof(float),(const void*)(3*sizeof(float)));
  }
  if(program->a_texcoord>=0){
    backend->gl.EnableVertexAttribArray((GLuint)program->a_texcoord);
    backend->gl.VertexAttribPointer((GLuint)program->a_texcoord,2,GL_FLOAT,GL_FALSE,9*(GLsizei)sizeof(float),(const void*)(7*sizeof(float)));
  }
  backend->gl.DrawArrays(GL_TRIANGLE_STRIP,0,4);
  if(counters) counters->draw_calls++;
  backend->gl.DisableVertexAttribArray((GLuint)program->a_position);
  if(program->a_colour>=0) backend->gl.DisableVertexAttribArray((GLuint)program->a_colour);
  if(program->a_texcoord>=0) backend->gl.DisableVertexAttribArray((GLuint)program->a_texcoord);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,0);
  for(uint32_t index=0;index<GML_PLAN_MAX_SAMPLERS;index++){
    backend->gl.ActiveTexture(units[index]);
    backend->gl.BindTexture(GL_TEXTURE_2D,0);
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  /* The presentation program expects its source sampled by the nearest texel. */
  set_sampler_filter(backend,source_name,0);
  backend->gl.UseProgram(backend->program);
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
                       GmlRenderPlan *plan,GmlGpuCounters *counters,
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
    if(op->opcode==GML_PLAN_OP_SHADER_DRAW){
      if(!execute_shader_draw(backend,plan,op,counters,error,error_capacity)){ failed=1; break; }
      continue;
    }
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
