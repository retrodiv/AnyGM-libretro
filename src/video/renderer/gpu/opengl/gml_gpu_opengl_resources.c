/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing OpenGL source mirrors and exact integer axis maps. */
#include "gml_gpu_opengl_internal.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>

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
         slot->origin==axis->origin && slot->extent==axis->extent;
}

int gml_gpu_opengl_upload_axis(GmlGpuBackend *backend,uint32_t slot_index,const GmlPlanAxis *axis,
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
  if(image->framebuffer) backend->gl.DeleteFramebuffers(1,&image->framebuffer);
  if(image->texture) backend->gl.DeleteTextures(1,&image->texture);
  memset(image,0,sizeof *image);
}

void gml_gpu_opengl_scene_forget(GmlGpuBackend *backend){
  GlScene *scene=&backend->scene;
  memset(scene->images,0,sizeof scene->images);
  scene->program=scene->vertex_buffer=scene->vertex_array=0;
  scene->exact_program=scene->snapshot_texture=0;
  scene->snapshot_width=scene->snapshot_height=0;
  scene->max_texture_size=0;
  scene->available=0;
}

void gml_gpu_opengl_scene_destroy(GmlGpuBackend *backend,int context_is_current){
  GlScene *scene=&backend->scene;
  if(context_is_current && backend->loaded){
    for(size_t index=0;index<GL_SCENE_MAX_IMAGES;index++)
      scene_delete_image(backend,&scene->images[index]);
    if(scene->program) backend->gl.DeleteProgram(scene->program);
    if(scene->exact_program) backend->gl.DeleteProgram(scene->exact_program);
    if(scene->snapshot_texture) backend->gl.DeleteTextures(1,&scene->snapshot_texture);
    if(scene->vertex_array) backend->gl.DeleteVertexArrays(1,&scene->vertex_array);
    if(scene->vertex_buffer) backend->gl.DeleteBuffers(1,&scene->vertex_buffer);
  }
  free(scene->vertices);
  free(scene->scratch);
  scene->vertices=NULL;
  scene->scratch=NULL;
  scene->scratch_capacity=0;
  gml_gpu_opengl_scene_forget(backend);
}

/* Optional scene entry points never make an otherwise working hybrid context fail adoption. */
void gml_gpu_opengl_scene_reset(GmlGpuBackend *backend,const GmlGpuContext *context){
  GmlGpuProc proc;
  GlScene *scene=&backend->scene;
  gml_gpu_opengl_scene_forget(backend);
#define GL_SCENE_LOAD(field,name) do { \
  proc=context->get_proc_address(context->userdata,name); \
  memcpy(&backend->gl.field,&proc,sizeof proc); \
} while(0)
  GL_SCENE_LOAD(GetIntegerv,"glGetIntegerv");
  GL_SCENE_LOAD(CopyTexSubImage2D,"glCopyTexSubImage2D");
  GL_SCENE_LOAD(BlendFuncSeparate,"glBlendFuncSeparate");
  GL_SCENE_LOAD(BlendEquationSeparate,"glBlendEquationSeparate");
  GL_SCENE_LOAD(Flush,"glFlush");
  GL_SCENE_LOAD(Finish,"glFinish");
#undef GL_SCENE_LOAD
  if(!backend->gl.GetIntegerv || !backend->gl.Finish) return;
  backend->gl.GetIntegerv(GL_MAX_TEXTURE_SIZE,&scene->max_texture_size);
  backend->gl.GetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&scene->max_texture_units);
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
                             (UINT64_C(1)<<GML_RENDER_OP_CONTENT_SHADER_DRAW);
  caps->supported_families=GML_RENDER_FAMILY_TEXTURE|GML_RENDER_FAMILY_PRIMITIVE|
                           GML_RENDER_FAMILY_EFFECT;
  caps->supports_rgb_alpha_separate=backend->gl.BlendFuncSeparate &&
                                     backend->gl.BlendEquationSeparate;
  caps->soft_cache_bytes=128u*1024u*1024u;
  return 1;
}

static GlSceneImage *scene_slot(GmlGpuBackend *backend,GmlRenderImageRef ref){
  GlSceneImage *free_slot=NULL,*oldest=NULL;
  for(size_t index=0;index<GL_SCENE_MAX_IMAGES;index++){
    GlSceneImage *candidate=&backend->scene.images[index];
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
    if(candidate->texture && candidate->valid && scene_same_version(candidate->ref,ref)){
      candidate->last_used=++backend->scene.use_clock;
      return candidate;
    }
  }
  return NULL;
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

int gml_gpu_opengl_scene_upload(GmlGpuBackend *backend,const GmlRenderImageUpload *transfer,
                                int *did_upload){
  GlScene *scene;
  GlSceneImage *slot;
  size_t row_bytes,bytes;
  const uint8_t *source;
  int first,resize,rendered_origin;
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
  if(first || resize){
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,(GLsizei)transfer->storage_width,
                           (GLsizei)transfer->storage_height,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  }
  backend->gl.TexSubImage2D(GL_TEXTURE_2D,0,transfer->region.x,
                            rendered_origin?(GLint)(transfer->storage_height-
                              (uint32_t)transfer->region.y-transfer->region.height)
                                           :transfer->region.y,
                            (GLsizei)transfer->region.width,(GLsizei)transfer->region.height,
                            GL_RGBA,GL_UNSIGNED_BYTE,scene->scratch);
  backend->gl.PixelStorei(GL_UNPACK_ALIGNMENT,4);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(backend->gl.GetError()!=GL_NO_ERROR){ slot->valid=0; return 0; }
  slot->ref=transfer->image;
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
  }
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(!slot->framebuffer){
    backend->gl.GenFramebuffers(1,&slot->framebuffer);
    if(!slot->framebuffer){ slot->valid=0; return NULL; }
  }
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,slot->framebuffer);
  backend->gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
                                   slot->texture,0);
  if(backend->gl.CheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE ||
     backend->gl.GetError()!=GL_NO_ERROR){ slot->valid=0; return NULL; }
  slot->ref=target->image;
  slot->width=target->width;
  slot->height=target->height;
  slot->pixel_format=target->pixel_format;
  slot->target=1;
  slot->last_used=++scene->use_clock;
  return slot;
}

int gml_gpu_opengl_scene_readback(GmlGpuBackend *backend,const GmlRenderImageReadback *transfer){
  GlSceneImage *image;
  GlScene *scene;
  size_t row_bytes,bytes;
  if(!backend || !transfer || !transfer->bytes || !backend->loaded) return 0;
  scene=&backend->scene;
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
  bytes=row_bytes*transfer->region.height;
  if(!scene_scratch_ready(scene,bytes)) return 0;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,image->framebuffer);
  backend->gl.PixelStorei(GL_PACK_ALIGNMENT,1);
  backend->gl.ReadPixels(transfer->region.x,
    (GLint)(image->height-(uint32_t)transfer->region.y-transfer->region.height),
    (GLsizei)transfer->region.width,(GLsizei)transfer->region.height,
    GL_RGBA,GL_UNSIGNED_BYTE,scene->scratch);
  backend->gl.PixelStorei(GL_PACK_ALIGNMENT,4);
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  for(uint32_t row=0;row<transfer->region.height;row++){
    const uint8_t *input=scene->scratch+(size_t)(transfer->region.height-1u-row)*row_bytes;
    uint8_t *output=(uint8_t*)transfer->bytes+(size_t)row*transfer->stride_bytes;
    if(transfer->pixel_format==GML_RENDER_PIXEL_RGBA8888){
      memcpy(output,input,row_bytes);
    } else for(uint32_t column=0;column<transfer->region.width;column++){
      const uint8_t *pixel=input+(size_t)column*4u;
      uint32_t packed=((uint32_t)(transfer->pixel_format==GML_RENDER_PIXEL_XRGB8888
                        ?255u:pixel[3])<<24)|((uint32_t)pixel[0]<<16)|
                        ((uint32_t)pixel[1]<<8)|pixel[2];
      memcpy(output+(size_t)column*4u,&packed,sizeof packed);
    }
  }
  return 1;
}

void gml_gpu_opengl_scene_retire(GmlGpuBackend *backend,GmlRenderImageRef image){
  int retire_incarnation=image.resource_class==GML_RENDER_RESOURCE_ATLAS ||
                     image.resource_class==GML_RENDER_RESOURCE_SCREEN ||
                     image.resource_class==GML_RENDER_RESOURCE_APPLICATION ||
                     image.resource_class==GML_RENDER_RESOURCE_SURFACE ||
                     image.resource_class==GML_RENDER_RESOURCE_SCRATCH ||
                     image.resource_class==GML_RENDER_RESOURCE_SHADER_SAMPLER;
  if(!backend || !backend->loaded) return;
  for(size_t index=0;index<GL_SCENE_MAX_IMAGES;index++){
    GlSceneImage *candidate=&backend->scene.images[index];
    /* A CPU handoff or runtime atlas growth can advance the version after its last device
     * upload. Retirement ends that incarnation even if the resident version is older. */
    if(candidate->texture && scene_same_logical(candidate->ref,image) &&
       (retire_incarnation || scene_same_version(candidate->ref,image))){
      scene_delete_image(backend,candidate);
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
