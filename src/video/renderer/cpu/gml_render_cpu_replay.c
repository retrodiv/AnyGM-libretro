/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Replay of the first exactly qualified resolved scene variants. Existing CPU channel/fill
 * kernels remain the arithmetic owner; this unit borrows the checkpoint and retained source. */
#include "gml_render_cpu_replay.h"
#include "gml_render_cpu_backend.h"
#include "gml_render_cpu_blit_internal.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static int same_ref(GmlRenderImageRef a,GmlRenderImageRef b){
  return a.resource_class==b.resource_class && a.identity==b.identity &&
         a.incarnation==b.incarnation && a.content_version==b.content_version;
}

static int full_rect(GmlRenderRect rect,uint32_t width,uint32_t height){
  return rect.x==0 && rect.y==0 && rect.width==width && rect.height==height;
}

static int source_rect(GmlRenderRect rect,uint32_t width,uint32_t height){
  return rect.x>=0 && rect.y>=0 && rect.width && rect.height &&
         (uint64_t)(uint32_t)rect.x+rect.width<=width &&
         (uint64_t)(uint32_t)rect.y+rect.height<=height;
}

static int same_rect(GmlRenderRect a,GmlRenderRect b){
  return a.x==b.x && a.y==b.y && a.width==b.width && a.height==b.height;
}

static int coordinate_matches(double actual,double expected){
  return isfinite(actual) && fabs(actual-expected)<=1e-12;
}

static int source_valid(const GmlRenderResourceSource *source){
  uint64_t required;
  if(!source || !source->bytes || !source->width || !source->height ||
     source->width>INT_MAX || source->height>INT_MAX ||
     source->width>UINT32_MAX/4u || source->stride_bytes<source->width*4u ||
     (source->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
      source->pixel_format!=GML_RENDER_PIXEL_ARGB8888 &&
      source->pixel_format!=GML_RENDER_PIXEL_RGBA8888)) return 0;
  required=(uint64_t)(source->height-1u)*source->stride_bytes+(uint64_t)source->width*4u;
  return required<=source->byte_capacity;
}

static int blend_rule(const GmlRenderDrawStateSnapshot *state){
  if(!state->rgb_source_factor && !state->rgb_destination_factor &&
     !state->alpha_source_factor && !state->alpha_destination_factor &&
     !state->rgb_equation && !state->alpha_equation) return 0;
  if(state->rgb_source_factor==GML_RENDER_BLEND_SOURCE_ALPHA &&
     state->rgb_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA &&
     state->alpha_source_factor==GML_RENDER_BLEND_ONE &&
     state->alpha_destination_factor==GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA){
    if(state->rgb_equation==GML_RENDER_BLEND_EXACT8_TRUNC &&
       state->alpha_equation==GML_RENDER_BLEND_EXACT8_TRUNC) return 1;
    if(state->rgb_equation==GML_RENDER_BLEND_ADD &&
       state->alpha_equation==GML_RENDER_BLEND_ADD) return 2;
  }
  return -1;
}

int gml_render_cpu_replay_eligible(const GmlRenderDrawOp *operation,
                                    const GmlRenderDrawStateSnapshot *state,
                                    const GmlRenderResourceSource *source){
  const GmlRenderTargetRef *target;
  const GmlRenderQuad *quad;
  GmlRenderRect region;
  double u0,u1,v0,v1;
  double x0,x1,y0,y1;
  if(!operation || operation->reconstruction_mode!=GML_RENDER_REPLAY_EXACT) return 0;
  target=&operation->target;
  if(!target->width || !target->height || target->width>INT_MAX || target->height>INT_MAX ||
     (uint64_t)target->width*target->height>INT_MAX ||
     (target->pixel_format!=GML_RENDER_PIXEL_XRGB8888 &&
      target->pixel_format!=GML_RENDER_PIXEL_ARGB8888) ||
     !full_rect(target->region,target->width,target->height)) return 0;
  if(operation->opcode==GML_RENDER_OP_CLEAR)
    return operation->source_count==0u;
  if(operation->opcode!=GML_RENDER_OP_TEXTURED_QUAD || !state || !source_valid(source) ||
     operation->source_count!=1u || !operation->sources ||
     !same_ref(operation->sources[0],source->image) ||
     (operation->sources[0].resource_class==target->image.resource_class &&
      operation->sources[0].identity==target->image.identity &&
      operation->sources[0].incarnation==target->image.incarnation) ||
     state->write_mask!=15u || state->colour!=0xFFFFFFFFu || state->alpha!=255u ||
     state->alpha_test_enabled || state->coverage_policy ||
     state->material_identity || state->material_version ||
     state->sampler_count!=1u || blend_rule(state)<0 ||
     (blend_rule(state)==1 && target->pixel_format!=GML_RENDER_PIXEL_XRGB8888) ||
     !((state->clip.width==0u && state->clip.height==0u) ||
       full_rect(state->clip,target->width,target->height))) return 0;
  quad=&operation->payload.quad;
  region=quad->sampling.source_region;
  if(quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED ||
     quad->sampling.filter!=GML_RENDER_FILTER_NEAREST ||
     quad->sampling.edge_rule!=GML_RENDER_EDGE_CLAMP_SUBIMAGE ||
     quad->sampling.quantisation || quad->sampling.logical_margin_mask ||
     !source_rect(region,source->width,source->height) ||
     state->samplers[0].filter!=quad->sampling.filter ||
     state->samplers[0].edge_rule!=quad->sampling.edge_rule ||
     state->samplers[0].logical_margin_mask!=quad->sampling.logical_margin_mask ||
     !same_rect(state->samplers[0].source_region,region)) return 0;
  u0=(double)region.x/source->width;
  u1=(double)((uint32_t)region.x+region.width)/source->width;
  v0=(double)region.y/source->height;
  v1=(double)((uint32_t)region.y+region.height)/source->height;
  x0=quad->x[0];x1=quad->x[1];y0=quad->y[0];y1=quad->y[2];
  if(!isfinite(x0) || !isfinite(x1) || !isfinite(y0) || !isfinite(y1) ||
     x0<0.0 || y0<0.0 || x1>(double)target->width || y1>(double)target->height ||
     x0!=(double)(int)x0 || x1!=(double)(int)x1 ||
     y0!=(double)(int)y0 || y1!=(double)(int)y1 ||
     x1<=x0 || y1<=y0 ||
     x1-x0!=(double)(uint32_t)(x1-x0) ||
     y1-y0!=(double)(uint32_t)(y1-y0) ||
     (uint32_t)(x1-x0)<region.width || (uint32_t)(y1-y0)<region.height ||
     (uint32_t)(x1-x0)%region.width || (uint32_t)(y1-y0)%region.height ||
     quad->x[3]!=x0 || quad->x[2]!=x1 || quad->y[1]!=y0 || quad->y[3]!=y1 ||
     !coordinate_matches(quad->u[0],quad->u[3]) ||
     !coordinate_matches(quad->u[1],quad->u[2]) ||
     !coordinate_matches(quad->v[0],quad->v[1]) ||
     !coordinate_matches(quad->v[2],quad->v[3]) ||
     !((coordinate_matches(quad->u[0],u0) && coordinate_matches(quad->u[1],u1)) ||
       (coordinate_matches(quad->u[0],u1) && coordinate_matches(quad->u[1],u0))) ||
     !((coordinate_matches(quad->v[0],v0) && coordinate_matches(quad->v[2],v1)) ||
       (coordinate_matches(quad->v[0],v1) && coordinate_matches(quad->v[2],v0))))
    return 0;
  for(uint32_t corner=0;corner<4u;corner++)
    if(quad->corner_colour[corner]!=0xFFFFFFFFu) return 0;
  if(blend_rule(state)==2 && source->pixel_format!=GML_RENDER_PIXEL_XRGB8888)
    for(uint32_t y=0;y<region.height;y++) for(uint32_t x=0;x<region.width;x++){
      const uint8_t *bytes=(const uint8_t*)source->bytes+
        (size_t)((uint32_t)region.y+y)*source->stride_bytes+
        (size_t)((uint32_t)region.x+x)*4u;
      uint32_t packed;
      uint8_t coverage;
      memcpy(&packed,bytes,sizeof packed);
      coverage=source->pixel_format==GML_RENDER_PIXEL_RGBA8888
        ?bytes[3]:(uint8_t)(packed>>24);
      if(coverage!=0u && coverage!=255u) return 0;
    }
  return 1;
}

static uint32_t source_pixel(const GmlRenderResourceSource *source,uint32_t x,uint32_t y){
  const uint8_t *bytes=(const uint8_t*)source->bytes+(size_t)y*source->stride_bytes+(size_t)x*4u;
  uint32_t pixel;
  if(source->pixel_format==GML_RENDER_PIXEL_RGBA8888)
    return ((uint32_t)bytes[3]<<24)|((uint32_t)bytes[0]<<16)|
           ((uint32_t)bytes[1]<<8)|bytes[2];
  memcpy(&pixel,bytes,sizeof pixel);
  if(source->pixel_format==GML_RENDER_PIXEL_XRGB8888) pixel|=0xFF000000u;
  return pixel;
}

int gml_render_cpu_replay_one(uint32_t *target,uint32_t width,uint32_t height,
                               const GmlRenderDrawOp *operation,
                               const GmlRenderDrawStateSnapshot *state,
                               const GmlRenderResourceSource *source){
  if(!target || !operation || operation->target.width!=width ||
     operation->target.height!=height ||
     !gml_render_cpu_replay_eligible(operation,state,source)) return 0;
  if(operation->opcode==GML_RENDER_OP_CLEAR){
    gml_render_cpu_backend_fill_xrgb(target,(int)((size_t)width*height),
      operation->target.pixel_format==GML_RENDER_PIXEL_XRGB8888
        ?operation->payload.clear_colour|0xFF000000u:operation->payload.clear_colour);
    return 1;
  }
  {
    const GmlRenderQuad *quad=&operation->payload.quad;
    GmlRenderRect region=quad->sampling.source_region;
    uint32_t left=(uint32_t)quad->x[0],top=(uint32_t)quad->y[0];
    uint32_t draw_width=(uint32_t)(quad->x[1]-quad->x[0]);
    uint32_t draw_height=(uint32_t)(quad->y[2]-quad->y[0]);
    uint32_t scale_x=draw_width/region.width,scale_y=draw_height/region.height;
    int mirror_x=quad->u[0]>quad->u[1],mirror_y=quad->v[0]>quad->v[2];
    int exact=blend_rule(state)==1;
    int binary=blend_rule(state)==2;
    for(uint32_t y=0;y<draw_height;y++) for(uint32_t x=0;x<draw_width;x++){
      uint32_t sample_x=x/scale_x,sample_y=y/scale_y;
      uint32_t source_x=(uint32_t)region.x+
        (mirror_x?region.width-1u-sample_x:sample_x);
      uint32_t source_y=(uint32_t)region.y+
        (mirror_y?region.height-1u-sample_y:sample_y);
      uint32_t pixel=source_pixel(source,source_x,source_y);
      uint32_t *destination=&target[(size_t)(top+y)*width+left+x];
      if(exact){
        uint32_t factor=pixel>>24;
        uint32_t inverse;
        if(factor==255u) factor=256u;
        inverse=256u-factor;
        *destination=blend_fast8_cached(*destination,
          (pixel&0x00FF00FFu)*factor,(pixel&0x0000FF00u)*factor,inverse,0);
      } else if(!binary || (pixel>>24)==255u)
        *destination=operation->target.pixel_format==GML_RENDER_PIXEL_XRGB8888
        ?pixel|0xFF000000u:pixel;
    }
  }
  return 1;
}
