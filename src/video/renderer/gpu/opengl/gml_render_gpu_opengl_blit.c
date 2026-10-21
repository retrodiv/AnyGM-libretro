/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* One prepared quad becomes six ordered vertices in a bounded batch; no mutable renderer or VM
 * state is consulted here. The batch owner issues a single draw for compatible consecutive quads. */
#include "gml_gpu_opengl_internal.h"

#include <math.h>

/* R03-ATLAS-CLAMPED-WINDOW: a logical crop may exceed its immutable physical
 * image. Only the already typed hardware axis sampler clamps these taps;
 * exact recipes, raw glyph fetches and rendered surfaces remain contained. */
int gml_gpu_opengl_clamped_atlas_valid(const GmlRenderDrawOp *op,
                                      uint32_t source_width,uint32_t source_height){
  if(!op || !source_width || !source_height ||
     source_width>16384u || source_height>16384u ||
     op->opcode!=GML_RENDER_OP_TEXTURED_QUAD ||
     (op->scene_family!=GML_RENDER_SCENE_SPRITE &&
      op->scene_family!=GML_RENDER_SCENE_BACKGROUND &&
      op->scene_family!=GML_RENDER_SCENE_TEXT) ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     op->source_count!=1u || !op->sources ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS) return 0;
  const GmlRenderQuad *q=&op->payload.quad;
  GmlRenderRect region=q->sampling.source_region;
  int byte_background=((op->scene_family==GML_RENDER_SCENE_BACKGROUND &&
    q->span_active==GML_RENDER_SPAN_FILTERED_POSITIVE_ROW) ||
    (op->scene_family==GML_RENDER_SCENE_SPRITE &&
     q->span_active==GML_RENDER_SPAN_FILTERED_AXIS_SUPPORT_ROW)) &&
    (q->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
     q->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND);
  return (op->scene_family!=GML_RENDER_SCENE_TEXT ||
          (region.width<=256u && region.height<=256u)) &&
    q->shape==GML_RENDER_QUAD_AXIS_ALIGNED && (!q->span_active || byte_background) &&
    q->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
    (q->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN ||
     q->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN || byte_background) &&
    q->sampling.logical_margin_mask<=15u &&
    q->sampling.edge_rule==(q->sampling.logical_margin_mask
      ?GML_RENDER_EDGE_LOGICAL_TPAG:GML_RENDER_EDGE_CLAMP_SUBIMAGE) &&
    region.x>=0 && region.x<=65536 && region.y>=0 && region.y<=65536 &&
    region.width>0u && region.width<=4096u && region.height>0u && region.height<=4096u &&
    ((uint64_t)(uint32_t)region.x+region.width>source_width ||
     (uint64_t)(uint32_t)region.y+region.height>source_height);
}

/* R03-SPR-FIRSTGEN-GUI-HALF-POINT: only this independently certified
 * reciprocal map can extrapolate a vertex by half a texel at an atlas edge.
 * Fragment centres still sample contained odd texels, never that vertex. */
int gml_gpu_opengl_half_gui_point_valid(const GmlRenderDrawOp *op,
                                        const GmlRenderDrawStateSnapshot *state,
                                        uint32_t source_width,uint32_t source_height){
  if(!op || !state || !source_width || !source_height ||
     source_width>16384u || source_height>16384u) return 0;
  const GmlRenderQuad *q=&op->payload.quad;
  GmlRenderRect region=q->sampling.source_region;
  if(op->opcode!=GML_RENDER_OP_TEXTURED_QUAD ||
     op->scene_family!=GML_RENDER_SCENE_SPRITE ||
     op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     op->source_count!=1u || !op->sources ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active ||
     q->sampling.filter!=GML_RENDER_FILTER_NEAREST ||
     q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     q->sampling.quantisation || q->sampling.logical_margin_mask ||
     region.x<0 || region.y<0 || !region.width || !region.height ||
     (region.width&1u) || (region.height&1u) ||
     region.width>4096u || region.height>4096u ||
     (uint64_t)(uint32_t)region.x+region.width>source_width ||
     (uint64_t)(uint32_t)region.y+region.height>source_height ||
     state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
     state->write_mask!=15u || state->alpha!=255u || state->colour!=0xFFFFFFFFu ||
     state->rgb_source_factor!=GML_RENDER_BLEND_SOURCE_ALPHA ||
     state->rgb_destination_factor!=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA ||
     state->alpha_source_factor!=GML_RENDER_BLEND_ONE ||
     state->alpha_destination_factor!=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA ||
     state->rgb_equation!=GML_RENDER_BLEND_ADD ||
     state->alpha_equation!=GML_RENDER_BLEND_ADD ||
     !isfinite(q->filtered_draw_alpha) || q->filtered_draw_alpha<0x1p-64 ||
     q->filtered_draw_alpha>1.0 ||
     q->x[1]-q->x[0]!=region.width*0.5 ||
     q->y[2]-q->y[0]!=region.height*0.5) return 0;
  for(unsigned c=0;c<4u;c++){
    double x=q->x[0]+((c==1u || c==2u)?region.width*0.5:0.0);
    double y=q->y[0]+(c>=2u?region.height*0.5:0.0);
    double u=((double)region.x+0.5+((c==1u || c==2u)?region.width:0u))/source_width;
    double v=((double)region.y+0.5+(c>=2u?region.height:0u))/source_height;
    if(!isfinite(q->x[c]) || !isfinite(q->y[c]) ||
       q->x[c]!=x || q->y[c]!=y || x!=floor(x) || y!=floor(y) ||
       fabs(x)>65536.0 || fabs(y)>65536.0 ||
       !isfinite(q->u[c]) || !isfinite(q->v[c]) ||
       fabs(q->u[c]-u)>1e-12 || fabs(q->v[c]-v)>1e-12 ||
       (q->corner_colour[c]&0xFFFFFFu)!=0xFFFFFFu ||
       q->corner_colour[c]!=q->corner_colour[0] ||
       (q->corner_colour[c]>>24)!=(uint32_t)lround(q->filtered_draw_alpha*255.0)) return 0;
  }
  return 1;
}

int gml_render_gpu_opengl_draw_textured_quad(const GmlRenderDrawOp *op,
                                             const GmlRenderDrawStateSnapshot *state,
                                             uint32_t source_width,uint32_t source_height,
                                             int rendered_source_origin,
                                             float *vertices){
  static const uint8_t corners[6]={0,1,2,0,2,3};
  const GmlRenderQuad *quad;
  GmlRenderRect region;
  uint32_t region_storage_y;
  int rotated,warped,stretched_float,stretched_blend,automatic_surface,destination_surface,half_gui;
  if(!op || !state || !vertices || !source_width || !source_height) return 0;
  quad=&op->payload.quad;
  half_gui=!rendered_source_origin &&
    gml_gpu_opengl_half_gui_point_valid(op,state,source_width,source_height);
  rotated=quad->shape==GML_RENDER_QUAD_CARDINAL || quad->shape==GML_RENDER_QUAD_GENERAL;
  warped=quad->shape==GML_RENDER_QUAD_WARPED;
  stretched_float=quad->sampling.quantisation==GML_RENDER_QUANTISE_STRETCHED_FLOAT;
  stretched_blend=stretched_float &&
    state->rgb_equation==GML_RENDER_BLEND_EXACT_STRETCHED_FLOAT;
  automatic_surface=quad->sampling.quantisation==GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_FIRSTGEN ||
                    quad->sampling.quantisation==GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_MODERN;
  if(automatic_surface && !gml_render_automatic_surface_quad_valid(quad,
       op->target.width,op->target.height)) return 0;
  if(quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED && !rotated && !warped) return 0;
  if(warped && (rendered_source_origin ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     (quad->sampling.filter!=GML_RENDER_FILTER_NEAREST &&
      quad->sampling.filter!=GML_RENDER_FILTER_BILINEAR))) return 0;
  if(rotated && (rendered_source_origin ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     (quad->sampling.filter!=GML_RENDER_FILTER_BILINEAR &&
      quad->sampling.filter!=GML_RENDER_FILTER_NEAREST))) return 0;
  region=quad->sampling.source_region;
  if((region.x<0 || region.y<0 || !region.width || !region.height ||
     (uint64_t)(uint32_t)region.x+region.width>source_width ||
     (uint64_t)(uint32_t)region.y+region.height>source_height) &&
     (rendered_source_origin ||
      !gml_gpu_opengl_clamped_atlas_valid(op,source_width,source_height))) return 0;
  /* Exact full-surface destination-factor sampling is resolved by its integer
   * fragment rule, not the stretched sprite's logical-trim span metadata. */
  destination_surface=op->scene_family==GML_RENDER_SCENE_SURFACE &&
    op->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT &&
    quad->shape==GML_RENDER_QUAD_AXIS_ALIGNED && !quad->span_active &&
    state->rgb_equation>=GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
    state->rgb_equation<=GML_RENDER_BLEND_EXACT_DESTINATION_MIN &&
    state->alpha_equation>=GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
    state->alpha_equation<=GML_RENDER_BLEND_EXACT_DESTINATION_MIN &&
    quad->sampling.edge_rule==GML_RENDER_EDGE_CLAMP_SUBIMAGE &&
    !quad->sampling.quantisation && !quad->sampling.logical_margin_mask &&
    region.x==0 && region.y==0 && region.width==source_width && region.height==source_height;
  if(!automatic_surface && !destination_surface &&
     quad->sampling.quantisation!=GML_RENDER_QUANTISE_SURFACE_BOX_FUNCTION &&
     (quad->sampling.filter==GML_RENDER_FILTER_BOX || stretched_float ||
      quad->sampling.quantisation==GML_RENDER_QUANTISE_FIXED_STRETCHED)){
    if(rotated || rendered_source_origin || quad->span_active ||
       (!stretched_float && op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT)) return 0;
    for(uint32_t axis=0;axis<2;axis++){
      const double *span=axis?quad->span_y:quad->span_x;
      double extent=axis?quad->y[2]-quad->y[0]:quad->x[1]-quad->x[0];
      uint32_t physical=axis?region.height:region.width;
      for(uint32_t field=0;field<4;field++)
        if(!isfinite(span[field]) || span[field]!=floor(span[field])) return 0;
      double maximum=stretched_float?16384.0:4096.0;
      double destination_maximum=stretched_blend?1000000.0:maximum;
      uint32_t box_edge=quad->sampling.quantisation==GML_RENDER_QUANTISE_FIXED_STRETCHED?64u:8u;
      if(span[0]<1.0 || span[0]>maximum || span[1]<1.0 || span[1]>destination_maximum ||
         span[2]<0.0 || span[3]!=physical ||
         span[2]+span[3]>span[0] || extent!=span[1] ||
         ((uint32_t)span[0]+(uint32_t)span[1]-1u)/(uint32_t)span[1]>box_edge)
        return 0;
      if(stretched_float){
        double first=axis?quad->y[0]:quad->x[0];
        double target_extent=axis?op->target.height:op->target.width;
        if(span[1]<=span[0] || (!stretched_blend &&
           (span[2]!=0.0 || span[3]!=span[0])) || first!=floor(first) ||
           first<=-span[1] || first>=target_extent) return 0;
        uint32_t phase_first=stretched_blend?(uint32_t)fmax(0.0,-first):0u;
        uint32_t phase_last=stretched_blend
          ?(uint32_t)fmin(span[1],target_extent-first):(uint32_t)span[1];
        if(!gml_render_stretched_float_visible_phase_exact((uint32_t)span[0],(uint32_t)span[1],
                                                           phase_first,phase_last-phase_first)) return 0;
      }
    }
  }
  region_storage_y=rendered_source_origin
    ?source_height-((uint32_t)region.y+region.height):(uint32_t)region.y;
  if(!rotated && !warped && (quad->x[1]<=quad->x[0] || quad->y[2]<=quad->y[0])) return 0;
  if(rotated || warped){
    double area=(quad->x[1]-quad->x[0])*(quad->y[3]-quad->y[0])-
                (quad->y[1]-quad->y[0])*(quad->x[3]-quad->x[0]);
    if(!isfinite(area) || fabs(area)<1e-10) return 0;
  }
  if(rotated && state->coverage_policy){
    /* R03-P9-GUI-ROTATED-SPRITE: retain the original double for the shared
     * LUT preparator. The packet alpha policy replaces unused margin bits;
     * scalar tails stamp alpha while vector packets blend all four channels. */
    if(!isfinite(quad->filtered_draw_alpha) || quad->filtered_draw_alpha<=0.0 ||
       quad->filtered_draw_alpha>1.0) return 0;
  }
  for(uint32_t index=0;index<6;index++){
    uint32_t corner=corners[index];
    uint32_t colour=quad->corner_colour[corner];
    float *vertex=vertices+(size_t)index*GL_SCENE_VERTEX_FLOATS;
    if(!isfinite(quad->x[corner]) || !isfinite(quad->y[corner]) ||
       (!rotated && (!isfinite(quad->u[corner]) || !isfinite(quad->v[corner]))) ||
       quad->x[corner]<-10000000.0 || quad->x[corner]>10000000.0 ||
       quad->y[corner]<-10000000.0 || quad->y[corner]>10000000.0 ||
       (!rotated && !warped && !half_gui &&
        quad->sampling.quantisation!=GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN &&
        quad->sampling.quantisation!=GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN &&
        quad->sampling.quantisation!=GML_RENDER_QUANTISE_HARDWARE_SURFACE_LEADING &&
        quad->sampling.quantisation!=GML_RENDER_QUANTISE_SURFACE_TILED_LEADING &&
        quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT &&
        !(op->scene_family==GML_RENDER_SCENE_SPRITE &&
          quad->span_active==GML_RENDER_SPAN_FILTERED_AXIS_SUPPORT_ROW &&
          quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
          (quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
           quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND)) &&
        !(op->scene_family==GML_RENDER_SCENE_BACKGROUND &&
          quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
          (state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
           (quad->span_active==GML_RENDER_SPAN_FILTERED_POSITIVE_ROW &&
            (quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
             quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND)))) &&
        (quad->u[corner]<0.0 || quad->u[corner]>1.0 ||
                               quad->v[corner]<0.0 || quad->v[corner]>1.0)) ||
       (warped && (!isfinite(quad->u[corner]) || !isfinite(quad->v[corner]) ||
                   quad->u[corner]<0.0 || quad->u[corner]>region.width ||
                   quad->v[corner]<0.0 || quad->v[corner]>region.height))) return 0;
    vertex[0]=(float)quad->x[corner];
    vertex[1]=(float)quad->y[corner];
    vertex[2]=rotated?(float)quad->inverse_y[0]:(float)quad->u[corner];
    vertex[3]=rotated?(float)quad->inverse_y[1]
                     :(rendered_source_origin?1.0f-(float)quad->v[corner]
                                            :(float)quad->v[corner]);
    vertex[4]=(float)((colour>>16)&255u)/255.0f;
    vertex[5]=(float)((colour>>8)&255u)/255.0f;
    vertex[6]=(float)(colour&255u)/255.0f;
    vertex[7]=(float)((colour>>24)&255u)/255.0f*(float)state->alpha/255.0f;
    if(quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_ROW_TINT_TRUNC ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_FLAT_RGB ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_POINT ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_ROTATED_POINT ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_BOX_FUNCTION ||
       half_gui || stretched_blend || (rotated && state->coverage_policy) ||
       (quad->sampling.filter==GML_RENDER_FILTER_BOX &&
        state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
       (op->scene_family==GML_RENDER_SCENE_SURFACE &&
        state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
       quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR)
      vertex[7]=(float)quad->filtered_draw_alpha;
    vertex[8]=(float)region.x;
    vertex[9]=(float)region_storage_y;
    vertex[10]=(float)((uint32_t)region.x+region.width-1u);
    vertex[11]=(float)(region_storage_y+region.height-1u);
    vertex[12]=rotated?(float)quad->inverse_anchor[0]:(float)quad->x[0];
    vertex[13]=rotated?(float)quad->inverse_anchor[1]:(float)quad->y[0];
    vertex[14]=warped?0.0f:rotated?(float)quad->inverse_x[0]
                      :(float)(quad->u[0]*source_width-region.x);
    vertex[15]=warped?0.0f:rotated?(float)quad->inverse_x[1]
                      :(float)((rendered_source_origin?1.0-quad->v[0]:quad->v[0])*
                                source_height-region_storage_y);
    vertex[16]=warped?0.0f:rotated?(float)quad->inverse_offset[0]
                      :(float)((quad->u[1]-quad->u[0])*source_width/
                               (quad->x[1]-quad->x[0])-1.0);
    vertex[17]=warped?0.0f:rotated?(float)quad->inverse_offset[1]
                      :(float)((rendered_source_origin?quad->v[0]-quad->v[2]
                                :quad->v[2]-quad->v[0])*source_height/
                               (quad->y[2]-quad->y[0])-1.0);
    if(quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT ||
       quad->sampling.filter==GML_RENDER_FILTER_VIEW_PIXEL_CENTRE ||
       quad->sampling.filter==GML_RENDER_FILTER_VIEW_SEED ||
       quad->sampling.filter==GML_RENDER_FILTER_SAMPLE_PLANE_COPY ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_POINT ||
       quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_BOX_FUNCTION){
      /* Preserve the authored rectangle, not interpolated UVs at its clipped edge. */
      vertex[12]=(float)quad->inverse_anchor[0];
      vertex[13]=(float)quad->inverse_anchor[1];
      vertex[14]=(float)quad->inverse_x[0];
      vertex[15]=(float)quad->inverse_y[1];
      vertex[16]=rendered_source_origin?1.0f:0.0f;
      vertex[17]=(float)op->target.height;
    }
    vertex[18]=quad->sampling.edge_rule==GML_RENDER_EDGE_TRANSPARENT
      ?16.0f+(float)quad->sampling.logical_margin_mask
      :(float)quad->sampling.logical_margin_mask;
    if(rotated && state->coverage_policy)
      vertex[18]=state->coverage_policy==GML_RENDER_COVERAGE_ROTATED_VECTOR_ALPHA?1.0f:0.0f;
    for(uint32_t span=0;span<4;span++){
      vertex[19+span]=(float)quad->span_x[span];
      vertex[23+span]=(float)quad->span_y[span];
    }
    if(!rotated && state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
       quad->sampling.quantisation!=GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION &&
       quad->sampling.quantisation!=GML_RENDER_QUANTISE_SURFACE_TILED_POINT &&
       quad->sampling.quantisation!=GML_RENDER_QUANTISE_SURFACE_ROTATED_POINT &&
       quad->sampling.filter!=GML_RENDER_FILTER_BOX &&
       quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR &&
       quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT){
      /* The axis screen rule has no rotated/box span. Retain low residuals of
       * its existing phase and reciprocal so a wide quad does not accumulate
       * a float-step error into a different rounded source channel. */
      double phase_x=quad->u[0]*source_width-region.x;
      double phase_y=quad->v[0]*source_height-region_storage_y;
      double step_x=(quad->u[1]-quad->u[0])*source_width/
                     (quad->x[1]-quad->x[0])-1.0;
      double step_y=(quad->v[2]-quad->v[0])*source_height/
                     (quad->y[2]-quad->y[0])-1.0;
      vertex[19]=(float)(phase_x-(double)vertex[14]);
      vertex[20]=(float)(step_x-(double)vertex[16]);
      vertex[23]=(float)(phase_y-(double)vertex[15]);
      vertex[24]=(float)(step_y-(double)vertex[17]);
    }
    vertex[27]=quad->span_active?1.0f:0.0f;
  }
  if(rotated){
    for(uint32_t coordinate=0;coordinate<2;coordinate++)
      if(!isfinite(quad->inverse_anchor[coordinate]) ||
         !isfinite(quad->inverse_x[coordinate]) ||
         !isfinite(quad->inverse_y[coordinate]) ||
         !isfinite(quad->inverse_offset[coordinate]) ||
         fabs(quad->inverse_anchor[coordinate])>16384.0 ||
         fabs(quad->inverse_x[coordinate])>16384.0 ||
         fabs(quad->inverse_y[coordinate])>16384.0 ||
         fabs(quad->inverse_offset[coordinate])>16384.0) return 0;
    if(quad->span_active){
      if(quad->sampling.filter!=GML_RENDER_FILTER_BILINEAR &&
         quad->sampling.filter!=GML_RENDER_FILTER_NEAREST) return 0;
      for(uint32_t corner=0;corner<4;corner++)
        if(!isfinite(quad->span_x[corner]) || !isfinite(quad->span_y[corner]) ||
           fabs(quad->span_x[corner])>16384.0 ||
           fabs(quad->span_y[corner])>16384.0) return 0;
    }
  }
  return 1;
}
