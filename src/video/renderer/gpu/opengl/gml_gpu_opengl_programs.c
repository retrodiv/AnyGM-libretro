/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing OpenGL program and content-shader pipeline. */
#include "gml_gpu_opengl_internal.h"
#include "gml_gpu_opengl_shaders.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GLuint compile_stage(GmlGpuBackend *backend,GLenum stage,const char *source,
                            char *error,size_t error_capacity){
  GLuint shader=backend->gl.CreateShader(stage);
  GLint status=GL_FALSE;
  if(!shader){
    gml_gpu_opengl_record_error(error,error_capacity,"graphics shader object could not be created");
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
    gml_gpu_opengl_record_error(error,error_capacity,"graphics %s shader failed: %s",
                 stage==GL_VERTEX_SHADER?"vertex":"fragment",log);
    backend->gl.DeleteShader(shader);
    return 0;
  }
  return shader;
}

int gml_gpu_opengl_build_program(GmlGpuBackend *backend,char *error,size_t error_capacity){
  char vertex_source[512];
  char fragment_source[1536];
  const char *prefix=backend->api==GML_GPU_API_OPENGLES3
    ?GML_GPU_OPENGL_PREFIX_ES:GML_GPU_OPENGL_PREFIX_DESKTOP;
  GLuint vertex,fragment;
  GLint status=GL_FALSE;
  snprintf(vertex_source,sizeof vertex_source,"%s%s",prefix,GML_GPU_OPENGL_VERTEX_BODY);
  snprintf(fragment_source,sizeof fragment_source,"%s%s%s%s",prefix,
           GML_GPU_OPENGL_FRAGMENT_BODY_HEAD,
           backend->little_endian?GML_GPU_OPENGL_FRAGMENT_SWIZZLE_LITTLE
                                 :GML_GPU_OPENGL_FRAGMENT_SWIZZLE_BIG,
           GML_GPU_OPENGL_FRAGMENT_BODY_TAIL);
  vertex=compile_stage(backend,GL_VERTEX_SHADER,vertex_source,error,error_capacity);
  if(!vertex) return 0;
  fragment=compile_stage(backend,GL_FRAGMENT_SHADER,fragment_source,error,error_capacity);
  if(!fragment){ backend->gl.DeleteShader(vertex); return 0; }
  backend->program=backend->gl.CreateProgram();
  if(!backend->program){
    gml_gpu_opengl_record_error(error,error_capacity,"graphics program object could not be created");
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
    gml_gpu_opengl_record_error(error,error_capacity,"graphics program failed to link: %s",log);
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
  backend->u_source_height=backend->gl.GetUniformLocation(backend->program,"u_source_height");
  backend->u_resident_source=backend->gl.GetUniformLocation(backend->program,"u_resident_source");
  if(backend->u_source<0 || backend->u_map_x<0 || backend->u_map_y<0 ||
     backend->u_dest_x<0 || backend->u_dest_y<0 || backend->u_target_height<0 ||
     backend->u_source_height<0 || backend->u_resident_source<0){
    gml_gpu_opengl_record_error(error,error_capacity,"graphics program is missing a required uniform");
    backend->gl.DeleteProgram(backend->program);
    backend->program=0;
    return 0;
  }
  return 1;
}

int gml_gpu_opengl_scene_program(GmlGpuBackend *backend,char *error,size_t error_capacity){
  GlScene *scene=&backend->scene;
  const char *prefix=backend->api==GML_GPU_API_OPENGLES3
    ?GML_GPU_OPENGL_PREFIX_ES:GML_GPU_OPENGL_PREFIX_DESKTOP;
  char vertex_source[1024],fragment_source[4096];
  GLuint vertex,fragment;
  GLint status=GL_FALSE;
  if(scene->program) return 1;
  snprintf(vertex_source,sizeof vertex_source,"%s%s",prefix,GML_GPU_OPENGL_SCENE_VERTEX_BODY);
  snprintf(fragment_source,sizeof fragment_source,"%s%s",prefix,GML_GPU_OPENGL_SCENE_FRAGMENT_BODY);
  vertex=compile_stage(backend,GL_VERTEX_SHADER,vertex_source,error,error_capacity);
  if(!vertex) return 0;
  fragment=compile_stage(backend,GL_FRAGMENT_SHADER,fragment_source,error,error_capacity);
  if(!fragment){ backend->gl.DeleteShader(vertex); return 0; }
  scene->program=backend->gl.CreateProgram();
  if(!scene->program){
    backend->gl.DeleteShader(vertex);
    backend->gl.DeleteShader(fragment);
    gml_gpu_opengl_record_error(error,error_capacity,"scene program object could not be created");
    return 0;
  }
  backend->gl.AttachShader(scene->program,vertex);
  backend->gl.AttachShader(scene->program,fragment);
  backend->gl.LinkProgram(scene->program);
  backend->gl.GetProgramiv(scene->program,GL_LINK_STATUS,&status);
  backend->gl.DeleteShader(vertex);
  backend->gl.DeleteShader(fragment);
  if(status!=GL_TRUE){
    char log[GL_INFO_LOG_LIMIT];
    GLsizei written=0;
    log[0]='\0';
    backend->gl.GetProgramInfoLog(scene->program,(GLsizei)sizeof log,&written,log);
    log[sizeof log-1]='\0';
    gml_gpu_opengl_record_error(error,error_capacity,"scene program failed to link: %s",log);
    backend->gl.DeleteProgram(scene->program);
    scene->program=0;
    return 0;
  }
  scene->a_position=backend->gl.GetAttribLocation(scene->program,"in_Position");
  scene->a_texcoord=backend->gl.GetAttribLocation(scene->program,"in_TextureCoord");
  scene->a_colour=backend->gl.GetAttribLocation(scene->program,"in_Colour");
  scene->a_clamp=backend->gl.GetAttribLocation(scene->program,"in_Clamp");
  scene->a_map=backend->gl.GetAttribLocation(scene->program,"in_Map");
  scene->a_step=backend->gl.GetAttribLocation(scene->program,"in_Step");
  scene->a_margin=backend->gl.GetAttribLocation(scene->program,"in_Margin");
  scene->a_span_x=backend->gl.GetAttribLocation(scene->program,"in_SpanX");
  scene->a_span_y=backend->gl.GetAttribLocation(scene->program,"in_SpanY");
  scene->a_span_active=backend->gl.GetAttribLocation(scene->program,"in_SpanActive");
  scene->u_extent=backend->gl.GetUniformLocation(scene->program,"u_extent");
  scene->u_image=backend->gl.GetUniformLocation(scene->program,"u_image");
  scene->u_filter=backend->gl.GetUniformLocation(scene->program,"u_filter");
  if(scene->a_position<0 || scene->a_texcoord<0 || scene->a_colour<0 ||
     scene->a_clamp<0 || scene->a_map<0 || scene->a_step<0 || scene->a_margin<0 ||
     scene->a_span_x<0 || scene->a_span_y<0 || scene->a_span_active<0 ||
     scene->u_extent<0 || scene->u_image<0 || scene->u_filter<0){
    gml_gpu_opengl_record_error(error,error_capacity,"scene program is missing an attribute or uniform");
    backend->gl.DeleteProgram(scene->program);
    scene->program=0;
    return 0;
  }
  return 1;
}

int gml_gpu_opengl_scene_exact_program(GmlGpuBackend *backend,char *error,
                                       size_t error_capacity){
  GlScene *scene=&backend->scene;
  const char *prefix=backend->api==GML_GPU_API_OPENGLES3
    ?GML_GPU_OPENGL_PREFIX_ES:GML_GPU_OPENGL_PREFIX_DESKTOP;
  char vertex_source[1024],fragment_source[2048];
  GLuint vertex,fragment;
  GLint status=GL_FALSE;
  if(scene->exact_program) return 1;
  snprintf(vertex_source,sizeof vertex_source,"%s%s",prefix,GML_GPU_OPENGL_SCENE_VERTEX_BODY);
  snprintf(fragment_source,sizeof fragment_source,"%s%s",prefix,
           GML_GPU_OPENGL_SCENE_EXACT_FRAGMENT_BODY);
  vertex=compile_stage(backend,GL_VERTEX_SHADER,vertex_source,error,error_capacity);
  if(!vertex) return 0;
  fragment=compile_stage(backend,GL_FRAGMENT_SHADER,fragment_source,error,error_capacity);
  if(!fragment){ backend->gl.DeleteShader(vertex); return 0; }
  scene->exact_program=backend->gl.CreateProgram();
  if(!scene->exact_program){
    backend->gl.DeleteShader(vertex);
    backend->gl.DeleteShader(fragment);
    gml_gpu_opengl_record_error(error,error_capacity,"exact scene program object could not be created");
    return 0;
  }
  backend->gl.AttachShader(scene->exact_program,vertex);
  backend->gl.AttachShader(scene->exact_program,fragment);
  backend->gl.LinkProgram(scene->exact_program);
  backend->gl.GetProgramiv(scene->exact_program,GL_LINK_STATUS,&status);
  backend->gl.DeleteShader(vertex);
  backend->gl.DeleteShader(fragment);
  if(status!=GL_TRUE){
    char log[GL_INFO_LOG_LIMIT];
    GLsizei written=0;
    log[0]='\0';
    backend->gl.GetProgramInfoLog(scene->exact_program,(GLsizei)sizeof log,&written,log);
    log[sizeof log-1]='\0';
    gml_gpu_opengl_record_error(error,error_capacity,"exact scene program failed to link: %s",log);
    backend->gl.DeleteProgram(scene->exact_program);
    scene->exact_program=0;
    return 0;
  }
  scene->exact_a_position=backend->gl.GetAttribLocation(scene->exact_program,"in_Position");
  scene->exact_a_texcoord=backend->gl.GetAttribLocation(scene->exact_program,"in_TextureCoord");
  scene->exact_a_colour=backend->gl.GetAttribLocation(scene->exact_program,"in_Colour");
  scene->exact_u_extent=backend->gl.GetUniformLocation(scene->exact_program,"u_extent");
  scene->exact_u_image=backend->gl.GetUniformLocation(scene->exact_program,"u_image");
  scene->exact_u_previous=backend->gl.GetUniformLocation(scene->exact_program,"u_previous");
  scene->exact_u_region_origin=backend->gl.GetUniformLocation(scene->exact_program,
                                                               "u_region_origin");
  scene->exact_u_repetitions=backend->gl.GetUniformLocation(scene->exact_program,
                                                             "u_repetitions");
  if(scene->exact_a_position<0 || scene->exact_a_texcoord<0 || scene->exact_a_colour<0 ||
     scene->exact_u_extent<0 || scene->exact_u_image<0 || scene->exact_u_previous<0 ||
     scene->exact_u_region_origin<0 || scene->exact_u_repetitions<0){
    gml_gpu_opengl_record_error(error,error_capacity,
      "exact scene program is missing an attribute or uniform");
    backend->gl.DeleteProgram(scene->exact_program);
    scene->exact_program=0;
    return 0;
  }
  return 1;
}

/* ---- The content's own programs ----
 *
 * A Studio payload ships its shaders as GLSL ES 1.00 text, with a desktop GLSL 1.20 rendering
 * beside it. Neither is what a core-profile context accepts, so the text is rewritten, token by
 * token and nothing more: the storage qualifiers and the sampling calls of the older dialect are
 * spelled the newer way, the fragment output is declared, and one helper is added that samples a
 * texture. The existing hybrid path reorders channels for its raw-word uploads; resident scene
 * textures are already RGBA and use the sampled channels directly. Content expressions stay
 * untouched. */

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
static char *glsl_rewrite(const char *source,int vertex,int desktop,int little_endian,
                          int scene_rgba){
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
  const char *sample=scene_rgba
    ?"vec4 anygm_sample(sampler2D s,vec2 c){ return texture(s,c); }\n"
    :little_endian
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

static GlContentProgram *content_slot(GmlGpuBackend *backend,const GmlPlanShader *shader,
                                      int scene_rgba){
  GlContentProgram *slot=NULL;
  uint32_t oldest=0xFFFFFFFFu;
  for(size_t index=0;index<GL_CONTENT_PROGRAMS;index++){
    GlContentProgram *candidate=&backend->content[index];
    if((candidate->program || candidate->failed) && candidate->identity==shader->identity &&
       candidate->fragment_key==shader->fragment_es &&
       candidate->scene_rgba==scene_rgba) return candidate;
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
  slot->scene_rgba=scene_rgba;
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
    gml_gpu_opengl_record_error(error,error_capacity,"graphics program object could not be created");
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
    gml_gpu_opengl_record_error(error,error_capacity,"content program failed to link: %s",log);
    backend->gl.DeleteProgram(program);
    return 0;
  }
  return program;
}

/* Compile the content's program for this context, or return the cached one. Returns NULL with the
 * slot marked failed when the context refuses it. */
static GlContentProgram *content_program(GmlGpuBackend *backend,const GmlPlanShader *shader,
                                         int scene_rgba,
                                         char *error,size_t error_capacity){
  GlContentProgram *slot=content_slot(backend,shader,scene_rgba);
  int desktop=backend->api!=GML_GPU_API_OPENGLES3;
  char *vertex=NULL,*fragment=NULL;
  if(!slot) return NULL;
  slot->last_used=++backend->use_clock;
  if(slot->program) return slot;
  if(slot->failed) return NULL;
  vertex=glsl_rewrite(shader->vertex_es,1,desktop,backend->little_endian,scene_rgba);
  fragment=glsl_rewrite(shader->fragment_es,0,desktop,backend->little_endian,scene_rgba);
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
    gml_gpu_opengl_record_error(error,error_capacity,"content program declares no in_Position attribute");
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
static int execute_shader_draw_with_source(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                            const GmlPlanOp *op,GmlGpuCounters *counters,
                                            GLuint resident_source,int rendered_source_origin,
                                            char *error,size_t error_capacity){
  const GmlPlanShader *shader=&plan->shader;
  const GmlPlanImage *image=&plan->images[op->source];
  GlContentProgram *program;
  GLuint source_name=resident_source;
  GLuint sampler_names[GML_PLAN_MAX_SAMPLERS]={0,0,0,0};
  float matrices[5*16];
  float vertices[4*9];
  const float w=(float)op->destination.width,h=(float)op->destination.height;
  static const GLenum units[GML_PLAN_MAX_SAMPLERS]={GL_TEXTURE1,GL_TEXTURE2,GL_TEXTURE3,GL_TEXTURE4};
  program=content_program(backend,shader,resident_source!=0,error,error_capacity);
  if(!program){
    plan->fallback_reason=GML_PLAN_FALLBACK_SHADER_FAILURE;
    return 0;
  }
  if(!resident_source &&
     !gml_gpu_opengl_upload_source(backend,image,counters,&source_name,op->linear?1:0)){
    gml_gpu_opengl_record_error(error,error_capacity,"graphics source upload failed");
    return 0;
  }
  for(uint32_t index=0;index<shader->sampler_count && index<GML_PLAN_MAX_SAMPLERS;index++){
    uint32_t bound=shader->samplers[index].image;
    if(bound==GML_PLAN_NO_IMAGE || bound>=plan->image_count) continue;
    if(!gml_gpu_opengl_upload_source(backend,&plan->images[bound],counters,&sampler_names[index],
                      shader->samplers[index].linear?1:0)){
      gml_gpu_opengl_record_error(error,error_capacity,"graphics sampler upload failed");
      return 0;
    }
  }
  gml_gpu_opengl_set_rect(backend,&op->destination,plan->target_height);
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
    set_sampler_filter(backend,sampler_names[index],shader->samplers[index].linear?1:0);
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  set_sampler_filter(backend,source_name,op->linear?1:0);
  /* Two triangles as a strip: top-left, top-right, bottom-left, bottom-right, each with a white
   * vertex colour and the texture coordinate of its corner. */
  {
    /* The texture coordinates span the part of the source the operation names, so a draw showing
     * a region of a surface shades that region and nothing else. */
    const float corners[4][2]={{0.0f,0.0f},{w,0.0f},{0.0f,h},{w,h}};
    float u0=0.0f,v0=0.0f,u1=1.0f,v1=1.0f;
    if(op->source_rect.width && op->source_rect.height && image->width && image->height){
      u0=(float)op->source_rect.x/(float)image->width;
      v0=(float)op->source_rect.y/(float)image->height;
      u1=(float)((uint32_t)op->source_rect.x+op->source_rect.width)/(float)image->width;
      v1=(float)((uint32_t)op->source_rect.y+op->source_rect.height)/(float)image->height;
    }
    for(int v=0;v<4;v++){
      float *row=vertices+v*9;
      row[0]=corners[v][0]; row[1]=corners[v][1]; row[2]=0.0f;
      row[3]=shader->vertex_colour[0]; row[4]=shader->vertex_colour[1];
      row[5]=shader->vertex_colour[2]; row[6]=shader->vertex_colour[3];
      row[7]=u0+(u1-u0)*(corners[v][0]/w);
      row[8]=v0+(v1-v0)*(corners[v][1]/h);
      if(rendered_source_origin) row[8]=1.0f-row[8];
    }
  }
  if(!backend->content_buffer){
    backend->gl.GenBuffers(1,&backend->content_buffer);
    if(!backend->content_buffer){
      gml_gpu_opengl_record_error(error,error_capacity,"graphics vertex buffer could not be created");
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

int gml_gpu_opengl_execute_shader_draw(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                        const GmlPlanOp *op,GmlGpuCounters *counters,
                                        char *error,size_t error_capacity){
  return execute_shader_draw_with_source(backend,plan,op,counters,0,0,
                                         error,error_capacity);
}

int gml_gpu_opengl_scene_shader_draw(GmlGpuBackend *backend,const GmlPlanShader *shader,
                                      const GmlRenderDrawStateSnapshot *state,
                                      GlSceneImage *target,GlSceneImage *source,
                                      char *error,size_t error_capacity){
  GmlRenderPlan plan={0};
  GmlPlanOp operation={0};
  int drawn;
  GLenum gl_error;
  if(!backend || !shader || !state || !target || !source || !source->texture ||
     !target->framebuffer || !target->width || !target->height) return 0;
  plan.target_width=target->width;
  plan.target_height=target->height;
  plan.image_count=1u;
  plan.images[0].width=source->width;
  plan.images[0].height=source->height;
  plan.shader=*shader;
  operation.opcode=GML_PLAN_OP_SHADER_DRAW;
  operation.source=0u;
  operation.linear=state->samplers[0].filter==GML_RENDER_FILTER_BILINEAR;
  operation.destination=(GmlPlanRect){0,0,target->width,target->height};
  operation.source_rect=(GmlPlanRect){0,0,source->width,source->height};
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  backend->gl.Disable(GL_BLEND);
  backend->gl.BindVertexArray(backend->scene.vertex_array);
  drawn=execute_shader_draw_with_source(backend,&plan,&operation,NULL,source->texture,
                                        source->rendered_origin,error,error_capacity);
  backend->gl.BindVertexArray(0);
  gl_error=backend->gl.GetError();
  if(gl_error!=GL_NO_ERROR)
    gml_gpu_opengl_record_error(error,error_capacity,"scene shader GL error %u",gl_error);
  return drawn && gl_error==GL_NO_ERROR;
}
