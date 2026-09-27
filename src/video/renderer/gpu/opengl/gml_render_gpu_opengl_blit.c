/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* One prepared quad becomes six ordered vertices in a bounded batch; no mutable renderer or VM
 * state is consulted here. The batch owner issues a single draw for compatible consecutive quads. */
#include "gml_gpu_opengl_internal.h"

#include <math.h>

int gml_render_gpu_opengl_draw_textured_quad(const GmlRenderDrawOp *op,
                                             const GmlRenderDrawStateSnapshot *state,
                                             uint32_t source_width,uint32_t source_height,
                                             int rendered_source_origin,
                                             float *vertices){
  static const uint8_t corners[6]={0,1,2,0,2,3};
  const GmlRenderQuad *quad;
  GmlRenderRect region;
  uint32_t region_storage_y;
  int rotated;
  if(!op || !state || !vertices || !source_width || !source_height) return 0;
  quad=&op->payload.quad;
  rotated=quad->shape==GML_RENDER_QUAD_CARDINAL || quad->shape==GML_RENDER_QUAD_GENERAL;
  if(quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED && !rotated) return 0;
  if(rotated && (rendered_source_origin ||
     op->reconstruction_mode!=GML_RENDER_DEVICE_CHECKPOINT ||
     (quad->sampling.filter!=GML_RENDER_FILTER_BILINEAR &&
      quad->sampling.filter!=GML_RENDER_FILTER_NEAREST))) return 0;
  region=quad->sampling.source_region;
  if(region.x<0 || region.y<0 || !region.width || !region.height ||
     (uint64_t)(uint32_t)region.x+region.width>source_width ||
     (uint64_t)(uint32_t)region.y+region.height>source_height) return 0;
  region_storage_y=rendered_source_origin
    ?source_height-((uint32_t)region.y+region.height):(uint32_t)region.y;
  if(quad->x[1]<=quad->x[0] || quad->y[2]<=quad->y[0]) return 0;
  for(uint32_t index=0;index<6;index++){
    uint32_t corner=corners[index];
    uint32_t colour=quad->corner_colour[corner];
    float *vertex=vertices+(size_t)index*GL_SCENE_VERTEX_FLOATS;
    if(!isfinite(quad->x[corner]) || !isfinite(quad->y[corner]) ||
       (!rotated && (!isfinite(quad->u[corner]) || !isfinite(quad->v[corner]))) ||
       quad->x[corner]<-10000000.0 || quad->x[corner]>10000000.0 ||
       quad->y[corner]<-10000000.0 || quad->y[corner]>10000000.0 ||
       (!rotated && (quad->u[corner]<0.0 || quad->u[corner]>1.0 ||
                    quad->v[corner]<0.0 || quad->v[corner]>1.0))) return 0;
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
    vertex[8]=(float)region.x;
    vertex[9]=(float)region_storage_y;
    vertex[10]=(float)((uint32_t)region.x+region.width-1u);
    vertex[11]=(float)(region_storage_y+region.height-1u);
    vertex[12]=rotated?(float)quad->inverse_anchor[0]:(float)quad->x[0];
    vertex[13]=rotated?(float)quad->inverse_anchor[1]:(float)quad->y[0];
    vertex[14]=rotated?(float)quad->inverse_x[0]
                      :(float)(quad->u[0]*source_width-region.x);
    vertex[15]=rotated?(float)quad->inverse_x[1]
                      :(float)((rendered_source_origin?1.0-quad->v[0]:quad->v[0])*
                                source_height-region_storage_y);
    vertex[16]=rotated?(float)quad->inverse_offset[0]
                      :(float)((quad->u[1]-quad->u[0])*source_width/
                               (quad->x[1]-quad->x[0])-1.0);
    vertex[17]=rotated?(float)quad->inverse_offset[1]
                      :(float)((rendered_source_origin?quad->v[0]-quad->v[2]
                                :quad->v[2]-quad->v[0])*source_height/
                               (quad->y[2]-quad->y[0])-1.0);
    vertex[18]=(float)quad->sampling.logical_margin_mask;
    for(uint32_t span=0;span<4;span++){
      vertex[19+span]=(float)quad->span_x[span];
      vertex[23+span]=(float)quad->span_y[span];
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
      if(quad->sampling.filter!=GML_RENDER_FILTER_BILINEAR) return 0;
      for(uint32_t corner=0;corner<4;corner++)
        if(!isfinite(quad->span_x[corner]) || !isfinite(quad->span_y[corner]) ||
           fabs(quad->span_x[corner])>16384.0 ||
           fabs(quad->span_y[corner])>16384.0) return 0;
    }
  }
  return 1;
}
