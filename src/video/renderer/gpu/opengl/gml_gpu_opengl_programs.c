/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing OpenGL program and content-shader pipeline. */
#include "gml_gpu_opengl_internal.h"
#include "gml_gpu_opengl_shaders.h"

#include <ctype.h>
#include <math.h>
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
  char fragment_source[2048];
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
  backend->u_rgb_query=backend->gl.GetUniformLocation(backend->program,"u_rgb_query");
  backend->u_box_destination=backend->gl.GetUniformLocation(backend->program,"u_box_destination");
  if(backend->u_source<0 || backend->u_map_x<0 || backend->u_map_y<0 ||
     backend->u_dest_x<0 || backend->u_dest_y<0 || backend->u_target_height<0 ||
     backend->u_source_height<0 || backend->u_resident_source<0 || backend->u_rgb_query<0 ||
     backend->u_box_destination<0){
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
  char vertex_source[sizeof(GML_GPU_OPENGL_PREFIX_ES)+
                     sizeof(GML_GPU_OPENGL_PREFIX_DESKTOP)+sizeof(GML_GPU_OPENGL_SCENE_VERTEX_BODY)];
  char fragment_source[sizeof(GML_GPU_OPENGL_PREFIX_ES)+
                       sizeof(GML_GPU_OPENGL_PREFIX_DESKTOP)+sizeof(GML_GPU_OPENGL_SCENE_FRAGMENT_BODY)];
  GLuint vertex,fragment;
  GLint status=GL_FALSE;
  if(scene->program) return 1;
  int vertex_length=snprintf(vertex_source,sizeof vertex_source,"%s%s",prefix,
                             GML_GPU_OPENGL_SCENE_VERTEX_BODY);
  int fragment_length=snprintf(fragment_source,sizeof fragment_source,"%s%s",prefix,
                               GML_GPU_OPENGL_SCENE_FRAGMENT_BODY);
  if(vertex_length<0 || (size_t)vertex_length>=sizeof vertex_source ||
     fragment_length<0 || (size_t)fragment_length>=sizeof fragment_source){
    gml_gpu_opengl_record_error(error,error_capacity,"scene shader source exceeds buffer capacity");
    return 0;
  }
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
  scene->u_alpha_test=backend->gl.GetUniformLocation(scene->program,"u_alpha_test");
  scene->u_previous=backend->gl.GetUniformLocation(scene->program,"u_previous");
  scene->u_region_origin=backend->gl.GetUniformLocation(scene->program,"u_region_origin");
  scene->u_rotated_alpha16=backend->gl.GetUniformLocation(scene->program,"u_rotated_alpha16[0]");
  scene->u_point_map_x=backend->gl.GetUniformLocation(scene->program,"u_point_map_x");
  scene->u_point_map_y=backend->gl.GetUniformLocation(scene->program,"u_point_map_y");
  scene->u_point_origin=backend->gl.GetUniformLocation(scene->program,"u_point_origin");
  scene->u_recovery_screen=backend->gl.GetUniformLocation(scene->program,"u_recovery_screen");
  scene->u_recovery_application=backend->gl.GetUniformLocation(scene->program,"u_recovery_application");
  scene->u_recovery_rect=backend->gl.GetUniformLocation(scene->program,"u_recovery_rect");
  scene->rotated_alpha16_valid=0;
  if(scene->a_position<0 || scene->a_texcoord<0 || scene->a_colour<0 ||
     scene->a_clamp<0 || scene->a_map<0 || scene->a_step<0 || scene->a_margin<0 ||
     scene->a_span_x<0 || scene->a_span_y<0 || scene->a_span_active<0 ||
     scene->u_extent<0 || scene->u_image<0 || scene->u_filter<0 || scene->u_alpha_test<0 ||
     scene->u_previous<0 || scene->u_region_origin<0 || scene->u_rotated_alpha16<0 ||
     scene->u_point_map_x<0 || scene->u_point_map_y<0 || scene->u_point_origin<0 ||
     scene->u_recovery_screen<0 || scene->u_recovery_application<0 || scene->u_recovery_rect<0){
    gml_gpu_opengl_record_error(error,error_capacity,"scene program is missing an attribute or uniform");
    backend->gl.DeleteProgram(scene->program);
    scene->program=0;
    return 0;
  }
  return 1;
}

static GlSceneExactProgram *build_exact_program(GmlGpuBackend *backend,
                                                GlSceneExactProgram *scene,
                                                int shader_font_mode,
                                                char *error,size_t error_capacity){
  int embedded=backend->api==GML_GPU_API_OPENGLES3;
  int specialised=embedded && shader_font_mode>0;
  int has_font=shader_font_mode!=0;
  const char *prefix=backend->api==GML_GPU_API_OPENGLES3
    ?GML_GPU_OPENGL_PREFIX_ES:GML_GPU_OPENGL_PREFIX_DESKTOP;
  char vertex_source[1024],fragment_source[32768];
  GLuint vertex,fragment;
  GLint status=GL_FALSE;
  int vertex_length,fragment_length;
  if(scene->exact_program) return scene;
  vertex_length=snprintf(vertex_source,sizeof vertex_source,"%s%s",prefix,
                         GML_GPU_OPENGL_SCENE_VERTEX_BODY);
  fragment_length=snprintf(fragment_source,sizeof fragment_source,
                           "%s#define GML_EXACT_FONT_MODE %d\n%s",prefix,
                           shader_font_mode,GML_GPU_OPENGL_SCENE_EXACT_FRAGMENT_BODY);
  if(vertex_length<0 || (size_t)vertex_length>=sizeof vertex_source ||
     fragment_length<0 || (size_t)fragment_length>=sizeof fragment_source){
    gml_gpu_opengl_record_error(error,error_capacity,"exact scene program source is too long");
    return NULL;
  }
  vertex=compile_stage(backend,GL_VERTEX_SHADER,vertex_source,error,error_capacity);
  if(!vertex) return NULL;
  fragment=compile_stage(backend,GL_FRAGMENT_SHADER,fragment_source,error,error_capacity);
  if(!fragment){ backend->gl.DeleteShader(vertex); return NULL; }
  scene->exact_program=backend->gl.CreateProgram();
  if(!scene->exact_program){
    backend->gl.DeleteShader(vertex);
    backend->gl.DeleteShader(fragment);
    gml_gpu_opengl_record_error(error,error_capacity,"exact scene program object could not be created");
    return NULL;
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
    return NULL;
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
  scene->exact_u_blend_rule=backend->gl.GetUniformLocation(scene->exact_program,
                                                            "u_blend_rule");
  scene->exact_u_additive_alpha=backend->gl.GetUniformLocation(scene->exact_program,
                                                                 "u_additive_alpha");
  scene->exact_u_effect_colour=backend->gl.GetUniformLocation(scene->exact_program,
                                                               "u_effect_colour");
  scene->exact_u_effect_intensity=backend->gl.GetUniformLocation(scene->exact_program,
                                                                  "u_effect_intensity");
  scene->exact_u_effect_zoom=backend->gl.GetUniformLocation(scene->exact_program,
                                                             "u_effect_zoom");
  scene->exact_u_effect_linear=backend->gl.GetUniformLocation(scene->exact_program,
                                                               "u_effect_linear");
  scene->exact_u_effect_seed=backend->gl.GetUniformLocation(scene->exact_program,
                                                             "u_effect_seed");
  scene->exact_u_font_origin=backend->gl.GetUniformLocation(scene->exact_program,"u_font_origin");
  scene->exact_u_font_scale=backend->gl.GetUniformLocation(scene->exact_program,"u_font_scale");
  scene->exact_u_font_alpha=backend->gl.GetUniformLocation(scene->exact_program,"u_font_alpha");
  scene->exact_u_font_crop=backend->gl.GetUniformLocation(scene->exact_program,"u_font_crop");
  scene->exact_u_font_tint=backend->gl.GetUniformLocation(scene->exact_program,"u_font_tint");
  scene->exact_u_font_margin=backend->gl.GetUniformLocation(scene->exact_program,"u_font_margin");
  scene->exact_u_font_mode=backend->gl.GetUniformLocation(scene->exact_program,"u_font_mode");
  scene->exact_u_font_channel=specialised
    ?backend->gl.GetUniformLocation(scene->exact_program,"u_font_channel"):-1;
  scene->exact_u_font_preserve_alpha=backend->gl.GetUniformLocation(scene->exact_program,
                                                                  "u_font_preserve_alpha");
  scene->exact_u_underlay_size=backend->gl.GetUniformLocation(scene->exact_program,"u_underlay_size");
  scene->exact_u_underlay_origin=backend->gl.GetUniformLocation(scene->exact_program,"u_underlay_origin");
  scene->exact_u_underlay_flags=backend->gl.GetUniformLocation(scene->exact_program,"u_underlay_flags");
  scene->exact_u_surface_rule=backend->gl.GetUniformLocation(scene->exact_program,"u_surface_rule");
  scene->exact_u_surface_tint=backend->gl.GetUniformLocation(scene->exact_program,"u_surface_tint");
  scene->exact_u_surface_alpha=backend->gl.GetUniformLocation(scene->exact_program,"u_surface_alpha");
  if(scene->exact_a_position<0 ||
     scene->exact_u_extent<0 || scene->exact_u_image<0 || scene->exact_u_previous<0 ||
     scene->exact_u_region_origin<0 ||
     (!specialised &&
      (scene->exact_a_texcoord<0 || scene->exact_a_colour<0 ||
       scene->exact_u_repetitions<0 || scene->exact_u_blend_rule<0 ||
       scene->exact_u_additive_alpha<0 || scene->exact_u_effect_colour<0 ||
       scene->exact_u_effect_intensity<0 || scene->exact_u_effect_zoom<0 ||
       scene->exact_u_effect_linear<0 || scene->exact_u_effect_seed<0 ||
       scene->exact_u_underlay_size<0 || scene->exact_u_underlay_origin<0 ||
       scene->exact_u_underlay_flags<0 || scene->exact_u_surface_rule<0 ||
       scene->exact_u_surface_tint<0 || scene->exact_u_surface_alpha<0)) ||
     (has_font &&
      (scene->exact_u_font_origin<0 || scene->exact_u_font_scale<0 ||
       scene->exact_u_font_alpha<0 || scene->exact_u_font_crop<0 || scene->exact_u_font_margin<0 ||
       scene->exact_u_font_tint<0 || scene->exact_u_font_preserve_alpha<0 ||
       (specialised?scene->exact_u_font_channel<0:scene->exact_u_font_mode<0)))){
    gml_gpu_opengl_record_error(error,error_capacity,
      "exact scene program is missing an attribute or uniform");
    backend->gl.DeleteProgram(scene->exact_program);
    scene->exact_program=0;
    return NULL;
  }
  return scene;
}

GlSceneExactProgram *gml_gpu_opengl_scene_exact_program(GmlGpuBackend *backend,int font_mode,
                                                       char *error,size_t error_capacity){
  GlScene *owner=&backend->scene;
  int embedded=backend->api==GML_GPU_API_OPENGLES3;
  unsigned slot=embedded?(font_mode==2?1u:font_mode==3?2u:0u):0u;
  if(font_mode!=0 && font_mode!=2 && font_mode!=3){
    gml_gpu_opengl_record_error(error,error_capacity,"exact font policy is unsupported");
    return NULL;
  }
  if(!embedded)
    return build_exact_program(backend,&owner->exact_programs[0],-1,error,error_capacity);
  if(!font_mode)
    return build_exact_program(backend,&owner->exact_programs[0],0,error,error_capacity);
  if(!owner->exact_font_split){
    char combined_error[GL_INFO_LOG_LIMIT];
    GlSceneExactProgram *combined=&owner->exact_programs[1];
    if(combined->exact_program) return combined;
    combined_error[0]='\0';
    if(build_exact_program(backend,combined,-1,combined_error,sizeof combined_error))
      return combined;
    /* Some embedded compilers cannot accept the combined integer program. Preserve the
     * established per-channel programs as a capability fallback without failing the backend. */
    memset(combined,0,sizeof *combined);
    owner->exact_font_split=1;
  }
  return build_exact_program(backend,&owner->exact_programs[slot],font_mode,error,error_capacity);
}

int gml_gpu_opengl_rgb_reduce_program(GmlGpuBackend *backend){
  GlScene *scene=&backend->scene;
  const char *prefix=backend->api==GML_GPU_API_OPENGLES3
    ?GML_GPU_OPENGL_PREFIX_ES:GML_GPU_OPENGL_PREFIX_DESKTOP;
  char vertex_source[512],fragment_source[2048];
  GLuint vertex,fragment;
  GLint status=GL_FALSE;
  if(scene->rgb_reduce_program) return 1;
  snprintf(vertex_source,sizeof vertex_source,"%s%s",prefix,GML_GPU_OPENGL_VERTEX_BODY);
  snprintf(fragment_source,sizeof fragment_source,"%s%s",prefix,GML_GPU_OPENGL_RGB_REDUCE_FRAGMENT_BODY);
  vertex=compile_stage(backend,GL_VERTEX_SHADER,vertex_source,NULL,0u);
  if(!vertex) return 0;
  fragment=compile_stage(backend,GL_FRAGMENT_SHADER,fragment_source,NULL,0u);
  if(!fragment){ backend->gl.DeleteShader(vertex); return 0; }
  scene->rgb_reduce_program=backend->gl.CreateProgram();
  if(scene->rgb_reduce_program){
    backend->gl.AttachShader(scene->rgb_reduce_program,vertex);
    backend->gl.AttachShader(scene->rgb_reduce_program,fragment);
    backend->gl.LinkProgram(scene->rgb_reduce_program);
    backend->gl.GetProgramiv(scene->rgb_reduce_program,GL_LINK_STATUS,&status);
  }
  backend->gl.DeleteShader(vertex);backend->gl.DeleteShader(fragment);
  if(status!=GL_TRUE) goto refused;
  scene->rgb_reduce_u_image=backend->gl.GetUniformLocation(scene->rgb_reduce_program,"u_image");
  scene->rgb_reduce_u_extent=backend->gl.GetUniformLocation(scene->rgb_reduce_program,"u_source_extent");
  if(scene->rgb_reduce_u_image>=0 && scene->rgb_reduce_u_extent>=0) return 1;
refused:
  if(scene->rgb_reduce_program) backend->gl.DeleteProgram(scene->rgb_reduce_program);
  scene->rgb_reduce_program=0;
  return 0;
}

int gml_gpu_opengl_checkpoint_program(GmlGpuBackend *backend){
  GlScene *scene=&backend->scene;
  const char *prefix=backend->api==GML_GPU_API_OPENGLES3
    ?GML_GPU_OPENGL_PREFIX_ES:GML_GPU_OPENGL_PREFIX_DESKTOP;
  char vertex_source[512],fragment_source[2048];
  GLuint vertex,fragment;
  GLint status=GL_FALSE;
  if(scene->checkpoint_program) return 1;
  if(scene->checkpoint_program_failed) return 0;
  scene->checkpoint_program_failed=1;
  snprintf(vertex_source,sizeof vertex_source,"%s%s",prefix,GML_GPU_OPENGL_VERTEX_BODY);
  snprintf(fragment_source,sizeof fragment_source,"%s%s",prefix,
           GML_GPU_OPENGL_CHECKPOINT_FRAGMENT_BODY);
  vertex=compile_stage(backend,GL_VERTEX_SHADER,vertex_source,NULL,0u);
  if(!vertex) return 0;
  fragment=compile_stage(backend,GL_FRAGMENT_SHADER,fragment_source,NULL,0u);
  if(!fragment){ backend->gl.DeleteShader(vertex); return 0; }
  scene->checkpoint_program=backend->gl.CreateProgram();
  if(scene->checkpoint_program){
    backend->gl.AttachShader(scene->checkpoint_program,vertex);
    backend->gl.AttachShader(scene->checkpoint_program,fragment);
    backend->gl.LinkProgram(scene->checkpoint_program);
    backend->gl.GetProgramiv(scene->checkpoint_program,GL_LINK_STATUS,&status);
  }
  backend->gl.DeleteShader(vertex);
  backend->gl.DeleteShader(fragment);
  if(status!=GL_TRUE) goto refused;
  scene->checkpoint_u_image=backend->gl.GetUniformLocation(scene->checkpoint_program,"u_image");
  scene->checkpoint_u_previous=backend->gl.GetUniformLocation(scene->checkpoint_program,"u_previous");
  scene->checkpoint_u_alpha=backend->gl.GetUniformLocation(scene->checkpoint_program,"u_alpha");
  if(scene->checkpoint_u_image<0 || scene->checkpoint_u_previous<0 ||
     scene->checkpoint_u_alpha<0) goto refused;
  scene->checkpoint_program_failed=0;
  return 1;
refused:
  if(scene->checkpoint_program) backend->gl.DeleteProgram(scene->checkpoint_program);
  scene->checkpoint_program=0;
  return 0;
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

static int glsl_line_leading(const char *text,size_t index){
  while(index && text[index-1u]!='\n'){
    unsigned char previous=(unsigned char)text[--index];
    if(previous!=' ' && previous!='\t' && previous!='\r') return 0;
  }
  return 1;
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

static size_t glsl_skip_layout(const char *source,size_t at,size_t end){
  for(;;){
    while(at<end && isspace((unsigned char)source[at])) at++;
    if(at+1u<end && source[at]=='/' && source[at+1u]=='/'){
      at+=2u;
      while(at<end && source[at]!='\n') at++;
      continue;
    }
    if(at+1u<end && source[at]=='/' && source[at+1u]=='*'){
      at+=2u;
      while(at+1u<end && !(source[at]=='*' && source[at+1u]=='/')) at++;
      at=at+1u<end?at+2u:end;
      continue;
    }
    return at;
  }
}

static int glsl_token_at(const char *source,size_t at,size_t end,const char *word){
  size_t length=strlen(word);
  return at+length<=end && !strncmp(source+at,word,length) &&
         (at+length==end || !(isalnum((unsigned char)source[at+length]) ||
                              source[at+length]=='_'));
}

static size_t glsl_identifier_end(const char *source,size_t at,size_t end){
  if(at>=end || !(isalpha((unsigned char)source[at]) || source[at]=='_')) return at;
  at++;
  while(at<end && (isalnum((unsigned char)source[at]) || source[at]=='_')) at++;
  return at;
}

static int glsl_main_header(const char *source,size_t start,size_t end){
  size_t at=glsl_skip_layout(source,start,end);
  if(!glsl_token_at(source,at,end,"void")) return 0;
  at=glsl_skip_layout(source,at+4u,end);
  if(!glsl_token_at(source,at,end,"main")) return 0;
  at=glsl_skip_layout(source,at+4u,end);
  if(at>=end || source[at++]!='(') return 0;
  at=glsl_skip_layout(source,at,end);
  if(glsl_token_at(source,at,end,"void")) at=glsl_skip_layout(source,at+4u,end);
  if(at>=end || source[at++]!=')') return 0;
  return glsl_skip_layout(source,at,end)==end;
}

static size_t glsl_directive_end(const char *source,size_t at,size_t end){
  for(;;){
    size_t line_start=at,prior;
    while(at<end && source[at]!='\n') at++;
    prior=at;
    while(prior>line_start && (source[prior-1u]==' ' || source[prior-1u]=='\t' ||
                               source[prior-1u]=='\r')) prior--;
    if(prior==line_start || source[prior-1u]!='\\' || at==end) return at;
    at++;
  }
}

static size_t glsl_main_open(const char *source){
  size_t start=0,depth=0,n=strlen(source);
  for(size_t at=0;at<n;at++){
    if(at+1u<n && source[at]=='/' && source[at+1u]=='/'){
      while(at<n && source[at]!='\n') at++;
      continue;
    }
    if(at+1u<n && source[at]=='/' && source[at+1u]=='*'){
      at+=2u;
      while(at+1u<n && !(source[at]=='*' && source[at+1u]=='/')) at++;
      at=at+1u<n?at+1u:n;
      continue;
    }
    if(!depth && source[at]=='#' && glsl_line_leading(source,at)){
      at=glsl_directive_end(source,at,n);
      start=at+1u;
      continue;
    }
    if(source[at]=='{'){
      if(!depth && glsl_main_header(source,start,at)) return at;
      depth++;
    }else if(source[at]=='}' && depth){
      if(!--depth) start=at+1u;
    }else if(source[at]==';' && !depth) start=at+1u;
  }
  return n;
}

static int glsl_mutable_type(const char *source,size_t start,size_t end){
  static const char *types[]={
    "float","int","uint","bool","double","vec2","vec3","vec4",
    "ivec2","ivec3","ivec4","uvec2","uvec3","uvec4",
    "bvec2","bvec3","bvec4","dvec2","dvec3","dvec4",
    "mat2","mat3","mat4","mat2x3","mat2x4","mat3x2","mat3x4",
    "mat4x2","mat4x3"
  };
  for(size_t index=0;index<sizeof types/sizeof types[0];index++){
    size_t length=strlen(types[index]);
    if(end-start==length && !strncmp(source+start,types[index],length)) return 1;
  }
  return 0;
}

/* Only simple, mutable global declarations are relocated. Complex declarators remain for the
 * driver's ordinary compile/refusal path; this never guesses at an authored expression. */
static int glsl_global_initializer(const char *source,size_t start,size_t end,
                                   size_t *name_start,size_t *name_end,size_t *equals){
  size_t at=glsl_skip_layout(source,start,end),type_end,identifier_end;
  if(glsl_token_at(source,at,end,"lowp")) at=glsl_skip_layout(source,at+4u,end);
  else if(glsl_token_at(source,at,end,"highp")) at=glsl_skip_layout(source,at+5u,end);
  else if(glsl_token_at(source,at,end,"mediump")) at=glsl_skip_layout(source,at+7u,end);
  type_end=glsl_identifier_end(source,at,end);
  if(!glsl_mutable_type(source,at,type_end)) return 0;
  at=glsl_skip_layout(source,type_end,end);
  *name_start=at;
  identifier_end=glsl_identifier_end(source,at,end);
  if(identifier_end==at) return 0;
  *name_end=identifier_end;
  at=glsl_skip_layout(source,identifier_end,end);
  if(at>=end || source[at]!='=') return 0;
  *equals=at;
  at=glsl_skip_layout(source,at+1u,end);
  if(at==end) return 0;
  {
    int parentheses=0,brackets=0;
    for(;at<end;at++){
      if(at+1u<end && source[at]=='/' && source[at+1u]=='/'){
        while(at<end && source[at]!='\n') at++;
        continue;
      }
      if(at+1u<end && source[at]=='/' && source[at+1u]=='*'){
        at+=2u;
        while(at+1u<end && !(source[at]=='*' && source[at+1u]=='/')) at++;
        at=at+1u<end?at+1u:end;
        continue;
      }
      if(source[at]=='(') parentheses++;
      else if(source[at]==')') parentheses--;
      else if(source[at]=='[') brackets++;
      else if(source[at]==']') brackets--;
      else if(source[at]=='{' || source[at]=='}' ||
              (source[at]==',' && !parentheses && !brackets)) return 0;
      if(parentheses<0 || brackets<0) return 0;
    }
    return !parentheses && !brackets;
  }
}

/* Older authored GLSL permits mutable globals initialized from uniforms; GLSL ES 3.00 requires
 * constant global initializers. Execute eligible original expressions at the start of main, in
 * their declaration order, before any authored statement or helper call. This is per invocation
 * in both dialects and preserves the globals and all later reads/writes. */
static char *glsl_relocate_global_initializers(char *source){
  size_t main_open=glsl_main_open(source),n=strlen(source),start=0,depth=0;
  char *assignments=NULL,*result=NULL;
  size_t assignment_length=0,assignment_capacity=0,result_length=0,result_capacity=0;
  if(main_open==n) return source;
  for(size_t at=0;at<main_open;at++){
    if(at+1u<main_open && source[at]=='/' && source[at+1u]=='/'){
      while(at<main_open && source[at]!='\n') at++;
      continue;
    }
    if(at+1u<main_open && source[at]=='/' && source[at+1u]=='*'){
      at+=2u;
      while(at+1u<main_open && !(source[at]=='*' && source[at+1u]=='/')) at++;
      at=at+1u<main_open?at+1u:main_open;
      continue;
    }
    if(!depth && source[at]=='#' && glsl_line_leading(source,at)){
      at=glsl_directive_end(source,at,main_open);
      start=at+1u;
      continue;
    }
    if(source[at]=='{') depth++;
    else if(source[at]=='}' && depth){ if(!--depth) start=at+1u; }
    else if(source[at]==';' && !depth){
      size_t name_start=0,name_end=0,equals=0;
      if(glsl_global_initializer(source,start,at,&name_start,&name_end,&equals)){
        if(!glsl_append(&assignments,&assignment_length,&assignment_capacity,
                        source+name_start,name_end-name_start) ||
           !glsl_append(&assignments,&assignment_length,&assignment_capacity," ",1u) ||
           !glsl_append(&assignments,&assignment_length,&assignment_capacity,
                        source+equals,at-equals+1u) ||
           !glsl_append(&assignments,&assignment_length,&assignment_capacity,"\n",1u)){
          free(assignments); free(source); return NULL;
        }
        for(size_t blank=equals;blank<at;blank++){
          if(source[blank]!='\n' && source[blank]!='\r') source[blank]=' ';
        }
      }
      start=at+1u;
    }
  }
  if(!assignment_length){ free(assignments); return source; }
  if(!glsl_append(&result,&result_length,&result_capacity,source,main_open+1u) ||
     !glsl_append(&result,&result_length,&result_capacity,"\n",1u) ||
     !glsl_append(&result,&result_length,&result_capacity,assignments,assignment_length) ||
     !glsl_append(&result,&result_length,&result_capacity,source+main_open+1u,n-main_open-1u)){
    free(assignments); free(result); free(source); return NULL;
  }
  free(assignments);
  free(source);
  return result;
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
    /* A version directive occupies its line. A precision declaration may share a line with
     * program code, so discard only that declaration in desktop GLSL. */
    if((i==0 || source[i-1]=='\n') && !strncmp(source+i,"#version",8)){
      while(i<n && source[i]!='\n') i++;
      continue;
    }
    if(desktop && glsl_line_leading(source,i) && !strncmp(source+i,"precision",9) &&
       glsl_word_boundary(source,i,9)){
      size_t end=i+9u;
      while(end<n && isspace((unsigned char)source[end])) end++;
      if(end<n && (!strncmp(source+end,"lowp",4) ||
                   !strncmp(source+end,"mediump",7) ||
                   !strncmp(source+end,"highp",5))){
        while(end<n && source[end]!=';' && source[end]!='\n') end++;
        if(end<n && source[end]==';'){
          i=end+1u;
          continue;
        }
      }
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
  return glsl_relocate_global_initializers(out);
}

static uint64_t content_source_key(const GmlPlanShader *shader){
  uint64_t hash=UINT64_C(1469598103934665603);
  const char *sources[2]={shader->vertex_es,shader->fragment_es};
  for(size_t stage=0;stage<2;stage++){
    const unsigned char *cursor=(const unsigned char*)sources[stage];
    while(*cursor){ hash^=*cursor++; hash*=UINT64_C(1099511628211); }
    hash^=0u;
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

int gml_gpu_opengl_content_program_failed(const GmlGpuBackend *backend,
                                           const GmlPlanShader *shader){
  uint64_t key;
  if(!backend || !shader || !shader->vertex_es || !shader->fragment_es) return 0;
  key=content_source_key(shader);
  for(size_t index=0;index<GL_CONTENT_PROGRAMS;index++){
    const GlContentProgram *slot=&backend->content[index];
    if(slot->failed && slot->identity==shader->identity &&
       slot->content_generation==shader->content_generation &&
       slot->source_key==key && slot->scene_rgba) return 1;
  }
  return 0;
}

static GlContentProgram *content_slot(GmlGpuBackend *backend,const GmlPlanShader *shader,
                                      int scene_rgba){
  GlContentProgram *slot=NULL;
  uint64_t source_key=content_source_key(shader);
  uint32_t oldest=0xFFFFFFFFu;
  for(size_t index=0;index<GL_CONTENT_PROGRAMS;index++){
    GlContentProgram *candidate=&backend->content[index];
    if((candidate->program || candidate->failed) && candidate->identity==shader->identity &&
       candidate->content_generation==shader->content_generation &&
       candidate->source_key==source_key &&
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
  slot->content_generation=shader->content_generation;
  slot->source_key=source_key;
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
    if(active<0 || active>GL_CONTENT_UNIFORMS){
      gml_gpu_opengl_record_error(error,error_capacity,
        "content program exceeds active-uniform capacity");
      backend->gl.DeleteProgram(slot->program);
      slot->program=0;
      slot->failed=1;
      return NULL;
    }
    slot->uniform_count=0;
    for(GLint index=0;index<active;index++){
      GlContentUniform *entry=&slot->uniforms[slot->uniform_count];
      GLsizei length=0;
      size_t name_length;
      entry->size=0;
      entry->type=0;
      entry->name[0]='\0';
      backend->gl.GetActiveUniform(slot->program,(GLuint)index,(GLsizei)sizeof entry->name,&length,
                                   &entry->size,&entry->type,entry->name);
      entry->name[sizeof entry->name-1]='\0';
      if(length<0 || (size_t)length>=sizeof entry->name-1u){
        gml_gpu_opengl_record_error(error,error_capacity,
          "content program uniform name exceeds capacity");
        backend->gl.DeleteProgram(slot->program);
        slot->program=0;
        slot->failed=1;
        return NULL;
      }
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
                                            const GLuint *resident_samplers,
                                            char *error,size_t error_capacity){
  const GmlPlanShader *shader=&plan->shader;
  const GmlPlanImage *image=&plan->images[op->source];
  GlContentProgram *program;
  GLuint source_name=resident_source;
  GLuint sampler_names[GML_PLAN_MAX_SAMPLERS]={0,0,0,0};
  float matrices[5*16];
  float vertices[4*9];
  const float w=(float)op->destination.width,h=(float)op->destination.height;
  const float x=shader->geometry.valid?(float)shader->geometry.origin_x:0.0f;
  const float y=shader->geometry.valid?(float)shader->geometry.origin_y:0.0f;
  const float width=shader->geometry.valid?(float)shader->geometry.extent_x:w;
  const float height=shader->geometry.valid?(float)shader->geometry.extent_y:h;
  const float scale_x=2.0f/width,scale_y=-2.0f/height;
  const float offset_x=-1.0f-scale_x*x,offset_y=1.0f-scale_y*y;
  static const GLenum units[GML_PLAN_MAX_SAMPLERS]={GL_TEXTURE1,GL_TEXTURE2,GL_TEXTURE3,GL_TEXTURE4};
  if(!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height) ||
     !isfinite(x+width) || !isfinite(y+height) || x+width==x || y+height==y ||
     !isfinite(scale_x) || !isfinite(scale_y) || !isfinite(offset_x) || !isfinite(offset_y)){
    gml_gpu_opengl_record_error(error,error_capacity,"content draw coordinates are not representable");
    plan->fallback_reason=GML_PLAN_FALLBACK_UNSUPPORTED_OPERATION;
    return 0;
  }
  if(shader->uniform_count>GML_PLAN_MAX_UNIFORMS ||
     shader->sampler_count>GML_PLAN_MAX_SAMPLERS){
    gml_gpu_opengl_record_error(error,error_capacity,"content program binding count exceeds capacity");
    plan->fallback_reason=GML_PLAN_FALLBACK_SHADER_FAILURE;
    return 0;
  }
  if(backend->scene.max_texture_units<1 ||
     shader->sampler_count>=(uint32_t)backend->scene.max_texture_units){
    gml_gpu_opengl_record_error(error,error_capacity,"content program exceeds texture-unit limit");
    plan->fallback_reason=GML_PLAN_FALLBACK_SHADER_FAILURE;
    return 0;
  }
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
    if(resident_samplers){
      sampler_names[index]=resident_samplers[index];
      continue;
    }
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
   * the projection is the same map so a program that multiplies by it alone still lands. The
   * position attribute retains authored coordinates; rebasing this projection, not the vertices,
   * keeps their mapped target rectangle unchanged. */
  memset(matrices,0,sizeof matrices);
  for(int m=0;m<5;m++){ matrices[m*16+0]=1.0f; matrices[m*16+5]=1.0f; matrices[m*16+10]=1.0f; matrices[m*16+15]=1.0f; }
  for(int m=1;m<5;m+=3){
    float *target=matrices+m*16;
    target[0]=scale_x; target[5]=scale_y; target[10]=1.0f;
    target[12]=offset_x; target[13]=offset_y; target[15]=1.0f;
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
      row[0]=x+width*(corners[v][0]/w);
      row[1]=y+height*(corners[v][1]/h); row[2]=0.0f;
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
                                         NULL,error,error_capacity);
}

int gml_gpu_opengl_scene_shader_draw(GmlGpuBackend *backend,const GmlPlanShader *shader,
                                      const GmlRenderDrawStateSnapshot *state,
                                      GlSceneImage *target,GmlRenderRect region,
                                      GlSceneImage *source,
                                      GlSceneImage *const *samplers,uint32_t sampler_count,
                                      int source_over,
                                      char *error,size_t error_capacity){
  GmlRenderPlan plan={0};
  GmlPlanOp operation={0};
  GLuint sampler_names[GML_PLAN_MAX_SAMPLERS]={0};
  GLuint source_name;
  int source_origin;
  int drawn;
  GLenum gl_error;
  if(!backend || !shader || !state || !target || !source || !source->texture ||
     !target->framebuffer || !target->width || !target->height ||
     sampler_count!=shader->sampler_count || sampler_count>GML_PLAN_MAX_SAMPLERS) return 0;
  source_name=source->texture;
  source_origin=source->rendered_origin;
  if(source_origin){
    source_name=gml_gpu_opengl_scene_content_source(backend,source);
    if(!source_name) return 0;
    source_origin=0;
  }
  plan.target_width=target->width;
  plan.target_height=target->height;
  plan.image_count=1u;
  plan.images[0].width=source->width;
  plan.images[0].height=source->height;
  plan.shader=*shader;
  for(uint32_t index=0;index<sampler_count;index++){
    if(!samplers || !samplers[index] || !samplers[index]->texture) return 0;
    sampler_names[index]=samplers[index]->rendered_origin
      ?gml_gpu_opengl_scene_content_source(backend,samplers[index]):samplers[index]->texture;
    if(!sampler_names[index]) return 0;
    plan.shader.samplers[index].image=index+1u;
  }
  operation.opcode=GML_PLAN_OP_SHADER_DRAW;
  operation.source=0u;
  operation.linear=state->samplers[0].filter==GML_RENDER_FILTER_BILINEAR;
  operation.destination=(GmlPlanRect){region.x,region.y,region.width,region.height};
  operation.source_rect=(GmlPlanRect){0,0,source->width,source->height};
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  if(source_over){
    backend->gl.BlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,
                                  GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    backend->gl.BlendEquationSeparate(GL_FUNC_ADD,GL_FUNC_ADD);
    backend->gl.Enable(GL_BLEND);
  } else backend->gl.Disable(GL_BLEND);
  backend->gl.BindVertexArray(backend->scene.vertex_array);
  drawn=execute_shader_draw_with_source(backend,&plan,&operation,NULL,source_name,
                                        source_origin,sampler_names,error,error_capacity);
  backend->gl.BindVertexArray(0);
  backend->gl.Disable(GL_BLEND);
  gl_error=backend->gl.GetError();
  if(gl_error!=GL_NO_ERROR)
    gml_gpu_opengl_record_error(error,error_capacity,"scene shader GL error %u",gl_error);
  return drawn && gl_error==GL_NO_ERROR;
}
