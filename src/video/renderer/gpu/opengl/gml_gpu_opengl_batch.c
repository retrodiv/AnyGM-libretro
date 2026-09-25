/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Bounded ordered submission for the first resident scene variants. Unsupported shapes are
 * rejected before writes; after a device error, the affected target is explicitly uncertain. */
#include "gml_gpu_opengl_internal.h"

#include <stdlib.h>
#include <string.h>

static int same_logical(GmlRenderImageRef a,GmlRenderImageRef b){
  return a.resource_class==b.resource_class && a.identity==b.identity &&
         a.incarnation==b.incarnation;
}

static int same_version(GmlRenderImageRef a,GmlRenderImageRef b){
  return same_logical(a,b) && a.content_version==b.content_version;
}

static int valid_region(GmlRenderRect region,uint32_t width,uint32_t height){
  return region.x>=0 && region.y>=0 && region.width && region.height &&
         (uint64_t)(uint32_t)region.x+region.width<=width &&
         (uint64_t)(uint32_t)region.y+region.height<=height;
}

static int full_region(GmlRenderRect region,uint32_t width,uint32_t height){
  return region.x==0 && region.y==0 && region.width==width && region.height==height;
}

static int existing_target(const GmlGpuBackend *backend,const GmlRenderTargetRef *target){
  for(size_t index=0;index<GL_SCENE_MAX_IMAGES;index++){
    const GlSceneImage *slot=&backend->scene.images[index];
    if(slot->target && slot->valid && slot->texture &&
       same_logical(slot->ref,target->image) && slot->width==target->width &&
       slot->height==target->height && slot->pixel_format==target->pixel_format) return 1;
  }
  return 0;
}

static int cleared_earlier(const GmlRenderImageRef *cleared,uint32_t count,
                           GmlRenderImageRef target){
  for(uint32_t index=0;index<count;index++)
    if(same_logical(cleared[index],target)) return 1;
  return 0;
}

static int scene_preflight(GmlGpuBackend *backend,const GmlRenderDrawBatch *batch){
  GmlRenderImageRef cleared[GL_SCENE_MAX_IMAGES];
  uint32_t clear_count=0;
  float probe[6*GL_SCENE_VERTEX_FLOATS];
  if(!backend || !batch || !batch->operations || batch->operation_count==0 ||
     batch->operation_count>8192u || !backend->loaded || !backend->scene.available) return 0;
  for(uint32_t index=0;index<batch->operation_count;index++){
    const GmlRenderDrawOp *op=&batch->operations[index];
    const GmlRenderTargetRef *target=&op->target;
    if(!target->image.incarnation || !target->image.content_version ||
       !target->storage_version ||
       target->width>(uint32_t)backend->scene.max_texture_size ||
       target->height>(uint32_t)backend->scene.max_texture_size ||
       !valid_region(target->region,target->width,target->height) ||
       target->pixel_format!=GML_RENDER_PIXEL_XRGB8888) return 0;
    if(op->opcode==GML_RENDER_OP_CLEAR){
      if(!full_region(target->region,target->width,target->height) &&
         !existing_target(backend,target) &&
         !cleared_earlier(cleared,clear_count,target->image)) return 0;
      if(full_region(target->region,target->width,target->height) &&
         !cleared_earlier(cleared,clear_count,target->image)){
        if(clear_count>=GL_SCENE_MAX_IMAGES) return 0;
        cleared[clear_count++]=target->image;
      }
      continue;
    }
    if(op->opcode!=GML_RENDER_OP_TEXTURED_QUAD || !batch->states ||
       op->state_index>=batch->state_count || op->source_count!=1 || !op->sources ||
       (!existing_target(backend,target) &&
        !cleared_earlier(cleared,clear_count,target->image))) return 0;
    {
      const GmlRenderDrawStateSnapshot *state=&batch->states[op->state_index];
      GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
      const GmlRenderSamplingPolicy *sampling=&op->payload.quad.sampling;
      if(!source || source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 ||
         same_logical(op->sources[0],target->image) ||
         state->write_mask!=15u || state->alpha!=255u ||
         state->colour!=0xFFFFFFFFu || state->alpha_test_enabled ||
         state->rgb_source_factor || state->rgb_destination_factor ||
         state->alpha_source_factor || state->alpha_destination_factor ||
         state->rgb_equation || state->alpha_equation ||
         state->sampler_count!=1u || state->material_identity || state->material_version ||
         state->coverage_policy ||
         !((state->clip.width==0 && state->clip.height==0) ||
           full_region(state->clip,target->width,target->height)) ||
         !full_region(target->region,target->width,target->height) ||
         !full_region(sampling->source_region,source->width,source->height) ||
         (sampling->filter!=GML_RENDER_FILTER_NEAREST &&
          sampling->filter!=GML_RENDER_FILTER_BILINEAR) ||
         sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
         sampling->quantisation ||
         state->samplers[0].filter!=sampling->filter ||
         state->samplers[0].edge_rule!=sampling->edge_rule ||
         !full_region(state->samplers[0].source_region,source->width,source->height) ||
         !gml_render_gpu_opengl_draw_textured_quad(op,state,probe)) return 0;
      for(uint32_t corner=0;corner<4;corner++)
        if((op->payload.quad.corner_colour[corner]>>24)!=255u) return 0;
    }
  }
  return 1;
}

static int scene_prepare_geometry(GmlGpuBackend *backend,char *error,size_t error_capacity){
  GlScene *scene=&backend->scene;
  if(!gml_gpu_opengl_scene_program(backend,error,error_capacity)) return 0;
  if(!scene->vertices){
    scene->vertices=(float*)malloc((size_t)GL_SCENE_MAX_QUADS*6u*GL_SCENE_VERTEX_FLOATS*
                                   sizeof(float));
    if(!scene->vertices) return 0;
  }
  if(!scene->vertex_array){
    backend->gl.GenVertexArrays(1,&scene->vertex_array);
    if(!scene->vertex_array) return 0;
  }
  if(!scene->vertex_buffer){
    backend->gl.GenBuffers(1,&scene->vertex_buffer);
    if(!scene->vertex_buffer) return 0;
  }
  return backend->gl.GetError()==GL_NO_ERROR;
}

static int flush_quads(GmlGpuBackend *backend,GlSceneImage *target,GlSceneImage *source,
                       uint32_t filter,uint32_t count){
  GlScene *scene=&backend->scene;
  GLfloat extent[2]={(GLfloat)target->width,(GLfloat)target->height};
  GLenum texture_filter=filter==GML_RENDER_FILTER_BILINEAR?GL_LINEAR:GL_NEAREST;
  GLsizei stride=GL_SCENE_VERTEX_FLOATS*(GLsizei)sizeof(float);
  if(!count) return 1;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  backend->gl.Viewport(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.Scissor(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,source->texture);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,(GLint)texture_filter);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,(GLint)texture_filter);
  backend->gl.UseProgram(scene->program);
  backend->gl.Uniform2fv(scene->u_extent,1,extent);
  backend->gl.Uniform1i(scene->u_image,0);
  backend->gl.BindVertexArray(scene->vertex_array);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,scene->vertex_buffer);
  backend->gl.BufferData(GL_ARRAY_BUFFER,
    (GLsizeiptr)((size_t)count*6u*GL_SCENE_VERTEX_FLOATS*sizeof(float)),
    scene->vertices,GL_STREAM_DRAW);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_position);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_texcoord);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_colour);
  backend->gl.VertexAttribPointer((GLuint)scene->a_position,2,GL_FLOAT,GL_FALSE,stride,(const void*)0);
  backend->gl.VertexAttribPointer((GLuint)scene->a_texcoord,2,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(2*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->a_colour,4,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(4*sizeof(float)));
  backend->gl.DrawArrays(GL_TRIANGLES,0,(GLsizei)(count*6u));
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_position);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_texcoord);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_colour);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,0);
  backend->gl.BindVertexArray(0);
  backend->gl.UseProgram(0);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(backend->gl.GetError()!=GL_NO_ERROR){ target->valid=0; return 0; }
  target->valid=1;
  return 1;
}

static void mark_uncertain(GmlRenderExecutionResult *result,GlSceneImage *target){
  result->status=GML_RENDER_EXEC_RECOVERABLE_ERROR;
  if(target){
    target->valid=0;
    result->uncertain_targets[0]=target->ref;
    result->uncertain_target_count=1;
  } else result->all_destinations_uncertain=1;
}

GmlRenderExecutionResult gml_gpu_opengl_scene_submit(GmlGpuBackend *backend,
                                                       const GmlRenderDrawBatch *batch,
                                                       uintptr_t frontend_framebuffer,
                                                       char *error,size_t error_capacity){
  GmlRenderExecutionResult result={0};
  GlSceneImage *active_target=NULL,*pending_source=NULL;
  uint32_t pending_filter=0,pending_count=0;
  result.status=GML_RENDER_EXEC_UNSUPPORTED;
  if(!scene_preflight(backend,batch)) return result;
  result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
  if(!scene_prepare_geometry(backend,error,error_capacity)) return result;
  backend->gl.Disable(GL_BLEND);
  backend->gl.Disable(GL_DEPTH_TEST);
  backend->gl.Disable(GL_STENCIL_TEST);
  backend->gl.Disable(GL_CULL_FACE);
  backend->gl.Disable(GL_DITHER);
  if(backend->api==GML_GPU_API_OPENGL_CORE) backend->gl.Disable(GL_FRAMEBUFFER_SRGB);
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  backend->gl.DepthMask(GL_FALSE);
  backend->gl.Enable(GL_SCISSOR_TEST);
  for(uint32_t index=0;index<batch->operation_count;index++){
    const GmlRenderDrawOp *op=&batch->operations[index];
    GlSceneImage *target;
    if(pending_count && (op->opcode==GML_RENDER_OP_CLEAR ||
       !active_target || !same_logical(active_target->ref,op->target.image) ||
       pending_count==GL_SCENE_MAX_QUADS ||
       !pending_source || !same_version(pending_source->ref,op->sources[0]) ||
       pending_filter!=op->payload.quad.sampling.filter)){
      if(!flush_quads(backend,active_target,pending_source,pending_filter,pending_count)){
        mark_uncertain(&result,active_target);
        break;
      }
      pending_count=0;
      result.accepted_prefix=index;
    }
    if(active_target && same_logical(active_target->ref,op->target.image) &&
       active_target->width==op->target.width && active_target->height==op->target.height &&
       active_target->pixel_format==op->target.pixel_format){
      target=active_target;
      target->ref=op->target.image;
    } else {
      target=gml_gpu_opengl_scene_target(backend,&op->target);
    }
    if(!target){
      result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
      break;
    }
    active_target=target;
    if(op->opcode==GML_RENDER_OP_CLEAR){
      if(!gml_render_gpu_opengl_clear(backend,op,target)){
        mark_uncertain(&result,target);
        break;
      }
      target->valid=1;
      result.accepted_prefix=index+1u;
      continue;
    }
    pending_source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
    if(!pending_source){
      result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
      break;
    }
    pending_filter=op->payload.quad.sampling.filter;
    if(!gml_render_gpu_opengl_draw_textured_quad(op,&batch->states[op->state_index],
       backend->scene.vertices+(size_t)pending_count*6u*GL_SCENE_VERTEX_FLOATS)){
      result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
      break;
    }
    pending_count++;
  }
  if(result.status==GML_RENDER_EXEC_RESOURCE_ERROR &&
     result.accepted_prefix+pending_count==batch->operation_count){
    if(!flush_quads(backend,active_target,pending_source,pending_filter,pending_count))
      mark_uncertain(&result,active_target);
    else {
      result.status=GML_RENDER_EXEC_ACCEPTED;
      result.accepted_prefix=batch->operation_count;
    }
  }
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,0);
  backend->gl.BindVertexArray(0);
  backend->gl.UseProgram(0);
  backend->gl.Disable(GL_SCISSOR_TEST);
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,(GLuint)frontend_framebuffer);
  if(backend->gl.GetError()!=GL_NO_ERROR) mark_uncertain(&result,NULL);
  return result;
}
