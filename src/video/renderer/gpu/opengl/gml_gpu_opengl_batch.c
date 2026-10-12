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

enum { GL_SCENE_MAX_EXACT_REPETITIONS=64, GL_SCENE_BLEND_INVERSE_RECT=12,
       GL_SCENE_BLEND_RECTANGLE_BASE=32, GL_SCENE_BLEND_RECTANGLE_LIMIT=55 };

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
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
     quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND){
    uint32_t base=quad->sampling.quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND?38u:37u;
    if(state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA) return base+12u;
    if(qualified_blend(state)==13) return base+7u+
      (state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?2u:0u);
    return !qualified_blend(state)?base+5u:
      state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA?base+2u:base;
  }
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST) return 27u;
  if(quad->sampling.quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_ROTATED_NEAREST) return 29u;
  if(quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT) return 30u;
  if((family==GML_RENDER_SCENE_SURFACE ||
      ((family==GML_RENDER_SCENE_SPRITE || family==GML_RENDER_SCENE_TEXT) &&
       quad->shape==GML_RENDER_QUAD_AXIS_ALIGNED)) &&
     quad->sampling.filter==GML_RENDER_FILTER_NEAREST &&
     state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA) return 25u;
  if(quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR)
    return state->coverage_policy==GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA
      ?21u:quad->sampling.quantisation==GML_RENDER_QUANTISE_HARDWARE_COMBINED
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

/* R03-SPR-INTEGER-FUNCTION: only whole physical atlas texels and uniform RGB
 * reach this cheap integer tint/fixed-blend packet. Clipping retains its UVs. */
static int integer_function_sprite_valid(const GmlRenderDrawOp *op,
                                          const GmlRenderDrawStateSnapshot *state,
                                          const GlSceneImage *source){
  const GmlRenderQuad *q=&op->payload.quad;
  GmlRenderRect region=q->sampling.source_region;
  int blend=qualified_blend(state);
  if(!source || (op->scene_family!=GML_RENDER_SCENE_SPRITE &&
                op->scene_family!=GML_RENDER_SCENE_BACKGROUND &&
                op->scene_family!=GML_RENDER_SCENE_TILE &&
                op->scene_family!=GML_RENDER_SCENE_TEXT) ||
     (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION &&
      op->target.image.resource_class!=GML_RENDER_RESOURCE_SURFACE) ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS || source->rendered_origin ||
     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 || source->width>16384u || source->height>16384u ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT || q->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     q->span_active || q->source_endpoint_rgb_mask || q->sampling.filter!=GML_RENDER_FILTER_NEAREST ||
     (q->sampling.quantisation!=GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC &&
      q->sampling.quantisation!=GML_RENDER_QUANTISE_INTEGER_TINT_ROUND) ||
     q->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE || q->sampling.logical_margin_mask ||
     !valid_region(region,source->width,source->height) || region.width>4096u || region.height>4096u ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1) || fabs(q->x[0])>65536.0 || fabs(q->y[0])>65536.0 ||
     !isfinite(q->filtered_draw_alpha) || q->filtered_draw_alpha<=0.0 || q->filtered_draw_alpha>1.0 ||
     state->alpha_test_enabled>1u || state->alpha_test_reference>255u ||
     (blend!=0 && blend!=1 && blend!=3 && blend!=13) ||
     (state->coverage_policy==GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA?blend!=0:
      state->coverage_policy && (state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
       (blend!=1 && blend!=13) || (op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN &&
         op->target.image.resource_class!=GML_RENDER_RESOURCE_APPLICATION)))) return 0;
  double width=q->x[1]-q->x[0],height=q->y[2]-q->y[0];
  if(width>16384.0 || height>16384.0) return 0;
  double u0=(double)region.x/source->width,u1=((double)region.x+region.width)/source->width;
  double v0=(double)region.y/source->height,v1=((double)region.y+region.height)/source->height;
  /* The shared point producer can clip a phased whole-scale interval or
   * fence a boundary with a constant texel-centre strip. Its geometry stays
   * integral and separable; hardware still samples nearest at whole scales. */
  for(unsigned axis=0;axis<2u;axis++){
    const double *uv=axis?q->v:q->u;
    double begin=uv[0],end=uv[axis?2u:1u],extent=axis?height:width;
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
      if(!isfinite(scale) || scale<(background?0.0625:1.0)-1e-9 ||
         scale>(background?64.0:8.0)+1e-9 ||
         (!background && fabs(scale-nearbyint(scale))>1e-9)) return 0;
    }
  }
  for(unsigned c=0;c<4u;c++){
    double u=(c==1u || c==2u)?q->u[1]:q->u[0],v=c>=2u?q->v[2]:q->v[0];
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

static int hardware_surface_quad_valid(const GmlRenderDrawOp *op,
                                       const GmlRenderDrawStateSnapshot *state,
                                       const GlSceneImage *source){
  const GmlRenderQuad *quad=&op->payload.quad;
  int destination_blend=qualified_blend(state)==11;
  int point=quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT;
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
      op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION) ||
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
     quad->sampling.quantisation || quad->sampling.logical_margin_mask || quad->span_active ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     !full_region(quad->sampling.source_region,source->width,source->height) ||
     (destination_blend?state->coverage_policy>GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
                        quad->sampling.filter==GML_RENDER_FILTER_HARDWARE_BILINEAR:
                        state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
                        qualified_blend(state)!=1) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1) ||
     (!point && !destination_blend && !integer_screen && (quad->sampling.filter==GML_RENDER_FILTER_NEAREST
       ?quad->x[1]-quad->x[0]!=(double)source->width ||
        quad->y[2]-quad->y[0]!=(double)source->height
       :(op->sources[0].resource_class!=GML_RENDER_RESOURCE_SURFACE &&
         op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION) ||
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
    if(destination_blend || op->sources[0].resource_class!=GML_RENDER_RESOURCE_APPLICATION ||
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
  for(unsigned corner=0;corner<4u;corner++)
    if(quad->corner_colour[corner]!=quad->corner_colour[0] ||
       quad->u[corner]!=(point?(quad->x[corner]-quad->inverse_anchor[0])/quad->inverse_x[0]:
                               ((corner==1u || corner==2u)?1.0:0.0)) ||
       quad->v[corner]!=(point?(quad->y[corner]-quad->inverse_anchor[1])/quad->inverse_y[1]:
                               (corner>=2u?1.0:0.0))) return 0;
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
     op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
     qualified_blend(state)!=1 || quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     quad->sampling.quantisation || quad->sampling.logical_margin_mask || quad->span_active ||
     !full_region(quad->sampling.source_region,1u,1u) ||
     quad->filtered_draw_alpha!=1.0 ||
     !exact_rect(op,NULL,NULL,NULL,NULL,0)) return 0;
  for(unsigned corner=0;corner<4u;corner++)
    if(quad->corner_colour[corner]!=0xFFFFFFFFu ||
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

/* Ordinary filtered SCREEN packets retain authored fractional phase in UVs.
 * This bounded rule does not admit trim, source mutations or exact recipes. */
static int hardware_screen_sprite_valid(const GmlRenderDrawOp *op,
                                       const GmlRenderDrawStateSnapshot *state,
                                       const GlSceneImage *source){
  const GmlRenderQuad *quad=&op->payload.quad;
  GmlRenderRect region=quad->sampling.source_region;
  double xs=quad->inverse_x[0],ys=quad->inverse_y[1];
  double left=quad->inverse_anchor[0],top=quad->inverse_anchor[1];
  uint32_t margin=quad->sampling.logical_margin_mask;
  if(!source || source->rendered_origin || source->pixel_format!=GML_RENDER_PIXEL_RGBA8888 ||
     op->sources[0].resource_class!=GML_RENDER_RESOURCE_ATLAS ||
     (op->scene_family!=GML_RENDER_SCENE_SPRITE && op->scene_family!=GML_RENDER_SCENE_TEXT) ||
     op->target.image.resource_class!=GML_RENDER_RESOURCE_SCREEN ||
     op->target.pixel_format!=GML_RENDER_PIXEL_ARGB8888 ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     qualified_blend(state)!=1 || state->coverage_policy!=GML_RENDER_COVERAGE_OPAQUE_WRITTEN_ALPHA ||
     quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     quad->sampling.filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR ||
     (margin
       ? op->scene_family!=GML_RENDER_SCENE_SPRITE || margin>15u ||
         quad->sampling.edge_rule!=GML_RENDER_EDGE_LOGICAL_TPAG ||
         ((margin&GML_RENDER_MARGIN_LEFT) && region.x<=0) ||
         ((margin&GML_RENDER_MARGIN_TOP) && region.y<=0) ||
         ((margin&GML_RENDER_MARGIN_RIGHT) && (uint64_t)(uint32_t)region.x+region.width>=source->width) ||
         ((margin&GML_RENDER_MARGIN_BOTTOM) && (uint64_t)(uint32_t)region.y+region.height>=source->height)
       : quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE) ||
     quad->sampling.quantisation || quad->span_active ||
     !isfinite(quad->filtered_draw_alpha) || quad->filtered_draw_alpha<=0.0 ||
     quad->filtered_draw_alpha>1.0 ||
     !isfinite(xs) || !isfinite(ys) || xs<=0.0 || ys<=0.0 ||
     (op->scene_family==GML_RENDER_SCENE_TEXT &&
      (xs>1.0 || ys>1.0 || region.width>256u || region.height>256u)) ||
     !isfinite(left) || !isfinite(top) ||
     !valid_region(region,source->width,source->height) ||
     !exact_rect(op,NULL,NULL,NULL,NULL,1)) return 0;
  for(unsigned corner=0;corner<4u;corner++){
    double u=((double)region.x+(quad->x[corner]-left)/xs)/source->width;
    double v=((double)region.y+(quad->y[corner]-top)/ys)/source->height;
    if(quad->corner_colour[corner]!=quad->corner_colour[0] ||
       (quad->corner_colour[corner]>>24)!=(uint32_t)lround(quad->filtered_draw_alpha*255.0) ||
       fabs(quad->u[corner]-u)>1e-12 || fabs(quad->v[corner]-v)>1e-12) return 0;
  }
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
     state->alpha_test_enabled || state->material_identity || state->material_version ||
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
      if(op->payload.primitive.kind==GML_RENDER_PRIMITIVE_PARTICLE_LINE ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_RECTANGLE ||
         op->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT){
        if((!gml_render_particle_line_valid(op) && !gml_render_classic_alpha_rect_valid(op) &&
            !gml_render_studio_alpha_rect_valid(op) &&
            !gml_render_rectangle_valid(op) &&
            !gml_render_classic_inverse_rect_valid(op)) ||
           (op->payload.primitive.alpha_blend &&
            (!backend->gl.BlendFuncSeparate || !backend->gl.BlendEquationSeparate)) ||
           (!existing_target(backend,target) &&
            !cleared_earlier(cleared,clear_count,target->image))) return 0;
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
      int hardware_surface=hardware_surface_quad_valid(op,state,source);
      int hardware_veil=hardware_screen_veil_valid(op,state,source);
      int hardware_stretch=hardware_screen_stretch_valid(op,state,source);
      int hardware_sprite=hardware_screen_sprite_valid(op,state,source);
      int integer_sourceover=integer_sourceover_sprite_valid(op,state,source) &&
        (blend==1 || blend==3);
      int integer_function=integer_function_sprite_valid(op,state,source);
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
      if(!source || (source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
                     source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
                     source->pixel_format!=GML_RENDER_PIXEL_RGBA8888) ||
         same_logical(op->sources[0],target->image) ||
         (state->write_mask!=15u && !masked_numbered_sprite && !modern_numbered_sprite) ||
         (sampling->quantisation==GML_RENDER_QUANTISE_SPRITE_MODERN_NEAREST &&
          !modern_numbered_sprite) || state->alpha!=255u ||
         ((sampling->quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_TRUNC ||
           sampling->quantisation==GML_RENDER_QUANTISE_INTEGER_TINT_ROUND) && !integer_function) ||
         state->colour!=0xFFFFFFFFu || (state->alpha_test_enabled && !integer_function) ||
         blend<0 || (blend==13 && !integer_function) ||
         (blend==11 && (!hardware_surface || !backend->gl.CopyTexSubImage2D)) ||
         ((integer_function || integer_sourceover || blend==1 || blend==9 || blend==10 ||
                       (blend==3 && sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR)) &&
                      (!backend->gl.BlendFuncSeparate ||
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
         ((blend==10 || (blend==3 && sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR)) &&
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
         (sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR && !hardware_surface && !hardware_veil && !hardware_stretch && !hardware_sprite &&
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
         (state->coverage_policy && !integer_function && !hardware_surface && !hardware_veil && !hardware_stretch && !hardware_sprite && !integer_sourceover &&
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
          sampling->filter!=GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT &&
          sampling->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR) ||
         (sampling->filter==GML_RENDER_FILTER_HARDWARE_PIXEL_CENTRE_POINT && !hardware_surface) ||
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
         (sampling->quantisation && !integer_function && blend!=6 && blend!=7 && !modern_numbered_sprite &&
          !(blend==3 && sampling->filter==GML_RENDER_FILTER_HARDWARE_BILINEAR &&
            sampling->quantisation==GML_RENDER_QUANTISE_HARDWARE_COMBINED) &&
          (sampling->quantisation!=GML_RENDER_QUANTISE_STRETCHED_FLOAT ||
           sampling->filter!=GML_RENDER_FILTER_BILINEAR ||
           op->scene_family!=GML_RENDER_SCENE_SPRITE || (blend!=0 && blend!=8) ||
           op->payload.quad.shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
           (sampling->edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE &&
            !(blend==8 && sampling->edge_rule==GML_RENDER_EDGE_TRANSPARENT)) ||
           source->rendered_origin)) ||
         (blend!=7 && blend!=11 && (sampling->filter==GML_RENDER_FILTER_BOX
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
        if(modern_numbered_sprite && (colour!=op->payload.quad.corner_colour[0] ||
           (colour>>24)!=(uint32_t)lround(op->payload.quad.filtered_draw_alpha*255.0))) return 0;
        if(blend==8 && colour!=op->payload.quad.corner_colour[0]) return 0;
        if(state->coverage_policy && !integer_sourceover &&
           (colour>>24)!=(uint32_t)lround(op->payload.quad.filtered_draw_alpha*255.0))
          return 0;
        if(colour!=0xFFFFFFFFu && blend!=6 && !integer_function &&
           (op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
            (sampling->filter!=GML_RENDER_FILTER_BILINEAR &&
             sampling->filter!=GML_RENDER_FILTER_BOX &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_COPY &&
             sampling->filter!=GML_RENDER_FILTER_NEAREST_PHASED_OPAQUE_ALPHA &&
             sampling->filter!=GML_RENDER_FILTER_HARDWARE_BILINEAR &&
             op->payload.quad.shape==GML_RENDER_QUAD_AXIS_ALIGNED &&
             !hardware_surface && !integer_sourceover && !integer_endpoint &&
             !(op->scene_family==GML_RENDER_SCENE_TEXT &&
               sampling->filter==GML_RENDER_FILTER_NEAREST)))) return 0;
        if((colour>>24)!=255u && !integer_function &&
           blend!=1 && blend!=3 && blend!=9 && blend!=10 && blend!=11) return 0;
        if((blend==4 || blend==5 ||
            (sampling->quantisation && blend!=6 && blend!=8 && !modern_numbered_sprite && !integer_function)) &&
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

static int flush_quads(GmlGpuBackend *backend,GlSceneImage *target,GlSceneImage *source,
                       uint32_t filter,int blend,uint32_t count,uint32_t write_mask,int alpha_test){
  GlScene *scene=&backend->scene;
  GLfloat extent[2]={(GLfloat)target->width,(GLfloat)target->height};
  GLenum texture_filter=filter==18u || filter==19u || filter==20u || filter==21u
    ?GL_LINEAR:GL_NEAREST;
  GLsizei stride=GL_SCENE_VERTEX_FLOATS*(GLsizei)sizeof(float);
  if(!count) return 1;
  backend->gl.BindFramebuffer(GL_FRAMEBUFFER,target->framebuffer);
  backend->gl.Viewport(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.Scissor(0,0,(GLsizei)target->width,(GLsizei)target->height);
  backend->gl.ActiveTexture(GL_TEXTURE0);
  backend->gl.BindTexture(GL_TEXTURE_2D,source?source->texture:0u);
  if(source){
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,(GLint)texture_filter);
    backend->gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,(GLint)texture_filter);
  }
  if(blend && blend!=8 && filter!=15u && filter!=17u){
    if(blend>=GL_SCENE_BLEND_RECTANGLE_BASE && blend<=GL_SCENE_BLEND_RECTANGLE_LIMIT){
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
    } else if(blend==13)
      backend->gl.BlendFuncSeparate(GL_ZERO,GL_ONE_MINUS_SRC_COLOR,GL_ZERO,GL_ONE_MINUS_SRC_ALPHA);
    else if(blend==GL_SCENE_BLEND_INVERSE_RECT)
      backend->gl.BlendFuncSeparate(GL_ZERO,GL_ONE_MINUS_SRC_COLOR,GL_ONE,GL_ZERO);
    else backend->gl.BlendFuncSeparate(blend==9 || blend==10?GL_ONE:GL_SRC_ALPHA,
                                  blend==9?GL_ONE:GL_ONE_MINUS_SRC_ALPHA,
                                  blend==3 || blend==9 || blend==10?GL_SRC_ALPHA:GL_ONE,
                                  GL_ONE_MINUS_SRC_ALPHA);
    backend->gl.BlendEquationSeparate(GL_FUNC_ADD,GL_FUNC_ADD);
    backend->gl.Enable(GL_BLEND);
  } else backend->gl.Disable(GL_BLEND);
  int stamp_alpha=filter==9u || filter==12u || filter==21u || filter==23u ||
                  filter==25u || filter==32u || filter==35u || filter==39u || filter==40u ||
                  filter==46u || filter==47u;
  backend->gl.ColorMask((write_mask&1u)!=0u,(write_mask&2u)!=0u,(write_mask&4u)!=0u,
                        !stamp_alpha && (write_mask&8u)!=0u);
  backend->gl.UseProgram(scene->program);
  backend->gl.Uniform2fv(scene->u_extent,1,extent);
  backend->gl.Uniform1i(scene->u_image,0);
  backend->gl.Uniform1i(scene->u_filter,(GLint)filter);
  backend->gl.Uniform1i(scene->u_alpha_test,alpha_test);
  backend->gl.Uniform1i(scene->u_previous,1);
  /* Integer map samplers must never alias the floating image samplers, even
   * for a draw whose dynamic branch does not fetch either point map. */
  backend->gl.Uniform1i(scene->u_point_map_x,2);
  backend->gl.Uniform1i(scene->u_point_map_y,3);
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
  if(stamp_alpha && (write_mask&8u)){
    /* The CPU screen writer keeps alpha opaque at every covered texel,
     * even when its prior alpha is unknown. Reuse the sampled vertices and
     * the same sampler and cutout in an alpha-only pass, preserving that recipe's coverage. */
    backend->gl.ColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_TRUE);
    backend->gl.Disable(GL_BLEND);
    backend->gl.Uniform1i(scene->u_filter,(filter==46u || filter==47u)?48:(filter==39u || filter==40u)?41:filter==35u?36:filter==32u?33:filter==25u?26:
                         filter==23u?24:filter==21u?22:filter==9u?10:13);
    backend->gl.DrawArrays(GL_TRIANGLES,0,(GLsizei)(count*6u));
  }
  backend->gl.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
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
  if(!flush_quads(backend,target,source,texel?29u:modern?27u:15u,0,count,base_state->write_mask,-1)) return -1;
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
                 qualified_blend(state)==7 || qualified_blend(state)==11) ||
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
  for(uint32_t index=0;index<batch->operation_count;index++){
    const GmlRenderDrawOp *op=&batch->operations[index];
    GlSceneImage *target;
    int particle=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_PARTICLE_LINE;
    int inverse_rect=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_INVERSE_RECT;
    int studio_alpha_rect=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_STUDIO_ALPHA_RECT;
    int rectangle=op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_RECTANGLE;
    unsigned rectangle_blend=rectangle?
      (op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_BLEND_MASK)>>
       GML_RENDER_RECTANGLE_BLEND_SHIFT:0u;
    int alpha_rect=inverse_rect || studio_alpha_rect || (op->opcode==GML_RENDER_OP_COLOURED_PRIMITIVE &&
      op->payload.primitive.kind==GML_RENDER_PRIMITIVE_CLASSIC_ALPHA_RECT);
    uint32_t primitive_filter=rectangle?
      (op->payload.primitive.alpha_blend && op->payload.primitive.alpha<1.0 &&
       (rectangle_blend==0u || rectangle_blend==1u || rectangle_blend==4u) &&
       !(op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_TARGET_ALPHA)?35u:34u):
      particle?28u:studio_alpha_rect?32u:31u;
    int primitive_blend=rectangle?
      (op->payload.primitive.alpha_blend?
        (rectangle_blend?(int)(GL_SCENE_BLEND_RECTANGLE_BASE+2u*rectangle_blend+
           ((op->payload.primitive.rectangle_flags&GML_RENDER_RECTANGLE_TARGET_ALPHA)!=0u)):
         op->payload.primitive.alpha<1.0?3:0):0):
      inverse_rect?GL_SCENE_BLEND_INVERSE_RECT:
      (particle || alpha_rect?(int)op->payload.primitive.alpha_blend:0);
    if(pending_count && (particle || alpha_rect || rectangle
       ? !active_target || !same_logical(active_target->ref,op->target.image) ||
         pending_count==GL_SCENE_MAX_QUADS || pending_filter!=primitive_filter ||
         pending_blend!=primitive_blend || pending_alpha_test!=-1
       : (op->opcode!=GML_RENDER_OP_TEXTURED_QUAD ||
       !active_target || !same_logical(active_target->ref,op->target.image) ||
       pending_count==GL_SCENE_MAX_QUADS ||
       !pending_source || !same_version(pending_source->ref,op->sources[0]) ||
       pending_filter!=scene_filter(&op->payload.quad,&batch->states[op->state_index],op->scene_family) ||
       pending_blend!=qualified_blend(&batch->states[op->state_index]) ||
       pending_write_mask!=batch->states[op->state_index].write_mask ||
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
                      pending_blend,pending_count,pending_write_mask,pending_alpha_test)){
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
      if(particle || alpha_rect || rectangle){
        const GmlRenderPrimitiveRect *p=&op->payload.primitive;
        static const unsigned corners[6]={0,1,2,0,2,3};
        double rad=p->angle*3.14159265358979323846/180.0;
        double co=cos(rad),si=sin(rad);
        for(unsigned vertex=0;vertex<6u;vertex++){
          unsigned corner=corners[vertex];
          float *v=backend->scene.vertices+
            ((size_t)pending_count*6u+vertex)*GL_SCENE_VERTEX_FLOATS;
          memset(v,0,GL_SCENE_VERTEX_FLOATS*sizeof *v);
          v[0]=(float)p->region.x+((corner==1u || corner==2u)?p->region.width:0u);
          v[1]=(float)p->region.y+(corner>=2u?p->region.height:0u);
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
            v[27]=(float)p->alpha_blend;
          }
        }
        pending_source=NULL; pending_filter=primitive_filter; pending_blend=primitive_blend;
        pending_alpha_test=-1;
        if(alpha_rect || rectangle) pending_write_mask=15u;
        pending_count++;
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
      if(!flush_quads(backend,target,pending_source,23u,1,1u,15u,-1)){
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
      if(!flush_quads(backend,target,pending_source,pending_filter,pending_blend,1u,pending_write_mask,pending_alpha_test)){
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
  if(result.status==GML_RENDER_EXEC_RESOURCE_ERROR &&
     result.accepted_prefix+pending_count==batch->operation_count){
    if(!flush_quads(backend,active_target,pending_source,pending_filter,
                    pending_blend,pending_count,pending_write_mask,pending_alpha_test))
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
