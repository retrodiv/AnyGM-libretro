/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* One prepared quad becomes six ordered vertices in a bounded batch; no mutable renderer or VM
 * state is consulted here. The batch owner issues a single draw for compatible consecutive quads. */
#include "gml_gpu_opengl_internal.h"

#include <math.h>

int gml_render_gpu_opengl_draw_textured_quad(const GmlRenderDrawOp *op,
                                             const GmlRenderDrawStateSnapshot *state,
                                             float *vertices){
  static const uint8_t corners[6]={0,1,2,0,2,3};
  const GmlRenderQuad *quad;
  if(!op || !state || !vertices) return 0;
  quad=&op->payload.quad;
  if(quad->shape!=GML_RENDER_QUAD_AXIS_ALIGNED) return 0;
  for(uint32_t index=0;index<6;index++){
    uint32_t corner=corners[index];
    uint32_t colour=quad->corner_colour[corner];
    float *vertex=vertices+(size_t)index*GL_SCENE_VERTEX_FLOATS;
    if(!isfinite(quad->x[corner]) || !isfinite(quad->y[corner]) ||
       !isfinite(quad->u[corner]) || !isfinite(quad->v[corner]) ||
       quad->x[corner]<-10000000.0 || quad->x[corner]>10000000.0 ||
       quad->y[corner]<-10000000.0 || quad->y[corner]>10000000.0 ||
       quad->u[corner]<0.0 || quad->u[corner]>1.0 ||
       quad->v[corner]<0.0 || quad->v[corner]>1.0) return 0;
    vertex[0]=(float)quad->x[corner];
    vertex[1]=(float)quad->y[corner];
    vertex[2]=(float)quad->u[corner];
    vertex[3]=(float)quad->v[corner];
    vertex[4]=(float)((colour>>16)&255u)/255.0f;
    vertex[5]=(float)((colour>>8)&255u)/255.0f;
    vertex[6]=(float)(colour&255u)/255.0f;
    vertex[7]=(float)((colour>>24)&255u)/255.0f*(float)state->alpha/255.0f;
  }
  return 1;
}
