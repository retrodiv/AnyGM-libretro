/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Bounded ordered submission for the first resident scene variants. Unsupported shapes are
 * rejected before writes; after a device error, the affected target is explicitly uncertain. */
#include "gml_gpu_opengl_internal.h"

#include <limits.h>
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

static uint32_t scene_filter(const GmlRenderQuad *quad){
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
          op->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_ZOOM_BLUR) ||
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
        if(!sampler || !sampler->texture || !sampler->rendered_origin ||
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
         blend<0 || (blend==1 && (!backend->gl.BlendFuncSeparate ||
                                 !backend->gl.BlendEquationSeparate)) ||
         (blend==2 && (!backend->gl.CopyTexSubImage2D ||
                       target->pixel_format!=GML_RENDER_PIXEL_XRGB8888 ||
                       sampling->filter!=GML_RENDER_FILTER_NEAREST ||
                       !exact_rect(op,NULL,NULL,NULL,NULL,0))) ||
         (blend==4 && (!backend->gl.CopyTexSubImage2D ||
                       op->scene_family!=GML_RENDER_SCENE_BACKGROUND ||
                       target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
                       sampling->filter!=GML_RENDER_FILTER_NEAREST ||
                       !exact_rect(op,NULL,NULL,NULL,NULL,1))) ||
         state->sampler_count!=1u || state->material_identity || state->material_version ||
         state->coverage_policy ||
         !((state->clip.width==0 && state->clip.height==0) ||
           full_region(state->clip,target->width,target->height)) ||
         !full_region(target->region,target->width,target->height) ||
         !valid_region(sampling->source_region,source->width,source->height) ||
         (sampling->filter!=GML_RENDER_FILTER_NEAREST &&
          sampling->filter!=GML_RENDER_FILTER_BILINEAR &&
          sampling->filter!=GML_RENDER_FILTER_BOX &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_COPY &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA) ||
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
         sampling->quantisation ||
         (sampling->filter==GML_RENDER_FILTER_BOX
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
                : sampling->logical_margin_mask!=0u)) ||
         state->samplers[0].filter!=sampling->filter ||
         state->samplers[0].edge_rule!=sampling->edge_rule ||
         state->samplers[0].logical_margin_mask!=sampling->logical_margin_mask ||
         !same_region(state->samplers[0].source_region,sampling->source_region) ||
         !gml_render_gpu_opengl_draw_textured_quad(op,state,source->width,source->height,
                                                   source->rendered_origin,probe)) return 0;
      for(uint32_t corner=0;corner<4;corner++){
        uint32_t colour=op->payload.quad.corner_colour[corner];
        if(colour!=0xFFFFFFFFu &&
           (op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
            (sampling->filter!=GML_RENDER_FILTER_BILINEAR &&
             sampling->filter!=GML_RENDER_FILTER_BOX &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_COPY &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA &&
             op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED &&
             !(op->scene_family==GML_RENDER_SCENE_TEXT &&
               sampling->filter==GML_RENDER_FILTER_NEAREST)))) return 0;
        if((colour>>24)!=255u && blend!=1 && blend!=3) return 0;
        if(blend==4 && colour!=0xFFFFFFFFu) return 0;
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

static int flush_quads(GmlGpuBackend *backend,GlSceneImage *target,GlSceneImage *source,
                       uint32_t filter,int blend,uint32_t count){
  GlScene *scene=&backend->scene;
  GLfloat extent[2]={(GLfloat)target->width,(GLfloat)target->height};
  GLenum texture_filter=GL_NEAREST;
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
                                  blend==3?GL_SRC_ALPHA:GL_ONE,
                                  GL_ONE_MINUS_SRC_ALPHA);
    backend->gl.BlendEquationSeparate(GL_FUNC_ADD,GL_FUNC_ADD);
    backend->gl.Enable(GL_BLEND);
  } else backend->gl.Disable(GL_BLEND);
  if(filter==9u) backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_FALSE);
  backend->gl.UseProgram(scene->program);
  backend->gl.Uniform2fv(scene->u_extent,1,extent);
  backend->gl.Uniform1i(scene->u_image,0);
  backend->gl.Uniform1i(scene->u_filter,(GLint)filter);
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
  if(filter==9u){
    /* The modern CPU world target keeps alpha opaque at every written texel,
     * even when its prior alpha is unknown. Reuse the sampled vertices in an
     * alpha-only pass; the shader discards source-zero texels. */
    backend->gl.ColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
    backend->gl.Disable(GL_BLEND);
    backend->gl.Uniform1i(scene->u_filter,10);
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

/* Returns 1 on success, 0 before any target write, and -1 if the draw may have modified it. The
 * snapshot is only the affected rectangle, never the entire surface per overlapping sprite. */
static int draw_exact_quad(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
                           const GmlRenderDrawStateSnapshot *state,
                           GlSceneImage *target,GlSceneImage *source,
                           uint32_t repetitions,const GmlRenderLayerEffect *effect,
                           char *error,size_t error_capacity){
  GlScene *scene=&backend->scene;
  GLint left,bottom,origin[2];
  GLsizei width,height,stride=GL_SCENE_VERTEX_FLOATS*(GLsizei)sizeof(float);
  GLfloat extent[2]={(GLfloat)target->width,(GLfloat)target->height};
  int outcome;
  if(!exact_rect(op,&left,&bottom,&width,&height,
                 qualified_blend(state)==4) ||
     !gml_gpu_opengl_scene_exact_program(backend,error,error_capacity) ||
     !gml_render_gpu_opengl_draw_textured_quad(op,state,source->width,source->height,
                                               source->rendered_origin,
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
  backend->gl.Uniform1i(scene->exact_u_blend_rule,
                        effect?(effect->kind==GML_RENDER_LAYER_EFFECT_TINT?2:
                                effect->kind==GML_RENDER_LAYER_EFFECT_COLOURISE?3:4):
                          (qualified_blend(state)==4?1:0));
  {
    GLfloat opacity=(GLfloat)state->additive_draw_alpha;
    backend->gl.Uniform1fv(scene->exact_u_additive_alpha,1,&opacity);
  }
  {
    uint32_t colour=effect?effect->colour:0xFFFFFFFFu;
    GLint rgba[4]={(GLint)((colour>>16)&255u),(GLint)((colour>>8)&255u),
                   (GLint)(colour&255u),(GLint)(colour>>24)};
    GLfloat intensity=effect?(GLfloat)effect->intensity:1.0f;
    backend->gl.Uniform4iv(scene->exact_u_effect_colour,1,rgba);
    backend->gl.Uniform1fv(scene->exact_u_effect_intensity,1,&intensity);
    GLfloat zoom[4]={effect?(GLfloat)effect->centre[0]:0.0f,
                     effect?(GLfloat)effect->centre[1]:0.0f,
                     intensity,effect?(GLfloat)effect->focus_radius:0.0f};
    backend->gl.Uniform4fv(scene->exact_u_effect_zoom,1,zoom);
    backend->gl.Uniform1i(scene->exact_u_effect_linear,
                          effect?(GLint)effect->linear:0);
  }
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
      operation->payload.effect.kind!=GML_RENDER_LAYER_EFFECT_ZOOM_BLUR)) return 0;
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
       pending_filter!=scene_filter(&op->payload.quad) ||
       pending_blend!=qualified_blend(&batch->states[op->state_index]) ||
       qualified_blend(&batch->states[op->state_index])==2 ||
       qualified_blend(&batch->states[op->state_index])==4)){
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
          &batch->states[op->state_index],target,source,samplers,
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
    pending_filter=scene_filter(&op->payload.quad);
    pending_blend=qualified_blend(&batch->states[op->state_index]);
    if(pending_blend==2 || pending_blend==4){
      uint32_t repetitions=exact_repetitions(batch,index);
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
