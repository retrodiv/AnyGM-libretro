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

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

enum { GL_SCENE_MAX_EXACT_REPETITIONS=64, GL_SCENE_BLEND_INVERSE_RECT=12,
       GL_SCENE_BLEND_RECTANGLE_BASE=32, GL_SCENE_BLEND_RECTANGLE_LIMIT=55,
       GL_SCENE_BLEND_RECTANGLE_ADD_BYTES=56,
       GL_SCENE_BLEND_SURFACE_DEST_BASE=64,GL_SCENE_BLEND_SURFACE_DEST_LIMIT=213 };

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
  if(state->rgb_equation==GML_RENDER_BLEND_ADD && state->alpha_equation==GML_RENDER_BLEND_ADD){
    int opaque=state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    if((!state->coverage_policy || opaque) &&
       state->rgb_source_factor==GML_RENDER_BLEND_DESTINATION_COLOUR &&
       state->rgb_destination_factor==GML_RENDER_BLEND_ZERO &&
       state->alpha_source_factor==(opaque?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_ZERO) &&
       state->alpha_destination_factor==(opaque?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_ONE)) return 22+opaque;
    if((!state->coverage_policy || opaque) &&
       state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
       state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR &&
       state->alpha_source_factor==(opaque?GML_RENDER_BLEND_ONE:GML_RENDER_BLEND_SOURCE_ALPHA) &&
       state->alpha_destination_factor==(opaque?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA)) return 24+opaque;
  }
  if((state->rgb_source_factor==GML_RENDER_BLEND_DESTINATION_COLOUR ||
      state->rgb_source_factor==GML_RENDER_BLEND_DESTINATION_ALPHA) &&
     (state->rgb_destination_factor==GML_RENDER_BLEND_SOURCE_COLOUR ||
      state->rgb_destination_factor==GML_RENDER_BLEND_DESTINATION_COLOUR) &&
     !(state->rgb_source_factor==GML_RENDER_BLEND_DESTINATION_ALPHA &&
       state->rgb_destination_factor==GML_RENDER_BLEND_SOURCE_COLOUR) &&
     state->alpha_source_factor==state->rgb_source_factor &&
     state->alpha_destination_factor==state->rgb_destination_factor &&
     (!state->coverage_policy || state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA)){
    int rgb=state->rgb_equation==GML_RENDER_BLEND_ADD?0:
      state->rgb_equation>=GML_RENDER_BLEND_FIXED_MAX && state->rgb_equation<=GML_RENDER_BLEND_FIXED_MIN?
        (int)state->rgb_equation-GML_RENDER_BLEND_FIXED_MAX+1:-1;
    int alpha=state->alpha_equation==GML_RENDER_BLEND_ADD?0:
      state->alpha_equation>=GML_RENDER_BLEND_FIXED_MAX && state->alpha_equation<=GML_RENDER_BLEND_FIXED_MIN?
        (int)state->alpha_equation-GML_RENDER_BLEND_FIXED_MAX+1:-1;
    unsigned mode=state->rgb_source_factor==GML_RENDER_BLEND_DESTINATION_ALPHA?2u:
      state->rgb_destination_factor==GML_RENDER_BLEND_SOURCE_COLOUR?0u:1u;
    if(rgb>=0 && alpha>=0) return GL_SCENE_BLEND_SURFACE_DEST_BASE+
      (int)(2u*(mode*25u+(unsigned)rgb*5u+(unsigned)alpha))+(state->coverage_policy==0u);
  }
  if(state->rgb_equation==GML_RENDER_BLEND_ADD && state->alpha_equation==GML_RENDER_BLEND_ADD &&
     (state->rgb_source_factor==GML_RENDER_BLEND_ONE_MINUS_DESTINATION_ALPHA ||
      state->rgb_source_factor==GML_RENDER_BLEND_ONE_MINUS_DESTINATION_COLOUR)){
    int colour=state->rgb_source_factor==GML_RENDER_BLEND_ONE_MINUS_DESTINATION_COLOUR;
    unsigned destination=colour?GML_RENDER_BLEND_ZERO:GML_RENDER_BLEND_DESTINATION_ALPHA;
    if(state->rgb_destination_factor!=destination) return -1;
    if(state->alpha_source_factor==GML_RENDER_BLEND_ONE_MINUS_DESTINATION_ALPHA &&
       state->alpha_destination_factor==destination && !state->coverage_policy) return 18+colour;
    if(state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
       state->alpha_destination_factor==GML_RENDER_BLEND_ZERO &&
       state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) return 20+colour;
    return -1;
  }
  if(state->alpha_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->rgb_equation==GML_RENDER_BLEND_ADD && state->alpha_equation==GML_RENDER_BLEND_ADD){
    if(state->rgb_source_factor==GML_RENDER_BLEND_ONE &&
       state->rgb_destination_factor==GML_RENDER_BLEND_ZERO) return 17;
    if(state->rgb_source_factor==GML_RENDER_BLEND_ZERO &&
       state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR) return 15;
    if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
       state->rgb_destination_factor==GML_RENDER_BLEND_ONE) return 16;
  }
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE &&
     state->rgb_equation==GML_RENDER_BLEND_ADD && state->alpha_equation==GML_RENDER_BLEND_ADD){
    if(state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
       state->alpha_destination_factor==GML_RENDER_BLEND_ONE) return 14;
  }
  if(state->rgb_source_factor==GML_RENDER_BLEND_ZERO &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_COLOUR &&
     state->alpha_source_factor==GML_RENDER_BLEND_ZERO &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->rgb_equation==GML_RENDER_BLEND_ADD &&
     state->alpha_equation==GML_RENDER_BLEND_ADD) return 13;
  if((state->rgb_source_factor==GML_RENDER_BLEND_DESTINATION_COLOUR ||
      state->rgb_source_factor==GML_RENDER_BLEND_DESTINATION_ALPHA) &&
     (state->rgb_destination_factor==GML_RENDER_BLEND_SOURCE_COLOUR ||
      state->rgb_destination_factor==GML_RENDER_BLEND_DESTINATION_COLOUR) &&
     !(state->rgb_source_factor==GML_RENDER_BLEND_DESTINATION_ALPHA &&
       state->rgb_destination_factor==GML_RENDER_BLEND_SOURCE_COLOUR) &&
     state->alpha_source_factor==state->rgb_source_factor &&
     state->alpha_destination_factor==state->rgb_destination_factor &&
     state->rgb_equation>=GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
     state->rgb_equation<=GML_RENDER_BLEND_EXACT_DESTINATION_MIN &&
     state->alpha_equation>=GML_RENDER_BLEND_EXACT_DESTINATION_ADD &&
     state->alpha_equation<=GML_RENDER_BLEND_EXACT_DESTINATION_MIN) return 11;
  if(state->rgb_source_factor==GML_RENDER_BLEND_ONE &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->alpha_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->rgb_equation==GML_RENDER_BLEND_ADD &&
     state->alpha_equation==GML_RENDER_BLEND_ADD) return 10;
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
                             const GmlRenderDrawStateSnapshot *state,uint32_t family){
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_POSITIONED_SAMPLE){
    int blend=qualified_blend(state);
    unsigned filtered=quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR;
    int opaque=state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    if(quad->sampling.filter==GML_RENDER_FILTER_BOX){
      if(opaque && blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT)
        return 170u;
      return opaque?168u:167u;
    }
    if(opaque && (blend==20 || blend==21 ||
       (blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT)))
      return 165u+filtered;
    return 159u+filtered+2u*opaque;
  }
  if((family==GML_RENDER_SCENE_BACKGROUND || family==GML_RENDER_SCENE_TILE ||
      ((family==GML_RENDER_SCENE_SPRITE || family==GML_RENDER_SCENE_TEXT) &&
       quad->span_active==GML_RENDER_SPAN_FILTERED_AXIS_SUPPORT_ROW)) &&
     quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
     (quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
      quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND)){
    unsigned base=quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND?151u:147u;
    int blend=qualified_blend(state);
    if(blend==9) return (base==151u?157u:155u)+
      (state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA);
    if(state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA) return base;
    if(!blend || blend==20 || blend==21 ||
       (blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT &&
        state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA)) return base+2u;
    return base+(state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA);
  }
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_ROTATED_POINT){
    int blend=qualified_blend(state);
    if(blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT &&
       state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) return 135u;
    return state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?133u:136u;
  }
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_BOX_FUNCTION){
    int blend=qualified_blend(state);
    if(!blend || (blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT &&
       state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA)) return 132u;
    return state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?130u:129u;
  }
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_FIXED_STRETCHED)
    return quad->sampling.filter==GML_RENDER_FILTER_BOX?75u:77u;
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN)
    return (quad->span_active==GML_RENDER_SPAN_FILTERED_POSITIVE_ROW ||
            quad->span_active==GML_RENDER_SPAN_FILTERED_AXIS_SUPPORT_ROW)
      ?!qualified_blend(state)?114u:
        state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?112u:115u:
      !qualified_blend(state)?81u:
      state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?80u:79u;
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN){
    int blend=qualified_blend(state);
    unsigned opaque=state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
    unsigned filter=!blend?state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA?73u:67u:
      blend==15?68u+opaque:blend==16?70u+opaque:65u+opaque;
    /* CPU-covered rows share the hardware color recipe but must not repeat
     * a rounded-alpha discard that could change their exact footprint. */
    return quad->span_active==GML_RENDER_SPAN_FILTERED_POSITIVE_ROW ||
      quad->span_active==GML_RENDER_SPAN_FILTERED_AXIS_SUPPORT_ROW?filter+72u:filter;
  }
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION ||
     quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_POINT){
    int blend=qualified_blend(state);
    if(blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT &&
       state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) return 56u;
    if(blend==13)
      return state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?60u:58u;
    return !qualified_blend(state)?56u:
      state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?53u:51u;
  }
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
     quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND ||
     quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_ROW_TINT_TRUNC ||
     quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_FLAT_RGB){
    uint32_t base=quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND?38u:37u;
    uint32_t filter;
    if(state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA) filter=83u;
    else if(state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA) filter=base+12u;
    else if((family==GML_RENDER_SCENE_BACKGROUND || family==GML_RENDER_SCENE_SPRITE || family==GML_RENDER_SCENE_TILE) &&
      qualified_blend(state)>=GL_SCENE_BLEND_SURFACE_DEST_BASE &&
      qualified_blend(state)<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT &&
      state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) filter=base+12u;
    /* Destination-alpha RGB observes each preceding fragment's coverage.
     * Opaque-target variants must therefore write alpha in this same draw,
     * not defer the ordinary source-over alpha stamp until a batch ends. */
    else if(qualified_blend(state)==20 || qualified_blend(state)==21) filter=base+5u;
    /* The tile CPU leaves keep nonzero binary64 opacity even when its float
     * packet alpha underflows. Preserve RGB factors and opaque additive
     * coverage; their raw-zero source rejection is explicit in the state. */
    else if(family==GML_RENDER_SCENE_TILE &&
      (qualified_blend(state)==14 || qualified_blend(state)==18 ||
       qualified_blend(state)==19 || qualified_blend(state)==24 || qualified_blend(state)==25))
      filter=base+7u+(state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?2u:0u);
    else if(qualified_blend(state)==13 ||
      ((family==GML_RENDER_SCENE_BACKGROUND || family==GML_RENDER_SCENE_SPRITE || family==GML_RENDER_SCENE_TILE) && qualified_blend(state)>=GL_SCENE_BLEND_SURFACE_DEST_BASE &&
       qualified_blend(state)<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT)) filter=base+7u+
      (state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?2u:0u);
    else filter=!qualified_blend(state)?base+5u:
      state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?base+2u:base;
    return quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_FLAT_RGB
      ?filter==83u?98u:filter+47u:filter;
  }
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST) return 27u;
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_ROTATED_NEAREST) return 29u;
  if(quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT) return 30u;
  if(quad->sampling.filter==GML_RENDER_FILTER_VIEW_PIXEL_CENTRE) return 101u;
  if(quad->sampling.filter==GML_RENDER_FILTER_VIEW_SEED) return 102u;
  if(quad->sampling.filter==GML_RENDER_FILTER_SAMPLE_PLANE_COPY) return 128u;
  if((family==GML_RENDER_SCENE_SURFACE ||
      ((family==GML_RENDER_SCENE_SPRITE || family==GML_RENDER_SCENE_TEXT) &&
       quad->shape==GML_RENDER_QUAD_AXIS_ALIGNED)) &&
     quad->sampling.filter==GML_RENDER_FILTER_NEAREST &&
     state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) return 25u;
  if(quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR)
    return state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA
      ?(family==GML_RENDER_SCENE_BACKGROUND?99u:21u):quad->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_COMBINED
      ?20u:qualified_blend(state)==10?19u:18u;
  if(qualified_blend(state)==8) return 14u;
  if(state->coverage_policy==GML_RENDER_COVERAGE_ROTATED_VECTOR_ALPHA) return 17u;
  if(state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA)
    return quad->shape==GML_RENDER_QUAD_AXIS_ALIGNED?12u:15u;
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_STRETCHED_FLOAT) return 11u;
  if(quad->sampling.filter==GML_RENDER_FILTER_BOX) return 4u;
  if(quad->sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED) return 7u;
  if(quad->sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED_COPY) return 8u;
  if(quad->sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA) return 9u;
  if(quad->sampling.filter==GML_RENDER_FILTER_NEAREST_PHASED_PACKET_ALPHA) return 110u;
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

/* Full-source surface packets carry integral destination-loop sampling and
 * versioned mutable sources through the existing fixed-blend executor. */
/* A one-pixel edge strip retains the CPU compositor's missing quarter-colour
 * contribution. Ordinary nearest copies stay untinted and preserve raw alpha. */
static int nearest_surface_fringe_valid(const GmlRenderDrawOp *op,
  const GmlRenderDrawStateSnapshot *state,const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  if(!source || !source->rendered_origin || source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     (op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
     op->scene_family!=GML_RENDER_SCENE_SURFACE ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active ||
     q->x[1]-q->x[0]!=1.0 || q->y[2]-q->y[0]!=1.0 ||
     q->sampling.filter!=GML_RENDER_FILTER_NEAREST || q->sampling.quantisation ||
     q->sampling.logical_margin_mask || q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     !full_region(q->sampling.source_region,source->width,source->height) ||
     q->filtered_draw_alpha!=1.0 || qualified_blend(state) || state->coverage_policy ||
     state->alpha_test_enabled || state->write_mask!=15u) return 0;
  double row=q->v[0]*source->height-.5;
  if(!isfinite(row) || row<0.0 || row>source->height-1.0 || fabs(row-nearbyint(row))>1e-9) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if(q->corner_colour[corner]!=0xFFBFBFBFu ||
       q->u[corner]!=((double)source->width-.5)/source->width ||
       q->v[corner]!=q->v[0]) return 0;
  return 1;
}

static int nearest_surface_function_valid(const GmlRenderDrawOp *op,
                                           const GmlRenderDrawStateSnapshot *state,
                                           const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  int blend=qualified_blend(state);
  int tiled=q->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_POINT;
  double width=q->x[1]-q->x[0],height=q->y[2]-q->y[0];
  if(!source || op->scene_family!=GML_RENDER_SCENE_SURFACE ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     (op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SURFACE &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
     source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     source->width>4096u || source->height>4096u ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SURFACE) ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active ||
     q->sampling.filter!=GML_RENDER_FILTER_NEAREST ||
     (q->sampling.quantisation!=GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION && !tiled) ||
     (!tiled && q->span_y[3]!=0.0 && blend!=0 && blend!=1 && blend!=3 && blend!=13 && blend!=14) ||
     q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE || q->sampling.logical_margin_mask ||
     !full_region(q->sampling.source_region,source->width,source->height) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1) ||
     width>16384.0 || height>16384.0 || fabs(q->x[0])>16384.0 || fabs(q->y[0])>16384.0 ||
     q->inverse_anchor[0]!=q->x[0] || q->inverse_anchor[1]!=q->y[0] ||
     fabs(q->inverse_x[0])!=width || fabs(q->inverse_y[1])!=height ||
     (q->span_x[0]!=0.0 && q->span_x[0]!=1.0 && q->span_x[0]!=2.0 && q->span_x[0]!=3.0) ||
     (q->span_x[1]!=0.0 && q->span_x[1]!=1.0 && q->span_x[1]!=2.0 && q->span_x[1]!=3.0) ||
     (q->span_y[0]!=0.0 && q->span_y[0]!=1.0 && q->span_y[0]!=2.0 && q->span_y[0]!=3.0 && q->span_y[0]!=4.0) ||
     (q->span_y[0]>0.0 && q->span_y[0]<3.0 && (blend || state->coverage_policy)) ||
     (q->span_y[0]==3.0 && (blend!=1 || state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA)) ||
     (q->span_y[0]==4.0 && ((!tiled && q->span_y[3]!=0.0) || (blend!=22 && blend!=23 &&
       (blend<GL_SCENE_BLEND_SURFACE_DEST_BASE || blend>GL_SCENE_BLEND_SURFACE_DEST_LIMIT)))) ||
     state->write_mask>15u || state->alpha_test_enabled>1u || state->alpha_test_reference>255u ||
     (state->coverage_policy && state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
     (blend!=0 && blend!=1 && blend!=3 && blend!=13 && blend!=14 &&
      (blend<18 || blend>25) &&
      (blend<GL_SCENE_BLEND_SURFACE_DEST_BASE || blend>GL_SCENE_BLEND_SURFACE_DEST_LIMIT)) ||
     !isfinite(q->filtered_draw_alpha) || q->filtered_draw_alpha<=0.0 || q->filtered_draw_alpha>1.0)
    return 0;
  if(q->span_y[3]!=0.0 && q->span_y[3]!=1.0 && q->span_y[3]!=2.0) return 0;
  if(q->span_y[3]==0.0){
    if(q->span_x[2]!=0.0 || q->span_x[3]!=0.0 || q->span_y[1]!=0.0 || q->span_y[2]!=0.0) return 0;
  }else{
    if(q->span_x[0]!=0.0 || q->span_x[1]!=0.0 ||
       !isfinite(q->span_x[2]) || !isfinite(q->span_x[3]) ||
       fabs(q->span_x[2])>16384.0 || fabs(q->span_x[3])>16384.0 ||
       !isfinite(q->span_y[1]) || !isfinite(q->span_y[2]) ||
       q->span_y[1]<=0.0 || q->span_y[2]<=0.0 ||
       q->span_y[1]>16384.0 || q->span_y[2]>16384.0) return 0;
    if(q->span_y[3]==2.0 && (q->span_x[2]!=floor(q->span_x[2]) ||
       q->span_x[3]!=floor(q->span_x[3]) || q->span_y[1]!=floor(q->span_y[1]) ||
       q->span_y[2]!=floor(q->span_y[2]))) return 0;
  }
  for(unsigned corner=0;corner<4u;corner++)
    if(q->corner_colour[corner]!=q->corner_colour[0] ||
       q->u[corner]!=(corner==0u || corner==3u?0.0:1.0) ||
       q->v[corner]!=(corner<2u?0.0:1.0) ||
       (q->corner_colour[corner]>>24)!=(uint32_t)lround(q->filtered_draw_alpha*255.0)) return 0;
  return 1;
}

static int rotated_surface_point_valid(const GmlRenderDrawOp *op,
  const GmlRenderDrawStateSnapshot *state,const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  int blend=qualified_blend(state);
  int destination=blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT;
  int ordinary=blend==0 || blend==1 || blend==13 || blend==14;
  int paint_zero=blend!=1;
  int rounded_alpha=destination || blend==19 || blend==21 || blend==24 || blend==25;
  if(!source || op->scene_family!=GML_RENDER_SCENE_SURFACE ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT || op->source_count!=1u || !op->sources ||
     (op->sources[0].resource_class!=GML_RENDER_RESOURCE_SURFACE &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
     source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 || source->width>4096u || source->height>4096u ||
     (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SURFACE) ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 || op->target.width>16384u || op->target.height>16384u ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active ||
     q->sampling.filter!=GML_RENDER_FILTER_NEAREST ||
     q->sampling.quantisation!=GML_RENDER_QUANTISE_SURFACE_ROTATED_POINT ||
     q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE || q->sampling.logical_margin_mask ||
     !full_region(q->sampling.source_region,source->width,source->height) || !exact_rect(op,NULL,NULL,NULL,NULL,1) ||
     q->x[0]<0 || q->y[0]<0 || q->x[1]>op->target.width || q->y[2]>op->target.height ||
     q->y[2]-q->y[0]!=1 || q->x[1]-q->x[0]>16384 ||
     (!ordinary && blend!=19 && blend!=21 && (blend<22 || blend>25) && !destination) ||
     (ordinary && state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
     (state->coverage_policy && state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
     !state->write_mask || state->write_mask>15u || state->alpha_test_enabled>1u ||
     state->alpha_test_reference>255u || (state->alpha_test_enabled && !destination) ||
     !isfinite(q->filtered_draw_alpha) || q->filtered_draw_alpha<0 || q->filtered_draw_alpha>1 ||
     (!q->filtered_draw_alpha && !destination) || q->span_y[0]!=paint_zero || q->span_y[1]!=rounded_alpha ||
     q->span_y[2] || q->span_y[3]) return 0;
  for(unsigned i=0;i<4u;i++) if(q->span_x[i]) return 0;
  for(unsigned i=0;i<2u;i++)
    if(q->inverse_anchor[i] || q->inverse_x[i] || q->inverse_y[i] || q->inverse_offset[i]) return 0;
  for(unsigned axis=0;axis<2u;axis++){
    double uv=axis?q->v[0]:q->u[0],size=axis?source->height:source->width;
    double selected=uv*size-.5;
    if(!isfinite(uv) || uv<=0 || uv>=1 || fabs(selected-nearbyint(selected))>1e-10) return 0;
  }
  for(unsigned corner=0;corner<4u;corner++)
    if(q->u[corner]!=q->u[0] || q->v[corner]!=q->v[0] ||
       q->corner_colour[corner]!=q->corner_colour[0]) return 0;
  return 1;
}

static int positioned_sample_valid(const GmlRenderDrawOp *op,
  const GmlRenderDrawStateSnapshot *state,const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  const GmlRenderSamplingPolicy *s=&q->sampling,*sampler=&state->samplers[0];
  int blend=qualified_blend(state);
  if(!source || !source->texture || source->rendered_origin ||
     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
     !source->width || !source->height || source->width>16384u || source->height>16384u ||
     op->scene_family!=GML_RENDER_SCENE_SPRITE ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     op->source_count!=1u || !op->sources || op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     same_logical(op->sources[0],op->target.image) ||
     (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SURFACE) ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->target.width>16384u || op->target.height>16384u ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active ||
     s->quantisation!=GML_RENDER_QUANTISE_POSITIONED_SAMPLE ||
     (s->filter!=GML_RENDER_FILTER_NEAREST && s->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR &&
      s->filter!=GML_RENDER_FILTER_BOX) ||
     s->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE || s->logical_margin_mask>15u ||
     (s->logical_margin_mask && s->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR) ||
     !valid_region(s->source_region,source->width,source->height) ||
     s->source_region.width>4096u || s->source_region.height>4096u ||
     !exact_rect(op,NULL,NULL,NULL,NULL,0) || q->y[2]-q->y[0]!=1.0 ||
     !isfinite(q->filtered_draw_alpha) || q->filtered_draw_alpha< -1.0 || q->filtered_draw_alpha>1.0 ||
     !isfinite(q->span_x[0]) || q->span_x[0]<0.0 || q->span_x[0]>19.0 ||
     q->span_x[0]!=floor(q->span_x[0]) ||
     q->span_x[1] || q->span_x[2] || q->span_x[3] ||
     q->span_y[1] || q->span_y[2] || q->span_y[3] ||
     !isfinite(q->span_y[0]) || q->span_y[0]<0.0 || q->span_y[0]>255.0 ||
     (q->span_x[0]<3.0 && q->span_y[0]) ||
     (s->filter==GML_RENDER_FILTER_BOX && (q->span_x[0]>3.0 ||
       (uint64_t)s->source_region.width*s->source_region.height>INT_MAX/255u)) ||
     (q->span_x[0]>=4.0 && (blend!=0 || state->write_mask!=15u)) ||
     !state->write_mask || state->write_mask>15u || state->colour!=0xFFFFFFFFu || state->alpha!=255u ||
     state->alpha_test_enabled || state->source_alpha_key ||
     (state->source_palette.enabled && s->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR) ||
     state->sampler_count!=1u ||
     sampler->filter!=s->filter || sampler->edge_rule!=s->edge_rule ||
     sampler->quantisation!=s->quantisation || sampler->logical_margin_mask!=s->logical_margin_mask ||
     !valid_region(sampler->source_region,source->width,source->height) ||
     s->source_region.x<sampler->source_region.x || s->source_region.y<sampler->source_region.y ||
     (uint64_t)(unsigned)s->source_region.x+s->source_region.width>
       (uint64_t)(unsigned)sampler->source_region.x+sampler->source_region.width ||
     (uint64_t)(unsigned)s->source_region.y+s->source_region.height>
       (uint64_t)(unsigned)sampler->source_region.y+sampler->source_region.height ||
     (state->coverage_policy && state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
     (blend!=0 && blend!=1 && blend!=3 && blend!=13 && blend!=14 && blend!=15 && blend!=16 &&
      (blend<18 || blend>25) &&
      (blend<GL_SCENE_BLEND_SURFACE_DEST_BASE || blend>GL_SCENE_BLEND_SURFACE_DEST_LIMIT))) return 0;
  for(unsigned i=0;i<2u;i++)
    if(q->inverse_anchor[i] || q->inverse_x[i] || q->inverse_y[i] || q->inverse_offset[i]) return 0;
  for(unsigned c=0;c<4u;c++)
    if(q->corner_colour[c]!=0xFFFFFFFFu || !isfinite(q->u[c]) || !isfinite(q->v[c]) ||
       q->u[c]!=q->u[0] || q->v[c]!=q->v[0] ||
       (s->logical_margin_mask?
         (q->u[c]*source->width< -16384.0 || q->u[c]*source->width>source->width+16384.0 ||
          q->v[c]*source->height< -16384.0 || q->v[c]*source->height>source->height+16384.0):
         (q->u[c]*source->width<s->source_region.x ||
          q->u[c]*source->width>s->source_region.x+s->source_region.width ||
          q->v[c]*source->height<s->source_region.y ||
          q->v[c]*source->height>s->source_region.y+s->source_region.height))) return 0;
  if(s->filter==GML_RENDER_FILTER_NEAREST || s->filter==GML_RENDER_FILTER_BOX){
    double px=q->u[0]*source->width-.5,py=q->v[0]*source->height-.5;
    if(fabs(px-nearbyint(px))>1e-9 || fabs(py-nearbyint(py))>1e-9) return 0;
  }
  return 1;
}

static int box_surface_function_valid(const GmlRenderDrawOp *op,
                                      const GmlRenderDrawStateSnapshot *state,
                                      const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  if(!source || q->sampling.filter!=GML_RENDER_FILTER_BOX ||
     q->sampling.quantisation!=GML_RENDER_QUANTISE_SURFACE_BOX_FUNCTION ||
     q->span_x[0] || q->span_x[1] || q->span_y[3] ||
     source->width>(q->x[1]-q->x[0])*8.0 ||
     source->height>(q->y[2]-q->y[0])*8.0) return 0;
  GmlRenderDrawOp point=*op;
  point.payload.quad.sampling.filter=GML_RENDER_FILTER_NEAREST;
  point.payload.quad.sampling.quantisation=GML_RENDER_QUANTISE_SURFACE_TILED_POINT;
  return nearest_surface_function_valid(&point,state,source);
}

/* R03-SPR-INTEGER-FUNCTION: only whole physical atlas texels and uniform RGB
 * reach this cheap integer tint/fixed-blend packet. Clipping retains its UVs. */
static int integer_function_sprite_valid(const GmlRenderDrawOp *op,
                                          const GmlRenderDrawStateSnapshot *state,
                                          const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  GmlRenderRect region=q->sampling.source_region;
  int blend=qualified_blend(state);
  int row=q->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_ROW_TINT_TRUNC;
  int flat=q->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_FLAT_RGB;
  int background=op->scene_family==GML_RENDER_SCENE_BACKGROUND || op->scene_family==GML_RENDER_SCENE_TILE;
  int text_empty=op->scene_family==GML_RENDER_SCENE_TEXT && !row && !flat &&
    region.width==1u && region.height==1u && q->filtered_draw_alpha==0.0 &&
    !q->corner_colour[0] && !q->corner_colour[1] &&
    !q->corner_colour[2] && !q->corner_colour[3] && state->write_mask==15u;
  int background_blend=(background || (op->scene_family==GML_RENDER_SCENE_SPRITE && !row && !flat)) && (blend==24 || blend==25 ||
    (blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT &&
     state->rgb_equation==GML_RENDER_BLEND_ADD && state->alpha_equation==GML_RENDER_BLEND_ADD));
  if(!source || (op->scene_family!=GML_RENDER_SCENE_SPRITE &&
                op->scene_family!=GML_RENDER_SCENE_BACKGROUND &&
                op->scene_family!=GML_RENDER_SCENE_TILE &&
                op->scene_family!=GML_RENDER_SCENE_TEXT) ||
     (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SURFACE &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS || source->rendered_origin ||
     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 || source->width>16384u || source->height>16384u ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT || q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     q->span_active || q->source_endpoint_rgb_mask || q->sampling.filter!=GML_RENDER_FILTER_NEAREST ||
     (q->sampling.quantisation!=GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC &&
      q->sampling.quantisation!=GML_RENDER_QUANTISE_INTEGER_TINT_ROUND && !row && !flat) ||
     (flat && op->scene_family!=GML_RENDER_SCENE_SPRITE) ||
     (row && (op->scene_family!=GML_RENDER_SCENE_SPRITE ||
       q->y[2]-q->y[0]!=1.0 || q->x[1]-q->x[0]>4096.0 ||
       !state->alpha_test_enabled || state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA)) ||
     q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE || q->sampling.logical_margin_mask ||
     !valid_region(region,source->width,source->height) || region.width>4096u || region.height>4096u ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1) || fabs(q->x[0])>65536.0 || fabs(q->y[0])>65536.0 ||
     !isfinite(q->filtered_draw_alpha) || q->filtered_draw_alpha<0.0 || q->filtered_draw_alpha>1.0 ||
     (q->filtered_draw_alpha==0.0 && !background_blend &&
      ((op->scene_family!=GML_RENDER_SCENE_SPRITE && !background && !text_empty) ||
       (state->coverage_policy!=GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA &&
        state->coverage_policy!=GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA))) ||
     state->alpha_test_enabled>1u || state->alpha_test_reference>255u ||
     (state->write_mask>15u || (op->scene_family!=GML_RENDER_SCENE_SPRITE &&
       !(background_blend || (background && blend>=18 && blend<=21)) && state->write_mask!=15u)) ||
     (blend==14 && op->scene_family!=GML_RENDER_SCENE_SPRITE && op->scene_family!=GML_RENDER_SCENE_TEXT && !background) ||
     (background && blend==14 && state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
     (blend>=18 && blend<=21 && ((op->scene_family!=GML_RENDER_SCENE_SPRITE && !background) || flat)) ||
     (blend!=0 && blend!=1 && blend!=3 && blend!=13 && blend!=14 && !background_blend && (blend<18 || blend>21)) ||
     ((state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA ||
       state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA)?
       (blend!=0 || (state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA &&
        ((op->scene_family!=GML_RENDER_SCENE_SPRITE && !background && !text_empty) ||
         (q->sampling.quantisation!=GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC && !flat)))):
      state->coverage_policy && (state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
       (blend!=1 && blend!=13 && blend!=14 && blend!=20 && blend!=21 && !background_blend) || (blend!=14 && !background_blend &&
        !(blend==13 && op->scene_family==GML_RENDER_SCENE_SPRITE &&
          (source->ref.identity&UINT32_C(0x80000000)) &&
          (q->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC || row)) &&
        op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
        op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
        op->target.image.resource_class!=GML_RENDER_RESOURCE_SCRATCH)))) return 0;
  double width=q->x[1]-q->x[0],height=q->y[2]-q->y[0];
  if(width>16384.0 || height>16384.0) return 0;
  double u0=(double)region.x/source->width,u1=((double)region.x+region.width)/source->width;
  double v0=(double)region.y/source->height,v1=((double)region.y+region.height)/source->height;
  /* The shared point producer can clip a phased nearest interval or
   * fence a boundary with a constant texel-centre strip. Its geometry stays
   * integral and separable; hardware samples nearest at the prepared phase. */
  for(unsigned axis=0;axis<2u;axis++){
    const double *uv=axis?q->v:q->u;
    double begin=uv[0],end=uv[row?1u:axis?2u:1u],extent=row?width:axis?height:width;
    double storage=axis?source->height:source->width;
    double span=fabs(end-begin)*storage;
    if(span<1e-12){
      double texel=begin*storage-0.5;
      if(fabs(texel-nearbyint(texel))>1e-9) return 0;
    }else{
      double scale=extent/span;
      int background=op->scene_family==GML_RENDER_SCENE_BACKGROUND ||
                     op->scene_family==GML_RENDER_SCENE_TILE ||
                     op->scene_family==GML_RENDER_SCENE_TEXT;
      /* A first-generation trailing tie can retain an adjacent physical
       * texel. A clipped interior still has no nearest boundary when both
       * UV endpoints lie strictly inside the same physical cell. */
      int single_cell=op->scene_family==GML_RENDER_SCENE_SPRITE &&
        ((region.width==1u && region.height==1u) ||
         (region.width<=2u && region.height<=2u &&
          floor(begin*storage)==floor(end*storage)));
      int whole_background=op->scene_family==GML_RENDER_SCENE_BACKGROUND;
      int wide_sprite=op->scene_family==GML_RENDER_SCENE_SPRITE && !single_cell;
      double maximum=whole_background?16384.0:
        single_cell?4096.0:wide_sprite?16384.0:64.0;
      /* Clipped UV subtraction loses relative precision on highly magnified
       * intervals. Compare the upper bound in destination units instead. */
      if(!isfinite(scale) || scale<(background?0.0625:0x1p-14)-1e-9 ||
         (!row && (whole_background || wide_sprite
           ?extent>maximum*span+1e-9:scale>maximum+1e-9))) return 0;
    }
  }
  for(unsigned c=0;c<4u;c++){
    double u=(c==1u || c==2u)?q->u[1]:q->u[0];
    double v=row?((c==1u || c==2u)?q->v[1]:q->v[0]):c>=2u?q->v[2]:q->v[0];
    if(q->corner_colour[c]!=q->corner_colour[0] ||
       (q->corner_colour[c]>>24)!=(uint32_t)lround(q->filtered_draw_alpha*255.0) ||
       !isfinite(q->u[c]) || !isfinite(q->v[c]) ||
       q->u[c]<u0-1e-12 || q->u[c]>u1+1e-12 || q->v[c]<v0-1e-12 || q->v[c]>v1+1e-12 ||
       fabs(q->u[c]-u)>1e-12 || fabs(q->v[c]-v)>1e-12) return 0;
  }
  return 1;
}

/* R03-SPR-INTEGER-SOURCEOVER: the shared crop supplies integral physical
 * coverage and exact nearest UV endpoints. Admit white-RGB draw alpha only;
 * SCREEN needs an opaque-target certificate or the separately qualified
 * unit GUI covered-alpha stamp. Other tint, filtering and geometry stay fenced. */
static int integer_sourceover_sprite_valid(const GmlRenderDrawOp *op,
                                           const GmlRenderDrawStateSnapshot *state,
                                           const GlSceneImage *source){
  const GmlRenderQuad *quad=&op->payload.quad;
  GmlRenderRect region=quad->sampling.source_region;
  if(source && !source->rendered_origin &&
     source->pixel_format==GML_RENDER_PIXEL_RGBA8888 &&
     gml_gpu_opengl_half_gui_point_valid(op,state,source->width,source->height))
    return exact_rect(op,NULL,NULL,NULL,NULL,1);
  int gui_coverage=op->target.image.resource_class==GML_RENDER_RESOURCE_SCREEN &&
    state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
    (op->scene_family==GML_RENDER_SCENE_SPRITE || op->scene_family==GML_RENDER_SCENE_TEXT) &&
    qualified_blend(state)==1 && quad->filtered_draw_alpha==1.0 &&
    (quad->corner_colour[0]>>24)==255u &&
    (op->scene_family==GML_RENDER_SCENE_TEXT ||
     quad->corner_colour[0]==0xFFFFFFFFu);
  if(!source || (op->scene_family!=GML_RENDER_SCENE_SPRITE && !gui_coverage) ||
     !(((op->target.image.resource_class==GML_RENDER_RESOURCE_APPLICATION ||
         op->target.image.resource_class==GML_RENDER_RESOURCE_SURFACE) &&
        !state->coverage_policy) ||
       (op->target.image.resource_class==GML_RENDER_RESOURCE_SCREEN &&
        state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_TARGET &&
        qualified_blend(state)==1) || gui_coverage) ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 || source->rendered_origin ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     quad->sampling.filter!=GML_RENDER_FILTER_NEAREST ||
     quad->sampling.quantisation || quad->sampling.logical_margin_mask ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     !valid_region(region,source->width,source->height) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,0)) return 0;
  double xs=(quad->x[1]-quad->x[0])/region.width;
  double ys=(quad->y[2]-quad->y[0])/region.height;
  if(gui_coverage && (xs!=1.0 || ys!=1.0)) return 0;
  double u0=(double)region.x/source->width,u1=((double)region.x+region.width)/source->width;
  double v0=(double)region.y/source->height,v1=((double)region.y+region.height)/source->height;
  int flip_x=fabs(quad->u[0]-u1)<1e-12,flip_y=fabs(quad->v[0]-v1)<1e-12;
  for(unsigned corner=0;corner<4u;corner++){
    double u=((corner==1u || corner==2u)^flip_x)?u1:u0;
    double v=((corner>=2u)^flip_y)?v1:v0;
    if(((!gui_coverage || op->scene_family!=GML_RENDER_SCENE_TEXT) &&
        (quad->corner_colour[corner]&0xFFFFFFu)!=0xFFFFFFu) ||
       quad->corner_colour[corner]!=quad->corner_colour[0]) return 0;
    if(gui_coverage && (fabs(quad->u[corner]-u)>1e-12 ||
                        fabs(quad->v[corner]-v)>1e-12)) return 0;
  }
  if(!gui_coverage){
    /* Rounded point geometry can retain an independent texture phase, including
     * constant texel-centre strips. Fixed source-over does not depend on that
     * phase; keep the existing typed nearest/atlas/authority guards above.
     * The numbered phased/exact samplers are deliberately not admitted here. */
    if(quad->span_active || quad->u[0]!=quad->u[3] || quad->u[1]!=quad->u[2] ||
       quad->v[0]!=quad->v[1] || quad->v[2]!=quad->v[3]) return 0;
    for(unsigned corner=0;corner<4u;corner++)
      if(!isfinite(quad->u[corner]) || !isfinite(quad->v[corner]) ||
         quad->u[corner]<0.0 || quad->u[corner]>1.0 ||
         quad->v[corner]<0.0 || quad->v[corner]>1.0) return 0;
  }
  return 1;
}

/* R03-SPR-PART-INTEGER-ENDPOINT: only the separately paired positive whole
 * endpoint-colour crop extends the ordinary nearest tint fence. The renderer
 * certifies binary source alpha before producing this checkpoint packet.
 * R03-SPR-PART-ENDPOINT-SOURCE-TINT additionally carries its small immutable
 * page's visible endpoint channels; no channel product needs float emulation. */
static int integer_endpoint_sprite_valid(const GmlRenderDrawOp *op,
                                         const GmlRenderDrawStateSnapshot *state,
                                         const GlSceneImage *source){
  const GmlRenderQuad *quad=&op->payload.quad;
  GmlRenderRect region=quad->sampling.source_region;
  if(!source || op->scene_family!=GML_RENDER_SCENE_SPRITE ||
     op->target.image.resource_class!=GML_RENDER_RESOURCE_SURFACE ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 || state->coverage_policy ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 || source->rendered_origin ||
     source->width>16384u || source->height>16384u ||
     quad->source_endpoint_rgb_mask>7u ||
     (quad->source_endpoint_rgb_mask && (uint64_t)region.width*region.height>65536u) ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || quad->span_active ||
     quad->sampling.filter!=GML_RENDER_FILTER_NEAREST ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     quad->sampling.quantisation || quad->sampling.logical_margin_mask ||
     !valid_region(region,source->width,source->height) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1) ||
     fabs(quad->x[0])>65536.0 || fabs(quad->y[0])>65536.0) return 0;
  double width=quad->x[1]-quad->x[0],height=quad->y[2]-quad->y[0];
  double xs=width/region.width,ys=height/region.height;
  if(width>65536.0 || height>65536.0 || xs<1.0 || xs>4.0 || ys<1.0 || ys>4.0 ||
     xs!=floor(xs) || ys!=floor(ys)) return 0;
  for(unsigned corner=0;corner<4u;corner++){
    uint32_t colour=quad->corner_colour[corner];
    double u=((double)region.x+((corner==1u || corner==2u)?region.width:0u))/source->width;
    double v=((double)region.y+(corner>=2u?region.height:0u))/source->height;
    if(colour!=quad->corner_colour[0] || (colour>>24)!=255u ||
       ((colour&0xFFu)!=0u && (colour&0xFFu)!=0xFFu && !(quad->source_endpoint_rgb_mask&4u)) ||
       ((colour&0xFF00u)!=0u && (colour&0xFF00u)!=0xFF00u && !(quad->source_endpoint_rgb_mask&2u)) ||
       ((colour&0xFF0000u)!=0u && (colour&0xFF0000u)!=0xFF0000u && !(quad->source_endpoint_rgb_mask&1u)) ||
       quad->u[corner]!=u || quad->v[corner]!=v) return 0;
  }
  return 1;
}

static int hardware_combined_quad_valid(const GmlRenderDrawOp *op,
                                        const GlSceneImage *source){
  const GmlRenderQuad *quad=&op->payload.quad;
  GmlRenderRect region=quad->sampling.source_region;
  if(op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1) ||
     quad->x[1]-quad->x[0]!=(double)region.width ||
     quad->y[2]-quad->y[0]!=(double)region.height) return 0;
  for(unsigned corner=0;corner<4u;corner++){
    double u=((double)region.x+((corner==1u || corner==2u)?region.width:0u))/source->width;
    double v=((double)region.y+(corner>=2u?region.height:0u))/source->height;
    if(quad->corner_colour[corner]!=0xFFFFFFFFu ||
       fabs(quad->u[corner]-u)>1e-12 || fabs(quad->v[corner]-v)>1e-12) return 0;
  }
  return 1;
}

static int sample_plane_copy_valid(const GmlRenderDrawOp *op,
  const GmlRenderDrawStateSnapshot *state,const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  const GmlRenderSamplingPolicy *s=&q->sampling;
  if(s->filter!=GML_RENDER_FILTER_SAMPLE_PLANE_COPY || !source ||
     source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     !source->width || !source->height || source->width>4096u || source->height>4096u ||
     (op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
     op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     !op->target.width || !op->target.height || op->target.width>4096u || op->target.height>4096u ||
     op->opcode!=GML_RENDER_OP_TEXTURED_QUAD || op->scene_family!=GML_RENDER_SCENE_SURFACE ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active || s->quantisation ||
     s->logical_margin_mask || s->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     !full_region(s->source_region,source->width,source->height) ||
     qualified_blend(state)!=0 || state->coverage_policy || state->alpha_test_enabled ||
     state->write_mask!=15u || (q->span_x[0]!=0.0 && q->span_x[0]!=1.0)) return 0;
  for(unsigned axis=0;axis<2u;axis++){
    double origin=q->inverse_anchor[axis],step=axis?q->inverse_y[1]:q->inverse_x[0];
    if(!isfinite(origin) || origin!=floor(origin) || fabs(origin)>16384.0 ||
       (step!=1.0 && step!=2.0)) return 0;
    double first=fmax(0.0,origin);
    double last=fmin(axis?op->target.height:op->target.width,
                    origin+step*(axis?source->height:source->width));
    if(last<=first) return 0;
    for(unsigned corner=0;corner<4u;corner++){
      unsigned far=axis?corner>=2u:corner==1u || corner==2u;
      if((axis?q->y[corner]:q->x[corner])!=(far?last:first)) return 0;
    }
  }
  for(unsigned corner=0;corner<4u;corner++)
    if(q->u[corner]!=(double)(corner==1u || corner==2u) ||
       q->v[corner]!=(double)(corner>=2u) || q->corner_colour[corner]!=0xFFFFFFFFu ||
       (corner && q->span_x[corner]!=0.0) || q->span_y[corner]!=0.0) return 0;
  return exact_rect(op,NULL,NULL,NULL,NULL,0);
}

static int view_copy_quad_valid(const GmlRenderDrawOp *op,
  const GmlRenderDrawStateSnapshot *state,const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  const GmlRenderSamplingPolicy *sampling=&q->sampling;
  if(sampling->filter!=GML_RENDER_FILTER_VIEW_PIXEL_CENTRE &&
     sampling->filter!=GML_RENDER_FILTER_VIEW_SEED) return 0;
  if(!source || source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     source->width>4096u || source->height>4096u ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCREEN ||
     (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION) ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->target.width>4096u || op->target.height>4096u ||
     op->opcode!=GML_RENDER_OP_TEXTURED_QUAD || op->scene_family!=GML_RENDER_SCENE_SURFACE ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active ||
     (sampling->filter!=GML_RENDER_FILTER_VIEW_PIXEL_CENTRE &&
      sampling->filter!=GML_RENDER_FILTER_VIEW_SEED) ||
     sampling->quantisation || sampling->logical_margin_mask ||
     sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     !full_region(sampling->source_region,source->width,source->height) ||
     qualified_blend(state)!=0 || state->coverage_policy || state->alpha_test_enabled ||
     state->write_mask!=15u || q->span_x[0]!=op->target.width ||
     q->span_x[1]!=op->target.height || !exact_rect(op,NULL,NULL,NULL,NULL,0)) return 0;
  for(unsigned axis=0;axis<2u;axis++){
    double origin=q->inverse_anchor[axis],extent=axis?q->inverse_y[1]:q->inverse_x[0];
    if(!isfinite(origin) || origin!=floor(origin) || fabs(origin)>16384.0 ||
       !isfinite(extent) || extent!=floor(extent) || extent<=0.0 || extent>16384.0) return 0;
    double bound=sampling->filter==GML_RENDER_FILTER_VIEW_SEED
      ?(axis?source->height:source->width):(axis?op->target.height:op->target.width);
    double first=fmax(0.0,origin),last=fmin(bound,origin+extent);
    if(last<=first) return 0;
    if(sampling->filter==GML_RENDER_FILTER_VIEW_SEED){
      first=0.0;last=axis?op->target.height:op->target.width;
    }
    for(unsigned corner=0;corner<4u;corner++){
      unsigned far=axis?corner>=2u:corner==1u || corner==2u;
      if((axis?q->y[corner]:q->x[corner])!=(far?last:first)) return 0;
    }
  }
  for(unsigned corner=0;corner<4u;corner++){
    unsigned right=corner==1u || corner==2u,bottom=corner>=2u;
    if(q->u[corner]!=(double)right || q->v[corner]!=(double)bottom ||
       q->corner_colour[corner]!=0xFFFFFFFFu) return 0;
  }
  return 1;
}

static int hardware_surface_quad_valid(const GmlRenderDrawOp *op,
                                       const GmlRenderDrawStateSnapshot *state,
                                       const GlSceneImage *source){
  const GmlRenderQuad *quad=&op->payload.quad;
  int destination_blend=qualified_blend(state)==11;
  int point=quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT;
  int leading=quad->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SURFACE_LEADING ||
    quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_LEADING;
  int tiled=quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_FILTERED ||
    quad->sampling.quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_LEADING;
  int integer_screen=source && source->width && source->height &&
    op->sources[0].resource_class==GML_RENDER_RESOURCE_SURFACE &&
    op->target.image.resource_class==GML_RENDER_RESOURCE_SCREEN &&
    quad->sampling.filter==GML_RENDER_FILTER_NEAREST &&
    quad->filtered_draw_alpha==1.0 && quad->corner_colour[0]==0xFFFFFFFFu &&
    quad->x[0]>=0.0 && quad->y[0]>=0.0 &&
    quad->x[1]<=op->target.width && quad->y[2]<=op->target.height &&
    quad->x[1]-quad->x[0]>=source->width && quad->y[2]-quad->y[0]>=source->height &&
    quad->x[1]-quad->x[0]<=16384.0 && quad->y[2]-quad->y[0]<=16384.0 &&
    fmod(quad->x[1]-quad->x[0],(double)source->width)==0.0 &&
    fmod(quad->y[2]-quad->y[0],(double)source->height)==0.0;
  if(!source || op->scene_family!=GML_RENDER_SCENE_SURFACE ||
     (op->sources[0].resource_class!=GML_RENDER_RESOURCE_SURFACE &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
     source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SURFACE) ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     (quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR &&
      quad->sampling.filter!=GML_RENDER_FILTER_NEAREST &&
      !point &&
      !(destination_blend && quad->sampling.filter==GML_RENDER_FILTER_BOX)) ||
     (quad->sampling.quantisation && !leading && !tiled) ||
     (tiled && quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR) ||
     (tiled && (state->alpha_test_enabled>1u || state->alpha_test_reference>255u)) ||
     quad->sampling.logical_margin_mask || quad->span_active ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     !full_region(quad->sampling.source_region,source->width,source->height) ||
     (destination_blend?state->coverage_policy>GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
                        quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR:
                        state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
                        (qualified_blend(state)!=1 &&
                         (!tiled || qualified_blend(state)!=0 || quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR))) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1) ||
     (!point && !destination_blend && !integer_screen && (quad->sampling.filter==GML_RENDER_FILTER_NEAREST
       ?quad->x[1]-quad->x[0]!=(double)source->width ||
        quad->y[2]-quad->y[0]!=(double)source->height
       :(op->sources[0].resource_class!=GML_RENDER_RESOURCE_SURFACE &&
         op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
         op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCREEN &&
         op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
        quad->x[1]-quad->x[0]<=(double)source->width ||
        quad->y[2]-quad->y[0]<=(double)source->height)) ||
     (destination_blend && (source->width>4096u || source->height>4096u ||
       quad->x[1]-quad->x[0]>4096.0 || quad->y[2]-quad->y[0]>4096.0 ||
       (quad->sampling.filter==GML_RENDER_FILTER_BOX &&
        ((double)source->width>(quad->x[1]-quad->x[0])*8.0 ||
         (double)source->height>(quad->y[2]-quad->y[0])*8.0)))) ||
     !isfinite(quad->filtered_draw_alpha) || quad->filtered_draw_alpha<=0.0 ||
     quad->filtered_draw_alpha>1.0 ||
     (destination_blend && quad->filtered_draw_alpha<0x1p-32)) return 0;
  if(point){
    if(destination_blend || (op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
       op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCREEN &&
       op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
       op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
       !quad->source_opaque || quad->filtered_draw_alpha!=1.0 ||
       quad->corner_colour[0]!=0xFFFFFFFFu) return 0;
    for(unsigned axis=0;axis<2u;axis++){
      double origin=quad->inverse_anchor[axis];
      double extent=axis?quad->inverse_y[1]:quad->inverse_x[0];
      double first=axis?quad->y[0]:quad->x[0];
      double last=axis?quad->y[2]:quad->x[1];
      double target_extent=axis?op->target.height:op->target.width;
      if(!isfinite(origin) || fabs(origin)>16384.0 ||
         !isfinite(extent) || extent<=0.0 || extent>16384.0 ||
         first!=fmax(0.0,ceil(origin-.5)) ||
         last!=fmin(target_extent,ceil(origin+extent-.5))) return 0;
    }
  }
  if(leading && (point || destination_blend ||
     (op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
     quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR ||
     quad->filtered_draw_alpha!=1.0 || quad->corner_colour[0]!=0xFFFFFFFFu)) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if(quad->corner_colour[corner]!=quad->corner_colour[0] ||
       quad->u[corner]!=(point?(quad->x[corner]-quad->inverse_anchor[0])/quad->inverse_x[0]:
                               ((corner==1u || corner==2u)?1.0:0.0)+
                               (leading?.5/source->width-.5/(quad->x[1]-quad->x[0]):0.0)) ||
       quad->v[corner]!=(point?(quad->y[corner]-quad->inverse_anchor[1])/quad->inverse_y[1]:
                               (corner>=2u?1.0:0.0)+
                               (leading?.5/source->height-.5/(quad->y[2]-quad->y[0]):0.0))) return 0;
  if(destination_blend && (quad->corner_colour[0]>>24)!=
     (uint32_t)lround(quad->filtered_draw_alpha*255.0)) return 0;
  return 1;
}

/* A constant full-screen veil shares hardware source-over and the existing batched
 * alpha-only coverage pass. No predecessor sampling or CPU arithmetic is needed. */
static int hardware_screen_veil_valid(const GmlRenderDrawOp *op,
                                      const GmlRenderDrawStateSnapshot *state,
                                      const GlSceneImage *source){
  const GmlRenderQuad *quad=&op->payload.quad;
  /* SCRATCH uploads use the render-target origin; reversing a single constant row
   * changes neither this texel nor the full-target coverage. */
  if(!source || source->width!=1u || source->height!=1u ||
     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_SCRATCH ||
     op->scene_family!=GML_RENDER_SCENE_SPRITE ||
     (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SURFACE &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
     (qualified_blend(state)!=1 && qualified_blend(state)!=0) ||
     quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     quad->sampling.quantisation || quad->sampling.logical_margin_mask || quad->span_active ||
     !full_region(quad->sampling.source_region,1u,1u) ||
     quad->filtered_draw_alpha!=1.0 ||
     !exact_rect(op,NULL,NULL,NULL,NULL,0)) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if((quad->corner_colour[corner]>>24)!=255u ||
       quad->corner_colour[corner]!=quad->corner_colour[0] ||
       quad->x[corner]!=((corner==1u || corner==2u)?(double)op->target.width:0.0) ||
       quad->y[corner]!=(corner>=2u?(double)op->target.height:0.0) ||
       quad->u[corner]!=((corner==1u || corner==2u)?1.0:0.0) ||
       quad->v[corner]!=(corner>=2u?1.0:0.0)) return 0;
  return 1;
}

/* R03-SPR-GUI-MAPPED-STRETCH: crop-clamped atlas magnification shares the
 * hardware surface sampler and its batched alpha-coverage pass. */
static int hardware_screen_stretch_valid(const GmlRenderDrawOp *op,
                                        const GmlRenderDrawStateSnapshot *state,
                                        const GlSceneImage *source){
  const GmlRenderQuad *quad=&op->payload.quad;
  GmlRenderRect region=quad->sampling.source_region;
  if(!source || source->rendered_origin || source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     op->scene_family!=GML_RENDER_SCENE_SPRITE ||
     op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     qualified_blend(state)!=1 || state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
     quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     quad->sampling.quantisation || quad->sampling.logical_margin_mask || quad->span_active ||
     quad->filtered_draw_alpha!=1.0 || !valid_region(region,source->width,source->height) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1) ||
     quad->x[1]-quad->x[0]<=(double)region.width ||
     quad->y[2]-quad->y[0]<=(double)region.height) return 0;
  for(unsigned corner=0;corner<4u;corner++){
    double u=((double)region.x+((corner==1u || corner==2u)?region.width:0u))/source->width;
    double v=((double)region.y+(corner>=2u?region.height:0u))/source->height;
    if(quad->corner_colour[corner]!=0xFFFFFFFFu ||
       fabs(quad->u[corner]-u)>1e-12 || fabs(quad->v[corner]-v)>1e-12) return 0;
  }
  return 1;
}

/* Ordinary filtered opaque-stage packets retain authored fractional phase in UVs.
 * This bounded rule does not admit trim, source mutations or exact recipes. */
static int hardware_screen_sprite_valid(const GmlRenderDrawOp *op,
                                       const GmlRenderDrawStateSnapshot *state,
                                       const GlSceneImage *source){
  const GmlRenderQuad *quad=&op->payload.quad;
  GmlRenderRect region=quad->sampling.source_region;
  double xs=quad->inverse_x[0],ys=quad->inverse_y[1];
  double left=quad->inverse_anchor[0],top=quad->inverse_anchor[1];
  uint32_t margin=quad->sampling.logical_margin_mask;
  int selected_axis_row=quad->span_active==GML_RENDER_SPAN_FILTERED_AXIS_SUPPORT_ROW;
  int tile_axis_row=selected_axis_row && op->scene_family==GML_RENDER_SCENE_TILE;
  int atlas_background=op->scene_family==GML_RENDER_SCENE_BACKGROUND || op->scene_family==GML_RENDER_SCENE_TILE;
  int text_axis_row=selected_axis_row && op->scene_family==GML_RENDER_SCENE_TEXT;
  int wide_axis_row=text_axis_row || (selected_axis_row && op->scene_family==GML_RENDER_SCENE_SPRITE);
  int positive_row=selected_axis_row || quad->span_active==GML_RENDER_SPAN_FILTERED_POSITIVE_ROW;
  int affine_row=positive_row || quad->span_active==GML_RENDER_SPAN_FILTERED_AFFINE_ROW;
  int blend=qualified_blend(state);
  int byte_background=(atlas_background ||
    ((op->scene_family==GML_RENDER_SCENE_SPRITE || op->scene_family==GML_RENDER_SCENE_TEXT) && selected_axis_row)) &&
    (quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
     quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND);
  int background_blend=byte_background && (blend==0 || blend==1 || blend==3 || blend==9 ||
    (text_axis_row && blend==15) ||
    (blend>=18 && blend<=21) || blend==24 || blend==25 ||
    (blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT &&
     state->rgb_equation==GML_RENDER_BLEND_ADD && state->alpha_equation==GML_RENDER_BLEND_ADD));
  if(byte_background && (!positive_row || !background_blend || state->write_mask>15u ||
     state->alpha_test_enabled || state->alpha_test_reference ||
     (quad->filtered_draw_alpha==0.0 &&
      state->coverage_policy!=GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA &&
      state->coverage_policy!=GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA &&
      !(blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT)) ||
     (source && (source->width>16384u || source->height>16384u)))) return 0;
  int background_row=positive_row && atlas_background && state->write_mask==15u;
  if(selected_axis_row && ((op->scene_family!=GML_RENDER_SCENE_SPRITE && !text_axis_row && !tile_axis_row) ||
      state->write_mask>15u)) return 0;
  int firstgen_row=(background_row || selected_axis_row) &&
    quad->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN;
  int covered_blend=((blend==0 || blend==1) &&
      state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
    (blend==17 && !firstgen_row && state->coverage_policy==0u) ||
    ((background_row || selected_axis_row) && blend==3 && state->coverage_policy==0u) ||
    (firstgen_row && blend==15 && (state->coverage_policy==0u ||
      state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA)) ||
    (firstgen_row && !blend && state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA);
  if(positive_row && ((!firstgen_row && !byte_background &&
       quad->sampling.quantisation!=GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN) ||
     (!covered_blend && !background_blend) ||
     (state->alpha_test_enabled && state->alpha_test_reference) ||
     (!byte_background && op->scene_family!=GML_RENDER_SCENE_TILE &&
      quad->filtered_draw_alpha<0x1p-32))) return 0;
  int physical=(quad->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN ||
                quad->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN || byte_background) &&
    (op->scene_family==GML_RENDER_SCENE_SPRITE ||
     (atlas_background && (state->write_mask==15u || byte_background)) ||
     (op->scene_family==GML_RENDER_SCENE_TEXT && (state->write_mask==15u || byte_background))) && state->write_mask<=15u &&
    (op->target.image.resource_class==GML_RENDER_RESOURCE_SCREEN ||
     op->target.image.resource_class==GML_RENDER_RESOURCE_APPLICATION ||
     op->target.image.resource_class==GML_RENDER_RESOURCE_SURFACE) &&
    (state->coverage_policy==0u ||
     state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
     (!qualified_blend(state) && (state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA ||
       (byte_background && state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_SOURCE_ALPHA)))) &&
    (qualified_blend(state)==0 || qualified_blend(state)==1 || qualified_blend(state)==3 ||
     qualified_blend(state)==15 || qualified_blend(state)==16 || qualified_blend(state)==17 || background_blend);
  if(margin==GML_RENDER_MARGIN_UNIT_TEXEL){
    if(!physical || affine_row ||
       xs!=1.0 || ys!=1.0 || left!=floor(left) || top!=floor(top) ||
       quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
       (op->scene_family!=GML_RENDER_SCENE_TEXT &&
        (quad->corner_colour[0]!=0xffffffffu || quad->filtered_draw_alpha!=1.0 ||
         (state->alpha_test_enabled && state->alpha_test_reference)))) return 0;
    margin=0u;
  }
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN &&
     ((state->alpha_test_enabled && state->alpha_test_reference) ||
      (qualified_blend(state)!=0 && qualified_blend(state)!=1 &&
       qualified_blend(state)!=3 && qualified_blend(state)!=17) ||
      state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA)) return 0;
  if(!source || source->rendered_origin || source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     (op->scene_family!=GML_RENDER_SCENE_SPRITE && op->scene_family!=GML_RENDER_SCENE_TEXT &&
      !(physical && atlas_background)) ||
     (!physical && op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      (op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION ||
       op->scene_family!=GML_RENDER_SCENE_SPRITE)) ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     (!physical && (qualified_blend(state)!=1 ||
                    state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA)) ||
     quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR ||
     (margin
       ? (!physical && op->scene_family!=GML_RENDER_SCENE_SPRITE) || margin>15u ||
         quad->sampling.edge_rule!=GML_RENDER_EDGE_LOGICAL_TPAG ||
         (!physical && (((margin&GML_RENDER_MARGIN_LEFT) && region.x<=0) ||
          ((margin&GML_RENDER_MARGIN_TOP) && region.y<=0) ||
          ((margin&GML_RENDER_MARGIN_RIGHT) && (uint64_t)(uint32_t)region.x+region.width>=source->width) ||
          ((margin&GML_RENDER_MARGIN_BOTTOM) && (uint64_t)(uint32_t)region.y+region.height>=source->height)))
       : quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE) ||
     (quad->sampling.quantisation && !physical) || (quad->span_active && !affine_row) ||
     !isfinite(quad->filtered_draw_alpha) || quad->filtered_draw_alpha<0.0 ||
     (!byte_background && quad->filtered_draw_alpha==0.0) ||
     quad->filtered_draw_alpha>1.0 ||
     !isfinite(xs) || !isfinite(ys) ||
     (!affine_row && (physical?fabs(xs)<0.125 || fabs(xs)>64.0 || fabs(ys)<0.125 || fabs(ys)>64.0:
               xs<=0.0 || ys<=0.0)) ||
     (op->scene_family==GML_RENDER_SCENE_TEXT &&
      ((!physical && (xs>1.0 || ys>1.0)) || region.width>256u || region.height>256u)) ||
     !isfinite(left) || !isfinite(top) ||
     (!valid_region(region,source->width,source->height) &&
      !(physical && gml_gpu_opengl_clamped_atlas_valid(op,source->width,source->height))) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1)) return 0;
  if(affine_row){
    /* Background parts share the modern CPU positive-alpha coverage rows.
     * Other affine families and background write masks stay unqualified. */
    int background_positive=positive_row && atlas_background &&
      (state->write_mask==15u || byte_background);
    int selected_axis_positive=background_positive || selected_axis_row;
    double sample_limit=wide_axis_row || op->scene_family==GML_RENDER_SCENE_TILE
      ?16384.0:selected_axis_positive?16.0:8.0;
    if(!physical || (op->scene_family!=GML_RENDER_SCENE_SPRITE && !background_positive && !text_axis_row) ||
       quad->y[2]-quad->y[0]!=1.0 ||
       region.width>4096u || region.height>4096u ||
       quad->x[1]-quad->x[0]>16384.0 ||
       fabs(left)>(text_axis_row?16640.0:wide_axis_row?20480.0:4097.0) ||
       fabs(top)>(text_axis_row?16640.0:wide_axis_row?20480.0:4097.0) ||
       fabs(quad->inverse_x[0])>sample_limit ||
       fabs(quad->inverse_x[1])>sample_limit ||
       (quad->inverse_x[0]==0.0 && quad->inverse_x[1]==0.0) ||
       !isfinite(quad->inverse_x[1]) || quad->inverse_y[0]!=0.0 || quad->inverse_y[1]!=0.0 ||
       quad->inverse_offset[0]!=0.0 || quad->inverse_offset[1]!=0.0) return 0;
    for(unsigned axis=0;axis<2u;axis++){
      double start=quad->inverse_anchor[axis];
      double end=start+(quad->x[1]-quad->x[0]-1.0)*quad->inverse_x[axis];
      double extent=axis?region.height:region.width;
      /* Rounded axis coverage can sample beyond the cropped page. The
       * physical sampler already repairs those logical/clamped atlas taps;
       * retain the rotation boundary for other rows. */
      double lower=wide_axis_row?-sample_limit:selected_axis_positive?-16.0:-1.0;
      double upper=extent+(wide_axis_row?sample_limit:selected_axis_positive?16.0:0.0);
      if(start<lower || start>=upper || end<lower-1e-8 || end>=upper+1e-8) return 0;
      for(unsigned component=0;component<4u;component++)
        if(quad->span_x[component]!=0.0 || quad->span_y[component]!=0.0) return 0;
    }
  }
  for(unsigned corner=0;corner<4u;corner++){
    double offset=quad->x[corner]-quad->x[0]-0.5;
    double u=affine_row?((double)region.x+left+0.5+offset*quad->inverse_x[0])/source->width:
      ((double)region.x+(quad->x[corner]-left)/xs)/source->width;
    double v=affine_row?((double)region.y+top+0.5+offset*quad->inverse_x[1])/source->height:
      ((double)region.y+(quad->y[corner]-top)/ys)/source->height;
    if(quad->corner_colour[corner]!=quad->corner_colour[0] ||
       (quad->corner_colour[corner]>>24)!=(uint32_t)lround(quad->filtered_draw_alpha*255.0) ||
       fabs(quad->u[corner]-u)>1e-12 || fabs(quad->v[corner]-v)>1e-12) return 0;
  }
  return 1;
}

static int fixed_stretched_quad_valid(const GmlRenderDrawOp *op,
  const GmlRenderDrawStateSnapshot *state,const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  int blend=qualified_blend(state);
  if(!source || source->rendered_origin || source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     op->scene_family!=GML_RENDER_SCENE_SPRITE ||
     (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SURFACE) ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     state->write_mask!=15u || (blend!=0 && blend!=1) ||
     state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     (q->sampling.filter!=GML_RENDER_FILTER_BOX && q->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR) ||
     q->sampling.edge_rule!=GML_RENDER_EDGE_TRANSPARENT ||
     q->sampling.quantisation!=GML_RENDER_QUANTISE_FIXED_STRETCHED ||
     q->sampling.logical_margin_mask || q->span_active ||
     !isfinite(q->filtered_draw_alpha) || q->filtered_draw_alpha<=0 || q->filtered_draw_alpha>255 ||
     !valid_region(q->sampling.source_region,source->width,source->height) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1)) return 0;
  for(unsigned corner=1;corner<4u;corner++)
    if(q->corner_colour[corner]!=q->corner_colour[0]) return 0;
  /* The shared vertex validator owns integral logical/destination/trim spans
   * and the bounded64-by64 fixed-stretch footprint before any target write. */
  return 1;
}

static int recovery_quad_valid(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
                                const GmlRenderDrawStateSnapshot *state){
  GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
  const GmlRenderQuad *q=&op->payload.quad;
  if(!source || !source->rendered_origin || source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION ||
     op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     same_logical(source->ref,op->target.image) ||
     op->scene_family!=GML_RENDER_SCENE_SURFACE ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     backend->scene.max_texture_units<4 ||
     (backend->api==GML_GPU_API_OPENGL_CORE && !backend->gl.PolygonMode) ||
     !backend->gl.BlendFuncSeparate || !backend->gl.BlendEquationSeparate ||
     state->write_mask!=15u || state->alpha!=255u || state->colour!=0xFFFFFFFFu ||
     state->alpha_test_enabled>1u || state->alpha_test_reference>255u ||
     state->material_identity || state->material_version ||
     qualified_blend(state)!=1 || state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
     state->sampler_count!=1u ||
     !((!state->clip.width && !state->clip.height) ||
       full_region(state->clip,op->target.width,op->target.height)) ||
     !full_region(op->target.region,op->target.width,op->target.height) ||
     q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED || q->span_active ||
     q->sampling.filter!=GML_RENDER_FILTER_NEAREST || q->sampling.quantisation ||
     q->sampling.logical_margin_mask || q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     !full_region(q->sampling.source_region,source->width,source->height) ||
     state->samplers[0].filter!=q->sampling.filter ||
     state->samplers[0].edge_rule!=q->sampling.edge_rule ||
     state->samplers[0].quantisation || state->samplers[0].logical_margin_mask ||
     !same_region(state->samplers[0].source_region,q->sampling.source_region) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1) || q->filtered_draw_alpha!=1.0 ||
     q->x[0]<-16384.0 || q->y[0]<-16384.0 ||
     q->x[1]-q->x[0]<source->width || q->y[2]-q->y[0]<source->height ||
     q->x[1]-q->x[0]>16384.0 || q->y[2]-q->y[0]>16384.0) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if(q->corner_colour[corner]!=0xFFFFFFFFu ||
       q->u[corner]!=((corner==1u || corner==2u)?1.0:0.0) ||
       q->v[corner]!=(corner>=2u?1.0:0.0)) return 0;
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

static int source_palette_valid(const GmlRenderSourcePalette *palette){
  if(palette->enabled>1u) return 0;
  if(!palette->enabled) return 1;
  if(palette->red>256u) return 0;
  for(unsigned index=0;index<10u;index++)
    if(palette->colour[index]&0xFF000000u) return 0;
  for(unsigned branch=0;branch<2u;branch++) for(unsigned band=0;band<4u;band++)
    if(palette->green[branch][band]>256u ||
       (band && palette->green[branch][band]>palette->green[branch][band-1u])) return 0;
  return 1;
}

static int scene_preflight(GmlGpuBackend *backend,const GmlRenderDrawBatch *batch){
  GmlRenderImageRef cleared[GL_SCENE_MAX_IMAGES];
  uint32_t clear_count=0;
  float probe[6*GL_SCENE_VERTEX_FLOATS];
  if(!backend || !batch || !batch->operations || batch->operation_count==0 ||
     batch->operation_count>65536u || !backend->loaded || !backend->scene.available) return 0;
  if(batch->state_count && !batch->states) return 0;
  int large_rows=0,extended_quads=0;
  for(uint32_t index=0;index<batch->operation_count;index++){
    const GmlRenderDrawOp *op=&batch->operations[index];
    const GmlRenderTargetRef *target=&op->target;
    if(op->state_index<batch->state_count){
      const GmlRenderSourcePalette *palette=&batch->states[op->state_index].source_palette;
      if(!source_palette_valid(palette) ||
         (palette->enabled && (op->opcode!=GML_RENDER_OP_TEXTURED_QUAD ||
          op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
          !backend->gl.Uniform3iv || !backend->gl.Uniform4iv))) return 0;
    }
    int vertex_span=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_RECTANGLE &&
      (op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_VERTEX_SPAN);
    if(batch->operation_count>8192u && !vertex_span &&
       (op->opcode!=GML_RENDER_OP_COLOURED_PRIMITIVE ||
        op->payload.primitive.kind!=GML_RENDER_PRIMITIVE_TEXTURED_VERTEX_ROW)){
      if(op->opcode!=GML_RENDER_OP_TEXTURED_QUAD && op->opcode!=GML_RENDER_OP_CLEAR) return 0;
      extended_quads=1;
      if(op->opcode==GML_RENDER_OP_TEXTURED_QUAD && op->scene_family==GML_RENDER_SCENE_SPRITE &&
         (op->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_ROW_TINT_TRUNC ||
          op->payload.quad.sampling.quantisation==GML_RENDER_QUANTISE_POSITIONED_SAMPLE)) large_rows=1;
    }
    if(batch->states && op->state_index<batch->state_count &&
       batch->states[op->state_index].source_alpha_key &&
       op->opcode!=GML_RENDER_OP_TEXTURED_QUAD) return 0;
    if(!target->image.incarnation || !target->image.content_version ||
       !target->storage_version ||
       target->width>(uint32_t)backend->scene.max_texture_size ||
       target->height>(uint32_t)backend->scene.max_texture_size ||
       !valid_region(target->region,target->width,target->height) ||
       (target->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
        target->pixel_format!=GML_RENDER_PIXEL_ARGB8888)) return 0;
    if(op->opcode==GML_RENDER_OP_COPY_IMAGE){
      if(!backend->gl.BlitFramebuffer || !backend->gl.CopyTexSubImage2D ||
         !batch->states || op->state_index>=batch->state_count ||
         op->source_count!=1u || !op->sources ||
         op->scene_family!=GML_RENDER_SCENE_SURFACE ||
         op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
         target->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
         !existing_target(backend,target)) return 0;
      GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
      const GmlRenderCopyRegion *copy=&op->payload.copy;
      if(!source || !source->rendered_origin ||
         source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
         (source->ref.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
          source->ref.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
          source->ref.resource_class!=GML_RENDER_RESOURCE_SURFACE &&
          source->ref.resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
         (target->image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
          target->image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
          target->image.resource_class!=GML_RENDER_RESOURCE_SURFACE &&
          target->image.resource_class!=GML_RENDER_RESOURCE_SCRATCH) ||
         copy->source.width!=copy->destination.width ||
         copy->source.height!=copy->destination.height ||
         copy->needs_source_snapshot!=(uint32_t)same_logical(source->ref,target->image)) return 0;
      GmlRenderRect regions[2]={copy->source,copy->destination};
      uint32_t pitches[2]={copy->source_pitch?copy->source_pitch:source->width,
                          copy->destination_pitch?copy->destination_pitch:target->width};
      uint64_t capacities[2]={(uint64_t)source->width*source->height,
                            (uint64_t)target->width*target->height};
      for(unsigned side=0;side<2u;side++){
        GmlRenderRect region=regions[side];
        uint64_t end=(uint64_t)(uint32_t)region.x+region.width;
        if(!pitches[side] || region.x<0 || region.y<0 || !region.width || !region.height ||
           end>pitches[side] || end>capacities[side] ||
           (uint64_t)(uint32_t)region.y+region.height-1u>
             (capacities[side]-end)/pitches[side]) return 0;
      }
      continue;
    }
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
      if(op->payload.primitive.kind==GML_RENDER_PRIMITIVE_PARTICLE_LINE ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_PARTICLE_DISC ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_RECTANGLE ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_TEXTURED_VERTEX_ROW ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT){
        if((!gml_render_particle_line_valid(op) && !gml_render_particle_disc_valid(op) &&
            !gml_render_classic_alpha_rect_valid(op) &&
            !gml_render_studio_alpha_rect_valid(op) &&
            !gml_render_rectangle_valid(op) && !gml_render_textured_vertex_row_valid(op) &&
            !gml_render_classic_inverse_rect_valid(op)) ||
           (op->payload.primitive.alpha_blend &&
            (!backend->gl.BlendFuncSeparate || !backend->gl.BlendEquationSeparate)) ||
           (!existing_target(backend,target) &&
            !cleared_earlier(cleared,clear_count,target->image))) return 0;
        if(op->payload.primitive.kind==GML_RENDER_PRIMITIVE_TEXTURED_VERTEX_ROW){
          GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
          if(!source || !source->texture || same_logical(source->ref,target->image) ||
             source->width>16384u || source->height>16384u ||
             (source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 &&
              source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
              source->pixel_format!=GML_RENDER_PIXEL_XRGB8888)) return 0;
        }
        continue;
      }
      if(op->reconstruction_mode!=GML_RENDER_REPLAY_EXACT ||
         op->payload.primitive.kind!=GML_RENDER_PRIMITIVE_FLAT ||
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
         !gml_render_plan_shader_has_program(shader) ||
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
         (state->coverage_policy &&
          (state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
           (shader->builtin_kind!=GML_PLAN_BUILTIN_SHADER_NONE &&
            shader->builtin_kind!=GML_PLAN_BUILTIN_SHADER_THRESHOLD_PALETTE) ||
           qualified_blend(state)!=1 ||
           (shader->builtin_kind==GML_PLAN_BUILTIN_SHADER_NONE &&
            (target->width>4096u || target->height>4096u)))) ||
         state->sampler_count!=1u ||
         (state->samplers[0].filter!=GML_RENDER_FILTER_NEAREST &&
          state->samplers[0].filter!=GML_RENDER_FILTER_BILINEAR) ||
         state->samplers[0].edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
         state->samplers[0].quantisation ||
         !valid_region(state->samplers[0].source_region,source->width,source->height) ||
         (!full_region(state->samplers[0].source_region,source->width,source->height) &&
          state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) ||
         (shader->builtin_kind!=GML_PLAN_BUILTIN_SHADER_NONE &&
          !full_region(state->samplers[0].source_region,source->width,source->height)) ||
         !((state->clip.width==0 && state->clip.height==0) ||
           full_region(state->clip,target->width,target->height))) return 0;
      for(uint32_t bound=0;bound<shader->sampler_count;bound++){
        GlSceneImage *sampler=gml_gpu_opengl_scene_image(backend,op->sources[bound+1u]);
        if(!sampler || !sampler->texture ||
           (sampler->rendered_origin && !backend->gl.BlitFramebuffer) ||
           same_logical(sampler->ref,target->image) ||
           (sampler->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
            sampler->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
            !(shader->builtin_kind==GML_PLAN_BUILTIN_SHADER_LUT &&
              sampler->pixel_format==GML_RENDER_PIXEL_RGBA8888))) return 0;
        if(shader->builtin_kind==GML_PLAN_BUILTIN_SHADER_LUT &&
           (shader->uniforms[0].value[0]+shader->uniforms[1].value[2]>(float)sampler->width ||
            shader->uniforms[0].value[1]+shader->uniforms[1].value[3]>(float)sampler->height)) return 0;
      }
      continue;
    }
    if((op->opcode!=GML_RENDER_OP_TEXTURED_QUAD &&
        op->opcode!=GML_RENDER_OP_RECOVER_EMPTY_SCREEN) || !batch->states ||
       op->state_index>=batch->state_count || op->source_count!=1 || !op->sources ||
       (!existing_target(backend,target) &&
        !cleared_earlier(cleared,clear_count,target->image))) return 0;
    if(op->opcode==GML_RENDER_OP_RECOVER_EMPTY_SCREEN){
      if(!recovery_quad_valid(backend,op,&batch->states[op->state_index])) return 0;
      continue;
    }
    {
      const GmlRenderDrawStateSnapshot *state=&batch->states[op->state_index];
      GlSceneImage *source=gml_gpu_opengl_scene_image(backend,op->sources[0]);
      const GmlRenderSamplingPolicy *sampling=&op->payload.quad.sampling;
      int blend=qualified_blend(state);
      if(sampling->quantisation==GML_RENDER_QUANTISE_POSITIONED_SAMPLE){
        if(!backend->gl.BlendFuncSeparate || !backend->gl.BlendEquationSeparate ||
           !positioned_sample_valid(op,state,source) ||
           !gml_render_gpu_opengl_draw_textured_quad(op,state,source->width,source->height,0,probe)) return 0;
        continue;
      }
      int hardware_surface=hardware_surface_quad_valid(op,state,source);
      int view_copy=view_copy_quad_valid(op,state,source);
      int sample_plane=sample_plane_copy_valid(op,state,source);
      int hardware_veil=hardware_screen_veil_valid(op,state,source);
      int hardware_stretch=hardware_screen_stretch_valid(op,state,source);
      int hardware_sprite=hardware_screen_sprite_valid(op,state,source);
      int stretched_box=fixed_stretched_quad_valid(op,state,source);
      int physical_sprite=hardware_sprite &&
        (sampling->quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_FIRSTGEN ||
         sampling->quantisation==GML_RENDER_QUANTISE_HARDWARE_SPRITE_MODERN ||
         ((op->scene_family==GML_RENDER_SCENE_BACKGROUND || op->scene_family==GML_RENDER_SCENE_TILE ||
           ((op->scene_family==GML_RENDER_SCENE_SPRITE || op->scene_family==GML_RENDER_SCENE_TEXT) &&
            op->payload.quad.span_active==GML_RENDER_SPAN_FILTERED_AXIS_SUPPORT_ROW)) &&
          (sampling->quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
           sampling->quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND)));
      int integer_sourceover=integer_sourceover_sprite_valid(op,state,source) &&
        (blend==1 || blend==3);
      int integer_function=integer_function_sprite_valid(op,state,source) ||
        nearest_surface_function_valid(op,state,source) || box_surface_function_valid(op,state,source) ||
        rotated_surface_point_valid(op,state,source);
      if(sampling->quantisation==GML_RENDER_QUANTISE_SURFACE_ROTATED_POINT &&
         !rotated_surface_point_valid(op,state,source)) return 0;
      if(state->source_alpha_key &&
         ((state->source_alpha_key>>9)<1u || (state->source_alpha_key>>9)>3u ||
          (state->source_alpha_key&511u)>256u || !integer_function ||
          (op->scene_family!=GML_RENDER_SCENE_BACKGROUND && op->scene_family!=GML_RENDER_SCENE_TILE) ||
          (sampling->quantisation!=GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC &&
           sampling->quantisation!=GML_RENDER_QUANTISE_INTEGER_TINT_ROUND))) return 0;
      int integer_endpoint=op->payload.quad.corner_colour[0]!=0xFFFFFFFFu &&
        integer_endpoint_sprite_valid(op,state,source) &&
        (blend==0 || blend==1);
      int modern_numbered_sprite=
        (sampling->quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST ||
         sampling->quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_ROTATED_NEAREST) &&
        state->write_mask<=15u &&
        op->scene_family==GML_RENDER_SCENE_SPRITE &&
        target->image.resource_class==GML_RENDER_RESOURCE_SURFACE &&
        target->pixel_format==GML_RENDER_PIXEL_ARGB8888 &&
        op->sources[0].resource_class==GML_RENDER_RESOURCE_ATLAS &&
        source && !source->rendered_origin && source->pixel_format==GML_RENDER_PIXEL_RGBA8888 &&
        op->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT && blend==3 &&
        op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED && !state->coverage_policy &&
        sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED &&
        sampling->edge_rule==GML_RENDER_EDGE_CLAMP_SUBIMAGE && !sampling->logical_margin_mask &&
        isfinite(op->payload.quad.filtered_draw_alpha) &&
        op->payload.quad.filtered_draw_alpha>0.0 && op->payload.quad.filtered_draw_alpha<=1.0 &&
        backend->gl.CopyTexSubImage2D && exact_rect(op,NULL,NULL,NULL,NULL,1);
      int modern_rotated_sprite=sampling->quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_ROTATED_NEAREST;
      if(sampling->quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST){
        const GmlRenderQuad *quad=&op->payload.quad;
        if(!modern_numbered_sprite || quad->span_active ||
           sampling->source_region.width>4096u || sampling->source_region.height>4096u ||
           quad->x[1]-quad->x[0]>4096.0 || quad->y[2]-quad->y[0]>4096.0 ||
           source->width>16384u || source->height>16384u ||
           backend->scene.max_texture_units<4) return 0;
        for(unsigned axis=0;axis<2u;axis++){
          double anchor=quad->inverse_anchor[axis];
          double scale=axis?quad->inverse_y[1]:quad->inverse_x[0];
          double phase=axis?quad->inverse_y[0]:quad->inverse_x[1];
          double start=axis?quad->y[0]:quad->x[0];
          double end=axis?quad->y[2]:quad->x[1];
          double source_extent=axis?sampling->source_region.height:sampling->source_region.width;
          double extent=floor(source_extent*fabs(scale)+0.5);
          if(!isfinite(anchor) || anchor!=floor(anchor) || fabs(anchor)>10000000.0 ||
             !isfinite(scale) || scale==0.0 || fabs(scale)>1000000.0 ||
             !isfinite(phase) || phase<0.0 || phase>1.0 ||
             extent>4096.0 ||
             (scale<0.0?phase!=0.5 || start<anchor-extent || end>anchor:
                         start<anchor || end>anchor+extent)) return 0;
        }
      }
      if(modern_rotated_sprite){
        const GmlRenderQuad *quad=&op->payload.quad;
        int row=quad->span_active==GML_RENDER_SPAN_FIXED20_ROW;
        if(!modern_numbered_sprite || state->write_mask!=15u || (quad->span_active && !row) ||
           source->width>16384u || source->height>16384u ||
           quad->y[2]-quad->y[0]!=1.0) return 0;
        if(row){
          if(target->width>16384u || target->height>16384u) return 0;
          for(unsigned corner=0;corner<4u;corner++)
            if(quad->u[corner]!=0.0 || quad->v[corner]!=0.0) return 0;
          int64_t last=(int64_t)(quad->x[1]-quad->x[0])-1;
          for(unsigned axis=0;axis<2u;axis++){
            const double *span=axis?quad->span_y:quad->span_x;
            for(unsigned component=0;component<4u;component++)
              if(!isfinite(span[component]) || span[component]!=floor(span[component]) ||
                 (component%2u
                   ?span[component]<0.0 || span[component]>65535.0
                   :span[component]<-32768.0 || span[component]>32767.0)) return 0;
            int64_t base=(int64_t)span[0]*65536+(int64_t)span[1];
            int64_t delta=(int64_t)span[2]*65536+(int64_t)span[3];
            if(last<0 || llabs(delta)*last>INT32_MAX ||
               base+delta*last<INT32_MIN || base+delta*last>INT32_MAX) return 0;
          }
        } else {
          double sample_x=nearbyint(quad->u[0]*source->width-0.5);
          double sample_y=nearbyint(quad->v[0]*source->height-0.5);
          if(!isfinite(sample_x) || !isfinite(sample_y) ||
             sample_x<sampling->source_region.x || sample_y<sampling->source_region.y ||
             sample_x>=(double)sampling->source_region.x+sampling->source_region.width ||
             sample_y>=(double)sampling->source_region.y+sampling->source_region.height) return 0;
          for(unsigned corner=0;corner<4u;corner++)
            if(quad->u[corner]!=(sample_x+0.5)/source->width ||
               quad->v[corner]!=(sample_y+0.5)/source->height) return 0;
        }
      }
      /* Independent channel writes do not change a qualified point sprite's sampling or
       * source-over RGB. Keep the new domain numbered, checkpointed and axis-aligned;
       * screen/other sampler and blend variants retain their existing refusal. */
      int masked_numbered_sprite=state->write_mask<15u &&
        op->scene_family==GML_RENDER_SCENE_SPRITE &&
        target->image.resource_class==GML_RENDER_RESOURCE_SURFACE &&
        target->pixel_format==GML_RENDER_PIXEL_ARGB8888 &&
        op->sources[0].resource_class==GML_RENDER_RESOURCE_ATLAS &&
        source && !source->rendered_origin && source->pixel_format==GML_RENDER_PIXEL_RGBA8888 &&
        op->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT &&
        (blend==1 || (blend==3 && sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED)) &&
        op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED && !state->coverage_policy &&
        !sampling->quantisation && !sampling->logical_margin_mask &&
        (sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED ||
         sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA);
      /* Bounded nonpreserving axis sprites carry independent row coverage.
       * A mask/raw cutout changes no source map and needs no predecessor. */
      int screen_axis_sprite=op->scene_family==GML_RENDER_SCENE_SPRITE &&
        (target->image.resource_class==GML_RENDER_RESOURCE_SCREEN ||
         target->image.resource_class==GML_RENDER_RESOURCE_APPLICATION) &&
        target->pixel_format==GML_RENDER_PIXEL_ARGB8888 &&
        op->sources[0].resource_class==GML_RENDER_RESOURCE_ATLAS &&
        source && !source->rendered_origin && source->pixel_format==GML_RENDER_PIXEL_RGBA8888 &&
        op->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT && blend==1 &&
        op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED && !state->coverage_policy &&
        !sampling->quantisation && !sampling->logical_margin_mask &&
        (sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA ||
         sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_PACKET_ALPHA) &&
        (op->payload.quad.span_active==0u ||
         (sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_PACKET_ALPHA &&
          op->payload.quad.span_active==GML_RENDER_SPAN_OPAQUE_SCALAR_ALPHA)) &&
        state->write_mask<=15u && state->alpha_test_enabled<=1u &&
        state->alpha_test_reference<=255u;
      int hardware_gui_background=op->scene_family==GML_RENDER_SCENE_BACKGROUND &&
        target->image.resource_class==GML_RENDER_RESOURCE_SCREEN &&
        target->pixel_format==GML_RENDER_PIXEL_ARGB8888 &&
        op->sources[0].resource_class==GML_RENDER_RESOURCE_ATLAS &&
        source && !source->rendered_origin && source->pixel_format==GML_RENDER_PIXEL_RGBA8888 &&
        op->reconstruction_mode==GML_RENDER_DEVICE_CHECKPOINT &&
        op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED &&
        sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
        !sampling->quantisation && sampling->logical_margin_mask<=15u &&
        sampling->source_region.width<=4096u && sampling->source_region.height<=4096u &&
        target->width<=16384u && target->height<=16384u &&
        state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA &&
        (blend==3 || (!blend && op->payload.quad.filtered_draw_alpha==1.0 &&
                     (op->payload.quad.corner_colour[0]>>24)==255u)) &&
        isfinite(op->payload.quad.filtered_draw_alpha) &&
        op->payload.quad.filtered_draw_alpha>0.0 && op->payload.quad.filtered_draw_alpha<=1.0 &&
        op->payload.quad.u[0]==op->payload.quad.u[3] &&
        op->payload.quad.u[1]==op->payload.quad.u[2] &&
        op->payload.quad.v[0]==op->payload.quad.v[1] &&
        op->payload.quad.v[2]==op->payload.quad.v[3] &&
        op->payload.quad.corner_colour[0]==op->payload.quad.corner_colour[1] &&
        op->payload.quad.corner_colour[0]==op->payload.quad.corner_colour[2] &&
        op->payload.quad.corner_colour[0]==op->payload.quad.corner_colour[3];
      if(!source || (source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
                     source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
                     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888) ||
         same_logical(op->sources[0],target->image) ||
         (state->write_mask!=15u && !masked_numbered_sprite && !modern_numbered_sprite && !integer_function && !physical_sprite && !screen_axis_sprite) ||
         (sampling->quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST &&
          !modern_numbered_sprite) || state->alpha!=255u ||
         ((sampling->quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
           sampling->quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND ||
           sampling->quantisation==GML_RENDER_QUANTISE_INTEGER_ROW_TINT_TRUNC ||
           sampling->quantisation==GML_RENDER_QUANTISE_INTEGER_FLAT_RGB) && !integer_function && !physical_sprite) ||
         state->colour!=0xFFFFFFFFu || (state->alpha_test_enabled && !integer_function && !physical_sprite && !stretched_box && !screen_axis_sprite &&
           !(hardware_surface && (sampling->quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_FILTERED ||
                                 sampling->quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_LEADING))) ||
         blend<0 || ((blend==13 || blend==14 || (blend>=18 && blend<=21)) && !integer_function && !physical_sprite) ||
         ((blend==15 || blend==16 || blend==17) && !physical_sprite) ||
         (blend==11 && (!hardware_surface || !backend->gl.CopyTexSubImage2D)) ||
         ((integer_function || integer_sourceover || physical_sprite || blend==1 || blend==9 || blend==10 ||
                       (blend==3 && sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR)) &&
                      (!backend->gl.BlendFuncSeparate ||
                                 !backend->gl.BlendEquationSeparate)) ||
         (blend==9 && !physical_sprite && (sampling->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR ||
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
         (!hardware_sprite && !hardware_gui_background && (blend==10 || (blend==3 && sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR)) &&
                       (sampling->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR ||
                        sampling->quantisation!=(blend==3?GML_RENDER_QUANTISE_HARDWARE_COMBINED:0u) ||
                        (blend==3 && !hardware_combined_quad_valid(op,source)) ||
                        sampling->logical_margin_mask>15u ||
                        (sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE &&
                         sampling->edge_rule!=GML_RENDER_EDGE_LOGICAL_TPAG) ||
                        op->payload.quad.shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
                        op->scene_family!=GML_RENDER_SCENE_BACKGROUND ||
                        op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
                        source->rendered_origin || source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
                        op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
                        op->payload.quad.filtered_draw_alpha!=1.0)) ||
         (sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR && !stretched_box && !hardware_surface && !hardware_veil && !hardware_stretch && !hardware_sprite && !hardware_gui_background &&
          blend!=9 && blend!=10 &&
          !(blend==3 && sampling->quantisation==GML_RENDER_QUANTISE_HARDWARE_COMBINED)) ||
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
         (state->coverage_policy && !stretched_box && !integer_function && !hardware_surface && !hardware_veil && !hardware_stretch && !hardware_sprite && !hardware_gui_background && !integer_sourceover &&
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
         (!valid_region(sampling->source_region,source->width,source->height) &&
          !(physical_sprite && !source->rendered_origin &&
            gml_gpu_opengl_clamped_atlas_valid(op,source->width,source->height))) ||
         (sampling->filter!=GML_RENDER_FILTER_NEAREST &&
          sampling->filter!=GML_RENDER_FILTER_BILINEAR &&
          sampling->filter!=GML_RENDER_FILTER_BOX &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_COPY &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA &&
          sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_PACKET_ALPHA &&
          sampling->filter!=GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT &&
          sampling->filter!=GML_RENDER_FILTER_VIEW_PIXEL_CENTRE &&
          sampling->filter!=GML_RENDER_FILTER_VIEW_SEED &&
          sampling->filter!=GML_RENDER_FILTER_SAMPLE_PLANE_COPY &&
          sampling->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR) ||
         (sampling->filter==GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT && !hardware_surface) ||
         (sampling->filter==GML_RENDER_FILTER_SAMPLE_PLANE_COPY && !sample_plane) ||
         ((sampling->filter==GML_RENDER_FILTER_VIEW_PIXEL_CENTRE ||
           sampling->filter==GML_RENDER_FILTER_VIEW_SEED) && !view_copy) ||
         ((sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED ||
           sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_COPY ||
           sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA) &&
          (op->payload.quad.shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
           op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
           sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE)) ||
         (sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA &&
          (op->scene_family!=GML_RENDER_SCENE_SPRITE || blend!=1 ||
           target->pixel_format!=GML_RENDER_PIXEL_ARGB8888)) ||
         (sampling->filter==GML_RENDER_FILTER_NEAREST_PHASED_PACKET_ALPHA &&
          (!screen_axis_sprite || sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE)) ||
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
         (sampling->quantisation && !hardware_surface && !stretched_box && !integer_function && !physical_sprite && blend!=6 && blend!=7 && !modern_numbered_sprite &&
          !(blend==3 && sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
            sampling->quantisation==GML_RENDER_QUANTISE_HARDWARE_COMBINED) &&
          (sampling->quantisation!=GML_RENDER_QUANTISE_STRETCHED_FLOAT ||
           sampling->filter!=GML_RENDER_FILTER_BILINEAR ||
           op->scene_family!=GML_RENDER_SCENE_SPRITE || (blend!=0 && blend!=8) ||
           op->payload.quad.shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
           (sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE &&
            !(blend==8 && sampling->edge_rule==GML_RENDER_EDGE_TRANSPARENT)) ||
           source->rendered_origin)) ||
         (!stretched_box && !(integer_function && sampling->quantisation==GML_RENDER_QUANTISE_SURFACE_BOX_FUNCTION) &&
          blend!=7 && blend!=11 && (sampling->filter==GML_RENDER_FILTER_BOX
           ? sampling->edge_rule!=GML_RENDER_EDGE_TRANSPARENT ||
             sampling->logical_margin_mask!=0u
           : (sampling->edge_rule==GML_RENDER_EDGE_LOGICAL_TPAG ||
              sampling->edge_rule==GML_RENDER_EDGE_TRANSPARENT
                ? (sampling->filter!=GML_RENDER_FILTER_BILINEAR &&
                   !((blend==10 || blend==3 || hardware_sprite) &&
                     sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR)) ||
                  !sampling->logical_margin_mask ||
                  (sampling->logical_margin_mask>15u &&
                   sampling->logical_margin_mask!=GML_RENDER_MARGIN_VISIBLE_CENTRE) ||
                  (sampling->logical_margin_mask==GML_RENDER_MARGIN_VISIBLE_CENTRE &&
                   (sampling->edge_rule!=GML_RENDER_EDGE_LOGICAL_TPAG ||
                    (op->payload.quad.shape!=GML_RENDER_QUAD_CARDINAL &&
                     op->payload.quad.shape!=GML_RENDER_QUAD_GENERAL)))
                : sampling->logical_margin_mask!=0u &&
                  !(hardware_sprite &&
                    sampling->logical_margin_mask==GML_RENDER_MARGIN_UNIT_TEXEL)))) ||
         state->samplers[0].filter!=sampling->filter ||
         state->samplers[0].edge_rule!=sampling->edge_rule ||
         state->samplers[0].quantisation!=sampling->quantisation ||
         state->samplers[0].logical_margin_mask!=sampling->logical_margin_mask ||
         !same_region(state->samplers[0].source_region,sampling->source_region) ||
         !gml_render_gpu_opengl_draw_textured_quad(op,state,source->width,source->height,
                                                   source->rendered_origin,probe)) return 0;
      for(uint32_t corner=0;corner<4;corner++){
        uint32_t colour=op->payload.quad.corner_colour[corner];
        if(modern_numbered_sprite && (colour!=op->payload.quad.corner_colour[0] ||
           (colour>>24)!=(uint32_t)lround(op->payload.quad.filtered_draw_alpha*255.0))) return 0;
        if(blend==8 && colour!=op->payload.quad.corner_colour[0]) return 0;
        if(state->coverage_policy && !integer_sourceover &&
           (colour>>24)!=(uint32_t)lround((stretched_box
             ? fmin(op->payload.quad.filtered_draw_alpha,1.0)
             : op->payload.quad.filtered_draw_alpha)*255.0))
          return 0;
        if(colour!=0xFFFFFFFFu && blend!=6 && !integer_function &&
           !nearest_surface_fringe_valid(op,state,source) &&
           (op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
            (sampling->filter!=GML_RENDER_FILTER_BILINEAR &&
             sampling->filter!=GML_RENDER_FILTER_BOX &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_COPY &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_PACKET_ALPHA &&
             sampling->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR &&
             op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED &&
             !hardware_surface && !integer_sourceover && !integer_endpoint &&
             !(op->scene_family==GML_RENDER_SCENE_TEXT &&
               sampling->filter==GML_RENDER_FILTER_NEAREST)))) return 0;
        if((colour>>24)!=255u && !integer_function && !physical_sprite && !stretched_box &&
           !(hardware_surface && (sampling->quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_FILTERED ||
                                 sampling->quantisation==GML_RENDER_QUANTISE_SURFACE_TILED_LEADING)) &&
           blend!=1 && blend!=3 && blend!=9 && blend!=10 && blend!=11) return 0;
        if((blend==4 || blend==5 ||
            (sampling->quantisation && !stretched_box && !hardware_surface && blend!=6 && blend!=8 && !modern_numbered_sprite && !integer_function && !physical_sprite)) &&
           colour!=0xFFFFFFFFu) return 0;
      }
      if(op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED &&
         sampling->filter==GML_RENDER_FILTER_NEAREST &&
         !gml_gpu_opengl_half_gui_point_valid(op,state,source->width,source->height) &&
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
  return !extended_quads || large_rows;
}

static int scene_prepare_geometry(GmlGpuBackend *backend,char *error,size_t error_capacity){
  GlScene *scene=&backend->scene;
  if(!gml_gpu_opengl_scene_program(backend,error,error_capacity)) return 0;
  if(!scene->vertices){
    scene->vertices=(float*)malloc((size_t)GL_SCENE_MAX_QUADS*6u*GL_SCENE_VERTEX_FLOATS*
                                   sizeof(float));
    if(!scene->vertices) return 0;
  }
  if(!scene->instances){
    scene->instances=(float*)malloc((size_t)GL_SCENE_MAX_QUADS*GL_SCENE_INSTANCE_FLOATS*
                                    sizeof(float));
    if(!scene->instances) return 0;
  }
  if(!scene->vertex_array){
    backend->gl.GenVertexArrays(1,&scene->vertex_array);
    if(!scene->vertex_array) return 0;
  }
  if(!scene->vertex_buffer){
    backend->gl.GenBuffers(1,&scene->vertex_buffer);
    if(!scene->vertex_buffer) return 0;
  }
  if(!scene->instance_array){
    backend->gl.GenVertexArrays(1,&scene->instance_array);
    if(!scene->instance_array) return 0;
  }
  if(!scene->instance_buffer){
    backend->gl.GenBuffers(1,&scene->instance_buffer);
    if(!scene->instance_buffer) return 0;
  }
  if(!scene->instance_storage_ready){
    backend->gl.BindBuffer(GL_ARRAY_BUFFER,scene->instance_buffer);
    backend->gl.BufferData(GL_ARRAY_BUFFER,GL_SCENE_STREAM_BYTES,NULL,GL_STREAM_DRAW);
    backend->gl.BindBuffer(GL_ARRAY_BUFFER,0);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
    scene->instance_bytes_used=0;
    scene->instance_span_count=scene->instance_span_next=0;
    scene->instance_storage_ready=1;
  }
  if(!scene->instance_reuse_attempted){
    /* Reuse is optional: allocation failure leaves the established append stream available. */
    if(!scene->instance_mirror) scene->instance_mirror=(uint8_t*)malloc(GL_SCENE_STREAM_BYTES);
    scene->instance_reuse_attempted=1;
  }
  return backend->gl.GetError()==GL_NO_ERROR;
}

static int scene_snapshot_storage(GmlGpuBackend *backend,GLsizei width,GLsizei height){
  GlScene *scene=&backend->scene;
  if(!scene->snapshot_texture){
    backend->gl.GenTextures(1,&scene->snapshot_texture);
    if(!scene->snapshot_texture) return 0;
  }
  backend->gl.ActiveTexture(GL_TEXTURE1);
  backend->gl.BindTexture(GL_TEXTURE_2D,scene->snapshot_texture);
  if(scene->snapshot_width<(uint32_t)width || scene->snapshot_height<(uint32_t)height){
    if(scene->snapshot_width>(uint32_t)width) width=(GLsizei)scene->snapshot_width;
    if(scene->snapshot_height>(uint32_t)height) height=(GLsizei)scene->snapshot_height;
    backend->gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,
                           GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
    scene->snapshot_width=(uint32_t)width;
    scene->snapshot_height=(uint32_t)height;
  }
  return backend->gl.GetError()==GL_NO_ERROR;
}

int gml_gpu_opengl_scene_prepare(GmlGpuBackend *backend,uint32_t width,uint32_t height,
                                 char *error,size_t error_capacity){
  if(!backend || !backend->loaded || !backend->scene.available || !width || !height ||
     width>(uint32_t)INT_MAX || height>(uint32_t)INT_MAX ||
     width>(uint32_t)backend->scene.max_texture_size ||
     height>(uint32_t)backend->scene.max_texture_size) return 0;
  if(!scene_prepare_geometry(backend,error,error_capacity) ||
     !gml_gpu_opengl_scene_exact_program(backend,0,error,error_capacity) ||
     !gml_gpu_opengl_scene_exact_program(backend,2,error,error_capacity) ||
     !gml_gpu_opengl_scene_exact_program(backend,3,error,error_capacity)) return 0;
  /* Storage alone has no image identity or pixel authority. Prepare the fixed font policies too;
   * content-owned programs remain demand-driven. Desktop policies reuse the common program. */
  int prepared=scene_snapshot_storage(backend,(GLsizei)width,(GLsizei)height);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  return prepared;
}

/* An ordered device-local copy is shared by exact draw rules. No target write
 * occurs until this succeeds; the existing snapshot allocation is reused. */
static int scene_snapshot_region(GmlGpuBackend *backend,GlSceneImage *target,
                                  GLint left,GLint bottom,GLsizei width,GLsizei height){
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  if(!scene_snapshot_storage(backend,width,height)) return 0;
  backend->gl.CopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,left,bottom,width,height);
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  return 1;
}

/* Raw copies ignore all GML drawing state. Alias input is captured once before
 * the first write,including when logical rows wrap the physical image pitch. */
int gml_render_gpu_opengl_copy_image(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
                                      GlSceneImage *target,GlSceneImage *source){
  if(!backend || !op || !target || !source || !backend->gl.BlitFramebuffer ||
     op->opcode!=GML_RENDER_OP_COPY_IMAGE) return 0;
  const GmlRenderCopyRegion *copy=&op->payload.copy;
  uint32_t sp=copy->source_pitch?copy->source_pitch:source->width;
  uint32_t dp=copy->destination_pitch?copy->destination_pitch:target->width;
  GmlRenderTargetRef source_target={source->ref,1u,source->width,source->height,
    source->pixel_format,{0,0,source->width,source->height}};
  if(!gml_gpu_opengl_scene_target(backend,&source_target)) return 0;
  GLuint read_framebuffer=source->framebuffer;
  if(copy->needs_source_snapshot){
    if(!scene_snapshot_region(backend,source,0,0,(GLsizei)source->width,(GLsizei)source->height)) return 0;
    if(!backend->scene.copy_framebuffer)
      backend->gl.GenFramebuffers(1,&backend->scene.copy_framebuffer);
    if(!backend->scene.copy_framebuffer) return 0;
    read_framebuffer=backend->scene.copy_framebuffer;
    backend->gl.BindFramebuffer(GL_READ_FRAMEBUFFER,read_framebuffer);
    backend->gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,
      backend->scene.snapshot_texture,0);
    if(backend->gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE ||
       backend->gl.GetError()!=GL_NO_ERROR) return 0;
  }
  backend->gl.BindFramebuffer(GL_READ_FRAMEBUFFER,read_framebuffer);
  backend->gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,target->framebuffer);
  backend->gl.Scissor(0,0,(GLsizei)target->width,(GLsizei)target->height);
  if(sp==source->width && dp==target->width){
    GLint sx=copy->source.x,sy=(GLint)(source->height-(uint32_t)copy->source.y-copy->source.height);
    GLint dx=copy->destination.x,dy=(GLint)(target->height-(uint32_t)copy->destination.y-copy->destination.height);
    backend->gl.BlitFramebuffer(sx,sy,sx+(GLint)copy->source.width,sy+(GLint)copy->source.height,
      dx,dy,dx+(GLint)copy->destination.width,dy+(GLint)copy->destination.height,
      GL_COLOR_BUFFER_BIT,GL_NEAREST);
  }else for(uint32_t row=0;row<copy->source.height;row++){
    uint64_t from=((uint64_t)(uint32_t)copy->source.y+row)*sp+(uint32_t)copy->source.x;
    uint64_t to=((uint64_t)(uint32_t)copy->destination.y+row)*dp+(uint32_t)copy->destination.x;
    uint32_t remaining=copy->source.width;
    while(remaining){
      uint32_t sx=(uint32_t)(from%source->width),sy=source->height-1u-(uint32_t)(from/source->width);
      uint32_t dx=(uint32_t)(to%target->width),dy=target->height-1u-(uint32_t)(to/target->width);
      uint32_t count=source->width-sx;
      if(count>target->width-dx) count=target->width-dx;
      if(count>remaining) count=remaining;
      backend->gl.BlitFramebuffer((GLint)sx,(GLint)sy,(GLint)(sx+count),(GLint)(sy+1u),
        (GLint)dx,(GLint)dy,(GLint)(dx+count),(GLint)(dy+1u),GL_COLOR_BUFFER_BIT,GL_NEAREST);
      from+=count;to+=count;remaining-=count;
    }
  }
  return backend->gl.GetError()==GL_NO_ERROR;
}

static void compact_quad_instances(GlScene *scene,uint32_t count){
  static const unsigned corner_vertex[4]={0u,1u,2u,5u};
  for(uint32_t quad=0;quad<count;quad++){
    const float *expanded=scene->vertices+(size_t)quad*6u*GL_SCENE_VERTEX_FLOATS;
    float *instance=scene->instances+(size_t)quad*GL_SCENE_INSTANCE_FLOATS;
    for(unsigned corner=0;corner<4u;corner++){
      const float *vertex=expanded+(size_t)corner_vertex[corner]*GL_SCENE_VERTEX_FLOATS;
      instance[corner*2u]=vertex[0];
      instance[corner*2u+1u]=vertex[1];
      instance[8u+corner*2u]=vertex[2];
      instance[8u+corner*2u+1u]=vertex[3];
      for(unsigned component=0;component<4u;component++)
        instance[16u+corner*4u+component]=vertex[4u+component];
    }
    for(unsigned component=0;component<8u;component++) instance[32u+component]=expanded[8u+component];
    instance[40]=expanded[16];
    instance[41]=expanded[17];
    instance[42]=expanded[18];
    instance[43]=expanded[27];
    for(unsigned component=0;component<4u;component++){
      instance[44u+component]=expanded[19u+component];
      instance[48u+component]=expanded[23u+component];
    }
  }
}

/* The append-only stream owns this range until wrap/orphan. Write it directly rather than
 * asking the driver to copy a client packet into host-visible storage. Streaming/word stores
 * preserve float bits and avoid a slow byte-copy instruction on write-combined mappings. */
static int upload_quad_instances(GmlGpuBackend *backend,size_t offset,size_t bytes){
  GlScene *scene=&backend->scene;
  void *mapped=backend->gl.MapBufferRange(GL_ARRAY_BUFFER,(GLintptr)offset,(GLsizeiptr)bytes,
                                        GL_MAP_WRITE_BIT|GL_MAP_UNSYNCHRONIZED_BIT);
  if(mapped){
    volatile uint32_t *destination=(volatile uint32_t*)mapped;
    const unsigned char *source=(const unsigned char*)scene->instances;
    size_t copied=0;
#if defined(__SSE2__)
    if(!((uintptr_t)mapped&15u)){
      for(;copied+64u<=bytes;copied+=64u){
        _mm_stream_si128((__m128i*)((unsigned char*)mapped+copied),
                         _mm_loadu_si128((const __m128i*)(source+copied)));
        _mm_stream_si128((__m128i*)((unsigned char*)mapped+copied+16u),
                         _mm_loadu_si128((const __m128i*)(source+copied+16u)));
        _mm_stream_si128((__m128i*)((unsigned char*)mapped+copied+32u),
                         _mm_loadu_si128((const __m128i*)(source+copied+32u)));
        _mm_stream_si128((__m128i*)((unsigned char*)mapped+copied+48u),
                         _mm_loadu_si128((const __m128i*)(source+copied+48u)));
      }
      for(;copied+16u<=bytes;copied+=16u)
        _mm_stream_si128((__m128i*)((unsigned char*)mapped+copied),
                         _mm_loadu_si128((const __m128i*)(source+copied)));
    }
#endif
    for(;copied<bytes;copied+=sizeof(uint32_t)){
      uint32_t value;
      memcpy(&value,source+copied,sizeof value);
      destination[copied/sizeof value]=value;
    }
#if defined(__SSE2__)
    /* Streaming stores must finish before unmapping exposes their bytes to the device. */
    _mm_sfence();
#endif
    return backend->gl.UnmapBuffer(GL_ARRAY_BUFFER)!=GL_FALSE;
  }
  /* A declined map with no GL error can retain the established upload path.
   * An error or corrupted unmap fails the submission before its draw. */
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  backend->gl.BufferSubData(GL_ARRAY_BUFFER,(GLintptr)offset,(GLsizeiptr)bytes,scene->instances);
  return 1;
}

static size_t scene_instance_span(GmlGpuBackend *backend,size_t bytes){
  _Static_assert((GL_SCENE_INSTANCE_FLOATS*sizeof(float))%sizeof(uint64_t)==0,
                 "Instance fingerprint words must cover whole records.");
  GlScene *scene=&backend->scene;
  const uint8_t *input=(const uint8_t*)scene->instances;
  uint64_t hash=UINT64_C(14695981039346656037);
  size_t offset;
  if(scene->instance_mirror){
    /* All records contain a multiple of eight bytes. Hashes only reject candidates;
     * equality always compares every byte, including signed zero and trailing operands. */
    for(size_t index=0;index<bytes;index+=sizeof(uint64_t)){
      uint64_t word;
      memcpy(&word,input+index,sizeof word);
      hash=(hash^word)*UINT64_C(1099511628211);
    }
    for(unsigned index=0;index<scene->instance_span_count;index++)
      if(scene->instance_spans[index].bytes==bytes && scene->instance_spans[index].hash==hash &&
         !memcmp(scene->instance_mirror+scene->instance_spans[index].offset,input,bytes))
        return scene->instance_spans[index].offset;
  }
  /* Existing ranges remain immutable until orphaning; no fence or overwrite is added. */
  if(bytes>GL_SCENE_STREAM_BYTES-scene->instance_bytes_used){
    scene->instance_span_count=scene->instance_span_next=0;
    backend->gl.BufferData(GL_ARRAY_BUFFER,GL_SCENE_STREAM_BYTES,NULL,GL_STREAM_DRAW);
    scene->instance_bytes_used=0;
  }
  offset=scene->instance_bytes_used;
  if(!upload_quad_instances(backend,offset,bytes)) return SIZE_MAX;
  scene->instance_bytes_used+=bytes;
  if(scene->instance_mirror){
    unsigned index=scene->instance_span_next;
    memcpy(scene->instance_mirror+offset,input,bytes);
    scene->instance_spans[index].offset=offset;
    scene->instance_spans[index].bytes=bytes;
    scene->instance_spans[index].hash=hash;
    scene->instance_span_next=(index+1u)%GL_SCENE_INSTANCE_SPANS;
    if(scene->instance_span_count<GL_SCENE_INSTANCE_SPANS) scene->instance_span_count++;
  }
  return offset;
}

static int flush_quads(GmlGpuBackend *backend,GlSceneImage *target,GlSceneImage *source,
                       uint32_t filter,int blend,uint32_t count,uint32_t write_mask,int alpha_test,
                       uint32_t source_alpha_key,const GmlRenderSourcePalette *palette){
  GlScene *scene=&backend->scene;
  GLfloat extent[2]={(GLfloat)target->width,(GLfloat)target->height};
  GLenum texture_filter=(filter>=106u && filter<=108u) || filter==99u || filter==100u || filter==77u || filter==18u || filter==19u || filter==20u || filter==21u ||
    (filter>=65u && filter<=74u) || (filter>=79u && filter<=82u) ||
    filter==112u || filter==114u || filter==115u || (filter>=137u && filter<=158u) ||
    filter==160u || filter==162u || filter==164u || filter==166u
    ?GL_LINEAR:GL_NEAREST;
  GLsizei stride=GL_SCENE_INSTANCE_FLOATS*(GLsizei)sizeof(float);
  size_t bytes=(size_t)count*GL_SCENE_INSTANCE_FLOATS*sizeof(float),offset;
  GLint attributes[13];
  int upload_ok=1;
  if(!count) return 1;
  if(count>GL_SCENE_MAX_QUADS) return 0;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  backend->gl.Viewport(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.Scissor(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,source?source->texture:0u);
  if(source){
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,(GLint)texture_filter);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,(GLint)texture_filter);
    GLint wrap=filter>=103u && filter<=108u?GL_REPEAT:GL_CLAMP_TO_EDGE;
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,wrap);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,wrap);
  }
  if(blend && blend!=8 && filter!=15u && filter!=17u){
    GLenum rgb_equation=GL_FUNC_ADD,alpha_equation=GL_FUNC_ADD;
    if(blend>=GL_SCENE_BLEND_SURFACE_DEST_BASE && blend<=GL_SCENE_BLEND_SURFACE_DEST_LIMIT){
      unsigned encoded=(unsigned)(blend-GL_SCENE_BLEND_SURFACE_DEST_BASE)/2u;
      unsigned mode=encoded/25u,rgb=(encoded%25u)/5u,alpha=encoded%5u;
      static const GLenum equations[]={GL_FUNC_ADD,GL_MAX,GL_FUNC_REVERSE_SUBTRACT,GL_FUNC_SUBTRACT,GL_MIN};
      int preserve=(blend&1)!=0;
      backend->gl.BlendFuncSeparate(mode==2u?GL_DST_ALPHA:GL_DST_COLOR,
        mode==0u?GL_SRC_COLOR:GL_DST_COLOR,preserve?GL_DST_ALPHA:GL_ONE,
        preserve?(mode==0u?GL_SRC_ALPHA:GL_DST_ALPHA):GL_ZERO);
      rgb_equation=equations[rgb];alpha_equation=preserve?equations[alpha]:GL_FUNC_ADD;
    }else if(blend>=22 && blend<=25){
      int multiply=blend<=23,preserve=(blend&1)==0;
      backend->gl.BlendFuncSeparate(multiply?GL_DST_COLOR:GL_SRC_ALPHA,
        multiply?GL_ZERO:GL_ONE_MINUS_SRC_COLOR,
        preserve?(multiply?GL_ZERO:GL_SRC_ALPHA):GL_ONE,
        preserve?(multiply?GL_ONE:GL_ONE_MINUS_SRC_ALPHA):GL_ZERO);
    }else if(blend==GL_SCENE_BLEND_RECTANGLE_ADD_BYTES ||
       blend==GL_SCENE_BLEND_RECTANGLE_ADD_BYTES+1){
      backend->gl.BlendFuncSeparate(GL_ONE,GL_ONE,GL_ONE,
        blend==GL_SCENE_BLEND_RECTANGLE_ADD_BYTES+1?GL_ONE:GL_ZERO);
    } else if(blend>=GL_SCENE_BLEND_RECTANGLE_BASE && blend<=GL_SCENE_BLEND_RECTANGLE_LIMIT){
      unsigned mode=(unsigned)(blend-GL_SCENE_BLEND_RECTANGLE_BASE)/2u;
      int preserve=(blend&1)!=0;
      GLenum src=GL_SRC_ALPHA,dst=GL_ONE_MINUS_SRC_ALPHA;
      GLenum alpha_src=GL_SRC_ALPHA,alpha_dst=GL_ONE_MINUS_SRC_ALPHA;
      if(mode==1u){dst=GL_ONE;alpha_src=alpha_dst=GL_ONE;}
      else if(mode==2u){src=GL_ZERO;dst=GL_ONE_MINUS_SRC_COLOR;
        alpha_src=GL_ZERO;alpha_dst=GL_ONE_MINUS_SRC_ALPHA;}
      else if(mode==4u) dst=GL_ONE_MINUS_SRC_COLOR;
      else if(mode==5u){src=GL_SRC_COLOR;dst=GL_ONE;alpha_dst=GL_ONE;}
      else if(mode==7u){src=alpha_src=GL_ONE_MINUS_DST_ALPHA;dst=alpha_dst=GL_DST_ALPHA;}
      else if(mode==8u){src=GL_ONE_MINUS_DST_COLOR;dst=alpha_dst=GL_ZERO;alpha_src=GL_ONE_MINUS_DST_ALPHA;}
      else if(mode>=9u){src=mode==11u?GL_DST_ALPHA:GL_DST_COLOR;
        dst=mode==9u?GL_SRC_COLOR:GL_DST_COLOR;
        alpha_src=GL_DST_ALPHA;alpha_dst=mode==9u?GL_SRC_ALPHA:GL_DST_ALPHA;}
      if(!preserve){alpha_src=GL_ONE;alpha_dst=GL_ZERO;}
      backend->gl.BlendFuncSeparate(src,dst,alpha_src,alpha_dst);
    } else if(blend>=18 && blend<=21){
      int colour=(blend&1)!=0,preserve=blend<20;
      backend->gl.BlendFuncSeparate(colour?GL_ONE_MINUS_DST_COLOR:GL_ONE_MINUS_DST_ALPHA,
        colour?GL_ZERO:GL_DST_ALPHA,preserve?GL_ONE_MINUS_DST_ALPHA:GL_ONE,
        preserve && !colour?GL_DST_ALPHA:GL_ZERO);
    } else if(blend==15)
      backend->gl.BlendFuncSeparate(GL_ZERO,GL_ONE_MINUS_SRC_COLOR,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    else if(blend==16)
      backend->gl.BlendFuncSeparate(GL_SRC_ALPHA,GL_ONE,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    else if(blend==17)
      backend->gl.BlendFuncSeparate(GL_ONE,GL_ZERO,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    else if(blend==13)
      backend->gl.BlendFuncSeparate(GL_ZERO,GL_ONE_MINUS_SRC_COLOR,GL_ZERO,GL_ONE_MINUS_SRC_ALPHA);
    else if(blend==14)
      backend->gl.BlendFuncSeparate(GL_SRC_ALPHA,GL_ONE,GL_ONE,GL_ONE);
    else if(blend==GL_SCENE_BLEND_INVERSE_RECT)
      backend->gl.BlendFuncSeparate(GL_ZERO,GL_ONE_MINUS_SRC_COLOR,GL_ONE,GL_ZERO);
    else backend->gl.BlendFuncSeparate(blend==9 || blend==10?GL_ONE:GL_SRC_ALPHA,
                                  blend==9?GL_ONE:GL_ONE_MINUS_SRC_ALPHA,
                                  blend==3 || blend==9 || blend==10?GL_SRC_ALPHA:GL_ONE,
                                  GL_ONE_MINUS_SRC_ALPHA);
    backend->gl.BlendEquationSeparate(rgb_equation,alpha_equation);
    backend->gl.Enable(GL_BLEND);
  } else backend->gl.Disable(GL_BLEND);
  int stamp_alpha=filter==133u || filter==130u || filter==104u || filter==107u || filter==99u || filter==77u || filter==75u || filter==9u || filter==12u || filter==21u || filter==23u ||
                  filter==25u || filter==32u || filter==35u || filter==39u || filter==40u ||
                  filter==46u || filter==47u || filter==53u || filter==60u ||
                  filter==66u || filter==69u || filter==71u || filter==80u ||
                  filter==86u || filter==93u || filter==110u || filter==112u ||
                  filter==138u || filter==141u || filter==143u || filter==148u || filter==152u ||
                  filter==156u || filter==158u || filter==161u || filter==162u || filter==168u;
  backend->gl.ColorMask((write_mask&1u)!=0u,(write_mask&2u)!=0u,(write_mask&4u)!=0u,
                        (!stamp_alpha || filter==110u) && (write_mask&8u)!=0u);
  backend->gl.UseProgram(scene->program);
  backend->gl.Uniform2fv(scene->u_extent,1,extent);
  backend->gl.Uniform1i(scene->u_image,0);
  backend->gl.Uniform1i(scene->u_filter,(GLint)filter);
  backend->gl.Uniform1i(scene->u_alpha_test,alpha_test);
  backend->gl.Uniform1i(scene->u_source_alpha_key,(GLint)source_alpha_key);
  backend->gl.Uniform1i(scene->u_source_palette_enabled,palette && palette->enabled);
  if(palette && palette->enabled){
    GLint colours[30],green[2][4];
    for(unsigned index=0;index<10u;index++) for(unsigned channel=0;channel<3u;channel++)
      colours[index*3u+channel]=(GLint)((palette->colour[index]>>(16u-8u*channel))&255u);
    for(unsigned branch=0;branch<2u;branch++) for(unsigned band=0;band<4u;band++)
      green[branch][band]=(GLint)palette->green[branch][band];
    backend->gl.Uniform1i(scene->u_source_palette_red,(GLint)palette->red);
    backend->gl.Uniform4iv(scene->u_source_palette_green[0],1,green[0]);
    backend->gl.Uniform4iv(scene->u_source_palette_green[1],1,green[1]);
    backend->gl.Uniform3iv(scene->u_source_palette_colour,10,colours);
  }
  backend->gl.Uniform1i(scene->u_previous,1);
  /* Integer map samplers must never alias the floating image samplers, even
   * for a draw whose dynamic branch does not fetch either point map. */
  backend->gl.Uniform1i(scene->u_point_map_x,2);
  backend->gl.Uniform1i(scene->u_point_map_y,3);
  compact_quad_instances(scene,count);
  backend->gl.BindVertexArray(scene->instance_array);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,scene->instance_buffer);
  offset=scene_instance_span(backend,bytes);
  upload_ok=offset!=SIZE_MAX;
  if(!upload_ok) goto restore_stream_state;
  attributes[0]=scene->a_position01;
  attributes[1]=scene->a_position23;
  attributes[2]=scene->a_texcoord01;
  attributes[3]=scene->a_texcoord23;
  for(unsigned index=0;index<4u;index++) attributes[4u+index]=scene->a_colour[index];
  attributes[8]=scene->a_clamp;
  attributes[9]=scene->a_map;
  attributes[10]=scene->a_step_margin_active;
  attributes[11]=scene->a_span_x;
  attributes[12]=scene->a_span_y;
  for(unsigned index=0;index<13u;index++){
    backend->gl.EnableVertexAttribArray((GLuint)attributes[index]);
    backend->gl.VertexAttribPointer((GLuint)attributes[index],4,GL_FLOAT,GL_FALSE,stride,
                                    (const void*)(offset+(size_t)index*4u*sizeof(float)));
    backend->gl.VertexAttribDivisor((GLuint)attributes[index],1u);
  }
  backend->gl.DrawArraysInstanced(GL_TRIANGLES,0,6,(GLsizei)count);
  if(stamp_alpha && (write_mask&8u)){
    /* The CPU screen writer keeps alpha opaque at every covered texel,
     * even when its prior alpha is unknown. Reuse the sampled vertices and
     * the same sampler and cutout in an alpha-only pass, preserving that recipe's coverage. */
    backend->gl.ColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
    backend->gl.Disable(GL_BLEND);
    backend->gl.Uniform1i(scene->u_filter,filter==168u?169:filter==161u || filter==162u?(GLint)filter+2:filter==148u || filter==156u?150:filter==152u || filter==158u?154:filter>=137u && filter<=146u?(filter==141u?146:144):filter==133u?134:filter==130u?131:filter==112u?113:filter==110u?111:filter==104u || filter==107u?(GLint)filter+1:filter==99u?100:filter==93u?95:filter==86u?88:filter==80u?82:filter==77u?78:filter==75u?76:filter==69u?74:filter>=65u?72:filter==60u?62:filter==53u?55:(filter==46u || filter==47u)?48:(filter==39u || filter==40u)?41:filter==35u?36:filter==32u?33:filter==25u?26:
                         filter==23u?24:filter==21u?22:filter==9u?10:13);
    backend->gl.DrawArraysInstanced(GL_TRIANGLES,0,6,(GLsizei)count);
  }
restore_stream_state:
  if(source && filter>=103u && filter<=108u){
    /* Repeat belongs to this row sampler, not to a later content-program or
     * presentation consumer of the same versioned image. */
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  }
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  if(upload_ok) for(unsigned index=0;index<13u;index++)
    backend->gl.DisableVertexAttribArray((GLuint)attributes[index]);
  backend->gl.BindBuffer(GL_ARRAY_BUFFER,0);
  backend->gl.BindVertexArray(0);
  backend->gl.UseProgram(0);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.Disable(GL_BLEND);
  if(backend->gl.GetError()!=GL_NO_ERROR || !upload_ok){
    scene->instance_span_count=scene->instance_span_next=0;
    target->valid=0;
    return 0;
  }
  target->valid=1;
  target->rendered_origin=1;
  return 1;
}

/* The CPU handoff shades an RGBA8 plane before its ordinary unit surface draw.
 * Reuse that surface sampler, fixed RGB blend and opaque-covered-alpha pass;
 * the private intermediate never crosses the device boundary. */
int gml_gpu_opengl_compose_shader_output(GmlGpuBackend *backend,GlSceneImage *target,
                                         GmlRenderRect region){
  GlSceneImage source={0};
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GmlRenderQuad *quad=&operation.payload.quad;
  if(!backend || !target || !backend->readback_texture ||
     backend->readback_width!=target->width || backend->readback_height!=target->height ||
     !valid_region(region,target->width,target->height)) return 0;
  source.texture=backend->readback_texture;
  source.width=target->width;source.height=target->height;
  source.pixel_format=GML_RENDER_PIXEL_ARGB8888;source.rendered_origin=1;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.scene_family=GML_RENDER_SCENE_SURFACE;
  operation.target.width=target->width;operation.target.height=target->height;
  operation.target.region=region;
  quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  quad->sampling=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,
    GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,source.width,source.height},
    GML_RENDER_QUANTISE_SURFACE_NEAREST_FUNCTION,0u};
  quad->x[0]=quad->x[3]=region.x;quad->x[1]=quad->x[2]=(double)region.x+region.width;
  quad->y[0]=quad->y[1]=region.y;quad->y[2]=quad->y[3]=(double)region.y+region.height;
  quad->u[0]=quad->u[3]=(double)region.x/source.width;
  quad->u[1]=quad->u[2]=(double)(region.x+(int32_t)region.width)/source.width;
  quad->v[0]=quad->v[1]=(double)region.y/source.height;
  quad->v[2]=quad->v[3]=(double)(region.y+(int32_t)region.height)/source.height;
  quad->inverse_anchor[0]=region.x;quad->inverse_anchor[1]=region.y;
  quad->inverse_x[0]=region.width;quad->inverse_y[1]=region.height;
  quad->span_x[2]=region.x;quad->span_x[3]=region.y;
  quad->span_y[1]=region.width;quad->span_y[2]=region.height;quad->span_y[3]=2.0;
  quad->filtered_draw_alpha=1.0;
  for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]=0xFFFFFFFFu;
  state.colour=0xFFFFFFFFu;state.alpha=255u;state.write_mask=15u;
  state.coverage_policy=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA;
  if(!gml_render_gpu_opengl_draw_textured_quad(&operation,&state,source.width,source.height,1,
                                             backend->scene.vertices)) return 0;
  return flush_quads(backend,target,&source,53u,1,1u,15u,-1,0u,NULL);
}

/* Each pixel of an ordered, disjoint row run reads one immutable device-local
 * predecessor. Overlapping or differently versioned runs take a fresh snapshot;
 * scalar and vector packets carry their own alpha policy in the vertex operand. */
static int draw_rotated_regions(GmlGpuBackend *backend,const GmlRenderDrawBatch *batch,
                                 uint32_t first,GlSceneImage *target,GlSceneImage *source,
                                 uint32_t *consumed){
  GlScene *scene=&backend->scene;
  const GmlRenderDrawOp *base=&batch->operations[first];
  const GmlRenderDrawStateSnapshot *base_state=&batch->states[base->state_index];
  int modern=scene_filter(&base->payload.quad,base_state,base->scene_family)==27u;
  int texel=scene_filter(&base->payload.quad,base_state,base->scene_family)==29u;
  int alpha_mode=texel?2:modern?1:0;
  GLint left=0,bottom=0,right=0,top=0,last_left=0,last_top=0;
  GLsizei last_width=0,last_height=0;
  uint32_t count=0;
  while(first+count<batch->operation_count && count<GL_SCENE_MAX_QUADS){
    /* A point quad's two maps are keyed to its own original snapped anchor.
     * Keep its ordered predecessor separate from the next quad's maps. */
    if(modern && count) break;
    const GmlRenderDrawOp *op=&batch->operations[first+count];
    GLint x,y;
    GLsizei w,h;
    if(op->opcode!=GML_RENDER_OP_TEXTURED_QUAD || op->source_count!=1u ||
       (texel
         ?scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family)!=29u
         :modern
         ?scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family)!=27u
         :scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family)!=15u &&
          scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family)!=17u) ||
       batch->states[op->state_index].write_mask!=base_state->write_mask ||
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
  if(!count) return 0;
  if(modern){
    const GmlRenderQuad *quad=&base->payload.quad;
    GLuint map_x,map_y;
    int first_y=(int)target->height-top;
    if(!gml_gpu_opengl_upload_point_axis(backend,0u,quad->inverse_anchor[0],
         quad->inverse_x[1],quad->inverse_x[0],left,(uint32_t)(right-left),
         quad->sampling.source_region.width,&map_x) ||
       !gml_gpu_opengl_upload_point_axis(backend,1u,quad->inverse_anchor[1],
         quad->inverse_y[0],quad->inverse_y[1],first_y,(uint32_t)(top-bottom),
         quad->sampling.source_region.height,&map_y)) return 0;
    backend->gl.UseProgram(scene->program);
    GLint point_origin[2]={left,first_y};
    backend->gl.Uniform2iv(scene->u_point_origin,1,point_origin);
    backend->gl.Uniform1i(scene->u_point_map_x,2);
    backend->gl.Uniform1i(scene->u_point_map_y,3);
    backend->gl.ActiveTexture(GL_TEXTURE2);
    backend->gl.BindTexture(GL_TEXTURE_2D,map_x);
    backend->gl.ActiveTexture(GL_TEXTURE3);
    backend->gl.BindTexture(GL_TEXTURE_2D,map_y);
    if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  }
  if(!scene_snapshot_region(backend,target,left,bottom,right-left,top-bottom))
    return 0;
  backend->gl.UseProgram(scene->program);
  if(!scene->rotated_alpha16_valid || scene->rotated_alpha16_modern_nearest!=alpha_mode ||
     scene->rotated_alpha16_value!=base->payload.quad.filtered_draw_alpha){
    GLint factors[256];
    for(unsigned alpha=0;alpha<256u;alpha++){
      double draw_alpha=base->payload.quad.filtered_draw_alpha;
      if(texel){
        unsigned factor=gml_render_rotated_alpha16_factor(alpha,draw_alpha);
        unsigned source_alpha=(unsigned)lround(alpha*draw_alpha);
        factors[alpha]=(GLint)((source_alpha<<17)|factor);
      } else if(modern){
        /* RGB retains a combined 22-bit weight; alpha independently uses the CPU's
         * binary64 rounded source byte. Both fit in one positive signed uniform. */
        double weight=(alpha/255.0)*draw_alpha;
        unsigned factor=(unsigned)lround(weight*4194304.0);
        unsigned source_alpha=(unsigned)lround(alpha*draw_alpha);
        factors[alpha]=(GLint)((source_alpha<<23)|factor);
      } else factors[alpha]=(GLint)gml_render_rotated_alpha16_factor(alpha,draw_alpha);
    }
    /* Pack four entries per vector to remain below the GLES uniform minimum. */
    backend->gl.Uniform4iv(scene->u_rotated_alpha16,64,factors);
    if(backend->gl.GetError()!=GL_NO_ERROR){ scene->rotated_alpha16_valid=0; return 0; }
    scene->rotated_alpha16_value=base->payload.quad.filtered_draw_alpha;
    scene->rotated_alpha16_modern_nearest=alpha_mode;
    scene->rotated_alpha16_valid=1;
  }
  GLint origin[2]={left,bottom};
  backend->gl.Uniform2iv(scene->u_region_origin,1,origin);
  if(backend->gl.GetError()!=GL_NO_ERROR) return 0;
  if(!flush_quads(backend,target,source,texel?29u:modern?27u:15u,0,count,base_state->write_mask,-1,0u,&base_state->source_palette)) return -1;
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
  if(!exact_rect(op,&left,&bottom,&width,&height,
                 qualified_blend(state)==4 || qualified_blend(state)==6 ||
                 qualified_blend(state)==7 || qualified_blend(state)==11) ||
     !(exact=gml_gpu_opengl_scene_exact_program(backend,font_mode,error,error_capacity)) ||
     !gml_render_gpu_opengl_draw_textured_quad(op,state,source->width,source->height,
                                               source->rendered_origin,
                                               scene->vertices)) return 0;
  if(font_mode && backend->api==GML_GPU_API_OPENGLES3 &&
     exact->exact_u_font_channel>=0 && !backend->gl.Flush){
    gml_gpu_opengl_record_error(error,error_capacity,"exact font submission is unavailable");
    return 0;
  }
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
                          (qualified_blend(state)==11?9:qualified_blend(state)==7?8:qualified_blend(state)==6?7:
                           qualified_blend(state)==5?6:qualified_blend(state)==4?1:0));
  if(qualified_blend(state)==11){
    GLint alpha_words[2];
    uint32_t colour=op->payload.quad.corner_colour[0];
    GLint tint[4]={(GLint)((colour>>16)&255u),(GLint)((colour>>8)&255u),
                   (GLint)(colour&255u),255};
    GLint rule[4]={state->rgb_source_factor==GML_RENDER_BLEND_DESTINATION_ALPHA?11:
                    state->rgb_destination_factor==GML_RENDER_BLEND_SOURCE_COLOUR?9:10,
                  (GLint)(state->rgb_equation-GML_RENDER_BLEND_EXACT_DESTINATION_ADD)+1,
                  (GLint)(state->alpha_equation-GML_RENDER_BLEND_EXACT_DESTINATION_ADD)+1,
                  state->coverage_policy==0u};
    double_words(op->payload.quad.filtered_draw_alpha,alpha_words);
    backend->gl.Uniform4iv(exact->exact_u_surface_rule,1,rule);
    backend->gl.Uniform4iv(exact->exact_u_surface_tint,1,tint);
    backend->gl.Uniform2iv(exact->exact_u_surface_alpha,1,alpha_words);
  }
  if(qualified_blend(state)==7 || qualified_blend(state)==11){
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
  if(font_mode && backend->api==GML_GPU_API_OPENGLES3 && exact->exact_u_font_channel>=0){
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

int gml_gpu_opengl_prepare_automatic_draw(GmlGpuBackend *backend,GLuint source_texture,
                                          char *error,size_t error_capacity){
  GmlRenderDrawOp operation={0};
  GmlRenderDrawStateSnapshot state={0};
  GlSceneImage target={.texture=backend->readback_texture,
    .framebuffer=backend->readback_framebuffer,.width=1u,.height=1u,
    .pixel_format=GML_RENDER_PIXEL_ARGB8888};
  GlSceneImage source={.texture=source_texture,.width=1u,.height=1u,
    .pixel_format=GML_RENDER_PIXEL_ARGB8888,.rendered_origin=1};
  if(!source_texture || !target.texture || !target.framebuffer) return 0;
  operation.opcode=GML_RENDER_OP_TEXTURED_QUAD;
  operation.target.width=operation.target.height=1u;
  GmlRenderQuad *quad=&operation.payload.quad;
  quad->shape=GML_RENDER_QUAD_AXIS_ALIGNED;
  quad->x[1]=quad->x[2]=quad->y[2]=quad->y[3]=1.0;
  quad->u[1]=quad->u[2]=quad->v[2]=quad->v[3]=1.0;
  quad->sampling=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,
    GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,1u,1u},GML_RENDER_QUANTISE_AUTOMATIC_SURFACE_MODERN,0u};
  quad->source_opaque=1u;
  for(unsigned corner=0;corner<4u;corner++) quad->corner_colour[corner]=0xFFFFFFFFu;
  state.write_mask=15u;state.colour=0xFFFFFFFFu;state.alpha=255u;
  state.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA;
  state.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.alpha_source_factor=GML_RENDER_BLEND_ONE;
  state.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA;
  state.rgb_equation=state.alpha_equation=GML_RENDER_BLEND_EXACT_AUTOMATIC_SURFACE;
  /* Reuse the actual composition writer, including its private predecessor copy. Neither
   * image is entered in the scene registry or given a content identity or authority. */
  return draw_exact_quad(backend,&operation,&state,&target,&source,1u,NULL,
                         error,error_capacity)>0;
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
  uint32_t pending_filter=0,pending_count=0,pending_write_mask=15u;
  uint32_t pending_source_alpha_key=0u;
  GmlRenderSourcePalette pending_palette={0};
  uint32_t index=0u;
  int pending_blend=0,pending_alpha_test=-1;
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
  for(;index<batch->operation_count;index++){
    const GmlRenderDrawOp *op=&batch->operations[index];
    GlSceneImage *target;
    int disc=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_PARTICLE_DISC;
    int particle=disc || (op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_PARTICLE_LINE);
    int inverse_rect=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT;
    int studio_alpha_rect=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT;
    int textured_vertex=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_TEXTURED_VERTEX_ROW;
    int rectangle=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      (op->payload.primitive.kind==GML_RENDER_PRIMITIVE_RECTANGLE || textured_vertex);
    GlSceneImage *primitive_source=textured_vertex?gml_gpu_opengl_scene_image(backend,op->sources[0]):NULL;
    unsigned rectangle_blend=rectangle?
      (op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_BLEND_MASK)>>
       GML_RENDER_RECTANGLE_BLEND_SHIFT:0u;
    int vertex_span=rectangle &&
      (op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_VERTEX_SPAN);
    int additive_bytes=rectangle && rectangle_blend==1u &&
      op->payload.primitive.alpha_blend &&
      (!(op->payload.primitive.rectangle_flags&
         (GML_RENDER_RECTANGLE_OUTLINE|GML_RENDER_RECTANGLE_GRADIENT)) ||
       (op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_PIXEL_SPAN));
    uint32_t primitive_write_mask=rectangle &&
      (op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_PIXEL_SPAN)?
      op->payload.primitive.pixel_write_mask:15u;
    int alpha_rect=inverse_rect || studio_alpha_rect || (op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT);
    uint32_t primitive_filter=rectangle?
      (op->payload.primitive.alpha_blend && (op->payload.primitive.alpha<1.0 || vertex_span) &&
       (rectangle_blend==0u || rectangle_blend==1u || rectangle_blend==4u) &&
       !(op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_TARGET_ALPHA)?35u:34u):
      disc?109u:particle?28u:studio_alpha_rect?32u:31u;
    if(textured_vertex) primitive_filter=(op->payload.primitive.interpolate?106u:103u)+
      (primitive_filter==35u?1u:0u);
    int primitive_blend=additive_bytes?
      (int)(GL_SCENE_BLEND_RECTANGLE_ADD_BYTES+
        ((op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_TARGET_ALPHA)!=0u)):rectangle?
      (op->payload.primitive.alpha_blend?
        ((op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_SOURCE_OVER_ALPHA)?1:
         rectangle_blend?(int)(GL_SCENE_BLEND_RECTANGLE_BASE+2u*rectangle_blend+
           ((op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_TARGET_ALPHA)!=0u)):
         (op->payload.primitive.alpha<1.0 || vertex_span)?3:0):0):
      inverse_rect?GL_SCENE_BLEND_INVERSE_RECT:
      (particle || alpha_rect?(int)op->payload.primitive.alpha_blend:0);
    if(pending_count && (particle || alpha_rect || rectangle
       ? pending_palette.enabled || !active_target || !same_logical(active_target->ref,op->target.image) ||
         pending_count==GL_SCENE_MAX_QUADS || pending_filter!=primitive_filter ||
         pending_blend!=primitive_blend || pending_alpha_test!=-1 ||
         pending_write_mask!=primitive_write_mask ||
         (textured_vertex?(!pending_source || !same_version(pending_source->ref,op->sources[0])):
                          pending_source!=NULL)
       : (op->opcode!=GML_RENDER_OP_TEXTURED_QUAD ||
       !active_target || !same_logical(active_target->ref,op->target.image) ||
       pending_count==GL_SCENE_MAX_QUADS ||
       !pending_source || !same_version(pending_source->ref,op->sources[0]) ||
       pending_filter!=scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family) ||
       pending_blend!=qualified_blend(&batch->states[op->state_index]) ||
       pending_write_mask!=batch->states[op->state_index].write_mask ||
       pending_source_alpha_key!=batch->states[op->state_index].source_alpha_key ||
       memcmp(&pending_palette,&batch->states[op->state_index].source_palette,sizeof pending_palette) ||
       pending_alpha_test!=(batch->states[op->state_index].alpha_test_enabled
         ?(int)batch->states[op->state_index].alpha_test_reference:-1) ||
       qualified_blend(&batch->states[op->state_index])==2 ||
       qualified_blend(&batch->states[op->state_index])==4 ||
       qualified_blend(&batch->states[op->state_index])==5 ||
       qualified_blend(&batch->states[op->state_index])==6 ||
       qualified_blend(&batch->states[op->state_index])==11 ||
       scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family)==15u ||
       scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family)==17u ||
       scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family)==29u ||
       scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family)==27u))){
      if(!flush_quads(backend,active_target,pending_source,pending_filter,
                      pending_blend,pending_count,pending_write_mask,pending_alpha_test,
                      pending_source_alpha_key,&pending_palette)){
        mark_uncertain(&result,active_target);
        break;
      }
      pending_count=0;
      result.accepted_prefix=index;
    }
    GlSceneImage *copy_source=op->opcode==GML_RENDER_OP_COPY_IMAGE
      ?gml_gpu_opengl_scene_image(backend,op->sources[0]):NULL;
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
    if(op->opcode==GML_RENDER_OP_COPY_IMAGE){
      if(!copy_source || !gml_render_gpu_opengl_copy_image(backend,op,target,copy_source)){
        mark_uncertain(&result,target);break;
      }
      target->ref=op->target.image;target->valid=1;target->rendered_origin=1;
      result.accepted_prefix=index+1u;continue;
    }
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
      if(particle || alpha_rect || rectangle){
        const GmlRenderPrimitiveRect *p=&op->payload.primitive;
        uint32_t additive_rgb=0u;
        unsigned additive_alpha=0u;
        if(additive_bytes && !vertex_span){
          /* The unchanged CPU filled span truncates constant source increments.
           * Quantise those four known bytes once, not destination pixels. */
          additive_rgb=((uint32_t)(((p->colour>>16)&255u)*p->alpha)<<16)|
            ((uint32_t)(((p->colour>>8)&255u)*p->alpha)<<8)|
            (uint32_t)((p->colour&255u)*p->alpha);
          additive_alpha=(unsigned)(255.0*p->alpha);
        }
        static const unsigned corners[6]={0,1,2,0,2,3};
        double rad=p->angle*3.14159265358979323846/180.0;
        double co=cos(rad),si=sin(rad);
        /* Repeat is invariant under integer periods. Remove the row's large
         * common offset before conversion so it cannot consume texel phase. */
        double texture_u=textured_vertex?p->centre_x-floor(p->centre_x):0.0;
        double texture_v=textured_vertex?p->centre_y-floor(p->centre_y):0.0;
        int line=particle && !disc;
        uint32_t region_count=line?p->region.height:1u;
        for(uint32_t row=0;row<region_count;row++){
          GmlRenderRect region=p->region;
          if(line && !gml_render_particle_line_row_region(p,co,si,p->region.y+(int)row,&region))
            continue;
          if(line) while(row+1u<region_count){
            GmlRenderRect next={0};
            if(!gml_render_particle_line_row_region(p,co,si,p->region.y+(int)row+1,&next) ||
               next.x!=region.x || next.width!=region.width) break;
            region.height++;row++;
          }
          if(pending_count==GL_SCENE_MAX_QUADS){
            if(!flush_quads(backend,active_target,pending_source,pending_filter,
                            pending_blend,pending_count,pending_write_mask,pending_alpha_test,
                            pending_source_alpha_key,&pending_palette)){
              mark_uncertain(&result,active_target);goto scene_finish;
            }
            pending_count=0u;
            /* A capacity flush can finish rows, not the current logical draw. */
            result.accepted_prefix=index;
          }
          for(unsigned vertex=0;vertex<6u;vertex++){
            unsigned corner=corners[vertex];
            float *v=backend->scene.vertices+
              ((size_t)pending_count*6u+vertex)*GL_SCENE_VERTEX_FLOATS;
            memset(v,0,GL_SCENE_VERTEX_FLOATS*sizeof *v);
            v[0]=(float)region.x+((corner==1u || corner==2u)?region.width:0u);
            v[1]=(float)region.y+(corner>=2u?region.height:0u);
            v[4]=(float)((p->colour>>16)&255u)/255.0f;
            v[5]=(float)((p->colour>>8)&255u)/255.0f;
            v[6]=(float)(p->colour&255u)/255.0f;
            v[7]=(float)p->alpha;
            v[12]=(float)p->centre_x; v[13]=(float)p->centre_y;
            v[14]=(float)co; v[15]=(float)si;
            v[16]=(float)p->scale_x; v[17]=(float)p->scale_y;
            v[21]=(float)p->interpolate; v[22]=(float)p->alpha_blend;
            if(rectangle){
              v[12]=(float)p->authored_region.x; v[13]=(float)p->authored_region.y;
              v[14]=(float)(p->authored_region.width-1u);
              v[15]=(float)(p->authored_region.height-1u);
              v[18]=(float)p->rectangle_flags;
              for(unsigned c=0;c<4u;c++) v[19u+c]=(float)(p->corner_colour[c]&0xFFFFFFu);
              v[23]=(float)p->region.x; v[24]=(float)(p->region.width-1u);
              v[25]=(float)additive_rgb;v[26]=(float)additive_alpha;
              v[27]=(float)p->alpha_blend;
              if(p->rectangle_flags&GML_RENDER_RECTANGLE_CIRCLE_FAN){
                v[2]=(float)p->centre_x;v[3]=(float)p->centre_y;
                v[16]=(float)p->scale_x;v[17]=(float)p->scale_y;
                v[25]=(float)p->angle;
              }
              if(vertex_span){
                for(unsigned c=0;c<4u;c++){
                  v[12u+c]=(float)p->vertex_first[c];
                  v[19u+c]=(float)p->vertex_step[c];
                }
                if(textured_vertex){
                  double offset=corner==1u || corner==2u?p->region.width-0.5:-0.5;
                  v[2]=(float)(texture_u+offset*p->scale_x);
                  v[3]=(float)(texture_v+offset*p->scale_y);
                  v[8]=primitive_source->rendered_origin?1.0f:0.0f;
                }
              }
            }
          }
          pending_source=primitive_source; pending_filter=primitive_filter; pending_blend=primitive_blend;
          pending_source_alpha_key=0u;
          memset(&pending_palette,0,sizeof pending_palette);
          pending_alpha_test=-1;
          if(alpha_rect || rectangle) pending_write_mask=primitive_write_mask;
          pending_count++;
        }
        target->valid=1; target->rendered_origin=1;
        continue;
      }
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
    if(op->opcode==GML_RENDER_OP_RECOVER_EMPTY_SCREEN){
      GLint rect[4]={(GLint)op->payload.quad.x[0],(GLint)op->payload.quad.y[0],
        (GLint)(op->payload.quad.x[1]-op->payload.quad.x[0]),
        (GLint)(op->payload.quad.y[2]-op->payload.quad.y[0])};
      if(!gml_gpu_opengl_scene_reduce_rgb(backend,target,0u) ||
         !gml_gpu_opengl_scene_reduce_rgb(backend,pending_source,1u) ||
         !gml_render_gpu_opengl_draw_textured_quad(op,&batch->states[op->state_index],
           pending_source->width,pending_source->height,pending_source->rendered_origin,
           backend->scene.vertices)){
        result.status=GML_RENDER_EXEC_RESOURCE_ERROR;
        break;
      }
      backend->gl.UseProgram(backend->scene.program);
      backend->gl.Uniform1i(backend->scene.u_recovery_screen,2);
      backend->gl.Uniform1i(backend->scene.u_recovery_application,3);
      backend->gl.Uniform4iv(backend->scene.u_recovery_rect,1,rect);
      backend->gl.ActiveTexture(GL_TEXTURE2);
      backend->gl.BindTexture(GL_TEXTURE_2D,backend->scene.rgb_reduce_texture[2]);
      backend->gl.ActiveTexture(GL_TEXTURE3);
      backend->gl.BindTexture(GL_TEXTURE_2D,backend->scene.rgb_reduce_texture[3]);
      int cutoff=batch->states[op->state_index].alpha_test_enabled
        ?(int)batch->states[op->state_index].alpha_test_reference:-1;
      if(!flush_quads(backend,target,pending_source,23u,1,1u,15u,cutoff,0u,&batch->states[op->state_index].source_palette)){
        mark_uncertain(&result,target);
        break;
      }
      result.accepted_prefix=index+1u;
      pending_count=0;
      continue;
    }
    pending_filter=scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family);
    pending_blend=qualified_blend(&batch->states[op->state_index]);
    pending_write_mask=batch->states[op->state_index].write_mask;
    pending_source_alpha_key=batch->states[op->state_index].source_alpha_key;
    pending_palette=batch->states[op->state_index].source_palette;
    pending_alpha_test=batch->states[op->state_index].alpha_test_enabled
      ?(int)batch->states[op->state_index].alpha_test_reference:-1;
    if(pending_filter==15u || pending_filter==17u || pending_filter==27u || pending_filter==29u){
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
      if(!flush_quads(backend,target,pending_source,pending_filter,pending_blend,1u,pending_write_mask,pending_alpha_test,pending_source_alpha_key,&pending_palette)){
        mark_uncertain(&result,target);
        break;
      }
      result.accepted_prefix=index+1u;
      pending_count=0;
      continue;
    }
    if(pending_blend==2 || pending_blend==4 || pending_blend==5 || pending_blend==6 ||
       pending_blend==7 || pending_blend==11){
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
scene_finish:
  if(result.status==GML_RENDER_EXEC_RESOURCE_ERROR && index==batch->operation_count){
    if(!flush_quads(backend,active_target,pending_source,pending_filter,
                    pending_blend,pending_count,pending_write_mask,pending_alpha_test,
                    pending_source_alpha_key,&pending_palette))
      mark_uncertain(&result,active_target);
    else {
      result.status=GML_RENDER_EXEC_ACCEPTED;
      result.accepted_prefix=batch->operation_count;
    }
  }
  backend->gl.ActiveTexture(GL_TEXTURE3);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
  backend->gl.ActiveTexture(GL_TEXTURE2);
  backend->gl.BindTexture(GL_TEXTURE_2D,0);
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
