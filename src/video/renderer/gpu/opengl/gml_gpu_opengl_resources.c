/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing OpenGL source mirrors and exact integer axis maps. */
#include "gml_gpu_opengl_internal.h"

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

static void scene_delete_image(GmlGpuBackend *backend,GlSceneImage *image){
  if(image->content_source_framebuffer)
    backend->gl.DeleteFramebuffers(1,&image->content_source_framebuffer);
  if(image->content_source_texture) backend->gl.DeleteTextures(1,&image->content_source_texture);
  if(image->framebuffer) backend->gl.DeleteFramebuffers(1,&image->framebuffer);
  if(image->texture) backend->gl.DeleteTextures(1,&image->texture);
  memset(image,0,sizeof *image);
}

void gml_gpu_opengl_scene_forget(GmlGpuBackend *backend){
  GlScene *scene=&backend->scene;
  memset(scene->images,0,sizeof scene->images);
  memset(scene->checkpoints,0,sizeof scene->checkpoints);
  scene->checkpoint_program=scene->checkpoint_mask=scene->checkpoint_framebuffer=0;
  scene->checkpoint_mask_width=scene->checkpoint_mask_height=0;
  scene->checkpoint_program_failed=0;
  memset(&scene->main_program,0,sizeof scene->main_program);
  memset(&scene->filtered_program,0,sizeof scene->filtered_program);
  memset(scene->compact_programs,0,sizeof scene->compact_programs);
  scene->vertex_buffer=scene->vertex_array=0;
  scene->instance_buffer=scene->instance_array=0;
  scene->upload_buffer=0;
  scene->instance_bytes_used=0;
  scene->instance_storage_ready=0;
  scene->instance_span_count=scene->instance_span_next=0;
  scene->instance_reuse_attempted=0;
  memset(scene->exact_programs,0,sizeof scene->exact_programs);
  scene->snapshot_texture=scene->copy_framebuffer=scene->rgb_query=0;
  scene->rgb_reduce_program=scene->rgb_reduce_framebuffer=0;
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
    for(size_t index=0;index<2u;index++)
      if(scene->checkpoints[index].texture)
        backend->gl.DeleteTextures(1,&scene->checkpoints[index].texture);
    if(scene->checkpoint_program) backend->gl.DeleteProgram(scene->checkpoint_program);
    if(scene->checkpoint_mask) backend->gl.DeleteTextures(1,&scene->checkpoint_mask);
    if(scene->checkpoint_framebuffer)
      backend->gl.DeleteFramebuffers(1,&scene->checkpoint_framebuffer);
    if(scene->main_program.program) backend->gl.DeleteProgram(scene->main_program.program);
    if(scene->filtered_program.program) backend->gl.DeleteProgram(scene->filtered_program.program);
    for(unsigned index=0;index<9u;index++)
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
  GL_SCENE_LOAD(CopyTexSubImage2D,"glCopyTexSubImage2D");
  GL_SCENE_LOAD(BlitFramebuffer,"glBlitFramebuffer");
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
  caps->supported_families=GML_RENDER_FAMILY_TEXTURE|GML_RENDER_FAMILY_PRIMITIVE|
                           GML_RENDER_FAMILY_EFFECT|GML_RENDER_FAMILY_SHADER|
                           GML_RENDER_FAMILY_SURFACE;
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
 * Separate final flags keep SCREEN alive while APPLICATION reuses both scratch planes. */
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

/* Keep one bounded staging name. Orphan its store before each write so an unsynchronized map
 * never overwrites an earlier texture transfer. Tiny and larger transfers retain the direct path.
 * No buffer or pointer survives context loss; mapping uses the established context entry points. */
static int scene_stage_upload(GmlGpuBackend *backend,size_t bytes,int *staged){
  void *mapped;
  GlScene *scene=&backend->scene;
  *staged=0;
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  if(bytes<64u*1024u || bytes>16u*1024u*1024u) return 1;
  if(!scene->upload_buffer){
    backend->gl.GenBuffers(1,&scene->upload_buffer);
    if(!scene->upload_buffer) return 0;
  }
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,scene->upload_buffer);
  backend->gl.BufferData(GL_PIXEL_UNPACK_BUFFER,(GLsizeiptr)bytes,NULL,GL_STREAM_DRAW);
  if(backend->gl.GetError()!=GL_NO_ERROR) goto refused;
  mapped=backend->gl.MapBufferRange(GL_PIXEL_UNPACK_BUFFER,0,(GLsizeiptr)bytes,
                                    GL_MAP_WRITE_BIT|GL_MAP_UNSYNCHRONIZED_BIT);
  if(!mapped) goto refused;
  memcpy(mapped,scene->scratch,bytes);
  if(!backend->gl.UnmapBuffer(GL_PIXEL_UNPACK_BUFFER)) goto refused;
  if(backend->gl.GetError()!=GL_NO_ERROR) goto refused;
  *staged=1;
  return 1;
refused:
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  (void)backend->gl.GetError();
  return 0;
}

int gml_gpu_opengl_scene_upload(GmlGpuBackend *backend,const GmlRenderImageUpload *transfer,
                                int *did_upload){
  GlScene *scene;
  GlSceneImage *slot;
  size_t row_bytes,bytes;
  const uint8_t *source;
  int first,resize,rendered_origin,staged;
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
     slot->texture && slot->valid && scene_same_version(slot->ref,transfer->image) &&
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
  first=!slot->texture;
  resize=slot->width!=transfer->storage_width || slot->height!=transfer->storage_height;
  if((first || resize || !scene_same_version(slot->ref,transfer->image)) &&
     (transfer->region.x || transfer->region.y ||
      transfer->region.width!=transfer->storage_width ||
      transfer->region.height!=transfer->storage_height)) return 0;
  if(!scene_scratch_ready(scene,bytes)) return 0;
  source=(const uint8_t*)transfer->bytes;
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
  if(first){
    backend->gl.GenTextures(1,&slot->texture);
    if(!slot->texture) return 0;
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,slot->texture);
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,1);
  backend->gl.PixelStorei(GL_UNPACK_ROW_LENGTH,0);
  backend->gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
  if(first || resize){
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)transfer->storage_width,
                           (GLsizei)transfer->storage_height,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  }
  if(!scene_stage_upload(backend,bytes,&staged)){
    backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,4);
    backend->gl.BindTexture(GL_TEXTURE_2D,0);
    slot->valid=0;
    slot->alpha_opaque=0;
    return 0;
  }
  backend->gl.TexSubImage2D(GL_TEXTURE_2D,0,transfer->region.x,
                            rendered_origin?(GLint)(transfer->storage_height-
                              (uint32_t)transfer->region.y-transfer->region.height)
                                           :transfer->region.y,
                            (GLsizei)transfer->region.width,(GLsizei)transfer->region.height,
                            GL_RGBA,GL_UNSIGNED_BYTE,staged?NULL:scene->scratch);
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
  slot->last_used=++scene->use_clock;
  if(did_upload) *did_upload=1;
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
  first=!slot->texture;
  resize=slot->width!=target->width || slot->height!=target->height;
  if(first){
    backend->gl.GenTextures(1,&slot->texture);
    if(!slot->texture) return NULL;
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,slot->texture);
  if(first || resize){
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)target->width,
                           (GLsizei)target->height,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    slot->valid=0;
    slot->alpha_opaque=0;
  }
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(!slot->framebuffer){
    backend->gl.GenFramebuffers(1,&slot->framebuffer);
    if(!slot->framebuffer){ slot->valid=0; slot->alpha_opaque=0; return NULL; }
  }
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,slot->framebuffer);
  backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                   slot->texture,0);
  if(backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE ||
     backend->gl.GetError()!=GL_NO_ERROR){ slot->valid=0; slot->alpha_opaque=0; return NULL; }
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

/* The established row/channel packing body serves both full and bounded reads. */
static int scene_readback_pixels(GmlGpuBackend *backend,GlSceneImage *image,
                                  const GmlRenderImageReadback *transfer,
                                  GmlRenderImageReadbackResult *result){
  GlScene *scene=&backend->scene;
  size_t row_bytes=(size_t)transfer->region.width*4u;
  size_t bytes=row_bytes*transfer->region.height;
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

/* A mask never becomes a CPU picture. It describes exact device differences from an explicitly
 * preserved successful checkpoint; the one union rectangle bounds the existing pixel reader. */
static int scene_checkpoint_region(GmlGpuBackend *backend,GlSceneImage *image,
                                    const GlSceneCheckpoint *checkpoint,uint32_t format,
                                    GmlRenderImageReadbackResult *result){
  GlScene *scene=&backend->scene;
  uint32_t columns=(image->width+31u)/32u,rows=(image->height+31u)/32u;
  uint32_t left=columns,right=0,bottom=rows,top=0;
  size_t bytes=(size_t)columns*rows*4u;
  GLint active=0;
  if(!backend->gl.GetQueryiv || !scene_scratch_ready(scene,bytes) ||
     !gml_gpu_opengl_checkpoint_program(backend)) return 0;
  backend->gl.GetQueryiv(GL_ANY_SAMPLES_PASSED,GL_CURRENT_QUERY,&active);
  if(active) return 0;
  backend->gl.GetQueryiv(backend->api==GML_GPU_API_OPENGL_CORE?GL_SAMPLES_PASSED:
                           GL_ANY_SAMPLES_PASSED_CONSERVATIVE,GL_CURRENT_QUERY,&active);
  if(active || backend->gl.GetError()!=GL_NO_ERROR) return 0;
  if(!scene->checkpoint_mask) backend->gl.GenTextures(1,&scene->checkpoint_mask);
  if(!scene->checkpoint_framebuffer)
    backend->gl.GenFramebuffers(1,&scene->checkpoint_framebuffer);
  if(!scene->checkpoint_mask || !scene->checkpoint_framebuffer) return 0;
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,scene->checkpoint_mask);
  if(scene->checkpoint_mask_width<columns || scene->checkpoint_mask_height<rows){
    uint32_t width=scene->checkpoint_mask_width>columns?scene->checkpoint_mask_width:columns;
    uint32_t height=scene->checkpoint_mask_height>rows?scene->checkpoint_mask_height:rows;
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)width,(GLsizei)height,
                            0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
    scene->checkpoint_mask_width=width;scene->checkpoint_mask_height=height;
  }
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,scene->checkpoint_framebuffer);
  backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                    scene->checkpoint_mask,0);
  if(backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return 0;
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
  backend->gl.Viewport(0,0,(GLsizei)columns,(GLsizei)rows);
  backend->gl.BindVertexArray(backend->vertex_array);
  backend->gl.UseProgram(scene->checkpoint_program);
  backend->gl.Uniform1i(scene->checkpoint_u_image,0);
  backend->gl.Uniform1i(scene->checkpoint_u_previous,1);
  backend->gl.Uniform1i(scene->checkpoint_u_alpha,format!=GML_RENDER_PIXEL_XRGB8888);
  backend->gl.ActiveTexture(GL_TEXTURE0);backend->gl.BindTexture(GL_TEXTURE_2D,image->texture);
  backend->gl.ActiveTexture(GL_TEXTURE1);backend->gl.BindTexture(GL_TEXTURE_2D,checkpoint->texture);
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
  for(uint32_t y=0;y<rows;y++) for(uint32_t x=0;x<columns;x++)
    if(scene->scratch[((size_t)y*columns+x)*4u]){
      if(x<left) left=x;
      if(x+1u>right) right=x+1u;
      if(y<bottom) bottom=y;
      if(y+1u>top) top=y+1u;
    }
  if(!right){ result->updated_region=(GmlRenderRect){0};return 1; }
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
     (uint64_t)image->width*image->height*4u<=32u*1024u*1024u){
    if(image->ref.resource_class==GML_RENDER_RESOURCE_SCREEN) checkpoint=&backend->scene.checkpoints[0];
    if(image->ref.resource_class==GML_RENDER_RESOURCE_APPLICATION) checkpoint=&backend->scene.checkpoints[1];
  }
  if(checkpoint && transfer->checkpoint_baseline_version &&
     scene_same_logical(checkpoint->image,image->ref) &&
     checkpoint->image.content_version==transfer->checkpoint_baseline_version &&
     checkpoint->width==image->width && checkpoint->height==image->height &&
     checkpoint->pixel_format==transfer->pixel_format){
    if(!scene_checkpoint_region(backend,image,checkpoint,transfer->pixel_format,&result)){
      /* Optional detection failure cannot weaken an otherwise available exact full read. */
      backend->gl.GetError();
      result.updated_region=transfer->region;
    }
  }
  if(result.updated_region.width){
    size_t offset=(size_t)(uint32_t)result.updated_region.y*transfer->stride_bytes+
                   (size_t)(uint32_t)result.updated_region.x*4u;
    selected=*transfer;selected.region=result.updated_region;
    if(checkpoint){ selected.bytes=(uint8_t*)transfer->bytes+offset;selected.byte_capacity-=offset; }
    success=scene_readback_pixels(backend,image,&selected,&result);
  }
  if(checkpoint){
    if(success) scene_checkpoint_remember(backend,image,checkpoint,transfer->pixel_format,
                                          result.updated_region.width!=0u);
    else checkpoint->image.content_version=0;
  }
  if(transfer->result) *transfer->result=result;
  return success;
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
  for(size_t index=0;index<2u;index++)
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
