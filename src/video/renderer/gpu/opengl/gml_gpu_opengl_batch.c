/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Bounded ordered submission for the first resident scene variants. Unsupported shapes are
 * rejected before writes; after a device error, the affected target is explicitly uncertain. */
#include "gml_gpu_opengl_internal.h"

#include <limits.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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

static int same_region(GmlRenderRect a,GmlRenderRect b){
  return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
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
  if(state->rgb_source_factor==GML_RENDER_BLEND_ONE &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->rgb_equation==GML_RENDER_BLEND_ADD &&
     state->alpha_equation==GML_RENDER_BLEND_ADD) return 9;
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ZERO &&
     state->rgb_equation==GML_RENDER_BLEND_EXACT_STRETCHED_FLOAT &&
     state->alpha_equation==GML_RENDER_BLEND_EXACT_STRETCHED_FLOAT) return 8;
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->rgb_equation==GML_RENDER_BLEND_EXACT_AUTOMATIC_SURFACE &&
     state->alpha_equation==GML_RENDER_BLEND_EXACT_AUTOMATIC_SURFACE) return 7;
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     (state->alpha_source_factor==GML_RENDER_BLEND_ONE ||
      state->alpha_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA) &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->rgb_equation==GML_RENDER_BLEND_EXACT_FILTERED_FONT &&
     state->alpha_equation==GML_RENDER_BLEND_EXACT_FILTERED_FONT) return 6;
  if(!state->rgb_source_factor && !state->rgb_destination_factor &&
     !state->alpha_source_factor && !state->alpha_destination_factor &&
     !state->rgb_equation && !state->alpha_equation) return 0;
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA){
    /* First-generation application surfaces retain source coverage squared in alpha. */
    if(state->alpha_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
       state->rgb_equation==GML_RENDER_BLEND_ADD &&
       state->alpha_equation==GML_RENDER_BLEND_ADD) return 3;
    if(state->alpha_source_factor!=GML_RENDER_BLEND_ONE) return -1;
    if(state->rgb_equation==GML_RENDER_BLEND_ADD &&
       state->alpha_equation==GML_RENDER_BLEND_ADD) return 1;
    if(state->rgb_equation==GML_RENDER_BLEND_EXACT8_TRUNC &&
       state->alpha_equation==GML_RENDER_BLEND_EXACT8_TRUNC) return 2;
    if(state->rgb_equation==GML_RENDER_BLEND_EXACT8_ROUND_VEIL &&
       state->alpha_equation==GML_RENDER_BLEND_EXACT8_ROUND_VEIL) return 5;
  }
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ZERO &&
     state->rgb_equation==GML_RENDER_BLEND_EXACT8_ADDITIVE &&
     state->alpha_equation==GML_RENDER_BLEND_EXACT8_ADDITIVE &&
     isfinite(state->additive_draw_alpha) &&
     state->additive_draw_alpha>0.0 && state->additive_draw_alpha<=1.0) return 4;
  return -1;
}

static uint32_t scene_filter(const GmlRenderQuad *quad,
                             const GmlRenderDrawStateSnapshot *state){
  if(quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR) return 18u;
  if(qualified_blend(state)==8) return 14u;
  if(state->coverage_policy==GML_RENDER_COVERAGE_ROTATED_VECTOR_ALPHA) return 17u;
  if(state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA)
    return quad->shape==GML_RENDER_QUAD_AXIS_ALIGNED?12u:15u;
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_STRETCHED_FLOAT) return 11u;
  if(quad->sampling.filter==GML_RENDER_FILTER_BOX) return 4u;
  if(quad->sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED) return 7u;
  if(quad->sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED_COPY) return 8u;
  if(quad->sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA) return 9u;
  if(quad->shape==GML_RENDER_QUAD_WARPED)
    return quad->sampling.filter==GML_RENDER_FILTER_BILINEAR?6u:5u;
  return quad->shape==GML_RENDER_QUAD_AXIS_ALIGNED
    ?(quad->sampling.filter==GML_RENDER_FILTER_BILINEAR?1u:0u)
    :(quad->sampling.filter==GML_RENDER_FILTER_BILINEAR?2u:3u);
}

static int exact_rect(const GmlRenderDrawOp *op,GLint *left,GLint *bottom,
                      GLsizei *width,GLsizei *height,int allow_clip){
  const GmlRenderQuad *quad=&op->payload.quad;
  double x0=quad->x[0],x1=quad->x[1],y0=quad->y[0],y1=quad->y[2];
  if(!isfinite(x0) || !isfinite(x1) || !isfinite(y0) || !isfinite(y1) ||
     x0!=quad->x[3] || x1!=quad->x[2] || y0!=quad->y[1] || y1!=quad->y[3] ||
     x0<(double)INT_MIN || y0<(double)INT_MIN ||
     x1>(double)INT_MAX || y1>(double)INT_MAX ||
     x1<=x0 || y1<=y0 ||
     (!allow_clip && (x0<0.0 || y0<0.0 ||
       x1>(double)op->target.width || y1>(double)op->target.height)) ||
     x0!=(double)(GLint)x0 || y0!=(double)(GLint)y0 ||
     x1!=(double)(GLint)x1 || y1!=(double)(GLint)y1) return 0;
  if(allow_clip){
    x0=fmax(0.0,x0); y0=fmax(0.0,y0);
    x1=fmin((double)op->target.width,x1);
    y1=fmin((double)op->target.height,y1);
    if(x1<=x0 || y1<=y0) return 0;
  }
  if(left) *left=(GLint)x0;
  if(bottom) *bottom=(GLint)op->target.height-(GLint)y1;
  if(width) *width=(GLsizei)(x1-x0);
  if(height) *height=(GLsizei)(y1-y0);
  return 1;
}

static int same_resolved_quad(const GmlRenderQuad *a,const GmlRenderQuad *b){
  if(a->shape!=b->shape || a->sampling.filter!=b->sampling.filter ||
     a->source_opaque!=b->source_opaque ||
     a->sampling.edge_rule!=b->sampling.edge_rule ||
     a->sampling.quantisation!=b->sampling.quantisation ||
     a->sampling.logical_margin_mask!=b->sampling.logical_margin_mask ||
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
    if(op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE){
      if(op->reconstruction_mode!=GML_RENDER_REPLAY_EXACT ||
         op->source_count || op->sources ||
         !full_region(target->region,target->width,target->height) ||
         !valid_region(op->payload.primitive.region,target->width,target->height) ||
         (op->payload.primitive.colour>>24)!=255u ||
         (!existing_target(backend,target) &&
          !cleared_earlier(cleared,clear_count,target->image))) return 0;
      continue;
    }
    if(op->opcode==GML_RENDER_OP_LAYER_EFFECT){
      const GmlRenderDrawStateSnapshot *state;
      GlSceneImage *source;
      if(op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
         op->scene_family!=GML_RENDER_SCENE_EFFECT ||
         !backend->gl.CopyTexSubImage2D || !batch->states ||
         op->state_index>=batch->state_count || op->source_count!=1u ||
         !op->sources || target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
         !full_region(target->region,target->width,target->height) ||
         (!existing_target(backend,target) &&
          !cleared_earlier(cleared,clear_count,target->image)) ||
         (op->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_TINT &&
          op->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_COLOURISE &&
          op->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_ZOOM_BLUR &&
          op->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_RGB_NOISE) ||
         !isfinite(op->payload.effect.intensity)) return 0;
      if(op->payload.effect.kind==GML_RENDER_LAYER_EFFECT_ZOOM_BLUR &&
         (!isfinite(op->payload.effect.centre[0]) ||
          !isfinite(op->payload.effect.centre[1]) ||
          !isfinite(op->payload.effect.focus_radius) ||
          op->payload.effect.centre[0]<0.0 || op->payload.effect.centre[0]>1.0 ||
          op->payload.effect.centre[1]<0.0 || op->payload.effect.centre[1]>1.0 ||
          op->payload.effect.intensity<0.0 || op->payload.effect.intensity>1.0 ||
          op->payload.effect.focus_radius<0.0 ||
          op->payload.effect.focus_radius>8192.0 ||
          op->payload.effect.linear>1u)) return 0;
      if(op->payload.effect.kind==GML_RENDER_LAYER_EFFECT_RGB_NOISE &&
         (op->payload.effect.intensity<=0.0 ||
          op->payload.effect.intensity>1.0)) return 0;
      state=&batch->states[op->state_index];
      source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
      if(!source || !source->texture || source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
         same_logical(source->ref,target->image) ||
         source->width!=target->width || source->height!=target->height ||
         state->write_mask!=15u || state->alpha!=255u ||
         state->colour!=0xFFFFFFFFu || state->alpha_test_enabled ||
         qualified_blend(state)!=0 || state->coverage_policy ||
         state->material_identity || state->material_version ||
         state->sampler_count!=1u ||
         state->samplers[0].filter!=GML_RENDER_FILTER_NEAREST ||
         state->samplers[0].edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
         state->samplers[0].quantisation ||
         !full_region(state->samplers[0].source_region,source->width,source->height) ||
         !((state->clip.width==0 && state->clip.height==0) ||
           full_region(state->clip,target->width,target->height))) return 0;
      continue;
    }
    if(op->opcode==GML_RENDER_OP_CONTENT_SHADER_DRAW){
      const GmlRenderDrawStateSnapshot *state;
      const GmlPlanShader *shader=op->payload.shader;
      GlSceneImage *source;
      if(op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
         !batch->states || op->state_index>=batch->state_count ||
         !op->sources || !shader ||
         !shader->vertex_es || !shader->fragment_es ||
         shader->sampler_count>GML_PLAN_MAX_SAMPLERS ||
         op->source_count!=1u+shader->sampler_count ||
         shader->uniform_count>GML_PLAN_MAX_UNIFORMS ||
         backend->scene.max_texture_units<=((int)shader->sampler_count) ||
         (!existing_target(backend,target) &&
          !cleared_earlier(cleared,clear_count,target->image))) return 0;
      state=&batch->states[op->state_index];
      source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
      if(!source || same_logical(source->ref,target->image) ||
         (source->rendered_origin && !backend->gl.BlitFramebuffer) ||
         (source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
          source->pixel_format!=GML_RENDER_PIXEL_ARGB8888) ||
         state->write_mask!=15u || state->alpha!=255u ||
         state->colour!=0xFFFFFFFFu || state->alpha_test_enabled ||
         (qualified_blend(state)!=0 && qualified_blend(state)!=1) ||
         (qualified_blend(state)==1 && (!backend->gl.BlendFuncSeparate ||
                                         !backend->gl.BlendEquationSeparate)) ||
         state->coverage_policy ||
         state->sampler_count!=1u ||
         (state->samplers[0].filter!=GML_RENDER_FILTER_NEAREST &&
          state->samplers[0].filter!=GML_RENDER_FILTER_BILINEAR) ||
         state->samplers[0].edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
         state->samplers[0].quantisation ||
         !full_region(state->samplers[0].source_region,source->width,source->height) ||
         !((state->clip.width==0 && state->clip.height==0) ||
           full_region(state->clip,target->width,target->height))) return 0;
      for(uint32_t bound=0;bound<shader->sampler_count;bound++){
        GlSceneImage *sampler=gml_gpu_opengl_scene_image(backend,op->sources[bound+1u]);
        if(!sampler || !sampler->texture ||
           (sampler->rendered_origin && !backend->gl.BlitFramebuffer) ||
           same_logical(sampler->ref,target->image) ||
           (sampler->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
            sampler->pixel_format!=GML_RENDER_PIXEL_ARGB8888)) return 0;
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
      int blend=qualified_blend(state);
      if(!source || (source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
                     source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
                     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888) ||
         same_logical(op->sources[0],target->image) ||
         state->write_mask!=15u || state->alpha!=255u ||
         state->colour!=0xFFFFFFFFu || state->alpha_test_enabled ||
         blend<0 || ((blend==1 || blend==9) && (!backend->gl.BlendFuncSeparate ||
                                 !backend->gl.BlendEquationSeparate)) ||
         (blend==9 && (sampling->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR ||
                       sampling->quantisation || sampling->logical_margin_mask ||
                       sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
                       op->payload.quad.shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
                       op->scene_family!=GML_RENDER_SCENE_SPRITE ||
                       op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
                       source->rendered_origin || source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
                       op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
                       !isfinite(op->payload.quad.filtered_draw_alpha) ||
                       op->payload.quad.filtered_draw_alpha<=0.0 ||
                       op->payload.quad.filtered_draw_alpha>1.0)) ||
         (sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR && blend!=9) ||
         (blend==8 && (!backend->gl.CopyTexSubImage2D ||
                       op->scene_family!=GML_RENDER_SCENE_SPRITE ||
                       target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                       op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
                       source->rendered_origin ||
                       sampling->quantisation!=GML_RENDER_QUANTISE_STRETCHED_FLOAT ||
                       sampling->filter!=GML_RENDER_FILTER_BILINEAR ||
                       op->payload.quad.shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
                       op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
                       !isfinite(op->payload.quad.filtered_draw_alpha) ||
                       op->payload.quad.filtered_draw_alpha<0x1p-64 ||
                       op->payload.quad.filtered_draw_alpha>1.0 ||
                       !exact_rect(op,NULL,NULL,NULL,NULL,1))) ||
         (blend==2 && (!backend->gl.CopyTexSubImage2D ||
                       target->pixel_format!=GML_RENDER_PIXEL_XRGB8888 ||
                       sampling->filter!=GML_RENDER_FILTER_NEAREST ||
                       !exact_rect(op,NULL,NULL,NULL,NULL,0))) ||
         (blend==4 && (!backend->gl.CopyTexSubImage2D ||
                       op->scene_family!=GML_RENDER_SCENE_BACKGROUND ||
                       target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                       sampling->filter!=GML_RENDER_FILTER_NEAREST ||
                       !exact_rect(op,NULL,NULL,NULL,NULL,1))) ||
         (blend==5 && (!backend->gl.CopyTexSubImage2D ||
                       op->scene_family!=GML_RENDER_SCENE_SPRITE ||
                       target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                       source->width!=1u || source->height!=1u ||
                       sampling->filter!=GML_RENDER_FILTER_NEAREST ||
                       !full_region(sampling->source_region,1u,1u) ||
                       op->payload.quad.x[0]!=0.0 || op->payload.quad.y[0]!=0.0 ||
                       op->payload.quad.x[1]!=(double)target->width ||
                       op->payload.quad.y[2]!=(double)target->height ||
                       !exact_rect(op,NULL,NULL,NULL,NULL,0))) ||
         state->sampler_count!=1u || state->material_identity || state->material_version ||
         (state->coverage_policy &&
          ((state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
            state->coverage_policy!=GML_RENDER_COVERAGE_ROTATED_VECTOR_ALPHA) ||
           target->image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
           target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
           op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
           source->rendered_origin ||
           (op->scene_family!=GML_RENDER_SCENE_SPRITE &&
            op->scene_family!=GML_RENDER_SCENE_BACKGROUND &&
            op->scene_family!=GML_RENDER_SCENE_TILE) ||
           sampling->quantisation ||
           !((state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
              sampling->filter==GML_RENDER_FILTER_BILINEAR &&
              op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED) ||
             (sampling->filter==GML_RENDER_FILTER_NEAREST &&
              backend->gl.CopyTexSubImage2D && exact_rect(op,NULL,NULL,NULL,NULL,1) &&
              op->scene_family==GML_RENDER_SCENE_SPRITE &&
              (op->payload.quad.shape==GML_RENDER_QUAD_CARDINAL ||
               op->payload.quad.shape==GML_RENDER_QUAD_GENERAL) &&
              (op->payload.quad.x[1]-op->payload.quad.x[0])*
                (op->payload.quad.y[2]-op->payload.quad.y[0])<262144.0)) ||
           !isfinite(op->payload.quad.filtered_draw_alpha) ||
           op->payload.quad.filtered_draw_alpha<=0.0 ||
           op->payload.quad.filtered_draw_alpha>1.0 ||
           op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
           (state->coverage_policy==GML_RENDER_COVERAGE_ROTATED_VECTOR_ALPHA
             ?blend!=1:(blend!=0 && blend!=3)))) ||
         !((state->clip.width==0 && state->clip.height==0) ||
           full_region(state->clip,target->width,target->height)) ||
         !full_region(target->region,target->width,target->height) ||
         !valid_region(sampling->source_region,source->width,source->height) ||
         (sampling->filter!=GML_RENDER_FILTER_NEAREST &&
          sampling->filter!=GML_RENDER_FILTER_BILINEAR &&
          sampling->filter!=GML_RENDER_FILTER_BOX &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_COPY &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA &&
          sampling->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR) ||
         ((sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED ||
           sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_COPY ||
           sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA) &&
          (op->payload.quad.shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
           op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
           sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE)) ||
         (sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA &&
          (op->scene_family!=GML_RENDER_SCENE_SPRITE || blend!=1 ||
           target->pixel_format!=GML_RENDER_PIXEL_ARGB8888)) ||
         (sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE &&
          sampling->edge_rule!=GML_RENDER_EDGE_LOGICAL_TPAG &&
          sampling->edge_rule!=GML_RENDER_EDGE_TRANSPARENT) ||
         (blend==6 && (!backend->gl.CopyTexSubImage2D || source->rendered_origin ||
                       (sampling->quantisation==GML_RENDER_QUANTISE_SPRITE_FIRSTGEN_INDEPENDENT
                         ? op->scene_family!=GML_RENDER_SCENE_SPRITE ||
                           target->image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
                           target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                           state->alpha_source_factor!=GML_RENDER_BLEND_ONE ||
                           op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT
                         : op->scene_family!=GML_RENDER_SCENE_TEXT ||
                           op->reconstruction_mode!=GML_RENDER_REPLAY_EXACT) ||
                       source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
                       op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
                       !gml_render_filtered_font_quad_valid(&op->payload.quad,
                                                            target->width,target->height) ||
                       !exact_rect(op,NULL,NULL,NULL,NULL,1))) ||
         (blend==7 && (!backend->gl.CopyTexSubImage2D ||
                       op->scene_family!=GML_RENDER_SCENE_SURFACE ||
                       target->image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
                       target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                       source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                       op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION ||
                       !full_region(sampling->source_region,source->width,source->height) ||
                       op->reconstruction_mode!=GML_RENDER_REPLAY_EXACT ||
                       !gml_render_automatic_surface_quad_valid(&op->payload.quad,
                                                                 target->width,target->height) ||
                       !exact_rect(op,NULL,NULL,NULL,NULL,1))) ||
         (sampling->quantisation && blend!=6 && blend!=7 &&
          (sampling->quantisation!=GML_RENDER_QUANTISE_STRETCHED_FLOAT ||
           sampling->filter!=GML_RENDER_FILTER_BILINEAR ||
           op->scene_family!=GML_RENDER_SCENE_SPRITE || (blend!=0 && blend!=8) ||
           op->payload.quad.shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
           (sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE &&
            !(blend==8 && sampling->edge_rule==GML_RENDER_EDGE_TRANSPARENT)) ||
           source->rendered_origin)) ||
         (blend!=7 && (sampling->filter==GML_RENDER_FILTER_BOX
           ? sampling->edge_rule!=GML_RENDER_EDGE_TRANSPARENT ||
             sampling->logical_margin_mask!=0u
           : (sampling->edge_rule==GML_RENDER_EDGE_LOGICAL_TPAG ||
              sampling->edge_rule==GML_RENDER_EDGE_TRANSPARENT
                ? sampling->filter!=GML_RENDER_FILTER_BILINEAR ||
                  !sampling->logical_margin_mask ||
                  (sampling->logical_margin_mask>15u &&
                   sampling->logical_margin_mask!=GML_RENDER_MARGIN_VISIBLE_CENTRE) ||
                  (sampling->logical_margin_mask==GML_RENDER_MARGIN_VISIBLE_CENTRE &&
                   (sampling->edge_rule!=GML_RENDER_EDGE_LOGICAL_TPAG ||
                    (op->payload.quad.shape!=GML_RENDER_QUAD_CARDINAL &&
                     op->payload.quad.shape!=GML_RENDER_QUAD_GENERAL)))
                : sampling->logical_margin_mask!=0u))) ||
         state->samplers[0].filter!=sampling->filter ||
         state->samplers[0].edge_rule!=sampling->edge_rule ||
         state->samplers[0].quantisation!=sampling->quantisation ||
         state->samplers[0].logical_margin_mask!=sampling->logical_margin_mask ||
         !same_region(state->samplers[0].source_region,sampling->source_region) ||
         !gml_render_gpu_opengl_draw_textured_quad(op,state,source->width,source->height,
                                                   source->rendered_origin,probe)) return 0;
      for(uint32_t corner=0;corner<4;corner++){
        uint32_t colour=op->payload.quad.corner_colour[corner];
        if(blend==8 && colour!=op->payload.quad.corner_colour[0]) return 0;
        if(state->coverage_policy &&
           (colour>>24)!=(uint32_t)lround(op->payload.quad.filtered_draw_alpha*255.0))
          return 0;
        if(colour!=0xFFFFFFFFu && blend!=6 &&
           (op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
            (sampling->filter!=GML_RENDER_FILTER_BILINEAR &&
             sampling->filter!=GML_RENDER_FILTER_BOX &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_COPY &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA &&
             sampling->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR &&
             op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED &&
             !(op->scene_family==GML_RENDER_SCENE_TEXT &&
               sampling->filter==GML_RENDER_FILTER_NEAREST)))) return 0;
        if((colour>>24)!=255u && blend!=1 && blend!=3 && blend!=9) return 0;
        if((blend==4 || blend==5 ||
            (sampling->quantisation && blend!=6 && blend!=8)) &&
           colour!=0xFFFFFFFFu) return 0;
      }
      if(op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED &&
         sampling->filter==GML_RENDER_FILTER_NEAREST &&
         !full_region(sampling->source_region,source->width,source->height)){
        double u0=(double)sampling->source_region.x/source->width;
        double u1=(double)((uint32_t)sampling->source_region.x+
                           sampling->source_region.width)/source->width;
        double v0=(double)sampling->source_region.y/source->height;
        double v1=(double)((uint32_t)sampling->source_region.y+
                           sampling->source_region.height)/source->height;
        for(uint32_t corner=0;corner<4;corner++)
          if(op->payload.quad.u[corner]<u0-1e-12 ||
             op->payload.quad.u[corner]>u1+1e-12 ||
             op->payload.quad.v[corner]<v0-1e-12 ||
             op->payload.quad.v[corner]>v1+1e-12) return 0;
      }
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

/* An ordered device-local copy is shared by exact draw rules. No target write
 * occurs until this succeeds; the existing snapshot allocation is reused. */
static int scene_snapshot_region(GmlGpuBackend *backend,GlSceneImage *target,
                                  GLint left,GLint bottom,GLsizei width,GLsizei height){
  GlScene *scene=&backend->scene;
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
  return 1;
}

static int flush_quads(GmlGpuBackend *backend,GlSceneImage *target,GlSceneImage *source,
                       uint32_t filter,int blend,uint32_t count){
  GlScene *scene=&backend->scene;
  GLfloat extent[2]={(GLfloat)target->width,(GLfloat)target->height};
  GLenum texture_filter=filter==18u?GL_LINEAR:GL_NEAREST;
  GLsizei stride=GL_SCENE_VERTEX_FLOATS*(GLsizei)sizeof(float);
  if(!count) return 1;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  backend->gl.Viewport(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.Scissor(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,source->texture);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,(GLint)texture_filter);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,(GLint)texture_filter);
  if(blend && blend!=8 && filter!=15u && filter!=17u){
    backend->gl.BlendFuncSeparate(blend==9?GL_ONE:GL_SRC_ALPHA,
                                  blend==9?GL_ONE:GL_ONE_MINUS_SRC_ALPHA,
                                  blend==3 || blend==9?GL_SRC_ALPHA:GL_ONE,
                                  GL_ONE_MINUS_SRC_ALPHA);
    backend->gl.BlendEquationSeparate(GL_FUNC_ADD,GL_FUNC_ADD);
    backend->gl.Enable(GL_BLEND);
  } else backend->gl.Disable(GL_BLEND);
  if(filter==9u || filter==12u)
    backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_FALSE);
  backend->gl.UseProgram(scene->program);
  backend->gl.Uniform2fv(scene->u_extent,1,extent);
  backend->gl.Uniform1i(scene->u_image,0);
  backend->gl.Uniform1i(scene->u_filter,(GLint)filter);
  backend->gl.Uniform1i(scene->u_previous,1);
  backend->gl.BindVertexArray(scene->vertex_array);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,scene->vertex_buffer);
  backend->gl.BufferData(GL_ARRAY_BUFFER,
    (GLsizeiptr)((size_t)count*6u*GL_SCENE_VERTEX_FLOATS*sizeof(float)),
    scene->vertices,GL_STREAM_DRAW);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_position);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_texcoord);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_colour);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_clamp);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_map);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_step);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_margin);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_span_x);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_span_y);
  backend->gl.EnableVertexAttribArray((GLuint)scene->a_span_active);
  backend->gl.VertexAttribPointer((GLuint)scene->a_position,2,GL_FLOAT,GL_FALSE,stride,(const void*)0);
  backend->gl.VertexAttribPointer((GLuint)scene->a_texcoord,2,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(2*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->a_colour,4,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(4*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->a_clamp,4,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(8*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->a_map,4,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(12*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->a_step,2,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(16*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->a_margin,1,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(18*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->a_span_x,4,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(19*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->a_span_y,4,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(23*sizeof(float)));
  backend->gl.VertexAttribPointer((GLuint)scene->a_span_active,1,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)(27*sizeof(float)));
  backend->gl.DrawArrays(GL_TRIANGLES,0,(GLsizei)(count*6u));
  if(filter==9u || filter==12u){
    /* The CPU screen writer keeps alpha opaque at every covered texel,
     * even when its prior alpha is unknown. Reuse the sampled vertices and
     * the same sampler in an alpha-only pass; source-zero texels stay untouched. */
    backend->gl.ColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
    backend->gl.Disable(GL_BLEND);
    backend->gl.Uniform1i(scene->u_filter,filter==9u?10:13);
    backend->gl.DrawArrays(GL_TRIANGLES,0,(GLsizei)(count*6u));
    backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  }
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_position);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_texcoord);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_colour);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_clamp);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_map);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_step);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_margin);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_span_x);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_span_y);
  backend->gl.DisableVertexAttribArray((GLuint)scene->a_span_active);
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

/* Each pixel of an ordered, disjoint row run reads one immutable device-local
 * predecessor. Overlapping or differently versioned runs take a fresh snapshot;
 * scalar and vector packets carry their own alpha policy in the vertex operand. */
static int draw_rotated_regions(GmlGpuBackend *backend,const GmlRenderDrawBatch *batch,
                                 uint32_t first,GlSceneImage *target,GlSceneImage *source,
                                 uint32_t *consumed){
  GlScene *scene=&backend->scene;
  const GmlRenderDrawOp *base=&batch->operations[first];
  GLint left=0,bottom=0,right=0,top=0,last_left=0,last_top=0;
  GLsizei last_width=0,last_height=0;
  uint32_t count=0;
  while(first+count<batch->operation_count && count<GL_SCENE_MAX_QUADS){
    const GmlRenderDrawOp *op=&batch->operations[first+count];
    GLint x,y;
    GLsizei w,h;
    if(op->opcode!=GML_RENDER_OP_TEXTURED_QUAD || op->source_count!=1u ||
       (scene_filter(&op->payload.quad,&batch->states[op->state_index])!=15u &&
        scene_filter(&op->payload.quad,&batch->states[op->state_index])!=17u) ||
       !same_version(op->sources[0],base->sources[0]) ||
       !same_logical(op->target.image,base->target.image) ||
       op->target.storage_version!=base->target.storage_version ||
       op->target.width!=base->target.width || op->target.height!=base->target.height ||
       op->target.pixel_format!=base->target.pixel_format ||
       op->payload.quad.filtered_draw_alpha!=base->payload.quad.filtered_draw_alpha ||
       (count && (batch->operations[first+count-1u].target.image.content_version==UINT64_MAX ||
         op->target.image.content_version!=
           batch->operations[first+count-1u].target.image.content_version+1u)) ||
       !exact_rect(op,&x,&y,&w,&h,1)) break;
    GLint row=(GLint)target->height-y-h;
    if(count && !(row>=last_top+last_height ||
       (row==last_top && h==last_height && x>=last_left+last_width))) break;
    if(count && (uint64_t)(unsigned)((x+w>right?x+w:right)-(x<left?x:left))*
         (unsigned)((y+h>top?y+h:top)-(y<bottom?y:bottom))>=262144u) break;
    if(!gml_render_gpu_opengl_draw_textured_quad(op,&batch->states[op->state_index],
       source->width,source->height,source->rendered_origin,
       scene->vertices+(size_t)count*6u*GL_SCENE_VERTEX_FLOATS)) return 0;
    if(!count){ left=x; bottom=y; right=x+w; top=y+h; }
    else {
      if(x<left) left=x;
      if(y<bottom) bottom=y;
      if(x+w>right) right=x+w;
      if(y+h>top) top=y+h;
    }
    last_left=x; last_top=row; last_width=w; last_height=h;
    count++;
  }
  if(!count || !scene_snapshot_region(backend,target,left,bottom,right-left,top-bottom))
    return 0;
  backend->gl.UseProgram(scene->program);
  if(!scene->rotated_alpha16_valid ||
     scene->rotated_alpha16_value!=base->payload.quad.filtered_draw_alpha){
    GLint factors[256];
    for(unsigned alpha=0;alpha<256u;alpha++)
      factors[alpha]=(GLint)gml_render_rotated_alpha16_factor(
        alpha,base->payload.quad.filtered_draw_alpha);
    /* Pack four entries per vector to remain below the GLES uniform minimum. */
    backend->gl.Uniform4iv(scene->u_rotated_alpha16,64,factors);
    if(backend->gl.GetError()!=GL_NO_ERROR){ scene->rotated_alpha16_valid=0; return 0; }
    scene->rotated_alpha16_value=base->payload.quad.filtered_draw_alpha;
    scene->rotated_alpha16_valid=1;
  }
  GLint origin[2]={left,bottom};
  backend->gl.Uniform2iv(scene->u_region_origin,1,origin);
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  if(!flush_quads(backend,target,source,15u,0,count)) return -1;
  *consumed=count;
  return 1;
}

static void double_words(double value,GLint words[2]){
  _Static_assert(sizeof(double)==sizeof(uint64_t) && DBL_MANT_DIG==53 &&
                   DBL_MAX_EXP==1024,"Filtered font requires binary64 double");
  uint64_t bits;
  uint32_t halves[2];
  memcpy(&bits,&value,sizeof bits);
  halves[0]=(uint32_t)bits; halves[1]=(uint32_t)(bits>>32);
  memcpy(words,halves,sizeof halves);
}

/* Returns 1 on success, 0 before any target write, and -1 if the draw may have modified it. The
 * snapshot is only the affected rectangle, never the entire surface per overlapping sprite. */
static int draw_exact_quad(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
                           const GmlRenderDrawStateSnapshot *state,
                           GlSceneImage *target,GlSceneImage *source,
                           uint32_t repetitions,const GmlRenderLayerEffect *effect,
                           char *error,size_t error_capacity){
  GlScene *scene=&backend->scene;
  GlSceneExactProgram *exact;
  GLint left,bottom,origin[2];
  GLsizei width,height,stride=GL_SCENE_VERTEX_FLOATS*(GLsizei)sizeof(float);
  GLfloat extent[2]={(GLfloat)target->width,(GLfloat)target->height};
  int outcome,font_mode=0;
  if(qualified_blend(state)==6)
    font_mode=op->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_FONT_MODERN?3:2;
  if(font_mode && backend->api==GML_GPU_API_OPENGLES3 && !backend->gl.Flush){
    gml_gpu_opengl_record_error(error,error_capacity,"exact font submission is unavailable");
    return 0;
  }
  if(!exact_rect(op,&left,&bottom,&width,&height,
                 qualified_blend(state)==4 || qualified_blend(state)==6 ||
                 qualified_blend(state)==7) ||
     !(exact=gml_gpu_opengl_scene_exact_program(backend,font_mode,error,error_capacity)) ||
     !gml_render_gpu_opengl_draw_textured_quad(op,state,source->width,source->height,
                                               source->rendered_origin,
                                               scene->vertices)) return 0;
  if(!scene_snapshot_region(backend,target,left,bottom,width,height)) return 0;
  backend->gl.Disable(GL_BLEND);
  backend->gl.Viewport(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.Scissor(left,bottom,width,height);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,source->texture);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  backend->gl.UseProgram(exact->exact_program);
  backend->gl.Uniform2fv(exact->exact_u_extent,1,extent);
  backend->gl.Uniform1i(exact->exact_u_image,0);
  backend->gl.Uniform1i(exact->exact_u_previous,1);
  backend->gl.Uniform1i(exact->exact_u_repetitions,(GLint)repetitions);
  backend->gl.Uniform1i(exact->exact_u_blend_rule,
                        effect?(effect->kind==GML_RENDER_LAYER_EFFECT_TINT?2:
                                effect->kind==GML_RENDER_LAYER_EFFECT_COLOURISE?3:
                                effect->kind==GML_RENDER_LAYER_EFFECT_ZOOM_BLUR?4:5):
                          (qualified_blend(state)==7?8:qualified_blend(state)==6?7:
                           qualified_blend(state)==5?6:qualified_blend(state)==4?1:0));
  if(qualified_blend(state)==7){
    const GmlRenderQuad *q=&op->payload.quad;
    GLint size[4]={(GLint)source->width,(GLint)source->height,
                   (GLint)(q->x[1]-q->x[0]),(GLint)(q->y[2]-q->y[0])};
    GLint underlay_origin[2]={(GLint)q->x[0],(GLint)q->y[0]};
    GLint flags=(GLint)q->source_opaque |
      (q->sampling.filter==GML_RENDER_FILTER_BOX?2:0) |
      (q->sampling.quantisation==GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_FIRSTGEN?4:0) |
      (source->rendered_origin?8:0);
    backend->gl.Uniform4iv(exact->exact_u_underlay_size,1,size);
    backend->gl.Uniform2iv(exact->exact_u_underlay_origin,1,underlay_origin);
    backend->gl.Uniform1i(exact->exact_u_underlay_flags,flags);
  }
  if(qualified_blend(state)==6){
    const GmlRenderQuad *quad=&op->payload.quad;
    GmlRenderRect crop=quad->sampling.source_region;
    GLint origin_words[4],scale_words[4],alpha_words[2];
    uint32_t colour=quad->corner_colour[0];
    GLint region[4]={crop.x,crop.y,(GLint)crop.width,(GLint)crop.height};
    GLint tint[4]={(GLint)((colour>>16)&255u),(GLint)((colour>>8)&255u),
                   (GLint)(colour&255u),255};
    double_words(quad->inverse_anchor[0],origin_words);
    double_words(quad->inverse_anchor[1],origin_words+2);
    double_words(quad->inverse_x[0],scale_words);
    double_words(quad->inverse_y[1],scale_words+2);
    double_words(quad->filtered_draw_alpha,alpha_words);
    backend->gl.Uniform4iv(exact->exact_u_font_origin,1,origin_words);
    backend->gl.Uniform4iv(exact->exact_u_font_scale,1,scale_words);
    backend->gl.Uniform2iv(exact->exact_u_font_alpha,1,alpha_words);
    backend->gl.Uniform4iv(exact->exact_u_font_crop,1,region);
    backend->gl.Uniform4iv(exact->exact_u_font_tint,1,tint);
    backend->gl.Uniform1i(exact->exact_u_font_margin,
                          (GLint)quad->sampling.logical_margin_mask);
    backend->gl.Uniform1i(exact->exact_u_font_mode,
      quad->sampling.quantisation==GML_RENDER_QUANTISE_FONT_FIRSTGEN_INDEPENDENT ||
      quad->sampling.quantisation==GML_RENDER_QUANTISE_SPRITE_FIRSTGEN_INDEPENDENT
        ?GML_RENDER_QUANTISE_FONT_FIRSTGEN:(GLint)quad->sampling.quantisation);
    backend->gl.Uniform1i(exact->exact_u_font_preserve_alpha,
                          state->alpha_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA);
  }
  {
    GLfloat opacity=(GLfloat)state->additive_draw_alpha;
    backend->gl.Uniform1fv(exact->exact_u_additive_alpha,1,&opacity);
  }
  {
    uint32_t colour=effect?effect->colour:0xFFFFFFFFu;
    GLint rgba[4]={(GLint)((colour>>16)&255u),(GLint)((colour>>8)&255u),
                   (GLint)(colour&255u),(GLint)(colour>>24)};
    GLfloat intensity=effect?(GLfloat)effect->intensity:1.0f;
    backend->gl.Uniform4iv(exact->exact_u_effect_colour,1,rgba);
    backend->gl.Uniform1fv(exact->exact_u_effect_intensity,1,&intensity);
    GLfloat zoom[4]={effect?(GLfloat)effect->centre[0]:0.0f,
                     effect?(GLfloat)effect->centre[1]:0.0f,
                     intensity,effect?(GLfloat)effect->focus_radius:0.0f};
    backend->gl.Uniform4fv(exact->exact_u_effect_zoom,1,zoom);
    backend->gl.Uniform1i(exact->exact_u_effect_linear,
                          effect?(GLint)effect->linear:0);
    backend->gl.Uniform1i(exact->exact_u_effect_seed,
                          effect?(GLint)effect->phase_seed:0);
  }
  origin[0]=left;
  origin[1]=bottom;
  backend->gl.Uniform2iv(exact->exact_u_region_origin,1,origin);
  backend->gl.BindVertexArray(scene->vertex_array);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,scene->vertex_buffer);
  backend->gl.BufferData(GL_ARRAY_BUFFER,6u*GL_SCENE_VERTEX_FLOATS*sizeof(float),
                         scene->vertices,GL_STREAM_DRAW);
  backend->gl.EnableVertexAttribArray((GLuint)exact->exact_a_position);
  if(exact->exact_a_texcoord>=0)
    backend->gl.EnableVertexAttribArray((GLuint)exact->exact_a_texcoord);
  if(exact->exact_a_colour>=0)
    backend->gl.EnableVertexAttribArray((GLuint)exact->exact_a_colour);
  backend->gl.VertexAttribPointer((GLuint)exact->exact_a_position,2,GL_FLOAT,GL_FALSE,stride,
                                  (const void*)0);
  if(exact->exact_a_texcoord>=0)
    backend->gl.VertexAttribPointer((GLuint)exact->exact_a_texcoord,2,GL_FLOAT,GL_FALSE,stride,
                                    (const void*)(2*sizeof(float)));
  if(exact->exact_a_colour>=0)
    backend->gl.VertexAttribPointer((GLuint)exact->exact_a_colour,4,GL_FLOAT,GL_FALSE,stride,
                                    (const void*)(4*sizeof(float)));
  if(backend->gl.GetError()!=GL_NO_ERROR){ outcome=0; goto cleanup; }
  if(font_mode && backend->api==GML_GPU_API_OPENGLES3){
    /* All three writes sample the same immutable pre-glyph rectangle. Alpha is written
     * once, with the final colour, so overlaps retain the original ordered source-over. */
    outcome=-1;
    for(GLint channel=0;channel<3;channel++){
      backend->gl.Uniform1i(exact->exact_u_font_channel,channel);
      backend->gl.ColorMask(channel==0,channel==1,channel==2,channel==2);
      backend->gl.DrawArrays(GL_TRIANGLES,0,6);
      if(backend->gl.GetError()!=GL_NO_ERROR) goto cleanup;
    }
    /* V3D needs the complete masked write submitted before a later pass samples it.
     * Other identified renderers retain the context's ordinary ordered command stream. */
    if(scene->font_write_flush) backend->gl.Flush();
    if(backend->gl.GetError()!=GL_NO_ERROR) goto cleanup;
    outcome=1;
  } else {
    backend->gl.DrawArrays(GL_TRIANGLES,0,6);
    outcome=backend->gl.GetError()==GL_NO_ERROR?1:-1;
  }
cleanup:
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  backend->gl.DisableVertexAttribArray((GLuint)exact->exact_a_position);
  if(exact->exact_a_texcoord>=0)
    backend->gl.DisableVertexAttribArray((GLuint)exact->exact_a_texcoord);
  if(exact->exact_a_colour>=0)
    backend->gl.DisableVertexAttribArray((GLuint)exact->exact_a_colour);
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

int gml_render_gpu_opengl_apply_effect(GmlGpuBackend *backend,
                                       const GmlRenderDrawOp *operation,
                                       const GmlRenderDrawStateSnapshot *state,
                                       GlSceneImage *target,GlSceneImage *source,
                                       char *error,size_t error_capacity){
  GmlRenderDrawOp quad=*operation;
  GmlRenderQuad *shape=&quad.payload.quad;
  if(!target || !source || !state ||
     (operation->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_TINT &&
      operation->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_COLOURISE &&
      operation->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_ZOOM_BLUR &&
      operation->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_RGB_NOISE)) return 0;
  memset(shape,0,sizeof *shape);
  quad.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  shape->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  shape->sampling.filter=GML_RENDER_FILTER_NEAREST;
  shape->sampling.edge_rule=GML_RENDER_EDGE_CLAMP_SUBIMAGE;
  shape->sampling.source_region=(GmlRenderRect){0,0,source->width,source->height};
  shape->x[1]=shape->x[2]=(double)target->width;
  shape->y[2]=shape->y[3]=(double)target->height;
  shape->u[1]=shape->u[2]=1.0;
  shape->v[2]=shape->v[3]=1.0;
  for(int corner=0;corner<4;corner++) shape->corner_colour[corner]=0xFFFFFFFFu;
  return draw_exact_quad(backend,&quad,state,target,source,1u,
                         &operation->payload.effect,error,error_capacity);
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
       pending_filter!=scene_filter(&op->payload.quad,&batch->states[op->state_index]) ||
       pending_blend!=qualified_blend(&batch->states[op->state_index]) ||
       qualified_blend(&batch->states[op->state_index])==2 ||
       qualified_blend(&batch->states[op->state_index])==4 ||
       qualified_blend(&batch->states[op->state_index])==5 ||
       qualified_blend(&batch->states[op->state_index])==6 ||
       scene_filter(&op->payload.quad,&batch->states[op->state_index])==15u ||
       scene_filter(&op->payload.quad,&batch->states[op->state_index])==17u)){
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
    if(op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE){
      if(!gml_render_gpu_opengl_draw_primitive(backend,op,target)){
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
      GlSceneImage *samplers[GML_PLAN_MAX_SAMPLERS]={0};
      for(uint32_t bound=0;bound<op->payload.shader->sampler_count;bound++)
        samplers[bound]=gml_gpu_opengl_scene_image(backend,op->sources[bound+1u]);
      if(!source || !gml_gpu_opengl_scene_shader_draw(backend,op->payload.shader,
          &batch->states[op->state_index],target,op->target.region,source,samplers,
          op->payload.shader->sampler_count,
          qualified_blend(&batch->states[op->state_index])==1,error,error_capacity)){
        mark_uncertain(&result,target);
        break;
      }
      target->valid=1;
      target->rendered_origin=1;
      result.accepted_prefix=index+1u;
      continue;
    }
    if(op->opcode==GML_RENDER_OP_LAYER_EFFECT){
      GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
      int drawn=source?gml_render_gpu_opengl_apply_effect(backend,op,
        &batch->states[op->state_index],target,source,error,error_capacity):0;
      if(drawn<0) mark_uncertain(&result,target);
      else if(!drawn) result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
      if(drawn<=0) break;
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
    pending_filter=scene_filter(&op->payload.quad,&batch->states[op->state_index]);
    pending_blend=qualified_blend(&batch->states[op->state_index]);
    if(pending_filter==15u || pending_filter==17u){
      uint32_t consumed=0;
      int drawn=draw_rotated_regions(backend,batch,index,target,pending_source,&consumed);
      if(drawn<0) mark_uncertain(&result,target);
      else if(!drawn) result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
      if(drawn<=0) break;
      target->ref=batch->operations[index+consumed-1u].target.image;
      result.accepted_prefix=index+consumed;
      pending_count=0;
      index+=consumed-1u;
      continue;
    }
    if(pending_blend==8){
      GLint left,bottom;
      GLsizei width,height;
      if(!exact_rect(op,&left,&bottom,&width,&height,1) ||
         !gml_render_gpu_opengl_draw_textured_quad(op,&batch->states[op->state_index],
           pending_source->width,pending_source->height,pending_source->rendered_origin,
           backend->scene.vertices) ||
         !scene_snapshot_region(backend,target,left,bottom,width,height)){
        result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
        break;
      }
      GLint origin[2]={left,bottom};
      backend->gl.UseProgram(backend->scene.program);
      backend->gl.Uniform2iv(backend->scene.u_region_origin,1,origin);
      /* Each overlapping float blend snapshots its own ordered predecessor;
       * never merge these quads against one stale destination image. */
      if(!flush_quads(backend,target,pending_source,pending_filter,pending_blend,1u)){
        mark_uncertain(&result,target);
        break;
      }
      result.accepted_prefix=index+1u;
      pending_count=0;
      continue;
    }
    if(pending_blend==2 || pending_blend==4 || pending_blend==5 || pending_blend==6 ||
       pending_blend==7){
      uint32_t repetitions=pending_blend>=6?1u:exact_repetitions(batch,index);
      int drawn=draw_exact_quad(backend,op,&batch->states[op->state_index],
                                target,pending_source,repetitions,NULL,error,error_capacity);
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
       pending_source->width,pending_source->height,pending_source->rendered_origin,
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
