/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing OpenGL source mirrors and exact integer axis maps. */
#include "gml_gpu_opengl_internal.h"
#include "gml_render_sample_setup.h"
#include "gml_thread.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>

static GlSourceTexture *acquire_source(GmlGpuBackend *backend,const GmlPlanImage *image,int linear){
  GlSourceTexture *slot=NULL;
  uint32_t oldest=0xFFFFFFFFu;
  for(size_t index=0;index<GML_PLAN_MAX_IMAGES;index++){
    GlSourceTexture *candidate=&backend->sources[index];
    if(candidate->name && candidate->image_class==image->image_class &&
       candidate->identity==image->identity && candidate->linear==linear) return candidate;
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
  slot->linear=linear;
  slot->uploaded=0;
  slot->width=0;
  slot->height=0;
  return slot;
}

int gml_gpu_opengl_upload_source(GmlGpuBackend *backend,const GmlPlanImage *image,
                         GmlGpuCounters *counters,GLuint *out_name,int linear){
  GlSourceTexture *slot=acquire_source(backend,image,linear);
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
         slot->intermediate_extent==axis->intermediate_extent &&
         slot->origin==axis->origin && slot->extent==axis->extent;
}

static int reserve_axis_scratch(GmlGpuBackend *backend,uint32_t count){
  if(backend->map_scratch_capacity<count){
    uint16_t *grown=(uint16_t*)realloc(backend->map_scratch,(size_t)count*sizeof *grown);
    if(!grown) return 0;
    backend->map_scratch=grown;
    backend->map_scratch_capacity=count;
  }
  return 1;
}

/* Both terminal and scene maps use the same preserved integer texture upload.
 * Their distinct cache keys never select one another's sampling rule. */
static int upload_axis_map(GmlGpuBackend *backend,GlAxisMap *slot,const GmlPlanAxis *axis,
                           uint32_t count,GmlGpuCounters *counters,GLuint *out_name){
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
  /* Never publish a rejected upload's key. Storage failure also invalidates
   * the capacity hint so a retry allocates before supplying its new samples. */
  if(backend->gl.GetError()!=GL_NO_ERROR){
    slot->valid=0;
    slot->capacity=0;
    return 0;
  }
  slot->count=count;
  slot->rule=axis->rule;
  slot->source_extent=axis->source_extent;
  slot->destination_extent=axis->destination_extent;
  slot->destination_first=axis->destination_first;
  slot->intermediate_extent=axis->intermediate_extent;
  slot->origin=axis->origin;
  slot->extent=axis->extent;
  slot->valid=1;
  if(counters) counters->uploaded_bytes+=(uint64_t)count*2u;
  *out_name=slot->name;
  return 1;
}

int gml_gpu_opengl_upload_axis(GmlGpuBackend *backend,uint32_t slot_index,const GmlPlanAxis *axis,
                       uint32_t count,GmlGpuCounters *counters,GLuint *out_name){
  if(slot_index>=GL_PLAN_MAP_SLOTS || count==0u) return 0;
  GlAxisMap *slot=&backend->maps[slot_index];
  if(axis_matches(slot,axis,count) && slot->name){ *out_name=slot->name; return 1; }
  if(!reserve_axis_scratch(backend,count) ||
     !gml_render_plan_axis_map(axis,backend->map_scratch,count)) return 0;
  return upload_axis_map(backend,slot,axis,count,counters,out_name);
}

int gml_gpu_opengl_upload_point_axis(GmlGpuBackend *backend,unsigned axis,
                                    double anchor,double phase,double scale,
                                    int first,uint32_t count,uint32_t source_extent,
                                    GLuint *out_name){
  /* UINT32_MAX is a private cache identity, not a terminal plan-axis rule.
   * The extra two slots are disposable context-local scene maps. */
  if(axis>1u || first<0 || first>10000000 ||
     !count || count>4096u || !source_extent || source_extent>4096u ||
     !isfinite(anchor) || anchor!=floor(anchor) || fabs(anchor)>10000000.0 ||
     !isfinite(phase) || phase<0.0 || phase>1.0 ||
     !isfinite(scale) || scale==0.0 || fabs(scale)>1000000.0) return 0;
  int reflected=scale<0.0;
  int pixel=reflected?(int)anchor-1-first:first-(int)anchor;
  double extent=floor(source_extent*fabs(scale)+0.5);
  if(extent<1.0 || extent>4096.0 || pixel<0 || pixel>=extent ||
     (reflected?pixel<(int)count-1:pixel+(int)count>extent)) return 0;
  GmlPlanAxis key={.rule=UINT32_MAX,.source_extent=source_extent,
    .destination_extent=count,.destination_first=pixel,.origin=phase,.extent=scale};
  GlAxisMap *slot=&backend->maps[GL_PLAN_MAP_SLOTS+axis];
  if(axis_matches(slot,&key,count) && slot->name){ *out_name=slot->name; return 1; }
  if(!reserve_axis_scratch(backend,count)) return 0;
  for(uint32_t index=0;index<count;index++){
    int sample=gml_render_point_axis_index(pixel+(reflected?-(int)index:(int)index),
                                           phase,fabs(scale));
    backend->map_scratch[index]=sample>=0 && (uint32_t)sample<source_extent
      ?(uint16_t)sample:UINT16_MAX;
  }
  return upload_axis_map(backend,slot,&key,count,NULL,out_name);
}

/* Scene storage is derived and disposable. Logical identity and content versions are supplied by
 * the neutral renderer; this table owns only context-local texture and framebuffer names. */
static int scene_same_logical(GmlRenderImageRef a,GmlRenderImageRef b){
  return a.resource_class==b.resource_class && a.identity==b.identity &&
         a.incarnation==b.incarnation;
}

static int scene_same_version(GmlRenderImageRef a,GmlRenderImageRef b){
  return scene_same_logical(a,b) && a.content_version==b.content_version;
}

static void scene_delete_raw_depth(GmlGpuBackend *backend,GlSceneImage *image){
  if(image->triangle_framebuffer) backend->gl.DeleteFramebuffers(1,&image->triangle_framebuffer);
  if(image->triangle_texture) backend->gl.DeleteTextures(1,&image->triangle_texture);
  image->triangle_framebuffer=image->triangle_texture=0;
  for(unsigned i=0;i<2u;i++){
    if(image->raw_depth_framebuffer[i])
      backend->gl.DeleteFramebuffers(1,&image->raw_depth_framebuffer[i]);
    if(image->raw_depth_texture[i]) backend->gl.DeleteTextures(1,&image->raw_depth_texture[i]);
    image->raw_depth_framebuffer[i]=image->raw_depth_texture[i]=0;
  }
  image->raw_depth_epoch=image->raw_depth_version=0;
  image->raw_depth_front=0;
}

static void scene_delete_image(GmlGpuBackend *backend,GlSceneImage *image){
  scene_delete_raw_depth(backend,image);
  if(image->depth_renderbuffer) backend->gl.DeleteRenderbuffers(1,&image->depth_renderbuffer);
  if(image->content_source_framebuffer)
    backend->gl.DeleteFramebuffers(1,&image->content_source_framebuffer);
  if(image->content_source_texture) backend->gl.DeleteTextures(1,&image->content_source_texture);
  if(image->framebuffer) backend->gl.DeleteFramebuffers(1,&image->framebuffer);
  if(image->texture) backend->gl.DeleteTextures(1,&image->texture);
  if(image->completion_texture) backend->gl.DeleteTextures(1,&image->completion_texture);
  memset(image,0,sizeof *image);
}

void gml_gpu_opengl_scene_forget(GmlGpuBackend *backend){
  GlScene *scene=&backend->scene;
  memset(scene->images,0,sizeof scene->images);
  memset(scene->checkpoints,0,sizeof scene->checkpoints);
  scene->checkpoint_program=scene->checkpoint_mask=scene->checkpoint_framebuffer=0;
  scene->checkpoint_mask_width=scene->checkpoint_mask_height=0;
  scene->checkpoint_rows=scene->checkpoint_rows_framebuffer=0;
  scene->checkpoint_rows_width=scene->checkpoint_rows_height=0;
  scene->checkpoint_program_failed=0;
  scene->linear_program=scene->linear_buffer=0;
  scene->linear_capacity=0;
  scene->linear_failed=0;
  memset(&scene->main_program,0,sizeof scene->main_program);
  memset(&scene->filtered_program,0,sizeof scene->filtered_program);
  memset(scene->compact_programs,0,sizeof scene->compact_programs);
  scene->vertex_buffer=scene->vertex_array=0;
  memset(scene->depth_triangle_program,0,sizeof scene->depth_triangle_program);
  scene->depth_triangle_buffer=scene->depth_triangle_array=0;
  scene->instance_buffer=scene->instance_array=0;
  scene->upload_buffer=0;
  scene->upload_capacity=0;
  scene->upload_policy=0;
  scene->upload_clock=NULL;
  scene->upload_clock_userdata=NULL;
  scene->upload_client_ns=scene->upload_staged_ns=0;
  scene->instance_bytes_used=0;
  scene->instance_storage_ready=0;
  scene->instance_span_count=scene->instance_span_next=0;
  scene->instance_reuse_attempted=0;
  memset(scene->exact_programs,0,sizeof scene->exact_programs);
  scene->snapshot_texture=scene->copy_framebuffer=scene->rgb_query=0;
  scene->rgb_reduce_program=scene->rgb_reduce_framebuffer=0;
  scene->recovery_indirect_program=scene->recovery_indirect_buffer=0;
  scene->supports_recovery_indirect=scene->recovery_indirect_active=0;
  scene->recovery_indirect_failed=0;
  memset(scene->rgb_reduce_texture,0,sizeof scene->rgb_reduce_texture);
  memset(scene->rgb_reduce_width,0,sizeof scene->rgb_reduce_width);
  memset(scene->rgb_reduce_height,0,sizeof scene->rgb_reduce_height);
  scene->snapshot_width=scene->snapshot_height=0;
  scene->max_texture_size=0;
  scene->supports_sample_mask=0;
  scene->exact_font_split=0;
  scene->font_write_flush=1;
  scene->available=0;
}

void gml_gpu_opengl_scene_destroy(GmlGpuBackend *backend,int context_is_current){
  GlScene *scene=&backend->scene;
  if(context_is_current && backend->loaded){
    for(size_t index=0;index<GL_SCENE_MAX_IMAGES;index++)
      scene_delete_image(backend,&scene->images[index]);
    for(size_t index=0;index<GL_SCENE_CHECKPOINTS;index++)
      if(scene->checkpoints[index].texture)
        backend->gl.DeleteTextures(1,&scene->checkpoints[index].texture);
    if(scene->checkpoint_program) backend->gl.DeleteProgram(scene->checkpoint_program);
    if(scene->checkpoint_mask) backend->gl.DeleteTextures(1,&scene->checkpoint_mask);
    if(scene->checkpoint_framebuffer)
      backend->gl.DeleteFramebuffers(1,&scene->checkpoint_framebuffer);
    if(scene->checkpoint_rows) backend->gl.DeleteTextures(1,&scene->checkpoint_rows);
    if(scene->linear_program) backend->gl.DeleteProgram(scene->linear_program);
    if(scene->linear_buffer) backend->gl.DeleteBuffers(1,&scene->linear_buffer);
    if(scene->checkpoint_rows_framebuffer)
      backend->gl.DeleteFramebuffers(1,&scene->checkpoint_rows_framebuffer);
    if(scene->main_program.program) backend->gl.DeleteProgram(scene->main_program.program);
    for(unsigned pass=0;pass<2u;pass++)
      if(scene->depth_triangle_program[pass].program)
        backend->gl.DeleteProgram(scene->depth_triangle_program[pass].program);
    if(scene->depth_triangle_array) backend->gl.DeleteVertexArrays(1,&scene->depth_triangle_array);
    if(scene->depth_triangle_buffer) backend->gl.DeleteBuffers(1,&scene->depth_triangle_buffer);
    if(scene->filtered_program.program) backend->gl.DeleteProgram(scene->filtered_program.program);
    for(size_t index=0;index<sizeof scene->compact_programs/sizeof scene->compact_programs[0];index++)
      if(scene->compact_programs[index].program)
        backend->gl.DeleteProgram(scene->compact_programs[index].program);
    for(size_t index=0;index<3u;index++)
      if(scene->exact_programs[index].exact_program)
        backend->gl.DeleteProgram(scene->exact_programs[index].exact_program);
    if(scene->snapshot_texture) backend->gl.DeleteTextures(1,&scene->snapshot_texture);
    if(scene->copy_framebuffer) backend->gl.DeleteFramebuffers(1,&scene->copy_framebuffer);
    if(scene->rgb_query && backend->gl.DeleteQueries)
      backend->gl.DeleteQueries(1,&scene->rgb_query);
    if(scene->rgb_reduce_program) backend->gl.DeleteProgram(scene->rgb_reduce_program);
    if(scene->recovery_indirect_program) backend->gl.DeleteProgram(scene->recovery_indirect_program);
    if(scene->recovery_indirect_buffer) backend->gl.DeleteBuffers(1,&scene->recovery_indirect_buffer);
    if(scene->rgb_reduce_framebuffer)
      backend->gl.DeleteFramebuffers(1,&scene->rgb_reduce_framebuffer);
    for(size_t index=0;index<4u;index++)
      if(scene->rgb_reduce_texture[index])
        backend->gl.DeleteTextures(1,&scene->rgb_reduce_texture[index]);
    if(scene->vertex_array) backend->gl.DeleteVertexArrays(1,&scene->vertex_array);
    if(scene->vertex_buffer) backend->gl.DeleteBuffers(1,&scene->vertex_buffer);
    if(scene->instance_array) backend->gl.DeleteVertexArrays(1,&scene->instance_array);
    if(scene->instance_buffer) backend->gl.DeleteBuffers(1,&scene->instance_buffer);
    if(scene->upload_buffer) backend->gl.DeleteBuffers(1,&scene->upload_buffer);
  }
  free(scene->vertices);
  free(scene->instances);
  free(scene->instance_mirror);
  free(scene->scratch);
  scene->vertices=NULL;
  scene->instances=NULL;
  scene->instance_mirror=NULL;
  scene->scratch=NULL;
  scene->scratch_capacity=0;
  gml_gpu_opengl_scene_forget(backend);
}

/* Optional scene entry points never make an otherwise working hybrid context fail adoption. */
void gml_gpu_opengl_scene_reset(GmlGpuBackend *backend,const GmlGpuContext *context){
  GmlGpuProc proc;
  GlScene *scene=&backend->scene;
  gml_gpu_opengl_scene_forget(backend);
  scene->upload_clock=context->monotonic_time_ns;
  scene->upload_clock_userdata=context->time_userdata;
  /* This table belongs to one backend/engine. A reset starts a new context epoch even when a
   * frontend skipped the old destroy callback; forgotten names must not reach its replacement. */
  scene->context_epoch++;
  {
    const char *renderer=(const char*)backend->gl.GetString(GL_RENDERER);
    /* Same-context commands are ordered. V3D needs the measured masked-write
     * submission workaround; an unavailable identity retains that safe path. */
    scene->font_write_flush=!renderer || !*renderer || strstr(renderer,"V3D")!=NULL;
  }
#define GL_SCENE_LOAD(field,name) do { \
  proc=context->get_proc_address(context->userdata,name); \
  memcpy(&backend->gl.field,&proc,sizeof proc); \
} while(0)
  GL_SCENE_LOAD(GetIntegerv,"glGetIntegerv");
  GL_SCENE_LOAD(TransformFeedbackVaryings,"glTransformFeedbackVaryings");
  GL_SCENE_LOAD(BeginTransformFeedback,"glBeginTransformFeedback");
  GL_SCENE_LOAD(EndTransformFeedback,"glEndTransformFeedback");
  GL_SCENE_LOAD(IsEnabled,"glIsEnabled");
  GL_SCENE_LOAD(GetFloatv,"glGetFloatv");
  GL_SCENE_LOAD(LineWidth,"glLineWidth");
  GL_SCENE_LOAD(BindSampler,"glBindSampler");
  GL_SCENE_LOAD(CopyTexSubImage2D,"glCopyTexSubImage2D");
  GL_SCENE_LOAD(InvalidateFramebuffer,"glInvalidateFramebuffer");
  GL_SCENE_LOAD(ClearBufferfv,"glClearBufferfv");
  GL_SCENE_LOAD(BlitFramebuffer,"glBlitFramebuffer");
  GL_SCENE_LOAD(GetFramebufferAttachmentParameteriv,"glGetFramebufferAttachmentParameteriv");
  GL_SCENE_LOAD(GetTexLevelParameteriv,"glGetTexLevelParameteriv");
  GL_SCENE_LOAD(GenRenderbuffers,"glGenRenderbuffers");
  GL_SCENE_LOAD(BindRenderbuffer,"glBindRenderbuffer");
  GL_SCENE_LOAD(RenderbufferStorage,"glRenderbufferStorage");
  GL_SCENE_LOAD(FramebufferRenderbuffer,"glFramebufferRenderbuffer");
  GL_SCENE_LOAD(DeleteRenderbuffers,"glDeleteRenderbuffers");
  GL_SCENE_LOAD(BlendFuncSeparate,"glBlendFuncSeparate");
  GL_SCENE_LOAD(BlendEquationSeparate,"glBlendEquationSeparate");
  GL_SCENE_LOAD(Flush,"glFlush");
  GL_SCENE_LOAD(Finish,"glFinish");
  GL_SCENE_LOAD(GenQueries,"glGenQueries");
  GL_SCENE_LOAD(DeleteQueries,"glDeleteQueries");
  GL_SCENE_LOAD(BeginQuery,"glBeginQuery");
  GL_SCENE_LOAD(EndQuery,"glEndQuery");
  GL_SCENE_LOAD(GetQueryiv,"glGetQueryiv");
  GL_SCENE_LOAD(GetQueryObjectuiv,"glGetQueryObjectuiv");
  GL_SCENE_LOAD(PolygonMode,"glPolygonMode");
  GL_SCENE_LOAD(GetIntegeri_v,"glGetIntegeri_v");
  GL_SCENE_LOAD(BindBufferBase,"glBindBufferBase");
  GL_SCENE_LOAD(DispatchCompute,"glDispatchCompute");
  GL_SCENE_LOAD(ShaderMemoryBarrier,"glMemoryBarrier");
  GL_SCENE_LOAD(DrawArraysIndirect,"glDrawArraysIndirect");
#undef GL_SCENE_LOAD
  if(!backend->gl.GetIntegerv || !backend->gl.Finish) return;
  backend->gl.GetIntegerv(GL_MAX_TEXTURE_SIZE,&scene->max_texture_size);
  backend->gl.GetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&scene->max_texture_units);
  if(backend->api==GML_GPU_API_OPENGL_CORE) scene->supports_sample_mask=1;
  else {
    GLint major=0,minor=0;
    backend->gl.GetIntegerv(GL_MAJOR_VERSION,&major);
    backend->gl.GetIntegerv(GL_MINOR_VERSION,&minor);
    scene->supports_sample_mask=major>3 || (major==3 && minor>=1);
    /* Texture-level dimension queries first become core in ES 3.1. A resolver
     * may return a name for an unsupported entry point, so presence is insufficient. */
    if(!scene->supports_sample_mask) backend->gl.GetTexLevelParameteriv=NULL;
  }
  /* ES 3 has attachment invalidation. Desktop 3.3 need not expose its later core API. */
  if(backend->api==GML_GPU_API_OPENGL_CORE){
    GLint major=0,minor=0;
    backend->gl.GetIntegerv(GL_MAJOR_VERSION,&major);
    backend->gl.GetIntegerv(GL_MINOR_VERSION,&minor);
    if(major<4 || (major==4 && minor<3)) backend->gl.InvalidateFramebuffer=NULL;
  }
  {
    GLint major=0,minor=0;
    backend->gl.GetIntegerv(GL_MAJOR_VERSION,&major);
    backend->gl.GetIntegerv(GL_MINOR_VERSION,&minor);
    int version=backend->api==GML_GPU_API_OPENGLES3
      ?major>3 || (major==3 && minor>=1):major>4 || (major==4 && minor>=3);
    scene->supports_recovery_indirect=version && backend->gl.GetIntegeri_v &&
      backend->gl.BindBufferBase && backend->gl.DispatchCompute &&
      backend->gl.ShaderMemoryBarrier && backend->gl.DrawArraysIndirect;
  }
  scene->available=scene->max_texture_size>0;
}

int gml_gpu_opengl_scene_capabilities(const GmlGpuBackend *backend,GmlRenderDeviceCaps *caps){
  if(!backend || !caps || !backend->loaded || !backend->scene.available) return 0;
  memset(caps,0,sizeof *caps);
  caps->max_texture_extent=(uint32_t)backend->scene.max_texture_size;
  caps->max_batch_quads=GL_SCENE_MAX_QUADS;
  caps->supported_operations=(UINT64_C(1)<<GML_RENDER_OP_CLEAR)|
                             (UINT64_C(1)<<GML_RENDER_OP_COLOURED_PRIMITIVE)|
                             (UINT64_C(1)<<GML_RENDER_OP_TEXTURED_QUAD)|
                             (UINT64_C(1)<<GML_RENDER_OP_LAYER_EFFECT)|
                             (UINT64_C(1)<<GML_RENDER_OP_CONTENT_SHADER_DRAW)|
                             (UINT64_C(1)<<GML_RENDER_OP_RECOVER_EMPTY_SCREEN);
  if(backend->gl.BlitFramebuffer)
    caps->supported_operations|=UINT64_C(1)<<GML_RENDER_OP_COPY_IMAGE;
  if(backend->gl.BlitFramebuffer && backend->gl.BindSampler)
    caps->supported_operations|=UINT64_C(1)<<GML_RENDER_OP_SOFTWARE3D_PASS;
  caps->supported_families=GML_RENDER_FAMILY_TEXTURE|GML_RENDER_FAMILY_PRIMITIVE|
                           GML_RENDER_FAMILY_EFFECT|GML_RENDER_FAMILY_SHADER|
                           GML_RENDER_FAMILY_SURFACE;
  if(backend->gl.BlitFramebuffer && backend->gl.BindSampler)
    caps->supported_families|=GML_RENDER_FAMILY_SOFTWARE3D;
  caps->supports_rgb_alpha_separate=backend->gl.BlendFuncSeparate &&
                                     backend->gl.BlendEquationSeparate;
  caps->soft_cache_bytes=128u*1024u*1024u;
  return 1;
}

static GlSceneImage *scene_slot(GmlGpuBackend *backend,GmlRenderImageRef ref){
  GlSceneImage *free_slot=NULL,*oldest=NULL;
  for(size_t index=0;index<GL_SCENE_MAX_IMAGES;index++){
    GlSceneImage *candidate=&backend->scene.images[index];
    /* Retirement can be requested by frontend state restore without a current context. Its
     * name stays owned by this context until the next upload/target lookup, which is current. */
    if(candidate->retired){
      if(candidate->context_epoch==backend->scene.context_epoch)
        scene_delete_image(backend,candidate);
      else memset(candidate,0,sizeof *candidate);
    }
    if(candidate->texture && scene_same_logical(candidate->ref,ref)) return candidate;
    if(!candidate->texture && !free_slot) free_slot=candidate;
    if(candidate->texture && !candidate->target &&
       (!oldest || candidate->last_used<oldest->last_used)) oldest=candidate;
  }
  if(free_slot) return free_slot;
  if(oldest){ scene_delete_image(backend,oldest); return oldest; }
  return NULL;
}

GlSceneImage *gml_gpu_opengl_scene_image(GmlGpuBackend *backend,GmlRenderImageRef ref){
  if(!backend || !backend->scene.available) return NULL;
  for(size_t index=0;index<GL_SCENE_MAX_IMAGES;index++){
    GlSceneImage *candidate=&backend->scene.images[index];
    if(candidate->texture && !candidate->retired && candidate->valid &&
       candidate->context_epoch==backend->scene.context_epoch &&
       scene_same_version(candidate->ref,ref)){
      candidate->last_used=++backend->scene.use_clock;
      return candidate;
    }
  }
  return NULL;
}

/* Reduce actual RGB bytes to one exact flag, entirely in this context. Intermediate allocations
 * may be larger than the written level: the shader's explicit extent excludes stale border data.
 * Separate final flags keep SCREEN alive while APPLICATION reuses both scratch planes.
 * A positive probe anywhere in the input may set every output cell: replicating an
 * existing positive cannot change the final OR. Negative probes still scan every
 * tile, so sparse pixels, black images and alpha-only texels remain exact. The
 * scratch cells are private reduction intermediates, never per-tile image state. */
int gml_gpu_opengl_scene_reduce_rgb(GmlGpuBackend *backend,GlSceneImage *source,
                                    unsigned predicate){
  GlScene *scene=&backend->scene;
  GLuint input;
  uint32_t width,height;
  unsigned cursor=0;
  if(predicate>1u || !source || !source->valid || !source->rendered_origin ||
     !gml_gpu_opengl_rgb_reduce_program(backend)) return 0;
  if(!scene->rgb_reduce_framebuffer){
    backend->gl.GenFramebuffers(1,&scene->rgb_reduce_framebuffer);
    if(!scene->rgb_reduce_framebuffer) return 0;
  }
  backend->gl.Disable(GL_BLEND);
  backend->gl.Disable(GL_SCISSOR_TEST);
  backend->gl.Disable(GL_RASTERIZER_DISCARD);
  backend->gl.Disable(GL_SAMPLE_COVERAGE);
  backend->gl.Disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
  if(scene->supports_sample_mask) backend->gl.Disable(GL_SAMPLE_MASK);
  if(backend->api==GML_GPU_API_OPENGL_CORE)
    backend->gl.PolygonMode(GL_FRONT_AND_BACK,GL_FILL);
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  backend->gl.UseProgram(scene->rgb_reduce_program);
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  backend->gl.Uniform1i(scene->rgb_reduce_u_image,0);
  backend->gl.BindVertexArray(backend->vertex_array);
  input=source->texture;width=source->width;height=source->height;
  do {
    uint32_t next_width=(width+7u)/8u,next_height=(height+7u)/8u;
    unsigned slot=next_width==1u && next_height==1u?2u+predicate:cursor;
    GLint extent[2]={(GLint)width,(GLint)height};
    if(!scene->rgb_reduce_texture[slot]){
      backend->gl.GenTextures(1,&scene->rgb_reduce_texture[slot]);
      if(!scene->rgb_reduce_texture[slot]) return 0;
    }
    backend->gl.ActiveTexture(GL_TEXTURE0);
    backend->gl.BindTexture(GL_TEXTURE_2D,scene->rgb_reduce_texture[slot]);
    if(scene->rgb_reduce_width[slot]<next_width || scene->rgb_reduce_height[slot]<next_height){
      backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)next_width,(GLsizei)next_height,
                             0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
      backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
      backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
      backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
      backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
      if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
      scene->rgb_reduce_width[slot]=next_width;scene->rgb_reduce_height[slot]=next_height;
    }
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,scene->rgb_reduce_framebuffer);
    backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                     scene->rgb_reduce_texture[slot],0);
    if(backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 0;
    backend->gl.BindTexture(GL_TEXTURE_2D,input);
    backend->gl.Uniform2iv(scene->rgb_reduce_u_extent,1,extent);
    backend->gl.Viewport(0,0,(GLsizei)next_width,(GLsizei)next_height);
    backend->gl.DrawArrays(GL_TRIANGLES,0,3);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
    input=scene->rgb_reduce_texture[slot];width=next_width;height=next_height;
    cursor^=1u;
  } while(width!=1u || height!=1u);
  backend->gl.Enable(GL_SCISSOR_TEST);
  return 1;
}

int gml_gpu_opengl_scene_has_rgb(GmlGpuBackend *backend,GmlRenderImageRef image,int *has_rgb){
  GlSceneImage *source;
  GLint active=0;
  GLuint result=0;
  int succeeded;
  if(has_rgb) *has_rgb=0;
  if(!backend || !backend->loaded || !has_rgb ||
     !backend->gl.GenQueries || !backend->gl.DeleteQueries ||
     !backend->gl.BeginQuery || !backend->gl.EndQuery ||
     !backend->gl.GetQueryiv || !backend->gl.GetQueryObjectuiv ||
     (backend->api==GML_GPU_API_OPENGL_CORE && !backend->gl.PolygonMode) ||
     !(source=gml_gpu_opengl_scene_image(backend,image)) ||
     !source->rendered_origin || source->pixel_format!=GML_RENDER_PIXEL_ARGB8888) return 0;
  /* Never take a query owned by the frontend. Both desktop occlusion targets share one slot. */
  backend->gl.GetQueryiv(GL_ANY_SAMPLES_PASSED,GL_CURRENT_QUERY,&active);
  if(active) return 0;
  if(backend->api==GML_GPU_API_OPENGL_CORE){
    backend->gl.GetQueryiv(GL_SAMPLES_PASSED,GL_CURRENT_QUERY,&active);
    if(active) return 0;
  } else {
    backend->gl.GetQueryiv(GL_ANY_SAMPLES_PASSED_CONSERVATIVE,GL_CURRENT_QUERY,&active);
    if(active) return 0;
  }
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  if(!backend->scene.rgb_query){
    backend->gl.GenQueries(1,&backend->scene.rgb_query);
    if(!backend->scene.rgb_query) return 0;
  }
  /* A separate attachment prevents a texture feedback loop even though colour writes are off.
   * No CPU plane, image upload, axis map or full-frame readback is allocated for this query. */
  if(!gml_gpu_opengl_bind_readback_target(backend,source->width,source->height,NULL,0u)) return 0;
  backend->gl.Disable(GL_BLEND);
  backend->gl.Disable(GL_DEPTH_TEST);
  backend->gl.Disable(GL_STENCIL_TEST);
  backend->gl.Disable(GL_CULL_FACE);
  backend->gl.Disable(GL_DITHER);
  backend->gl.Disable(GL_SCISSOR_TEST);
  backend->gl.Disable(GL_RASTERIZER_DISCARD);
  backend->gl.Disable(GL_SAMPLE_COVERAGE);
  backend->gl.Disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
  if(backend->scene.supports_sample_mask) backend->gl.Disable(GL_SAMPLE_MASK);
  if(backend->api==GML_GPU_API_OPENGL_CORE)
    backend->gl.PolygonMode(GL_FRONT_AND_BACK,GL_FILL);
  backend->gl.ColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
  backend->gl.DepthMask(GL_FALSE);
  backend->gl.Viewport(0,0,(GLsizei)source->width,(GLsizei)source->height);
  backend->gl.BindVertexArray(backend->vertex_array);
  backend->gl.UseProgram(backend->program);
  backend->gl.Uniform1i(backend->u_source,0);
  backend->gl.Uniform1i(backend->u_map_x,1);
  backend->gl.Uniform1i(backend->u_map_y,2);
  backend->gl.Uniform1i(backend->u_rgb_query,1);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,source->texture);
  backend->gl.BeginQuery(GL_ANY_SAMPLES_PASSED,backend->scene.rgb_query);
  if(backend->gl.GetError()!=GL_NO_ERROR){ succeeded=0; goto query_done; }
  backend->gl.DrawArrays(GL_TRIANGLES,0,3);
  backend->gl.EndQuery(GL_ANY_SAMPLES_PASSED);
  backend->gl.GetQueryObjectuiv(backend->scene.rgb_query,GL_QUERY_RESULT,&result);
  succeeded=backend->gl.GetError()==GL_NO_ERROR;
query_done:
  backend->gl.Uniform1i(backend->u_rgb_query,0);
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.UseProgram(0);
  backend->gl.BindVertexArray(0);
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,0);
  if(!succeeded) return 0;
  *has_rgb=result!=0u;
  return 1;
}

static int scene_extent_valid(const GlScene *scene,uint32_t width,uint32_t height){
  return width && height && width<=(uint32_t)scene->max_texture_size &&
         height<=(uint32_t)scene->max_texture_size && width<=(uint32_t)INT_MAX &&
         height<=(uint32_t)INT_MAX && (uint64_t)width*height<=SIZE_MAX/4u;
}

static int scene_region_valid(GmlRenderRect region,uint32_t width,uint32_t height){
  return region.x>=0 && region.y>=0 && region.width && region.height &&
         (uint64_t)(uint32_t)region.x+region.width<=width &&
         (uint64_t)(uint32_t)region.y+region.height<=height;
}

static int scene_scratch_ready(GlScene *scene,size_t bytes){
  uint8_t *grown;
  if(bytes<=scene->scratch_capacity) return 1;
  grown=(uint8_t*)realloc(scene->scratch,bytes);
  if(!grown) return 0;
  scene->scratch=grown;
  scene->scratch_capacity=bytes;
  return 1;
}

/* Grow one bounded staging store geometrically. Ordinary map synchronization waits for pending
 * texture transfers before reuse, avoiding a new orphan for every write. Tiny and larger transfers
 * retain the direct path.
 * No buffer or pointer survives context loss; mapping uses the established context entry points. */
static int scene_stage_upload(GmlGpuBackend *backend,size_t bytes,const uint8_t *source,int *staged){
  void *mapped;
  GlScene *scene=&backend->scene;
  *staged=0;
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  if(scene->upload_policy<0 || bytes<GL_SCENE_STAGING_MIN_BYTES ||
     bytes>GL_SCENE_STAGING_MAX_BYTES) return 1;
  if(!scene->upload_buffer){
    backend->gl.GenBuffers(1,&scene->upload_buffer);
    if(!scene->upload_buffer) return 0;
  }
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,scene->upload_buffer);
  if(bytes>scene->upload_capacity){
    size_t capacity=GL_SCENE_STAGING_MIN_BYTES;
    while(capacity<bytes) capacity*=2u;
    backend->gl.BufferData(GL_PIXEL_UNPACK_BUFFER,(GLsizeiptr)capacity,NULL,GL_STREAM_DRAW);
    if(backend->gl.GetError()!=GL_NO_ERROR) goto refused;
    scene->upload_capacity=capacity;
  }
  mapped=backend->gl.MapBufferRange(GL_PIXEL_UNPACK_BUFFER,0,(GLsizeiptr)bytes,
                                    GL_MAP_WRITE_BIT);
  if(!mapped) goto refused;
  memcpy(mapped,source,bytes);
  if(!backend->gl.UnmapBuffer(GL_PIXEL_UNPACK_BUFFER)) goto refused;
  if(backend->gl.GetError()!=GL_NO_ERROR) goto refused;
  *staged=1;
  return 1;
refused:
  scene->upload_capacity=0;
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  (void)backend->gl.GetError();
  return 0;
}

static uint64_t upload_median(uint64_t a,uint64_t b,uint64_t c){
  uint64_t swap;
  if(a>b){swap=a;a=b;b=swap;}
  if(b>c){swap=b;b=c;c=swap;}
  if(a>b) b=a;
  return b;
}

/* Drivers can implement the same PBO submission with very different calling-thread costs. Compare the
 * existing two paths once, using a neutral image at the established staging lower bound. This
 * measures call cost, not GPU completion; no draw, readback or completion barrier is introduced. */
void gml_gpu_opengl_scene_prepare_uploads(GmlGpuBackend *backend){
  enum {SIDE=128, BYTES=GL_SCENE_STAGING_MIN_BYTES};
  _Static_assert(SIDE*SIDE*4==BYTES,"neutral transfer uses the staging lower bound");
  GlScene *scene=&backend->scene;
  GLuint texture=0;
  GLint active_texture=GL_TEXTURE0,previous_texture=0,previous_buffer=0;
  GLint alignment=4,row_length=0,skip_rows=0,skip_pixels=0;
  uint64_t elapsed[2][3]={{0}},last_sample=0;
  int measured=1;
  if(scene->upload_policy) return;
  scene->upload_policy=1;
  if(!scene->upload_clock || !scene->upload_clock(scene->upload_clock_userdata) ||
     scene->max_texture_size<SIDE || !scene_scratch_ready(scene,BYTES)) return;
  for(size_t index=0;index<BYTES;index++)
    scene->scratch[index]=(uint8_t)(index^(index>>8));
  backend->gl.GetIntegerv(GL_ACTIVE_TEXTURE,&active_texture);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  /* Preserve the caller's texture unit, binding and pixel-unpack state. */
  backend->gl.GetIntegerv(GL_TEXTURE_BINDING_2D,&previous_texture);
  backend->gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&previous_buffer);
  backend->gl.GetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);
  backend->gl.GetIntegerv(GL_UNPACK_ROW_LENGTH,&row_length);
  backend->gl.GetIntegerv(GL_UNPACK_SKIP_ROWS,&skip_rows);
  backend->gl.GetIntegerv(GL_UNPACK_SKIP_PIXELS,&skip_pixels);
  backend->gl.GenTextures(1,&texture);
  if(!texture){measured=0;goto restore;}
  backend->gl.BindTexture(GL_TEXTURE_2D,texture);
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,1);
  backend->gl.PixelStorei(GL_UNPACK_ROW_LENGTH,0);
  backend->gl.PixelStorei(GL_UNPACK_SKIP_ROWS,0);
  backend->gl.PixelStorei(GL_UNPACK_SKIP_PIXELS,0);
  backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,SIDE,SIDE,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  if(backend->gl.GetError()!=GL_NO_ERROR){measured=0;goto restore;}
  /* Warm each path, then alternate the order of three matched pairs. No content supplies setup. */
  for(int trial=-1;trial<3 && measured;trial++) for(int order=0;order<2;order++){
    int staged=0,path=order^(trial>0?(trial&1):0);
    uint64_t started=trial<0?0:scene->upload_clock(scene->upload_clock_userdata);
    if(path){
      if(!scene_stage_upload(backend,BYTES,scene->scratch,&staged) || !staged){measured=0;break;}
    } else backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
    backend->gl.TexSubImage2D(GL_TEXTURE_2D,0,0,0,SIDE,SIDE,GL_RGBA,GL_UNSIGNED_BYTE,
                              staged?NULL:scene->scratch);
    if(backend->gl.GetError()!=GL_NO_ERROR){measured=0;break;}
    if(trial>=0){
      uint64_t ended=scene->upload_clock(scene->upload_clock_userdata);
      if(!started || started<last_sample || ended<=started){measured=0;break;}
      elapsed[path][trial]=ended-started;
      last_sample=ended;
    }
  }
restore:
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,(GLuint)previous_buffer);
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,alignment);
  backend->gl.PixelStorei(GL_UNPACK_ROW_LENGTH,row_length);
  backend->gl.PixelStorei(GL_UNPACK_SKIP_ROWS,skip_rows);
  backend->gl.PixelStorei(GL_UNPACK_SKIP_PIXELS,skip_pixels);
  backend->gl.BindTexture(GL_TEXTURE_2D,(GLuint)previous_texture);
  backend->gl.ActiveTexture((GLenum)active_texture);
  if(texture) backend->gl.DeleteTextures(1,&texture);
  if(backend->gl.GetError()!=GL_NO_ERROR) measured=0;
  if(measured){
    scene->upload_client_ns=upload_median(elapsed[0][0],elapsed[0][1],elapsed[0][2]);
    scene->upload_staged_ns=upload_median(elapsed[1][0],elapsed[1][1],elapsed[1][2]);
    if(scene->upload_client_ns<scene->upload_staged_ns) scene->upload_policy=-1;
  }
}

/* A partial atlas page is completed in bands of about this many bytes, one band per offered
 * frame, so a page that keeps growing never needs one whole-page transfer in a single frame. */
enum { GL_SCENE_COMPLETION_BAND_BYTES=1u<<20 };

static void scene_completion_drop(GmlGpuBackend *backend,GlSceneImage *image){
  if(image->completion_texture) backend->gl.DeleteTextures(1,&image->completion_texture);
  image->completion_texture=0;
  image->completion_rows=0;
}

/* Defines up to row_limit further rows of the complete page from client bytes. The storage is
 * fresh and unreferenced by draws, so the write does not wait on queued readers; a staged first
 * partial definition is avoided as in scene_upload_image. Returns the rows defined, and
 * publishes the complete storage in place of the partial texture after its last row. */
static uint32_t scene_completion_advance(GmlGpuBackend *backend,GlSceneImage *image,
                                         const GmlRenderImageUpload *full,uint32_t row_limit){
  size_t row_bytes=(size_t)full->storage_width*4u;
  uint32_t rows;
  if(full->stride_bytes!=row_bytes || !row_limit || image->rendered_origin ||
     full->image.resource_class!=GML_RENDER_RESOURCE_ATLAS || image->target ||
     image->framebuffer || image->content_source_texture ||
     image->content_source_framebuffer) return 0;
  if(!image->completion_texture){
    backend->gl.GenTextures(1,&image->completion_texture);
    if(!image->completion_texture) return 0;
    image->completion_rows=0;
    backend->gl.ActiveTexture(GL_TEXTURE0);
    backend->gl.BindTexture(GL_TEXTURE_2D,image->completion_texture);
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)full->storage_width,
                           (GLsizei)full->storage_height,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  } else {
    backend->gl.ActiveTexture(GL_TEXTURE0);
    backend->gl.BindTexture(GL_TEXTURE_2D,image->completion_texture);
  }
  rows=full->storage_height-image->completion_rows;
  if(rows>row_limit) rows=row_limit;
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,1);
  backend->gl.PixelStorei(GL_UNPACK_ROW_LENGTH,0);
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  backend->gl.TexSubImage2D(GL_TEXTURE_2D,0,0,(GLint)image->completion_rows,
                            (GLsizei)full->storage_width,(GLsizei)rows,GL_RGBA,GL_UNSIGNED_BYTE,
                            (const uint8_t*)full->bytes+(size_t)image->completion_rows*row_bytes);
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,4);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(backend->gl.GetError()!=GL_NO_ERROR){ scene_completion_drop(backend,image); return 0; }
  image->completion_rows+=rows;
  if(image->completion_rows<full->storage_height) return rows;
  backend->gl.DeleteTextures(1,&image->texture);
  image->texture=image->completion_texture;
  image->completion_texture=0;
  image->completion_rows=0;
  image->source_partial=0;
  image->source_region=(GmlRenderRect){0,0,full->storage_width,full->storage_height};
  image->content_source_version=0;
  image->alpha_opaque=0;
  image->alpha_bounds_known=0;
  return rows;
}

void gml_gpu_opengl_scene_begin_frame(GmlGpuBackend *backend,uint64_t *completion_uploads,
                                      uint64_t *completion_bytes){
  if(completion_uploads) *completion_uploads=0;
  if(completion_bytes) *completion_bytes=0;
  if(!backend) return;
  backend->scene.completion_budget=1;
  if(completion_uploads) *completion_uploads=backend->scene.completion_uploads;
  if(completion_bytes) *completion_bytes=backend->scene.completion_bytes;
  backend->scene.completion_uploads=backend->scene.completion_bytes=0;
}

static int scene_upload_image(GmlGpuBackend *backend,const GmlRenderImageUpload *transfer,
                                int *did_upload,int source_region){
  GlScene *scene;
  GlSceneImage *slot;
  size_t row_bytes,bytes;
  const uint8_t *source,*upload_bytes;
  int first,resize,rendered_origin,staged=0,initial_client,initial_partial;
  if(did_upload) *did_upload=0;
  if(!backend || !transfer || !transfer->bytes || !backend->loaded) return 0;
  scene=&backend->scene;
  if(!scene->available || !scene_extent_valid(scene,transfer->storage_width,transfer->storage_height) ||
     !scene_region_valid(transfer->region,transfer->storage_width,transfer->storage_height) ||
     (transfer->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
      transfer->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
      transfer->pixel_format!=GML_RENDER_PIXEL_RGBA8888)) return 0;
  row_bytes=(size_t)transfer->region.width*4u;
  if(transfer->stride_bytes<row_bytes ||
     (uint64_t)(transfer->region.height-1u)*transfer->stride_bytes+row_bytes>
       transfer->byte_capacity) return 0;
  bytes=row_bytes*transfer->region.height;
  slot=scene_slot(backend,transfer->image);
  if(!slot) return 0;
  /* Mutable render targets use the FBO's high-edge origin, even when CPU bytes arrive first.
   * Authored atlas and sprite uploads retain the established low-edge texture convention. */
  rendered_origin=transfer->image.resource_class==GML_RENDER_RESOURCE_SCREEN ||
                  transfer->image.resource_class==GML_RENDER_RESOURCE_APPLICATION ||
                  transfer->image.resource_class==GML_RENDER_RESOURCE_SURFACE ||
                  transfer->image.resource_class==GML_RENDER_RESOURCE_SCRATCH ||
                  transfer->image.resource_class==GML_RENDER_RESOURCE_SHADER_SAMPLER;
  /* Atlas refs are cacheable by version: packaged pages stay at version one, while runtime
   * font pages advance on every mutation and get a fresh incarnation on reuse. Generic mutable
   * resources cannot infer same-version residency from their image reference. */
  if(transfer->image.resource_class==GML_RENDER_RESOURCE_ATLAS &&
     slot->texture && slot->valid && !slot->source_partial &&
     scene_same_version(slot->ref,transfer->image) &&
     slot->width==transfer->storage_width && slot->height==transfer->storage_height &&
     slot->pixel_format==transfer->pixel_format &&
     slot->rendered_origin==rendered_origin &&
     transfer->region.x==0 && transfer->region.y==0 &&
     transfer->region.width==transfer->storage_width &&
     transfer->region.height==transfer->storage_height){
    slot->last_used=++scene->use_clock;
    return 1;
  }
  if(slot->texture && slot->rendered_origin!=rendered_origin &&
     (transfer->region.x || transfer->region.y ||
      transfer->region.width!=transfer->storage_width ||
      transfer->region.height!=transfer->storage_height)) return 0;
  slot->content_source_version=0;
  slot->alpha_opaque=0;
  slot->alpha_bounds_known=0;
  slot->opaque_white_known=0;
  first=!slot->texture;
  resize=slot->width!=transfer->storage_width || slot->height!=transfer->storage_height;
  if(!source_region && (first || resize || slot->source_partial ||
       !scene_same_version(slot->ref,transfer->image)) &&
     (transfer->region.x || transfer->region.y ||
      transfer->region.width!=transfer->storage_width ||
      transfer->region.height!=transfer->storage_height)) return 0;
  gml_gpu_opengl_scene_prepare_uploads(backend);
  source=(const uint8_t*)transfer->bytes;
  upload_bytes=source;
  /* Already contiguous low-origin RGBA needs no packing. Client uploads consume it within this
   * call; staging copies it into the owned buffer before submission, retaining no caller pointer. */
  if(rendered_origin || transfer->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
     transfer->stride_bytes!=row_bytes){
    if(!scene_scratch_ready(scene,bytes)) return 0;
    for(uint32_t row=0;row<transfer->region.height;row++){
      const uint8_t *input=source+(size_t)row*transfer->stride_bytes;
      uint32_t destination_row=rendered_origin?transfer->region.height-1u-row:row;
      uint8_t *output=scene->scratch+(size_t)destination_row*row_bytes;
      if(transfer->pixel_format==GML_RENDER_PIXEL_RGBA8888){
        memcpy(output,input,row_bytes);
      } else for(uint32_t column=0;column<transfer->region.width;column++){
        uint32_t pixel;
        memcpy(&pixel,input+(size_t)column*4u,sizeof pixel);
        output[(size_t)column*4u+0]=(uint8_t)(pixel>>16);
        output[(size_t)column*4u+1]=(uint8_t)(pixel>>8);
        output[(size_t)column*4u+2]=(uint8_t)pixel;
        output[(size_t)column*4u+3]=transfer->pixel_format==GML_RENDER_PIXEL_XRGB8888
                                    ?255u:(uint8_t)(pixel>>24);
      }
    }
    upload_bytes=scene->scratch;
  }
  /* Region preparation supplies the complete defined atlas union. A distinct
   * native object lets queued readers retain the old storage during growth.
   * Mutable targets and complete-source consumers keep their current ownership. */
  if(source_region && !first && !resize && slot->source_partial &&
     transfer->image.resource_class==GML_RENDER_RESOURCE_ATLAS &&
     !slot->target && !slot->framebuffer && !slot->rendered_origin &&
     !slot->content_source_texture && !slot->content_source_framebuffer &&
     slot->pixel_format==transfer->pixel_format && scene_same_version(slot->ref,transfer->image)){
    GLuint fresh=0,old=slot->texture;
    backend->gl.GenTextures(1,&fresh);
    if(!fresh) return 0;
    backend->gl.DeleteTextures(1,&old);
    slot->texture=fresh;
    first=1;
  }
  if(first && !slot->texture){
    backend->gl.GenTextures(1,&slot->texture);
    if(!slot->texture) return 0;
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,slot->texture);
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,1);
  backend->gl.PixelStorei(GL_UNPACK_ROW_LENGTH,0);
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  /* Complete client transfers can initialize storage directly. Partial source preparation
   * first defines its region with client bytes: staging into new partial storage can lose
   * that region on tiled devices. Later complete updates retain the selected upload policy. */
  initial_partial=(first || resize) &&
    (transfer->region.x || transfer->region.y ||
     transfer->region.width!=transfer->storage_width ||
     transfer->region.height!=transfer->storage_height);
  initial_client=(first || resize) && scene->upload_policy<0 &&
    !transfer->region.x && !transfer->region.y &&
    transfer->region.width==transfer->storage_width &&
    transfer->region.height==transfer->storage_height;
  if(first || resize){
    scene_delete_raw_depth(backend,slot);
    if(slot->depth_renderbuffer){
      backend->gl.BindFramebuffer(GL_FRAMEBUFFER,slot->framebuffer);
      backend->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,0);
      backend->gl.DeleteRenderbuffers(1,&slot->depth_renderbuffer);
      slot->depth_renderbuffer=0;
    }
    slot->framebuffer_complete=0;
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)transfer->storage_width,
                           (GLsizei)transfer->storage_height,0,GL_RGBA,GL_UNSIGNED_BYTE,
                           initial_client?upload_bytes:NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  }
  if(!initial_client && !initial_partial &&
     !scene_stage_upload(backend,bytes,upload_bytes,&staged)){
    backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,4);
    backend->gl.BindTexture(GL_TEXTURE_2D,0);
    slot->valid=0;
    slot->alpha_opaque=0;
    slot->alpha_bounds_known=0;
    return 0;
  }
  if(!initial_client) backend->gl.TexSubImage2D(GL_TEXTURE_2D,0,transfer->region.x,
                            rendered_origin?(GLint)(transfer->storage_height-
                              (uint32_t)transfer->region.y-transfer->region.height)
                                           :transfer->region.y,
                            (GLsizei)transfer->region.width,(GLsizei)transfer->region.height,
                            GL_RGBA,GL_UNSIGNED_BYTE,staged?NULL:upload_bytes);
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,4);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(backend->gl.GetError()!=GL_NO_ERROR){ slot->valid=0; return 0; }
  slot->ref=transfer->image;
  slot->context_epoch=scene->context_epoch;
  slot->width=transfer->storage_width;
  slot->height=transfer->storage_height;
  slot->pixel_format=transfer->pixel_format;
  slot->valid=1;
  slot->rendered_origin=rendered_origin;
  if(!source_region){
    scene_completion_drop(backend,slot);
    slot->source_partial=0;
    slot->source_region=(GmlRenderRect){0,0,slot->width,slot->height};
    slot->source_upload_bytes=(uint64_t)slot->width*slot->height*4u;
  }
  if(transfer->image.resource_class==GML_RENDER_RESOURCE_ATLAS &&
     (uint64_t)slot->width*slot->height<=GL_SCENE_WHITE_TEXELS &&
     transfer->region.x==0 && transfer->region.y==0 &&
     transfer->region.width==slot->width && transfer->region.height==slot->height){
    memset(slot->opaque_white,0,sizeof slot->opaque_white);
    for(uint32_t pixel=0;pixel<slot->width*slot->height;pixel++){
      const uint8_t *p=upload_bytes+(size_t)pixel*4u;
      if(p[0]==255u && p[1]==255u && p[2]==255u && p[3]==255u)
        slot->opaque_white[pixel/8u]|=(uint8_t)(1u<<(pixel%8u));
    }
    slot->opaque_white_known=1;
  }
  slot->last_used=++scene->use_clock;
  if(did_upload) *did_upload=1;
  return 1;
}

int gml_gpu_opengl_scene_upload(GmlGpuBackend *backend,const GmlRenderImageUpload *transfer,
                                int *did_upload){
  return scene_upload_image(backend,transfer,did_upload,0);
}

static int scene_contains_region(GmlRenderRect outer,GmlRenderRect inner){
  return outer.x>=0 && outer.y>=0 && inner.x>=outer.x && inner.y>=outer.y &&
    (uint64_t)(uint32_t)inner.x+inner.width<=(uint64_t)(uint32_t)outer.x+outer.width &&
    (uint64_t)(uint32_t)inner.y+inner.height<=(uint64_t)(uint32_t)outer.y+outer.height;
}

int gml_gpu_opengl_scene_source_region_ready(const GlSceneImage *image,
                                             const GmlRenderDrawOp *operation){
  GmlRenderRect required;
  if(!image) return 0;
  return !image->source_partial ||
    (gml_render_operation_source_region(operation,image->width,image->height,&required) &&
     scene_contains_region(image->source_region,required));
}

int gml_gpu_opengl_scene_upload_source_region(GmlGpuBackend *backend,
  const GmlRenderImageUpload *full,GmlRenderRect required,uint64_t *uploaded_bytes){
  GlSceneImage *image;
  GmlRenderImageUpload transfer;
  GmlRenderRect selected=required;
  uint64_t previous=0,total,bytes;
  int did_upload=0,current;
  if(uploaded_bytes) *uploaded_bytes=0;
  if(!backend || !backend->loaded || !full || !full->bytes ||
     full->image.resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     full->pixel_format!=GML_RENDER_PIXEL_RGBA8888 || full->region.x || full->region.y ||
     full->region.width!=full->storage_width || full->region.height!=full->storage_height ||
     !scene_extent_valid(&backend->scene,full->storage_width,full->storage_height) ||
     !scene_region_valid(required,full->storage_width,full->storage_height) ||
     (uint64_t)full->stride_bytes<(uint64_t)full->storage_width*4u ||
     full->byte_capacity<(uint64_t)full->storage_width*4u ||
     (uint64_t)(full->storage_height-1u)>
       (full->byte_capacity-(uint64_t)full->storage_width*4u)/full->stride_bytes) return 0;
  image=scene_slot(backend,full->image);
  if(!image || image->target) return 0;
  current=image->texture && image->valid &&
    image->context_epoch==backend->scene.context_epoch &&
    scene_same_version(image->ref,full->image) &&
    image->width==full->storage_width && image->height==full->storage_height &&
    image->pixel_format==full->pixel_format && !image->rendered_origin;
  if(current){
    if(!image->source_partial){
      scene_completion_drop(backend,image);
      image->last_used=++backend->scene.use_clock;return 1;
    }
    if(scene_contains_region(image->source_region,required)){
      /* A resident page spends the frame's band on its complete storage. */
      if(backend->scene.completion_budget){
        uint32_t band=(uint32_t)(GL_SCENE_COMPLETION_BAND_BYTES/((size_t)full->storage_width*4u));
        uint32_t rows=scene_completion_advance(backend,image,full,band?band:1u);
        if(rows){
          backend->scene.completion_budget=0;
          backend->scene.completion_uploads++;
          backend->scene.completion_bytes+=(uint64_t)rows*full->storage_width*4u;
        }
      }
      image->last_used=++backend->scene.use_clock;return 1;
    }
    GmlRenderRect old=image->source_region;
    uint32_t x=(uint32_t)(old.x<required.x?old.x:required.x);
    uint32_t y=(uint32_t)(old.y<required.y?old.y:required.y);
    uint32_t end_x=(uint32_t)old.x+old.width;
    uint32_t end_y=(uint32_t)old.y+old.height;
    if((uint32_t)required.x+required.width>end_x) end_x=(uint32_t)required.x+required.width;
    if((uint32_t)required.y+required.height>end_y) end_y=(uint32_t)required.y+required.height;
    selected=(GmlRenderRect){(int32_t)x,(int32_t)y,end_x-x,end_y-y};
    previous=image->source_upload_bytes;
  } else if(image->completion_texture) scene_completion_drop(backend,image);
  total=(uint64_t)full->storage_width*full->storage_height*4u;
  bytes=(uint64_t)selected.width*selected.height*4u;
  /* Growth finishes the complete storage instead when its remaining rows cost no more than the
   * grown region, or when the region would otherwise become a whole-image transfer. */
  if(current && image->completion_texture){
    uint64_t remaining=(uint64_t)(full->storage_height-image->completion_rows)*
                       full->storage_width*4u;
    if(remaining<=bytes || bytes>=total || previous>=total-bytes){
      uint32_t rows=scene_completion_advance(backend,image,full,
                                             full->storage_height-image->completion_rows);
      if(rows && !image->source_partial){
        image->source_upload_bytes=previous+remaining;
        image->last_used=++backend->scene.use_clock;
        if(uploaded_bytes) *uploaded_bytes=remaining;
        return 1;
      }
    }
  }
  /* Stop growing the bounding region before cumulative partial copies cost a whole image.
   * This amortisation bound depends only on the resource, not on content or device identity. */
  if(bytes>=total || previous>=total-bytes){
    selected=full->region;bytes=total;
  }
  transfer=*full;transfer.region=selected;
  size_t offset=(size_t)(uint32_t)selected.y*full->stride_bytes+(size_t)(uint32_t)selected.x*4u;
  transfer.bytes=(const uint8_t*)full->bytes+offset;
  transfer.byte_capacity=full->byte_capacity-offset;
  if(!scene_upload_image(backend,&transfer,&did_upload,1)) return 0;
  image->source_partial=selected.x || selected.y ||
    selected.width!=full->storage_width || selected.height!=full->storage_height;
  image->source_region=selected;
  image->source_upload_bytes=previous+bytes;
  /* Keep the existing small-atlas white proof for the complete logical source. No input
   * pointer is retained; every source texel inspected here belongs to this call. */
  if((uint64_t)image->width*image->height<=GL_SCENE_WHITE_TEXELS){
    memset(image->opaque_white,0,sizeof image->opaque_white);
    for(uint32_t row=0;row<image->height;row++) for(uint32_t column=0;column<image->width;column++){
      const uint8_t *pixel=(const uint8_t*)full->bytes+(size_t)row*full->stride_bytes+(size_t)column*4u;
      uint32_t index=row*image->width+column;
      if(pixel[0]==255u && pixel[1]==255u && pixel[2]==255u && pixel[3]==255u)
        image->opaque_white[index/8u]|=(uint8_t)(1u<<(index%8u));
    }
    image->opaque_white_known=1;
  }
  if(uploaded_bytes && did_upload) *uploaded_bytes=bytes;
  return 1;
}

GlSceneImage *gml_gpu_opengl_scene_target(GmlGpuBackend *backend,const GmlRenderTargetRef *target){
  GlScene *scene;
  GlSceneImage *slot;
  int first,resize;
  if(!backend || !target || !backend->loaded) return NULL;
  scene=&backend->scene;
  if(!scene->available || !scene_extent_valid(scene,target->width,target->height) ||
     (target->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
      target->pixel_format!=GML_RENDER_PIXEL_ARGB8888)) return NULL;
  slot=scene_slot(backend,target->image);
  if(!slot) return NULL;
  if(slot->source_partial) return NULL;
  if(!slot->valid) slot->alpha_bounds_known=0;
  /* A target may be written without another CPU upload. Never retain a source
   * certificate across that ownership boundary, even before the first write. */
  slot->opaque_white_known=0;
  first=!slot->texture;
  resize=slot->width!=target->width || slot->height!=target->height;
  if(first){
    backend->gl.GenTextures(1,&slot->texture);
    if(!slot->texture) return NULL;
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,slot->texture);
  if(first || resize){
    scene_delete_raw_depth(backend,slot);
    if(slot->depth_renderbuffer){
      backend->gl.BindFramebuffer(GL_FRAMEBUFFER,slot->framebuffer);
      backend->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,0);
      backend->gl.DeleteRenderbuffers(1,&slot->depth_renderbuffer);
      slot->depth_renderbuffer=0;
    }
    slot->framebuffer_complete=0;
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)target->width,
                           (GLsizei)target->height,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    slot->valid=0;
    slot->alpha_opaque=0;
    slot->alpha_bounds_known=0;
  }
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(!slot->framebuffer){
    backend->gl.GenFramebuffers(1,&slot->framebuffer);
    if(!slot->framebuffer){
      slot->valid=slot->alpha_opaque=slot->alpha_bounds_known=0;return NULL;
    }
  }
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,slot->framebuffer);
  /* This framebuffer belongs only to this image. Reattaching unchanged storage
   * can end a tiled device's render pass between ordinary draws. */
  if(!slot->framebuffer_complete){
    backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                     slot->texture,0);
    if(backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE ||
       backend->gl.GetError()!=GL_NO_ERROR){
      slot->valid=slot->alpha_opaque=slot->alpha_bounds_known=0;return NULL;
    }
    slot->framebuffer_complete=1;
    backend->target_attachments++;
  }else{
    if(backend->gl.GetError()!=GL_NO_ERROR){
      slot->framebuffer_complete=slot->valid=slot->alpha_opaque=slot->alpha_bounds_known=0;
      return NULL;
    }
    backend->target_reuses++;
  }
  slot->ref=target->image;
  slot->context_epoch=scene->context_epoch;
  slot->width=target->width;
  slot->height=target->height;
  slot->pixel_format=target->pixel_format;
  slot->target=1;
  slot->last_used=++scene->use_clock;
  return slot;
}

/* Content programs see the terminal plan's low-edge coordinates, not merely upright samples.
 * A versioned device-only view preserves both procedural UV and nearest boundary selection;
 * ordinary render targets keep their established high-edge storage. */
GLuint gml_gpu_opengl_scene_content_source(GmlGpuBackend *backend,GlSceneImage *source){
  GmlRenderTargetRef source_target={0};
  if(!backend || !source || !source->valid || !source->texture ||
     !source->rendered_origin ||
     !backend->gl.BlitFramebuffer) return 0;
  if(source->content_source_texture && source->content_source_version==source->ref.content_version &&
     source->content_source_width==source->width && source->content_source_height==source->height)
    return source->content_source_texture;
  source->content_source_version=0;
  source_target.image=source->ref;source_target.storage_version=1u;
  source_target.width=source->width;source_target.height=source->height;
  source_target.pixel_format=source->pixel_format;
  source_target.region=(GmlRenderRect){0,0,source->width,source->height};
  if(gml_gpu_opengl_scene_target(backend,&source_target)!=source) return 0;
  if(!source->content_source_texture){
    backend->gl.GenTextures(1,&source->content_source_texture);
    if(!source->content_source_texture) return 0;
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,source->content_source_texture);
  if(source->content_source_width!=source->width || source->content_source_height!=source->height){
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)source->width,
                           (GLsizei)source->height,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
    source->content_source_width=source->width;
    source->content_source_height=source->height;
  }
  if(!source->content_source_framebuffer){
    backend->gl.GenFramebuffers(1,&source->content_source_framebuffer);
    if(!source->content_source_framebuffer) return 0;
  }
  backend->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,source->content_source_framebuffer);
  backend->gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                   source->content_source_texture,0);
  if(backend->gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
    return 0;
  backend->gl.BindFramebuffer(GL_READ_FRAMEBUFFER,source->framebuffer);
  backend->gl.Scissor(0,0,(GLsizei)source->width,(GLsizei)source->height);
  backend->gl.BlitFramebuffer(0,(GLint)source->height,(GLint)source->width,0,
                               0,0,(GLint)source->width,(GLint)source->height,
                               GL_COLOR_BUFFER_BIT,GL_NEAREST);
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  source->content_source_version=source->ref.content_version;
  return source->content_source_texture;
}

typedef struct LinearRows {
  uint8_t *destination;
  const uint8_t *source;
  size_t stride,row_bytes;
  uint32_t first,end;
} LinearRows;

static void *linear_copy_rows(void *argument){
  const LinearRows *rows=(const LinearRows*)argument;
  for(uint32_t row=rows->first;row<rows->end;row++)
    memcpy(rows->destination+(size_t)row*rows->stride,rows->source+(size_t)row*rows->row_bytes,
           rows->row_bytes);
  return NULL;
}

/* Reading uncached device memory is bound by each core's latency, not by the bus: a large copy
 * is shared among up to four cores by bands of rows. */
static void linear_copy(uint8_t *destination,size_t stride,const uint8_t *source,
                        size_t row_bytes,uint32_t height){
  enum { MAX_PARTS=4 };
  LinearRows parts[MAX_PARTS];
  gml_thread_t threads[MAX_PARTS];
  int started[MAX_PARTS]={0};
  int count=gml_ncpu();
  if(count>MAX_PARTS) count=MAX_PARTS;
  if(count<1 || (size_t)height*row_bytes<GL_SCENE_LINEAR_READ_MIN_BYTES || height<(uint32_t)count)
    count=1;
  for(int part=0;part<count;part++)
    parts[part]=(LinearRows){destination,source,stride,row_bytes,
                             (uint32_t)((uint64_t)height*part/count),
                             (uint32_t)((uint64_t)height*(part+1)/count)};
  for(int part=1;part<count;part++)
    started[part]=gml_thread_create(&threads[part],linear_copy_rows,&parts[part])==0;
  linear_copy_rows(&parts[0]);
  for(int part=1;part<count;part++){
    if(started[part]) gml_thread_join(threads[part]);
    else linear_copy_rows(&parts[part]);
  }
}

/* A tiled device makes the host untile a direct pixel read from uncached memory, the most
 * expensive part of a large read. Here the device writes the region's words, already packed in
 * the host format and top-down, into a linear buffer by transform feedback; the host only copies
 * rows out of the mapped buffer. Returns 0 when the path is unavailable or refused, leaving the
 * direct read to serve and to rewrite every destination row. */
static int scene_readback_linear(GmlGpuBackend *backend,GlSceneImage *image,
                                  const GmlRenderImageReadback *transfer){
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
  GlScene *scene=&backend->scene;
  const uint32_t width=transfer->region.width,height=transfer->region.height;
  size_t vertices=((size_t)width*height+3u)/4u,bytes=vertices*16u;
  GLint region[4],format[2];
  const uint8_t *mapped;
  int success;
  if(scene->linear_failed ||
     (size_t)width*height*4u<GL_SCENE_LINEAR_READ_MIN_BYTES || vertices>(size_t)INT32_MAX ||
     !backend->gl.MapBufferRange || !backend->gl.UnmapBuffer ||
     backend->gl.GetError()!=GL_NO_ERROR || !gml_gpu_opengl_linear_read_program(backend))
    return 0;
  if(!scene->linear_buffer) backend->gl.GenBuffers(1,&scene->linear_buffer);
  if(!scene->linear_buffer){ scene->linear_failed=1;return 0; }
  backend->gl.BindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,scene->linear_buffer);
  if(scene->linear_capacity<bytes){
    backend->gl.BufferData(GL_TRANSFORM_FEEDBACK_BUFFER,(GLsizeiptr)bytes,NULL,GL_STREAM_READ);
    scene->linear_capacity=backend->gl.GetError()==GL_NO_ERROR?bytes:0u;
  }
  success=scene->linear_capacity>=bytes;
  if(success){
    region[0]=transfer->region.x;region[1]=transfer->region.y;
    region[2]=(GLint)width;region[3]=(GLint)height;
    format[0]=(GLint)image->height;
    format[1]=transfer->pixel_format==GML_RENDER_PIXEL_RGBA8888?2:
              transfer->pixel_format==GML_RENDER_PIXEL_XRGB8888?1:0;
    backend->gl.Enable(GL_RASTERIZER_DISCARD);
    backend->gl.UseProgram(scene->linear_program);
    backend->gl.Uniform1i(scene->linear_u_image,0);
    backend->gl.Uniform4iv(scene->linear_u_region,1,region);
    backend->gl.Uniform2iv(scene->linear_u_format,1,format);
    backend->gl.ActiveTexture(GL_TEXTURE0);
    backend->gl.BindTexture(GL_TEXTURE_2D,image->texture);
    backend->gl.BindVertexArray(backend->vertex_array);
    backend->gl.BindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,scene->linear_buffer);
    backend->gl.BeginTransformFeedback(GL_POINTS);
    backend->gl.DrawArrays(GL_POINTS,0,(GLsizei)vertices);
    backend->gl.EndTransformFeedback();
    backend->gl.Disable(GL_RASTERIZER_DISCARD);
    success=backend->gl.GetError()==GL_NO_ERROR;
  }
  mapped=success?(const uint8_t*)backend->gl.MapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,
                                                             (GLsizeiptr)bytes,GL_MAP_READ_BIT):NULL;
  if(mapped){
    linear_copy((uint8_t*)transfer->bytes,transfer->stride_bytes,mapped,(size_t)width*4u,height);
    /* A buffer lost while mapped leaves the destination for the direct read to rewrite. */
    success=backend->gl.UnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER)==GL_TRUE;
  } else success=0;
  backend->gl.BindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);
  backend->gl.BindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,0);
  backend->gl.UseProgram(0);
  backend->gl.BindVertexArray(0);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(backend->gl.GetError()!=GL_NO_ERROR) success=0;
  if(!success) scene->linear_failed=1;
  return success;
#else
  (void)backend;(void)image;(void)transfer;
  return 0;
#endif
}

/* The established row/channel packing body serves both full and bounded reads. */
static int scene_readback_pixels(GmlGpuBackend *backend,GlSceneImage *image,
                                  const GmlRenderImageReadback *transfer,
                                  GmlRenderImageReadbackResult *result){
  GlScene *scene=&backend->scene;
  size_t row_bytes=(size_t)transfer->region.width*4u;
  size_t bytes=row_bytes*transfer->region.height;
  if(scene_readback_linear(backend,image,transfer)){
    backend->gl.BindFramebuffer(GL_FRAMEBUFFER,image->framebuffer);
    result->pixel_reads++;
    result->pixel_bytes+=bytes;
    return 1;
  }
  if(!scene_scratch_ready(scene,bytes)) return 0;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,image->framebuffer);
  backend->gl.PixelStorei(GL_PACK_ALIGNMENT,1);
  result->pixel_reads++;
  result->pixel_bytes+=bytes;
  backend->gl.ReadPixels(transfer->region.x,
    (GLint)(image->height-(uint32_t)transfer->region.y-transfer->region.height),
    (GLsizei)transfer->region.width,(GLsizei)transfer->region.height,
    GL_RGBA,GL_UNSIGNED_BYTE,scene->scratch);
  backend->gl.PixelStorei(GL_PACK_ALIGNMENT,4);
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  /* The destination can be byte-aligned. Snapshot the validated descriptor rather than
   * reloading possibly byte-aliased fields for every pixel; the packing rule is unchanged. */
  const uint32_t width=transfer->region.width,height=transfer->region.height;
  const uint32_t format=transfer->pixel_format,stride=transfer->stride_bytes;
  const uint32_t opaque_alpha=format==GML_RENDER_PIXEL_XRGB8888?0xFF000000u:0u;
  uint8_t *destination=(uint8_t*)transfer->bytes;
  for(uint32_t row=0;row<height;row++){
    const uint8_t *input=scene->scratch+(size_t)(height-1u-row)*row_bytes;
    uint8_t *output=destination+(size_t)row*stride;
    if(format==GML_RENDER_PIXEL_RGBA8888){
      memcpy(output,input,row_bytes);
    } else for(uint32_t column=0;column<width;column++){
      const uint8_t *pixel=input+(size_t)column*4u;
      uint32_t packed=((uint32_t)pixel[3]<<24)|((uint32_t)pixel[0]<<16)|
                        ((uint32_t)pixel[1]<<8)|pixel[2]|opaque_alpha;
      memcpy(output+(size_t)column*4u,&packed,sizeof packed);
    }
  }
  return 1;
}

/* Frame targets own fixed baselines. Numbered surfaces share the remaining slots within one
 * byte budget: a surface takes its own slot, a free one, or the least recently checkpointed one,
 * and older surface baselines are released until the budget holds. */
static GlSceneCheckpoint *scene_checkpoint_slot(GmlGpuBackend *backend,const GlSceneImage *image){
  GlScene *scene=&backend->scene;
  GlSceneCheckpoint *slot=NULL;
  uint64_t bytes=(uint64_t)image->width*image->height*4u,held=0;
  if(image->ref.resource_class==GML_RENDER_RESOURCE_SCREEN) return &scene->checkpoints[0];
  if(image->ref.resource_class==GML_RENDER_RESOURCE_APPLICATION) return &scene->checkpoints[1];
  if(image->ref.resource_class!=GML_RENDER_RESOURCE_SURFACE ||
     bytes>GL_SCENE_SURFACE_CHECKPOINT_BYTES) return NULL;
  for(unsigned index=GL_SCENE_FRAME_CHECKPOINTS;index<GL_SCENE_CHECKPOINTS && !slot;index++)
    if(scene->checkpoints[index].texture &&
       scene_same_logical(scene->checkpoints[index].image,image->ref))
      slot=&scene->checkpoints[index];
  for(unsigned index=GL_SCENE_FRAME_CHECKPOINTS;index<GL_SCENE_CHECKPOINTS && !slot;index++)
    if(!scene->checkpoints[index].texture) slot=&scene->checkpoints[index];
  if(!slot){
    slot=&scene->checkpoints[GL_SCENE_FRAME_CHECKPOINTS];
    for(unsigned index=GL_SCENE_FRAME_CHECKPOINTS+1u;index<GL_SCENE_CHECKPOINTS;index++)
      if(scene->checkpoints[index].last_used<slot->last_used) slot=&scene->checkpoints[index];
  }
  if(!scene_same_logical(slot->image,image->ref)) slot->image.content_version=0;
  for(;;){
    GlSceneCheckpoint *oldest=NULL;
    held=0;
    for(unsigned index=GL_SCENE_FRAME_CHECKPOINTS;index<GL_SCENE_CHECKPOINTS;index++){
      GlSceneCheckpoint *other=&scene->checkpoints[index];
      if(other==slot || !other->texture) continue;
      held+=(uint64_t)other->width*other->height*4u;
      if(!oldest || other->last_used<oldest->last_used) oldest=other;
    }
    if(held+bytes<=GL_SCENE_SURFACE_CHECKPOINT_BYTES || !oldest) break;
    backend->gl.DeleteTextures(1,&oldest->texture);
    memset(oldest,0,sizeof *oldest);
  }
  slot->last_used=++scene->use_clock;
  return slot;
}

/* A comparison pass draws into its own small colour target, grown and never shrunk. */
static int checkpoint_pass_target(GmlGpuBackend *backend,GLuint *texture,GLuint *framebuffer,
                                  uint32_t *held_width,uint32_t *held_height,
                                  uint32_t width,uint32_t height){
  if(!*texture) backend->gl.GenTextures(1,texture);
  if(!*framebuffer) backend->gl.GenFramebuffers(1,framebuffer);
  if(!*texture || !*framebuffer) return 0;
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,*texture);
  if(*held_width<width || *held_height<height){
    if(*held_width>width) width=*held_width;
    if(*held_height>height) height=*held_height;
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)width,(GLsizei)height,
                            0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
    *held_width=width;*held_height=height;
  }
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,*framebuffer);
  backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,*texture,0);
  return backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
}

/* A mask never becomes a CPU picture. It describes exact device differences from an explicitly
 * preserved successful checkpoint; its rows of changed tiles bound the existing pixel reader. */
static int scene_checkpoint_region(GmlGpuBackend *backend,GlSceneImage *image,
                                    const GlSceneCheckpoint *checkpoint,uint32_t format,
                                    GmlRenderImageReadbackResult *result,
                                    GmlRenderRect *bands,uint32_t *band_count){
  GlScene *scene=&backend->scene;
  uint32_t columns=(image->width+31u)/32u,rows=(image->height+31u)/32u;
  uint32_t left=columns,right=0,bottom=rows,top=0;
  *band_count=0;
  size_t bytes=(size_t)columns*rows*4u;
  GLint active=0,extent[2];
  if(!backend->gl.GetQueryiv || !scene_scratch_ready(scene,bytes) ||
     !gml_gpu_opengl_checkpoint_program(backend)) return 0;
  backend->gl.GetQueryiv(GL_ANY_SAMPLES_PASSED,GL_CURRENT_QUERY,&active);
  if(active) return 0;
  backend->gl.GetQueryiv(backend->api==GML_GPU_API_OPENGL_CORE?GL_SAMPLES_PASSED:
                           GL_ANY_SAMPLES_PASSED_CONSERVATIVE,GL_CURRENT_QUERY,&active);
  if(active || backend->gl.GetError()!=GL_NO_ERROR) return 0;
  if(!checkpoint_pass_target(backend,&scene->checkpoint_rows,&scene->checkpoint_rows_framebuffer,
                             &scene->checkpoint_rows_width,&scene->checkpoint_rows_height,
                             columns,image->height) ||
     !checkpoint_pass_target(backend,&scene->checkpoint_mask,&scene->checkpoint_framebuffer,
                             &scene->checkpoint_mask_width,&scene->checkpoint_mask_height,
                             columns,rows)) return 0;
  backend->gl.Disable(GL_BLEND);backend->gl.Disable(GL_DEPTH_TEST);
  backend->gl.Disable(GL_STENCIL_TEST);backend->gl.Disable(GL_CULL_FACE);
  backend->gl.Disable(GL_DITHER);backend->gl.Disable(GL_SCISSOR_TEST);
  backend->gl.Disable(GL_RASTERIZER_DISCARD);backend->gl.Disable(GL_SAMPLE_COVERAGE);
  backend->gl.Disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
  if(scene->supports_sample_mask) backend->gl.Disable(GL_SAMPLE_MASK);
  if(backend->api==GML_GPU_API_OPENGL_CORE){
    backend->gl.Disable(GL_FRAMEBUFFER_SRGB);
    backend->gl.PolygonMode(GL_FRONT_AND_BACK,GL_FILL);
  }
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  backend->gl.DepthMask(GL_FALSE);
  backend->gl.BindVertexArray(backend->vertex_array);
  backend->gl.UseProgram(scene->checkpoint_program);
  backend->gl.Uniform1i(scene->checkpoint_u_image,0);
  backend->gl.Uniform1i(scene->checkpoint_u_previous,1);
  backend->gl.Uniform1i(scene->checkpoint_u_alpha,format!=GML_RENDER_PIXEL_XRGB8888);
  /* Pass 0: each row's 32-pixel segments, compared with the baseline. */
  extent[0]=(GLint)image->width;extent[1]=(GLint)image->height;
  backend->gl.Uniform1i(scene->checkpoint_u_pass,0);
  backend->gl.Uniform2iv(scene->checkpoint_u_extent,1,extent);
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,scene->checkpoint_rows_framebuffer);
  backend->gl.Viewport(0,0,(GLsizei)columns,(GLsizei)image->height);
  backend->gl.ActiveTexture(GL_TEXTURE0);backend->gl.BindTexture(GL_TEXTURE_2D,image->texture);
  backend->gl.ActiveTexture(GL_TEXTURE1);backend->gl.BindTexture(GL_TEXTURE_2D,checkpoint->texture);
  backend->gl.DrawArrays(GL_TRIANGLES,0,3);
  /* Pass 1: 32 consecutive segments of one column make a tile. */
  extent[0]=(GLint)columns;
  backend->gl.Uniform1i(scene->checkpoint_u_pass,1);
  backend->gl.Uniform2iv(scene->checkpoint_u_extent,1,extent);
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,scene->checkpoint_framebuffer);
  backend->gl.Viewport(0,0,(GLsizei)columns,(GLsizei)rows);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE0);backend->gl.BindTexture(GL_TEXTURE_2D,scene->checkpoint_rows);
  backend->gl.DrawArrays(GL_TRIANGLES,0,3);
  backend->gl.PixelStorei(GL_PACK_ALIGNMENT,1);
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  result->mask_reads++;result->mask_bytes+=bytes;
  backend->gl.ReadPixels(0,0,(GLsizei)columns,(GLsizei)rows,GL_RGBA,GL_UNSIGNED_BYTE,scene->scratch);
  backend->gl.PixelStorei(GL_PACK_ALIGNMENT,4);
  backend->gl.UseProgram(0);backend->gl.BindVertexArray(0);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  /* Each row of changed tiles is read on its own, spanning only its changed columns: the
   * changes of one picture are usually a few separate objects, and their union rectangle can
   * cover most of it. Consecutive rows with the same span share one read. */
  for(uint32_t y=0;y<rows;y++){
    uint32_t row_left=columns,row_right=0;
    for(uint32_t x=0;x<columns;x++)
      if(scene->scratch[((size_t)y*columns+x)*4u]){
        if(x<row_left) row_left=x;
        row_right=x+1u;
      }
    if(!row_right) continue;
    if(row_left<left) left=row_left;
    if(row_right>right) right=row_right;
    if(y<bottom) bottom=y;
    top=y+1u;
    if(bands){
      uint32_t x0=row_left*32u,x1=row_right*32u>image->width?image->width:row_right*32u;
      uint32_t y1=(y+1u)*32u>image->height?image->height:(y+1u)*32u;
      GmlRenderRect *last=*band_count?&bands[*band_count-1u]:NULL;
      /* Mask rows count from the bottom; storage rows from the top. */
      if(last && last->x==(int32_t)x0 && last->width==x1-x0 &&
         last->y==(int32_t)(image->height-y*32u)){
        last->y=(int32_t)(image->height-y1);
        last->height+=y1-y*32u;
      } else bands[(*band_count)++]=(GmlRenderRect){(int32_t)x0,(int32_t)(image->height-y1),
                                                    x1-x0,y1-y*32u};
    }
  }
  if(!right){ result->updated_region=(GmlRenderRect){0};*band_count=0;return 1; }
  right=right*32u>image->width?image->width:right*32u;
  top=top*32u>image->height?image->height:top*32u;
  result->updated_region=(GmlRenderRect){(int32_t)(left*32u),(int32_t)(image->height-top),
                                         right-left*32u,top-bottom*32u};
  return 1;
}

static void scene_checkpoint_remember(GmlGpuBackend *backend,GlSceneImage *image,
                                       GlSceneCheckpoint *checkpoint,uint32_t format,
                                       int changed){
  int resize=checkpoint->width!=image->width || checkpoint->height!=image->height;
  if(!changed && checkpoint->image.content_version){ checkpoint->image=image->ref;return; }
  checkpoint->image.content_version=0;
  if(!backend->gl.CopyTexSubImage2D) return;
  if(!checkpoint->texture) backend->gl.GenTextures(1,&checkpoint->texture);
  if(!checkpoint->texture) return;
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,checkpoint->texture);
  if(resize){
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)image->width,(GLsizei)image->height,
                            0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if(backend->gl.GetError()!=GL_NO_ERROR) return;
    checkpoint->width=image->width;checkpoint->height=image->height;
  }
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,image->framebuffer);
  backend->gl.CopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,(GLsizei)image->width,(GLsizei)image->height);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(backend->gl.GetError()!=GL_NO_ERROR) return;
  checkpoint->image=image->ref;checkpoint->pixel_format=format;
}

int gml_gpu_opengl_scene_readback(GmlGpuBackend *backend,const GmlRenderImageReadback *transfer){
  GlSceneImage *image;
  GlSceneCheckpoint *checkpoint=NULL;
  GmlRenderImageReadbackResult result={0};
  GmlRenderImageReadback selected;
  GmlRenderRect *bands=NULL;
  uint32_t band_count=0;
  size_t row_bytes;
  int success=1;
  if(transfer && transfer->result) *transfer->result=result;
  if(!backend || !transfer || !transfer->bytes || !backend->loaded) return 0;
  image=gml_gpu_opengl_scene_image(backend,transfer->image);
  if(!image || !image->target || !image->framebuffer ||
     image->width!=transfer->storage_width || image->height!=transfer->storage_height ||
     !scene_region_valid(transfer->region,image->width,image->height) ||
     (transfer->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
      transfer->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
      transfer->pixel_format!=GML_RENDER_PIXEL_RGBA8888)) return 0;
  row_bytes=(size_t)transfer->region.width*4u;
  if(transfer->stride_bytes<row_bytes ||
     (uint64_t)(transfer->region.height-1u)*transfer->stride_bytes+row_bytes>
       transfer->byte_capacity) return 0;
  result.updated_region=transfer->region;
  if(transfer->checkpoint && transfer->result && transfer->region.x==0 && transfer->region.y==0 &&
     transfer->region.width==image->width && transfer->region.height==image->height &&
     (uint64_t)image->width*image->height*4u<=32u*1024u*1024u)
    checkpoint=scene_checkpoint_slot(backend,image);
  if(checkpoint && transfer->checkpoint_baseline_version &&
     scene_same_logical(checkpoint->image,image->ref) &&
     checkpoint->image.content_version==transfer->checkpoint_baseline_version &&
     checkpoint->width==image->width && checkpoint->height==image->height &&
     checkpoint->pixel_format==transfer->pixel_format){
    bands=(GmlRenderRect*)malloc(((size_t)image->height+31u)/32u*sizeof *bands);
    if(!scene_checkpoint_region(backend,image,checkpoint,transfer->pixel_format,&result,
                                bands,&band_count)){
      /* Optional detection failure cannot weaken an otherwise available exact full read. */
      backend->gl.GetError();
      result.updated_region=transfer->region;
      band_count=0;
    }
  }
  if(result.updated_region.width){
    /* Without bands, the one region is the union of the changes or the complete image. */
    uint32_t reads=band_count?band_count:1u;
    for(uint32_t index=0;index<reads && success;index++){
      GmlRenderRect region=band_count?bands[index]:result.updated_region;
      size_t offset=(size_t)(uint32_t)region.y*transfer->stride_bytes+
                     (size_t)(uint32_t)region.x*4u;
      selected=*transfer;selected.region=region;
      if(checkpoint){ selected.bytes=(uint8_t*)transfer->bytes+offset;selected.byte_capacity-=offset; }
      success=scene_readback_pixels(backend,image,&selected,&result);
    }
  }
  free(bands);
  if(checkpoint){
    if(success) scene_checkpoint_remember(backend,image,checkpoint,transfer->pixel_format,
                                          result.updated_region.width!=0u);
    else checkpoint->image.content_version=0;
    result.checkpoint_retained=success && scene_same_version(checkpoint->image,image->ref);
  }
  if(transfer->result) *transfer->result=result;
  return success;
}

/* Depth is raw integer storage beside colour, not a colour image format. Keep
 * all host transfer state local: a refused transfer must not poison a retry. */
typedef struct GlDepthTransferState {
  GLint active,texture,read,draw,buffer;
  GLint packing[4];
} GlDepthTransferState;

static const GLenum depth_pack_tokens[2][4]={
  {GL_UNPACK_ALIGNMENT,GL_UNPACK_ROW_LENGTH,GL_UNPACK_SKIP_ROWS,GL_UNPACK_SKIP_PIXELS},
  {GL_PACK_ALIGNMENT,GL_PACK_ROW_LENGTH,GL_PACK_SKIP_ROWS,GL_PACK_SKIP_PIXELS}
};

static void depth_transfer_begin(GmlGpuBackend *backend,GlDepthTransferState *state,int read){
  backend->gl.GetIntegerv(GL_ACTIVE_TEXTURE,&state->active);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.GetIntegerv(GL_TEXTURE_BINDING_2D,&state->texture);
  backend->gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&state->read);
  backend->gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&state->draw);
  backend->gl.GetIntegerv(read?GL_PIXEL_PACK_BUFFER_BINDING:GL_PIXEL_UNPACK_BUFFER_BINDING,
                           &state->buffer);
  backend->gl.BindBuffer(read?GL_PIXEL_PACK_BUFFER:GL_PIXEL_UNPACK_BUFFER,0);
  for(unsigned i=0;i<4u;i++){
    backend->gl.GetIntegerv(depth_pack_tokens[read][i],&state->packing[i]);
    backend->gl.PixelStorei(depth_pack_tokens[read][i],i?0:4);
  }
}

static void depth_transfer_end(GmlGpuBackend *backend,const GlDepthTransferState *state,int read){
  for(unsigned i=0;i<4u;i++) backend->gl.PixelStorei(depth_pack_tokens[read][i],state->packing[i]);
  backend->gl.BindBuffer(read?GL_PIXEL_PACK_BUFFER:GL_PIXEL_UNPACK_BUFFER,(GLuint)state->buffer);
  backend->gl.BindTexture(GL_TEXTURE_2D,(GLuint)state->texture);
  backend->gl.ActiveTexture((GLenum)state->active);
  backend->gl.BindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)state->read);
  backend->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)state->draw);
}

static GlSceneImage *depth_transfer_image(GmlGpuBackend *backend,
  const GmlRenderDepthPlaneRef *plane,uint32_t stride,size_t capacity,int independent_colour_version){
  if(!backend || !backend->loaded || !plane || !plane->epoch || !plane->version ||
     !plane->width || !plane->height || plane->width>UINT32_MAX/4u ||
     stride<plane->width*4u ||
     (uint64_t)(plane->height-1u)*stride+(uint64_t)plane->width*4u>capacity ||
     (uint64_t)plane->width*plane->height>SIZE_MAX/16u ||
     (plane->colour.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      plane->colour.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      plane->colour.resource_class!=GML_RENDER_RESOURCE_SURFACE &&
      plane->colour.resource_class!=GML_RENDER_RESOURCE_SCRATCH)) return NULL;
  GlSceneImage *image=gml_gpu_opengl_scene_image(backend,plane->colour);
  if(!image && independent_colour_version) for(unsigned i=0;i<GL_SCENE_MAX_IMAGES;i++){
    GlSceneImage *candidate=&backend->scene.images[i];
    if(candidate->valid && !candidate->retired && candidate->texture &&
       candidate->context_epoch==backend->scene.context_epoch &&
       scene_same_logical(candidate->ref,plane->colour)){image=candidate;break;}
  }
  if(!image || image->source_partial || !image->rendered_origin ||
     image->width!=plane->width || image->height!=plane->height) return NULL;
  return image;
}

static int scene_raw_depth_storage(GmlGpuBackend *backend,GlSceneImage *image,unsigned back){
  const uint32_t width=image->width,height=image->height;
  if(!image->raw_depth_texture[back]){
    backend->gl.GenTextures(1,&image->raw_depth_texture[back]);
    if(!image->raw_depth_texture[back]) return 0;
    backend->gl.BindTexture(GL_TEXTURE_2D,image->raw_depth_texture[back]);
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_R32UI,(GLsizei)width,(GLsizei)height,
                            0,GL_RED_INTEGER,GL_UNSIGNED_INT,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if(backend->gl.GetError()!=GL_NO_ERROR){
      backend->gl.DeleteTextures(1,&image->raw_depth_texture[back]);
      image->raw_depth_texture[back]=0;return 0;
    }
  }else backend->gl.BindTexture(GL_TEXTURE_2D,image->raw_depth_texture[back]);
  if(!image->raw_depth_framebuffer[back])
    backend->gl.GenFramebuffers(1,&image->raw_depth_framebuffer[back]);
  if(!image->raw_depth_framebuffer[back]) return 0;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,image->raw_depth_framebuffer[back]);
  backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                    image->raw_depth_texture[back],0);
  return backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE &&
    backend->gl.GetError()==GL_NO_ERROR;
}

int gml_gpu_opengl_depth_upload(GmlGpuBackend *backend,const GmlRenderDepthUpload *transfer){
  if(!transfer || !transfer->bytes) return 0;
  GlSceneImage *image=depth_transfer_image(backend,&transfer->plane,
                                          transfer->stride_bytes,transfer->byte_capacity,0);
  if(!image || transfer->plane.epoch<image->raw_depth_epoch ||
     (transfer->plane.epoch==image->raw_depth_epoch &&
      transfer->plane.version<=image->raw_depth_version)) return 0;
  const uint32_t width=transfer->plane.width,height=transfer->plane.height;
  size_t row_bytes=(size_t)width*4u;
  if(!scene_scratch_ready(&backend->scene,row_bytes*height)) return 0;
  for(uint32_t y=0;y<height;y++)
    memcpy(backend->scene.scratch+(size_t)(height-1u-y)*row_bytes,
           (const uint8_t*)transfer->bytes+(size_t)y*transfer->stride_bytes,row_bytes);
  GlDepthTransferState state;depth_transfer_begin(backend,&state,0);
  unsigned back=image->raw_depth_front^1u;
  int success=0;
  if(!scene_raw_depth_storage(backend,image,back)) goto depth_upload_end;
  backend->gl.TexSubImage2D(GL_TEXTURE_2D,0,0,0,(GLsizei)width,(GLsizei)height,
                             GL_RED_INTEGER,GL_UNSIGNED_INT,backend->scene.scratch);
  success=backend->gl.GetError()==GL_NO_ERROR;
depth_upload_end:
  if(!success) backend->gl.GetError();
  depth_transfer_end(backend,&state,0);
  if(!success) return 0;
  image->raw_depth_front=back;image->raw_depth_epoch=transfer->plane.epoch;
  image->raw_depth_version=transfer->plane.version;
  image->target=1; /* Volatile device authority must not be evicted as a source cache. */
  return 1;
}

int gml_gpu_opengl_depth_readback(GmlGpuBackend *backend,const GmlRenderDepthReadback *transfer){
  if(!transfer || !transfer->bytes) return 0;
  GlSceneImage *image=depth_transfer_image(backend,&transfer->plane,
                                          transfer->stride_bytes,transfer->byte_capacity,1);
  if(!image || image->raw_depth_epoch!=transfer->plane.epoch ||
     image->raw_depth_version!=transfer->plane.version ||
     !image->raw_depth_framebuffer[image->raw_depth_front]) return 0;
  const uint32_t width=transfer->plane.width,height=transfer->plane.height;
  size_t row_bytes=(size_t)width*16u,bytes=row_bytes*height;
  if(!scene_scratch_ready(&backend->scene,bytes)) return 0;
  GlDepthTransferState state;depth_transfer_begin(backend,&state,1);
  backend->gl.BindFramebuffer(GL_READ_FRAMEBUFFER,image->raw_depth_framebuffer[image->raw_depth_front]);
  backend->gl.ReadPixels(0,0,(GLsizei)width,(GLsizei)height,GL_RGBA_INTEGER,
                           GL_UNSIGNED_INT,backend->scene.scratch);
  int success=backend->gl.GetError()==GL_NO_ERROR;
  depth_transfer_end(backend,&state,1);
  if(!success) return 0;
  for(uint32_t y=0;y<height;y++) for(uint32_t x=0;x<width;x++)
    memcpy((uint8_t*)transfer->bytes+(size_t)y*transfer->stride_bytes+(size_t)x*4u,
           backend->scene.scratch+(size_t)(height-1u-y)*row_bytes+(size_t)x*16u,4u);
  return 1;
}

/* Point alpha is unweighted. A constant-depth line retains the CPU's scalar
 * multiply/divide before the fragment's actual device texture observation. */
static int depth_primitive_scalar_alpha(const GmlRenderDepthPrimitive *primitive,double *alpha){
  if(primitive->topology==GML_RENDER_DEPTH_TOPOLOGY_POINT){*alpha=primitive->vertex[0][7];return 1;}
  if(primitive->topology!=GML_RENDER_DEPTH_TOPOLOGY_LINE ||
     primitive->vertex[0][7]!=primitive->vertex[1][7] ||
     primitive->vertex[0][2]!=primitive->vertex[1][2])return 0;
  *alpha=(primitive->vertex[0][7]*primitive->vertex[0][2])/primitive->vertex[0][2];
  return 1;
}

/* Scalar state sampling prepares indices/weights only. Actual alpha bytes stay
 * on the device; the picture continues to use its ordinary hardware sampler. */
static int depth_triangle_alpha_setup(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
  GLint coordinates[4],GLint fractions[4],GLint weights[8],GLint *mode){
  const GmlRenderDepthPrimitive *p=&op->payload.depth_primitive;
  double alpha;*mode=0;
  if(!p->alpha_sample_policy || !p->textured ||
     !depth_primitive_scalar_alpha(p,&alpha) || alpha>=0x1p-16)return 1;
  unsigned count=p->topology==GML_RENDER_DEPTH_TOPOLOGY_POINT?1u:2u;
  for(unsigned v=1;v<count;v++)for(unsigned c=0;c<2u;c++)
    if(p->uv[v][c]!=p->uv[0][c])return 0;
  GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
  GmlRenderSampleSetup setup;
  if(!source || !gml_render_cpu_backend_sample_setup_repeat(p->uv[0][0],p->uv[0][1],
       (int)source->width,(int)source->height,(int)p->interpolate,&setup))return 0;
  coordinates[0]=setup.x0;coordinates[1]=setup.y0;
  coordinates[2]=setup.x1;coordinates[3]=setup.y1;
  fractions[0]=setup.ix;fractions[1]=setup.iy;
  for(unsigned i=0;i<4u;i++){
    double value=setup.weight[i];
    /* Existing shared d64 arithmetic accepts zero and bounded normal inputs. */
    if(!isfinite(value) || value<0 || value>1 || (value>0 && value<0x1p-900))return 0;
    uint64_t bits;memcpy(&bits,&value,sizeof bits);
    weights[i*2u]=(GLint)(uint32_t)bits;weights[i*2u+1u]=(GLint)(uint32_t)(bits>>32u);
  }
  *mode=p->interpolate?(GLint)p->alpha_sample_policy:3;
  return 1;
}

int gml_gpu_opengl_depth_triangle_valid(GmlGpuBackend *backend,const GmlRenderDrawBatch *batch){
  if(!backend || !backend->loaded || !backend->scene.available || !backend->gl.BlitFramebuffer ||
     !backend->gl.BindSampler || !batch || !batch->operations || batch->operation_count!=1u ||
     !batch->states || !batch->state_count) return 0;
  const GmlRenderDrawOp *op=batch->operations;
  const GmlRenderDepthPrimitive *triangle=&op->payload.depth_primitive;
  int point=triangle->topology==GML_RENDER_DEPTH_TOPOLOGY_POINT;
  int line=triangle->topology==GML_RENDER_DEPTH_TOPOLOGY_LINE;
  unsigned vertex_count=point?1u:line?2u:3u;
  if(triangle->topology>GML_RENDER_DEPTH_TOPOLOGY_LINE ||
     (line && (!backend->gl.GetFloatv || !backend->gl.LineWidth ||
       (backend->api==GML_GPU_API_OPENGL_CORE && !backend->gl.IsEnabled))) ||
     (point && backend->api==GML_GPU_API_OPENGL_CORE && !backend->gl.IsEnabled) ||
     op->opcode!=GML_RENDER_OP_SOFTWARE3D_PASS || triangle->textured>1u ||
     triangle->alpha_sample_policy>GML_RENDER_DEPTH_ALPHA_MODERN ||
     triangle->interpolate>1u || op->source_count!=triangle->textured ||
     (op->source_count && !op->sources) || (!op->source_count && op->sources) ||
     op->state_index>=batch->state_count || op->scene_family!=GML_RENDER_SCENE_PRIMITIVE ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 || !op->target.storage_version ||
     op->target.region.x || op->target.region.y || op->target.region.width!=op->target.width ||
     op->target.region.height!=op->target.height || op->target.width>16384u || op->target.height>16384u ||
     !scene_same_logical(triangle->previous_colour,op->target.image) ||
     triangle->previous_colour.content_version==UINT64_MAX ||
     op->target.image.content_version!=triangle->previous_colour.content_version+1u ||
     !triangle->depth_epoch || !triangle->depth_version || triangle->depth_version==UINT64_MAX ||
     triangle->next_depth_version!=triangle->depth_version+1u ||
     triangle->hidden>1u || triangle->write_depth>1u || triangle->normalized>1u ||
     triangle->blend>GML_RENDER_DEPTH_BLEND_OPAQUE_ALPHA_INPUT || triangle->stamp_alpha>1u || triangle->zero_alpha_writes>1u ||
     triangle->alpha_source_squared>1u) return 0;
  if(triangle->shading_flags>15u) return 0;
  if(triangle->shading_flags&1u)for(unsigned c=0;c<3u;c++)
    if(!isfinite(triangle->shade[c]) || triangle->shade[c]<0 || triangle->shade[c]>1e6f) return 0;
  if(triangle->shading_flags&2u){
    if(!isfinite(triangle->fog_start) || !isfinite(triangle->fog_scale) ||
       fabsf(triangle->fog_start)>1e20f || fabsf(triangle->fog_scale)>1e20f) return 0;
    for(unsigned c=0;c<3u;c++)
      if(!isfinite(triangle->fog_colour[c]) || triangle->fog_colour[c]<0 || triangle->fog_colour[c]>1 ||
         !isfinite(triangle->fog_distance[c]) || triangle->fog_distance[c]<0 || triangle->fog_distance[c]>1e20f) return 0;
  }
  GmlRenderDepthPlaneRef plane={triangle->previous_colour,op->target.width,op->target.height,
                                triangle->depth_epoch,triangle->depth_version};
  GlSceneImage *image=depth_transfer_image(backend,&plane,op->target.width*4u,
                                          (size_t)op->target.width*op->target.height*4u,0);
  if(!image || image->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     image->raw_depth_epoch!=triangle->depth_epoch || image->raw_depth_version!=triangle->depth_version ||
     !image->raw_depth_texture[image->raw_depth_front] ||
     !image->raw_depth_framebuffer[image->raw_depth_front]) return 0;
  if(triangle->textured){
    GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
    if(!source || source==image || !source->texture || source->retired || source->source_partial) return 0;
    for(unsigned v=0;v<vertex_count;v++)for(unsigned c=0;c<2u;c++)
      if(!isfinite(triangle->uv[v][c]) || fabs(triangle->uv[v][c])>65536) return 0;
  }
  const GmlRenderDrawStateSnapshot *state=&batch->states[op->state_index];
  if(state->write_mask>15u || state->alpha_test_enabled || state->source_alpha_key ||
     state->source_palette.enabled || state->sampler_count || state->material_identity ||
     state->material_version || (state->coverage_policy && triangle->blend<GML_RENDER_DEPTH_BLEND_INDEPENDENT) || state->alpha!=255u ||
     state->colour!=0xffffffffu ||
     (!state->clip.width && !state->clip.height && (state->clip.x || state->clip.y)) ||
     ((state->clip.width || state->clip.height) &&
      !scene_region_valid(state->clip,op->target.width,op->target.height))) return 0;
  if(triangle->blend>=GML_RENDER_DEPTH_BLEND_INDEPENDENT &&
     (!gml_render_blend_snapshot_native(state) || !backend->gl.BlendFuncSeparate ||
      !backend->gl.BlendEquationSeparate || triangle->alpha_source_squared ||
      triangle->stamp_alpha!=state->coverage_policy))return 0;
  if(triangle->blend==GML_RENDER_DEPTH_BLEND_INDEPENDENT && !triangle->zero_alpha_writes)return 0;
  if(triangle->blend==GML_RENDER_DEPTH_BLEND_OPAQUE_ALPHA_INPUT &&
     (triangle->stamp_alpha || state->coverage_policy))return 0;
  if(triangle->blend==1u && (state->rgb_source_factor!=GML_RENDER_BLEND_SOURCE_ALPHA ||
     state->rgb_destination_factor!=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA ||
     state->alpha_source_factor!=(triangle->alpha_source_squared?
       GML_RENDER_BLEND_SOURCE_ALPHA:GML_RENDER_BLEND_ONE) ||
     state->alpha_destination_factor!=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA ||
     state->rgb_equation!=GML_RENDER_BLEND_ADD || state->alpha_equation!=GML_RENDER_BLEND_ADD)) return 0;
  for(unsigned v=0;v<vertex_count;v++){
    for(unsigned component=0;component<8u;component++)
      if(!isfinite(triangle->vertex[v][component])) return 0;
    if(fabs(triangle->vertex[v][0])>65536.0 || fabs(triangle->vertex[v][1])>65536.0 ||
       triangle->vertex[v][2]<1e-8 || triangle->vertex[v][2]>1e8 ||
       fabs(triangle->vertex[v][3])>1e20) return 0;
    for(unsigned component=4u;component<8u;component++)
      if(triangle->vertex[v][component]<0 || triangle->vertex[v][component]>1) return 0;
  }
  GLint coordinates[4]={0},fractions[4]={0},weights[8]={0},sample_mode;
  if(!depth_triangle_alpha_setup(backend,op,coordinates,fractions,weights,&sample_mode))return 0;
  if(line){
    double alpha;
    if(((triangle->vertex[0][7]>0.0 && triangle->vertex[0][7]<0x1p-16) ||
        (triangle->vertex[1][7]>0.0 && triangle->vertex[1][7]<0x1p-16)) &&
       !depth_primitive_scalar_alpha(triangle,&alpha))return 0;
    return (float)triangle->vertex[0][0]!=(float)triangle->vertex[1][0] ||
      (float)triangle->vertex[0][1]!=(float)triangle->vertex[1][1];
  }
  if(point)return triangle->vertex[0][0]==floor(triangle->vertex[0][0])+0.5 &&
    triangle->vertex[0][1]==floor(triangle->vertex[0][1])+0.5;
  double area=(triangle->vertex[1][0]-triangle->vertex[0][0])*
    (triangle->vertex[2][1]-triangle->vertex[0][1])-
    (triangle->vertex[1][1]-triangle->vertex[0][1])*(triangle->vertex[2][0]-triangle->vertex[0][0]);
  return isfinite(area) && fabs(area)>1e-9;
}

static int triangle_colour_storage(GmlGpuBackend *backend,GlSceneImage *image){
  if(!image->framebuffer) backend->gl.GenFramebuffers(1,&image->framebuffer);
  if(!image->framebuffer) return 0;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,image->framebuffer);
  backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,image->texture,0);
  if(backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE ||
     backend->gl.GetError()!=GL_NO_ERROR) return 0;
  if(!image->triangle_texture){
    backend->gl.GenTextures(1,&image->triangle_texture);
    if(!image->triangle_texture) return 0;
    backend->gl.BindTexture(GL_TEXTURE_2D,image->triangle_texture);
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)image->width,(GLsizei)image->height,
                            0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if(backend->gl.GetError()!=GL_NO_ERROR){
      backend->gl.DeleteTextures(1,&image->triangle_texture);image->triangle_texture=0;return 0;
    }
  }
  if(!image->triangle_framebuffer) backend->gl.GenFramebuffers(1,&image->triangle_framebuffer);
  if(!image->triangle_framebuffer) return 0;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,image->triangle_framebuffer);
  backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,image->triangle_texture,0);
  if(image->depth_renderbuffer)
    backend->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,image->depth_renderbuffer);
  return backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE &&
    backend->gl.GetError()==GL_NO_ERROR;
}

static int depth_triangle_candidate(GmlGpuBackend *backend,const GmlRenderDrawBatch *batch,
                                      GLuint *sample_texture,char *error,size_t error_capacity){
  if(!gml_gpu_opengl_depth_triangle_valid(backend,batch)) return 0;
  const GmlRenderDrawOp *op=batch->operations;
  const GmlRenderDepthPrimitive *triangle=&op->payload.depth_primitive;
  const GmlRenderDrawStateSnapshot *state=&batch->states[op->state_index];
  GlScene *scene=&backend->scene;
  GlSceneImage *image=gml_gpu_opengl_scene_image(backend,triangle->previous_colour);
  unsigned back=image->raw_depth_front^1u;
  int point=triangle->topology==GML_RENDER_DEPTH_TOPOLOGY_POINT;
  int line=triangle->topology==GML_RENDER_DEPTH_TOPOLOGY_LINE;
  unsigned vertex_count=point?1u:line?2u:3u;
  GLenum topology=point?GL_POINTS:line?GL_LINES:GL_TRIANGLES;
  float vertices[33]={0},extent[2]={(float)image->width,(float)image->height};
  GLint flags[4]={(GLint)triangle->hidden,(GLint)triangle->write_depth,
                  (GLint)triangle->normalized,(GLint)triangle->blend};
  int native=triangle->blend>=GML_RENDER_DEPTH_BLEND_INDEPENDENT;
  int alpha_input=triangle->blend==GML_RENDER_DEPTH_BLEND_OPAQUE_ALPHA_INPUT;
  GLint policy[4]={native?0:(GLint)triangle->stamp_alpha,(GLint)triangle->zero_alpha_writes,
                   (GLint)triangle->alpha_source_squared,(GLint)triangle->textured};
  if(point || line)policy[0]|=2;
  if(triangle->textured){
    int constant=1;
    for(unsigned v=1;v<vertex_count;v++)for(unsigned c=0;c<2u;c++)
      if(triangle->uv[v][c]!=triangle->uv[0][c])constant=0;
    if(constant)policy[3]=2;
  }
  float constant_uv[2]={(float)triangle->uv[0][0],(float)triangle->uv[0][1]};
  GLint alpha_coordinates[4]={0},alpha_fraction[4]={0},alpha_weights[8]={0},alpha_mode;
  if(!depth_triangle_alpha_setup(backend,op,alpha_coordinates,alpha_fraction,alpha_weights,&alpha_mode))return 0;
  GLint input_alpha[4]={0};double scalar_alpha;
  if(depth_primitive_scalar_alpha(triangle,&scalar_alpha) && scalar_alpha<0x1p-16){
    uint64_t bits=0;if(scalar_alpha>0.0)memcpy(&bits,&scalar_alpha,sizeof bits);
    input_alpha[0]=(GLint)(uint32_t)bits;input_alpha[1]=(GLint)(uint32_t)(bits>>32u);
    input_alpha[2]=1;input_alpha[3]=alpha_mode;
  }
  for(unsigned v=0;v<vertex_count;v++){
    for(unsigned c=0;c<8u;c++)
      vertices[v*11u+c]=(float)(triangle->vertex[v][c]*(c>=4u?triangle->vertex[v][2]:1.0));
    for(unsigned c=0;c<2u;c++)
      vertices[v*11u+8u+c]=(float)(triangle->uv[v][c]*triangle->vertex[v][2]);
    vertices[v*11u+10u]=triangle->fog_distance[v];
  }
  if(!gml_gpu_opengl_depth_triangle_programs(backend,error,error_capacity)) return 0;
  backend->gl.ActiveTexture(GL_TEXTURE0);backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  if(!triangle_colour_storage(backend,image) || !scene_raw_depth_storage(backend,image,back)) return 0;
  if(!scene->depth_triangle_array) backend->gl.GenVertexArrays(1,&scene->depth_triangle_array);
  if(!scene->depth_triangle_buffer) backend->gl.GenBuffers(1,&scene->depth_triangle_buffer);
  if(!scene->depth_triangle_array || !scene->depth_triangle_buffer) return 0;
  backend->gl.BindVertexArray(scene->depth_triangle_array);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,scene->depth_triangle_buffer);
  backend->gl.BufferData(GL_ARRAY_BUFFER,sizeof vertices,vertices,GL_STREAM_DRAW);
  for(unsigned attr=0;attr<4u;attr++){
    backend->gl.EnableVertexAttribArray(attr);
    backend->gl.VertexAttribPointer(attr,attr==3u?1:attr==2u?2:4,GL_FLOAT,GL_FALSE,11*sizeof(float),
                                     (const void*)(uintptr_t)((attr==3u?10u:attr*4u)*sizeof(float)));
  }
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  backend->gl.Disable(GL_BLEND);backend->gl.Disable(GL_DEPTH_TEST);backend->gl.Disable(GL_STENCIL_TEST);
  backend->gl.Disable(GL_CULL_FACE);backend->gl.Disable(GL_DITHER);backend->gl.Disable(GL_SCISSOR_TEST);
  backend->gl.Disable(GL_RASTERIZER_DISCARD);
  if(backend->api==GML_GPU_API_OPENGL_CORE) backend->gl.Disable(GL_FRAMEBUFFER_SRGB);
  backend->gl.DepthMask(GL_FALSE);backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  if(triangle->textured){
    GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
    *sample_texture=source->rendered_origin?gml_gpu_opengl_scene_content_source(backend,source):source->texture;
    if(!*sample_texture) return 0;
    backend->gl.ActiveTexture(GL_TEXTURE2);backend->gl.BindTexture(GL_TEXTURE_2D,*sample_texture);
    backend->gl.BindSampler(2,0);
    GLint filter=triangle->interpolate?GL_LINEAR:GL_NEAREST;
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,filter);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,filter);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  }
  GLuint read_fbo[2]={image->framebuffer,image->raw_depth_framebuffer[image->raw_depth_front]};
  GLuint draw_fbo[2]={image->triangle_framebuffer,image->raw_depth_framebuffer[back]};
  for(unsigned pass=0;pass<2u;pass++){
    backend->gl.BindFramebuffer(GL_READ_FRAMEBUFFER,read_fbo[pass]);
    backend->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fbo[pass]);
    backend->gl.BlitFramebuffer(0,0,(GLint)image->width,(GLint)image->height,
      0,0,(GLint)image->width,(GLint)image->height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,image->raw_depth_texture[image->raw_depth_front]);
  backend->gl.BindSampler(0,0);
  backend->gl.ActiveTexture(GL_TEXTURE1);backend->gl.BindTexture(GL_TEXTURE_2D,image->texture);
  backend->gl.BindSampler(1,0);
  backend->gl.Viewport(0,0,(GLsizei)image->width,(GLsizei)image->height);
  if(state->clip.width){
    backend->gl.Enable(GL_SCISSOR_TEST);
    backend->gl.Scissor(state->clip.x,(GLint)image->height-state->clip.y-(GLint)state->clip.height,
                         (GLsizei)state->clip.width,(GLsizei)state->clip.height);
  }
  for(unsigned pass=0;pass<2u;pass++){
    GlDepthTriangleProgram *program=&scene->depth_triangle_program[pass];
    backend->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fbo[pass]);
    backend->gl.UseProgram(program->program);
    backend->gl.Uniform2fv(program->extent,1,extent);backend->gl.Uniform1i(program->depth,0);
    backend->gl.Uniform4iv(program->flags,1,flags);
    backend->gl.Uniform4iv(program->policy,1,policy);
    backend->gl.Uniform1i(program->source,2);
    backend->gl.Uniform2fv(program->flat_uv,1,constant_uv);
    backend->gl.Uniform4iv(program->input_alpha,1,input_alpha);
    backend->gl.Uniform4iv(program->alpha_coordinates,1,alpha_coordinates);
    backend->gl.Uniform4iv(program->alpha_fraction,1,alpha_fraction);
    backend->gl.Uniform4iv(program->alpha_weights,2,alpha_weights);
    if(!pass){
      float shade[4]={triangle->shade[0],triangle->shade[1],triangle->shade[2],
        (triangle->shading_flags&1u)?1.0f:0.0f};
      float fog[4]={triangle->fog_colour[0],triangle->fog_colour[1],triangle->fog_colour[2],
        (triangle->shading_flags&2u)?((triangle->shading_flags&4u)?2.0f:1.0f)+
          ((triangle->shading_flags&8u)?2.0f:0.0f):0.0f};
      float range[2]={triangle->fog_start,triangle->fog_scale};
      backend->gl.Uniform4fv(program->shade,1,shade);backend->gl.Uniform4fv(program->fog_colour,1,fog);
      backend->gl.Uniform2fv(program->fog_range,1,range);
      backend->gl.Uniform1i(program->colour,1);
      if(native){
        backend->gl.BlendFuncSeparate(gml_gpu_opengl_native_factor(state->rgb_source_factor),
          gml_gpu_opengl_native_factor(state->rgb_destination_factor),
          gml_gpu_opengl_native_factor(state->alpha_source_factor),
          gml_gpu_opengl_native_factor(state->alpha_destination_factor));
        backend->gl.BlendEquationSeparate(
          gml_gpu_opengl_native_equation(state->rgb_equation-GML_RENDER_BLEND_NATIVE_ADD),
          gml_gpu_opengl_native_equation(state->alpha_equation-GML_RENDER_BLEND_NATIVE_ADD));
        backend->gl.Enable(GL_BLEND);
      }
      backend->gl.ColorMask((state->write_mask&1u)!=0,(state->write_mask&2u)!=0,
        (state->write_mask&4u)!=0,(state->write_mask&8u)!=0 && !(native && (triangle->stamp_alpha || alpha_input)));
    }else{
      backend->gl.Disable(GL_BLEND);backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    }
    backend->gl.DrawArrays(topology,0,(GLsizei)vertex_count);
    if(backend->gl.GetError()!=GL_NO_ERROR)return 0;
    if(!pass && native && (triangle->stamp_alpha || alpha_input) && (state->write_mask&8u)){
      /* Stamp the same whole primitive before another command can consume its
       * destination alpha. All candidate storage remains unpublished. */
      GLint stamp_policy[4]={(point || line)?3:1,policy[1],policy[2],policy[3]};
      backend->gl.Disable(GL_BLEND);backend->gl.ColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
      backend->gl.Uniform4iv(program->policy,1,stamp_policy);
      backend->gl.DrawArrays(topology,0,(GLsizei)vertex_count);
      if(backend->gl.GetError()!=GL_NO_ERROR)return 0;
      if(alpha_input){
        /* The ordinary equation writer uses a constant opaque destination
         * operand for alpha. Seed only this unpublished channel, then let the
         * device execute the captured alpha equation over the same triangle. */
        backend->gl.Uniform4iv(program->policy,1,policy);backend->gl.Enable(GL_BLEND);
        backend->gl.DrawArrays(topology,0,(GLsizei)vertex_count);
        if(backend->gl.GetError()!=GL_NO_ERROR)return 0;
      }
    }
  }
  return 1;
}

int gml_render_gpu_opengl_depth_triangle(GmlGpuBackend *backend,const GmlRenderDrawBatch *batch,
                                   uintptr_t frontend_framebuffer,char *error,size_t error_capacity){
  if(!gml_gpu_opengl_depth_triangle_valid(backend,batch)) return 0;
  int point=batch->operations[0].payload.depth_primitive.topology==GML_RENDER_DEPTH_TOPOLOGY_POINT &&
    backend->api==GML_GPU_API_OPENGL_CORE;
  int line=batch->operations[0].payload.depth_primitive.topology==GML_RENDER_DEPTH_TOPOLOGY_LINE;
  GLfloat previous_line_width=1.0f;GLboolean previous_line_smooth=GL_FALSE;
  if(line){
    backend->gl.GetFloatv(GL_LINE_WIDTH,&previous_line_width);
    if(backend->api==GML_GPU_API_OPENGL_CORE)previous_line_smooth=backend->gl.IsEnabled(GL_LINE_SMOOTH);
    if(backend->gl.GetError()!=GL_NO_ERROR || !isfinite(previous_line_width) || previous_line_width<=0)return 0;
    backend->gl.LineWidth(1.0f);
    if(backend->api==GML_GPU_API_OPENGL_CORE)backend->gl.Disable(GL_LINE_SMOOTH);
  }
  GLboolean previous_point_size=GL_FALSE;
  if(point){
    previous_point_size=backend->gl.IsEnabled(GL_PROGRAM_POINT_SIZE);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
    backend->gl.Enable(GL_PROGRAM_POINT_SIZE);
  }
  GLuint sample_texture=0;
  int success=(!(point || line) || backend->gl.GetError()==GL_NO_ERROR) &&
    depth_triangle_candidate(backend,batch,&sample_texture,error,error_capacity);
  if(point && !previous_point_size) backend->gl.Disable(GL_PROGRAM_POINT_SIZE);
  if(line){
    backend->gl.LineWidth(previous_line_width);
    if(previous_line_smooth)backend->gl.Enable(GL_LINE_SMOOTH);
  }
  if(sample_texture){
    backend->gl.ActiveTexture(GL_TEXTURE2);backend->gl.BindTexture(GL_TEXTURE_2D,sample_texture);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    backend->gl.BindTexture(GL_TEXTURE_2D,0);
  }
  backend->gl.ActiveTexture(GL_TEXTURE1);backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE0);backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,0);backend->gl.BindVertexArray(0);
  backend->gl.UseProgram(0);backend->gl.Disable(GL_SCISSOR_TEST);backend->gl.Disable(GL_BLEND);
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)frontend_framebuffer);
  if(backend->gl.GetError()!=GL_NO_ERROR) success=0;
  if(!success) return 0;
  const GmlRenderDrawOp *op=batch->operations;
  const GmlRenderDepthPrimitive *triangle=&op->payload.depth_primitive;
  GlSceneImage *image=gml_gpu_opengl_scene_image(backend,triangle->previous_colour);
  /* No published storage was written above. Swap the pair only after success,
   * including cleanup, so a refused call retains both previous versions. */
  GLuint old_texture=image->texture,old_fbo=image->framebuffer;
  image->texture=image->triangle_texture;image->framebuffer=image->triangle_framebuffer;
  image->triangle_texture=old_texture;image->triangle_framebuffer=old_fbo;
  image->raw_depth_front^=1u;image->raw_depth_version=triangle->next_depth_version;
  image->ref=op->target.image;image->target=image->valid=image->rendered_origin=image->framebuffer_complete=1;
  image->alpha_opaque=image->alpha_bounds_known=image->opaque_white_known=0;
  image->content_source_version=0;
  return 1;
}

void gml_gpu_opengl_scene_retire(GmlGpuBackend *backend,GmlRenderImageRef image){
  int retire_incarnation=image.resource_class==GML_RENDER_RESOURCE_ATLAS ||
                     image.resource_class==GML_RENDER_RESOURCE_SCREEN ||
                     image.resource_class==GML_RENDER_RESOURCE_APPLICATION ||
                     image.resource_class==GML_RENDER_RESOURCE_SURFACE ||
                     image.resource_class==GML_RENDER_RESOURCE_SCRATCH ||
                     image.resource_class==GML_RENDER_RESOURCE_SHADER_SAMPLER ||
                     image.resource_class==GML_RENDER_RESOURCE_SHADER_SOURCE;
  if(!backend || !backend->loaded) return;
  for(size_t index=0;index<GL_SCENE_CHECKPOINTS;index++)
    if(scene_same_logical(backend->scene.checkpoints[index].image,image))
      backend->scene.checkpoints[index].image.content_version=0;
  for(size_t index=0;index<GL_SCENE_MAX_IMAGES;index++){
    GlSceneImage *candidate=&backend->scene.images[index];
    /* A CPU handoff or runtime atlas growth can advance the version after its last device
     * upload. Retirement ends that incarnation even if the resident version is older. */
    if(candidate->texture && candidate->context_epoch==backend->scene.context_epoch &&
       scene_same_logical(candidate->ref,image) &&
       (retire_incarnation || scene_same_version(candidate->ref,image))){
      /* The caller may be restoring a state while no GL context is current. Logical access ends
       * immediately; physical deletion happens at the next current scene allocation or destroy.
       * A context loss instead forgets the names, as they belong to the lost context. */
      candidate->retired=1;
      candidate->valid=0;
      candidate->alpha_opaque=0;
      candidate->alpha_bounds_known=0;
      return;
    }
  }
}

int gml_gpu_opengl_scene_flush(GmlGpuBackend *backend){
  if(!backend || !backend->loaded || !backend->scene.available || !backend->gl.Flush) return 0;
  backend->gl.Flush();
  return backend->gl.GetError()==GL_NO_ERROR;
}

int gml_gpu_opengl_scene_wait(GmlGpuBackend *backend){
  if(!backend || !backend->loaded || !backend->scene.available || !backend->gl.Finish) return 0;
  backend->gl.Finish();
  return backend->gl.GetError()==GL_NO_ERROR;
}
