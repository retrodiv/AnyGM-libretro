/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Bounded ordered submission for the first resident scene variants. Unsupported shapes are
 * rejected before writes; after a device error, the affected target is explicitly uncertain. */
#include "gml_gpu_opengl_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

enum { GL_SCENE_MAX_EXACT_REPETITIONS=64 };

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
    /* An uploaded, valid image can become a target when scene_target attaches its FBO.
     * Requiring an existing FBO here rejected the first draw over a CPU-current surface. */
    if(slot->valid && slot->texture &&
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

static int qualified_blend(const GmlRenderDrawStateSnapshot *state){
  if(!state->rgb_source_factor && !state->rgb_destination_factor &&
     !state->alpha_source_factor && !state->alpha_destination_factor &&
     !state->rgb_equation && !state->alpha_equation) return 0;
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA){
    if(state->rgb_equation==GML_RENDER_BLEND_ADD &&
       state->alpha_equation==GML_RENDER_BLEND_ADD) return 1;
    if(state->rgb_equation==GML_RENDER_BLEND_EXACT8_TRUNC &&
       state->alpha_equation==GML_RENDER_BLEND_EXACT8_TRUNC) return 2;
  }
  return -1;
}

static int exact_rect(const GmlRenderDrawOp *op,GLint *left,GLint *bottom,
                      GLsizei *width,GLsizei *height){
  const GmlRenderQuad *quad=&op->payload.quad;
  double x0=quad->x[0],x1=quad->x[1],y0=quad->y[0],y1=quad->y[2];
  if(!isfinite(x0) || !isfinite(x1) || !isfinite(y0) || !isfinite(y1) ||
     x0!=quad->x[3] || x1!=quad->x[2] || y0!=quad->y[1] || y1!=quad->y[3] ||
     x0<0.0 || y0<0.0 || x1<=x0 || y1<=y0 ||
     x1>(double)INT_MAX || y1>(double)INT_MAX ||
     x1>(double)op->target.width || y1>(double)op->target.height ||
     x0!=(double)(GLint)x0 || y0!=(double)(GLint)y0 ||
     x1!=(double)(GLint)x1 || y1!=(double)(GLint)y1) return 0;
  if(left) *left=(GLint)x0;
  if(bottom) *bottom=(GLint)op->target.height-(GLint)y1;
  if(width) *width=(GLsizei)(x1-x0);
  if(height) *height=(GLsizei)(y1-y0);
  return 1;
}

static int same_resolved_quad(const GmlRenderQuad *a,const GmlRenderQuad *b){
  if(a->shape!=b->shape || a->sampling.filter!=b->sampling.filter ||
     a->sampling.edge_rule!=b->sampling.edge_rule ||
     a->sampling.quantisation!=b->sampling.quantisation ||
     a->sampling.source_region.x!=b->sampling.source_region.x ||
     a->sampling.source_region.y!=b->sampling.source_region.y ||
     a->sampling.source_region.width!=b->sampling.source_region.width ||
     a->sampling.source_region.height!=b->sampling.source_region.height) return 0;
  for(uint32_t corner=0;corner<4u;corner++)
    if(a->x[corner]!=b->x[corner] || a->y[corner]!=b->y[corner] ||
       a->u[corner]!=b->u[corner] || a->v[corner]!=b->v[corner] ||
       a->corner_colour[corner]!=b->corner_colour[corner]) return 0;
  return 1;
}

/* A repeated operation has no intervening observer or state change. Folding only identical
 * resolved geometry and the same state snapshot retains each integer-truncated overlap. */
static uint32_t exact_repetitions(const GmlRenderDrawBatch *batch,uint32_t first){
  const GmlRenderDrawOp *base=&batch->operations[first];
  uint32_t count=1u;
  while(count<GL_SCENE_MAX_EXACT_REPETITIONS && first+count<batch->operation_count){
    const GmlRenderDrawOp *previous=&batch->operations[first+count-1u];
    const GmlRenderDrawOp *next=&batch->operations[first+count];
    if(next->opcode!=GML_RENDER_OP_TEXTURED_QUAD || next->source_count!=1u ||
       next->state_index!=base->state_index ||
       next->reconstruction_mode!=base->reconstruction_mode ||
       !same_version(next->sources[0],base->sources[0]) ||
       !same_logical(next->target.image,base->target.image) ||
       next->target.width!=base->target.width ||
       next->target.height!=base->target.height ||
       next->target.pixel_format!=base->target.pixel_format ||
       next->target.storage_version!=base->target.storage_version ||
       previous->target.image.content_version==UINT64_MAX ||
       next->target.image.content_version!=previous->target.image.content_version+1u ||
       !same_resolved_quad(&next->payload.quad,&base->payload.quad))
      break;
    count++;
  }
  return count;
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
       (target->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
        target->pixel_format!=GML_RENDER_PIXEL_ARGB8888)) return 0;
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
    if(op->opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW){
      const GmlRenderDrawStateSnapshot *state;
      const GmlPlanShader *shader=op->payload.shader;
      GlSceneImage *source;
      if(op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
         !batch->states || op->state_index>=batch->state_count ||
         op->source_count!=1u || !op->sources || !shader ||
         !shader->vertex_es || !shader->fragment_es || shader->sampler_count ||
         !full_region(target->region,target->width,target->height) ||
         (!existing_target(backend,target) &&
          !cleared_earlier(cleared,clear_count,target->image))) return 0;
      state=&batch->states[op->state_index];
      source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
      if(!source || same_logical(source->ref,target->image) ||
         (source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
          source->pixel_format!=GML_RENDER_PIXEL_ARGB8888) ||
         state->write_mask!=15u || state->alpha!=255u ||
         state->colour!=0xFFFFFFFFu || state->alpha_test_enabled ||
         qualified_blend(state)!=0 || state->coverage_policy ||
         state->sampler_count!=1u ||
         (state->samplers[0].filter!=GML_RENDER_FILTER_NEAREST &&
          state->samplers[0].filter!=GML_RENDER_FILTER_BILINEAR) ||
         state->samplers[0].edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
         state->samplers[0].quantisation ||
         !full_region(state->samplers[0].source_region,source->width,source->height) ||
         !((state->clip.width==0 && state->clip.height==0) ||
           full_region(state->clip,target->width,target->height))) return 0;
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
      int blend=qualified_blend(state);
      if(!source || (source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
                     source->pixel_format!=GML_RENDER_PIXEL_ARGB8888) ||
         same_logical(op->sources[0],target->image) ||
         state->write_mask!=15u || state->alpha!=255u ||
         state->colour!=0xFFFFFFFFu || state->alpha_test_enabled ||
         blend<0 || (blend==1 && (!backend->gl.BlendFuncSeparate ||
                                 !backend->gl.BlendEquationSeparate)) ||
         (blend==2 && (!backend->gl.CopyTexSubImage2D ||
                       target->pixel_format!=GML_RENDER_PIXEL_XRGB8888 ||
                       sampling->filter!=GML_RENDER_FILTER_NEAREST ||
                       !exact_rect(op,NULL,NULL,NULL,NULL))) ||
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
         !gml_render_gpu_opengl_draw_textured_quad(op,state,
                                                   source->rendered_origin,probe)) return 0;
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
                       uint32_t filter,int blend,uint32_t count){
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
  if(blend){
    backend->gl.BlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,
                                  GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    backend->gl.BlendEquationSeparate(GL_FUNC_ADD,GL_FUNC_ADD);
    backend->gl.Enable(GL_BLEND);
  } else backend->gl.Disable(GL_BLEND);
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
  backend->gl.Disable(GL_BLEND);
  if(backend->gl.GetError()!=GL_NO_ERROR){ target->valid=0; return 0; }
  target->valid=1;
  target->rendered_origin=1;
  return 1;
}

/* Returns 1 on success, 0 before any target write, and -1 if the draw may have modified it. The
 * snapshot is only the affected rectangle, never the entire surface per overlapping sprite. */
static int draw_exact_quad(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
                           const GmlRenderDrawStateSnapshot *state,
                           GlSceneImage *target,GlSceneImage *source,
                           uint32_t repetitions,
                           char *error,size_t error_capacity){
  GlScene *scene=&backend->scene;
  GLint left,bottom,origin[2];
  GLsizei width,height,stride=GL_SCENE_VERTEX_FLOATS*(GLsizei)sizeof(float);
  GLfloat extent[2]={(GLfloat)target->width,(GLfloat)target->height};
  int outcome;
  if(!exact_rect(op,&left,&bottom,&width,&height) ||
     !gml_gpu_opengl_scene_exact_program(backend,error,error_capacity) ||
     !gml_render_gpu_opengl_draw_textured_quad(op,state,source->rendered_origin,
                                               scene->vertices)) return 0;
  if(!scene->snapshot_texture){
    backend->gl.GenTextures(1,&scene->snapshot_texture);
    if(!scene->snapshot_texture) return 0;
  }
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  backend->gl.ActiveTexture(GL_TEXTURE1);
  backend->gl.BindTexture(GL_TEXTURE_2D,scene->snapshot_texture);
  if(scene->snapshot_width<(uint32_t)width || scene->snapshot_height<(uint32_t)height){
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,
                           GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    scene->snapshot_width=(uint32_t)width;
    scene->snapshot_height=(uint32_t)height;
  }
  backend->gl.CopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,left,bottom,width,height);
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  backend->gl.Disable(GL_BLEND);
  backend->gl.Viewport(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.Scissor(left,bottom,width,height);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,source->texture);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  backend->gl.UseProgram(scene->exact_program);
  backend->gl.Uniform2fv(scene->exact_u_extent,1,extent);
  backend->gl.Uniform1i(scene->exact_u_image,0);
  backend->gl.Uniform1i(scene->exact_u_previous,1);
  backend->gl.Uniform1i(scene->exact_u_repetitions,(GLint)repetitions);
  origin[0]=left;
  origin[1]=bottom;
  backend->gl.Uniform2iv(scene->exact_u_region_origin,1,origin);
  backend->gl.BindVertexArray(scene->vertex_array);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,scene->vertex_buffer);
  backend->gl.BufferData(GL_ARRAY_BUFFER,6u*GL_SCENE_VERTEX_FLOATS*sizeof(float),
                         scene->vertices,GL_STREAM_DRAW);
  backend->gl.EnableVertexAttribArray((GLuint)scene->exact_a_position);
  backend->gl.EnableVertexAttribArray((GLuint)scene->exact_a_texcoord);
  backend->gl.EnableVertexAttribArray((GLuint)scene->exact_a_colour);
  backend->gl.VertexAttribPointer((GLuint)scene->exact_a_position,2,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)0);
  backend->gl.VertexAttribPointer((GLuint)scene->exact_a_texcoord,2,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(2*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->exact_a_colour,4,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(4*sizeof(float)));
  if(backend->gl.GetError()!=GL_NO_ERROR){ outcome=0; goto cleanup; }
  backend->gl.DrawArrays(GL_TRIANGLES,0,6);
  outcome=backend->gl.GetError()==GL_NO_ERROR?1:-1;
cleanup:
  backend->gl.DisableVertexAttribArray((GLuint)scene->exact_a_position);
  backend->gl.DisableVertexAttribArray((GLuint)scene->exact_a_texcoord);
  backend->gl.DisableVertexAttribArray((GLuint)scene->exact_a_colour);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,0);
  backend->gl.BindVertexArray(0);
  backend->gl.UseProgram(0);
  backend->gl.ActiveTexture(GL_TEXTURE1);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  if(backend->gl.GetError()!=GL_NO_ERROR) outcome=-1;
  if(outcome>0){ target->valid=1; target->rendered_origin=1; }
  return outcome;
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
  int pending_blend=0;
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
    if(pending_count && (op->opcode!=GML_RENDER_OP_TEXTURED_QUAD ||
       !active_target || !same_logical(active_target->ref,op->target.image) ||
       pending_count==GL_SCENE_MAX_QUADS ||
       !pending_source || !same_version(pending_source->ref,op->sources[0]) ||
       pending_filter!=op->payload.quad.sampling.filter ||
       pending_blend!=qualified_blend(&batch->states[op->state_index]) ||
       qualified_blend(&batch->states[op->state_index])==2)){
      if(!flush_quads(backend,active_target,pending_source,pending_filter,
                      pending_blend,pending_count)){
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
      target->rendered_origin=1;
      result.accepted_prefix=index+1u;
      continue;
    }
    if(op->opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW){
      GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
      if(!source || !gml_gpu_opengl_scene_shader_draw(backend,op->payload.shader,
          &batch->states[op->state_index],target,source,error,error_capacity)){
        mark_uncertain(&result,target);
        break;
      }
      target->valid=1;
      target->rendered_origin=1;
      result.accepted_prefix=index+1u;
      continue;
    }
    pending_source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
    if(!pending_source){
      result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
      break;
    }
    pending_filter=op->payload.quad.sampling.filter;
    pending_blend=qualified_blend(&batch->states[op->state_index]);
    if(pending_blend==2){
      uint32_t repetitions=exact_repetitions(batch,index);
      int drawn=draw_exact_quad(backend,op,&batch->states[op->state_index],
                                target,pending_source,repetitions,error,error_capacity);
      if(drawn<0) mark_uncertain(&result,target);
      else if(!drawn) result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
      if(drawn<=0) break;
      target->ref=batch->operations[index+repetitions-1u].target.image;
      result.accepted_prefix=index+repetitions;
      pending_count=0;
      index+=repetitions-1u;
      continue;
    }
    if(!gml_render_gpu_opengl_draw_textured_quad(op,&batch->states[op->state_index],
       pending_source->rendered_origin,
       backend->scene.vertices+(size_t)pending_count*6u*GL_SCENE_VERTEX_FLOATS)){
      result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
      break;
    }
    pending_count++;
  }
  if(result.status==GML_RENDER_EXEC_RESOURCE_ERROR &&
     result.accepted_prefix+pending_count==batch->operation_count){
    if(!flush_quads(backend,active_target,pending_source,pending_filter,
                    pending_blend,pending_count))
      mark_uncertain(&result,active_target);
    else {
      result.status=GML_RENDER_EXEC_ACCEPTED;
      result.accepted_prefix=batch->operation_count;
    }
  }
  backend->gl.ActiveTexture(GL_TEXTURE1);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
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
