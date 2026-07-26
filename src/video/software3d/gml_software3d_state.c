/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Canonical software fixed-function state mapping. */
#include "gml_software3d_internal.h"

#include <string.h>
void gml_software3d_state_get(GmlSoftware3D *graphics,
                      int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT],
                      double values[GML_SOFTWARE3D_STATE_VALUE_COUNT],
                      uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]){
  if(!graphics) return;
  GmlD3State *d3=&graphics->d3;
  flags[0]=d3->active; flags[1]=d3->hidden; flags[2]=d3->lighting;
  int v=0; for(int i=0;i<3;i++) values[v++]=d3->eye[i];
  for(int i=0;i<3;i++) values[v++]=d3->right[i];
  for(int i=0;i<3;i++) values[v++]=d3->up[i];
  for(int i=0;i<3;i++) values[v++]=d3->forward[i];
  for(int i=0;i<8;i++){
    flags[3+i*2]=d3->light[i].defined; flags[4+i*2]=d3->light[i].enabled;
    values[v++]=d3->light[i].x; values[v++]=d3->light[i].y;
    values[v++]=d3->light[i].z; values[v++]=d3->light[i].range;
    colors[i]=d3->light[i].color;
  }
  flags[19]=d3->culling; flags[20]=d3->ortho;
  values[44]=d3->ortho_x; values[45]=d3->ortho_y;
  values[46]=d3->ortho_w; values[47]=d3->ortho_h; values[48]=d3->ortho_angle;
  flags[21]=d3->zwrite; flags[22]=d3->smooth; flags[23]=d3->fog;
  flags[24]=d3->perspective; flags[25]=d3->transform_stack_n;
  values[49]=d3->fov; values[50]=d3->aspect; values[51]=d3->near_clip;
  values[52]=d3->far_clip; values[53]=d3->draw_depth;
  values[54]=d3->fog_start; values[55]=d3->fog_end;
  memcpy(values+56,d3->transform,16*sizeof(*values));
  memcpy(values+72,d3->transform_stack,512*sizeof(*values));
  memcpy(values+584,graphics->matrix_view,16*sizeof(*values));
  memcpy(values+600,graphics->matrix_projection,16*sizeof(*values));
  colors[8]=d3->fog_color;
  colors[9]=d3->ambient_color;
}
void gml_software3d_state_set(GmlSoftware3D *graphics,
                      const int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT],
                      const double values[GML_SOFTWARE3D_STATE_VALUE_COUNT],
                      const uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]){
  gml_software3d_reset(graphics);
  if(!graphics) return;
  GmlD3State *d3=&graphics->d3;
  d3->active=flags[0]; d3->hidden=flags[1]; d3->lighting=flags[2];
  int v=0; for(int i=0;i<3;i++) d3->eye[i]=values[v++];
  for(int i=0;i<3;i++) d3->right[i]=values[v++];
  for(int i=0;i<3;i++) d3->up[i]=values[v++];
  for(int i=0;i<3;i++) d3->forward[i]=values[v++];
  for(int i=0;i<8;i++){
    d3->light[i].defined=flags[3+i*2]; d3->light[i].enabled=flags[4+i*2];
    d3->light[i].x=values[v++]; d3->light[i].y=values[v++];
    d3->light[i].z=values[v++]; d3->light[i].range=values[v++];
    d3->light[i].color=colors[i];
  }
  d3->culling=flags[19]; d3->ortho=flags[20];
  d3->ortho_x=values[44]; d3->ortho_y=values[45];
  d3->ortho_w=values[46]; d3->ortho_h=values[47]; d3->ortho_angle=values[48];
  d3->zwrite=flags[21]; d3->smooth=flags[22]; d3->fog=flags[23];
  d3->perspective=flags[24];
  d3->transform_stack_n=flags[25];
  if(d3->transform_stack_n<0) d3->transform_stack_n=0;
  if(d3->transform_stack_n>32) d3->transform_stack_n=32;
  d3->fov=values[49]; d3->aspect=values[50]; d3->near_clip=values[51];
  d3->far_clip=values[52]; d3->draw_depth=values[53];
  d3->fog_start=values[54]; d3->fog_end=values[55];
  memcpy(d3->transform,values+56,16*sizeof(*values));
  memcpy(d3->transform_stack,values+72,512*sizeof(*values));
  memcpy(graphics->matrix_view,values+584,16*sizeof(*values));
  memcpy(graphics->matrix_projection,values+600,16*sizeof(*values));
  /* Schema 1 always carries explicit view and projection matrices. Accept a
   * zero-filled matrix defensively and restore the neutral identity. */
  int view_zero=1,projection_zero=1;
  for(int i=0;i<16;i++){
    if(graphics->matrix_view[i]!=0) view_zero=0;
    if(graphics->matrix_projection[i]!=0) projection_zero=0;
  }
  if(view_zero) gml_software3d_matrix_identity(graphics->matrix_view);
  if(projection_zero) gml_software3d_matrix_identity(graphics->matrix_projection);
  d3->fog_color=colors[8];
  d3->ambient_color=colors[9];
}
