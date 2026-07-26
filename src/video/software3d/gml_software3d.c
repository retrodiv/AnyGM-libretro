/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Software fixed-function pipeline and context lifecycle. */
#include "gml_software3d_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void vertex_partial_init(GmlVertexBuffer *buffer){
  memset(&buffer->partial,0,sizeof(buffer->partial));
  buffer->partial.r=buffer->partial.g=buffer->partial.b=255;
  buffer->partial.alpha=1;
  buffer->attr_index=0;
}
static void vertex_resources_reset(GmlSoftware3D *graphics){
  if(!graphics) return;
  for(int i=0;i<GML_VERTEX_BUFFER_MAX;i++){
    free(graphics->vertex_buffer[i].vertex);
    memset(&graphics->vertex_buffer[i],0,sizeof(graphics->vertex_buffer[i]));
    graphics->vertex_buffer[i].format=-1;
  }
  memset(graphics->vertex_format,0,sizeof(graphics->vertex_format));
  memset(&graphics->vertex_builder,0,sizeof(graphics->vertex_builder));
  graphics->vertex_builder_active=0;
}
int gml_software3d_vertex_format_add(GmlSoftware3D *graphics,int kind,int type,int usage,int count,int bytes){
  if(!graphics || !graphics->vertex_builder_active ||
     graphics->vertex_builder.n_attr>=GML_VERTEX_ATTR_MAX || bytes<=0) return 0;
  GmlVertexAttr *attr=&graphics->vertex_builder.attr[graphics->vertex_builder.n_attr++];
  attr->kind=(unsigned char)kind; attr->type=(unsigned char)type;
  attr->usage=(unsigned char)usage; attr->count=(unsigned char)count;
  attr->bytes=(unsigned short)bytes;
  if(graphics->vertex_builder.stride<=INT_MAX-bytes) graphics->vertex_builder.stride+=bytes;
  return 1;
}
int gml_software3d_vertex_format_finish(GmlSoftware3D *graphics){
  if(!graphics || !graphics->vertex_builder_active || graphics->vertex_builder.n_attr<=0){
    if(graphics) graphics->vertex_builder_active=0;
    return -1;
  }
  for(int i=0;i<GML_VERTEX_FORMAT_MAX;i++) if(!graphics->vertex_format[i].used){
    graphics->vertex_format[i]=graphics->vertex_builder; graphics->vertex_format[i].used=1;
    graphics->vertex_builder_active=0;
    return i;
  }
  graphics->vertex_builder_active=0;
  return -1;
}
int gml_software3d_vertex_buffer_create(GmlSoftware3D *graphics,int reserve_bytes){
  if(!graphics) return -1;
  if(reserve_bytes<0) reserve_bytes=0;
  for(int i=0;i<GML_VERTEX_BUFFER_MAX;i++) if(!graphics->vertex_buffer[i].used){
    GmlVertexBuffer *buffer=&graphics->vertex_buffer[i];
    memset(buffer,0,sizeof(*buffer)); buffer->used=1; buffer->format=-1;
    buffer->reserve_bytes=reserve_bytes; vertex_partial_init(buffer);
    return i;
  }
  return -1;
}
static int vertex_buffer_grow(GmlVertexBuffer *buffer,int needed){
  if(!buffer || needed<0 || needed>GML_VERTEX_MAX) return 0;
  if(needed<=buffer->vertex_cap) return 1;
  int capacity=buffer->vertex_cap?buffer->vertex_cap:64;
  while(capacity<needed){
    if(capacity>GML_VERTEX_MAX/2){ capacity=GML_VERTEX_MAX; break; }
    capacity*=2;
  }
  GmlD3Vertex *grown=realloc(buffer->vertex,(size_t)capacity*sizeof(*grown));
  if(!grown) return 0;
  buffer->vertex=grown; buffer->vertex_cap=capacity;
  return 1;
}
int gml_software3d_vertex_buffer_begin(GmlSoftware3D *graphics,int id,int format){
  if(!graphics || id<0 || id>=GML_VERTEX_BUFFER_MAX || !graphics->vertex_buffer[id].used ||
     format<0 || format>=GML_VERTEX_FORMAT_MAX || !graphics->vertex_format[format].used) return 0;
  GmlVertexBuffer *buffer=&graphics->vertex_buffer[id];
  if(buffer->frozen) return 0;
  buffer->format=format; buffer->vertex_n=0; vertex_partial_init(buffer);
  int stride=graphics->vertex_format[format].stride;
  int wanted=stride>0?buffer->reserve_bytes/stride:0;
  if(wanted>GML_VERTEX_MAX) wanted=GML_VERTEX_MAX;
  return wanted<=0 || vertex_buffer_grow(buffer,wanted);
}
static int vertex_buffer_commit(GmlVertexBuffer *buffer){
  if(!buffer || buffer->format<0 || buffer->format>=GML_VERTEX_FORMAT_MAX) return 0;
  if(!vertex_buffer_grow(buffer,buffer->vertex_n+1)) return 0;
  buffer->vertex[buffer->vertex_n++]=buffer->partial;
  vertex_partial_init(buffer);
  return 1;
}
int gml_software3d_vertex_buffer_attribute(GmlSoftware3D *graphics,int id,int supplied_kind,const double value[4],int count){
  if(!graphics || id<0 || id>=GML_VERTEX_BUFFER_MAX || !graphics->vertex_buffer[id].used) return 0;
  GmlVertexBuffer *buffer=&graphics->vertex_buffer[id];
  if(buffer->frozen || buffer->format<0 || buffer->format>=GML_VERTEX_FORMAT_MAX) return 0;
  GmlVertexFormat *format=&graphics->vertex_format[buffer->format];
  if(!format->used || buffer->attr_index<0 || buffer->attr_index>=format->n_attr) return 0;
  GmlVertexAttr *attr=&format->attr[buffer->attr_index];
  int kind=attr->kind==GML_VERTEX_CUSTOM?supplied_kind:attr->kind;
  (void)count;
  if(kind==GML_VERTEX_POSITION2){ buffer->partial.x=value[0]; buffer->partial.y=value[1]; }
  else if(kind==GML_VERTEX_POSITION3){ buffer->partial.x=value[0]; buffer->partial.y=value[1]; buffer->partial.z=value[2]; }
  else if(kind==GML_VERTEX_NORMAL){ buffer->partial.nx=value[0]; buffer->partial.ny=value[1]; buffer->partial.nz=value[2]; buffer->partial.has_normal=1; }
  else if(kind==GML_VERTEX_TEXCOORD){ buffer->partial.u=value[0]; buffer->partial.v=value[1]; }
  else if(kind==GML_VERTEX_COLOR){
    uint32_t color=(uint32_t)value[0];
    buffer->partial.r=color&255; buffer->partial.g=(color>>8)&255; buffer->partial.b=(color>>16)&255;
    buffer->partial.alpha=value[1];
  }
  buffer->attr_index++;
  if(buffer->attr_index==format->n_attr) return vertex_buffer_commit(buffer);
  return 1;
}
void gml_software3d_model_clear_data(GmlD3Model *model){
  if(!model) return;
  free(model->vertex); free(model->batch);
  model->vertex=NULL; model->batch=NULL;
  model->vertex_n=model->vertex_cap=model->batch_n=model->batch_cap=0;
  model->building=-1;
}
void gml_software3d_destroy(GmlSoftware3D *graphics){
  if(!graphics) return;
  for(int i=0;i<GML_VERTEX_BUFFER_MAX;i++) free(graphics->vertex_buffer[i].vertex);
  for(int i=0;i<GML_D3_MODEL_MAX;i++) gml_software3d_model_clear_data(&graphics->d3_model[i]);
  free(graphics->d3.depth);
  free(graphics);
}
void gml_software3d_models_clear_all(GmlSoftware3D *graphics){
  if(!graphics) return;
  for(int i=0;i<GML_D3_MODEL_MAX;i++){
    gml_software3d_model_clear_data(&graphics->d3_model[i]);
    graphics->d3_model[i].used=0;
  }
}

void gml_software3d_matrix_identity(double matrix[16]){
  memset(matrix,0,16*sizeof(*matrix));
  matrix[0]=matrix[5]=matrix[10]=matrix[15]=1;
}

GmlSoftware3D *gml_software3d_create(void){
  GmlSoftware3D *graphics=calloc(1,sizeof(*graphics));
  if(graphics){
    graphics->d3.depth_frame=-1;
    graphics->d3_prim_texture=-1;
    graphics->prim_texture=-1;
  }
  return graphics;
}

void gml_software3d_reset(GmlSoftware3D *graphics){
  if(!graphics) return;
  float *depth=graphics->d3.depth; size_t cap=graphics->d3.depth_cap;
  memset(&graphics->d3,0,sizeof(graphics->d3));
  graphics->d3.depth=depth; graphics->d3.depth_cap=cap; graphics->d3.depth_frame=-1;
  graphics->d3.shade_r=graphics->d3.shade_g=graphics->d3.shade_b=1;
  graphics->d3.zwrite=1; graphics->d3.smooth=1; graphics->d3.perspective=1;
  graphics->d3.fov=41.2; graphics->d3.near_clip=1; graphics->d3.far_clip=32000;
  graphics->d3.ambient_color=0;
  gml_software3d_matrix_identity(graphics->d3.transform);
  gml_software3d_matrix_identity(graphics->matrix_view);
  gml_software3d_matrix_identity(graphics->matrix_projection);
  graphics->d3_prim_n=0; graphics->d3_prim_kind=0; graphics->d3_prim_texture=-1;
  graphics->prim_n=0; graphics->prim_kind=0; graphics->prim_texture=-1;
  gml_software3d_models_clear_all(graphics);
  vertex_resources_reset(graphics);
}

int gml_software3d_status_get(const GmlSoftware3D *graphics,
                              GmlSoftware3DStatus *status){
  if(status) memset(status,0,sizeof(*status));
  if(!graphics || !status) return 0;
  status->active=graphics->d3.active;
  return 1;
}

void gml_software3d_set_classic(GmlSoftware3D *graphics,int classic){
  if(graphics) graphics->d3.classic=classic!=0;
}

void gml_software3d_end(GmlSoftware3D *graphics){
  if(graphics) graphics->d3.active=0;
}

void gml_software3d_control_update(GmlSoftware3D *graphics,
                                   const GmlSoftware3DControl *control,
                                   uint32_t mask){
  if(!graphics || !control) return;
  GmlD3State *d3=&graphics->d3;
  if(mask&GML_SOFTWARE3D_CONTROL_HIDDEN) d3->hidden=control->hidden!=0;
  if(mask&GML_SOFTWARE3D_CONTROL_ZWRITE) d3->zwrite=control->zwrite!=0;
  if(mask&GML_SOFTWARE3D_CONTROL_SMOOTH) d3->smooth=control->smooth!=0;
  if(mask&GML_SOFTWARE3D_CONTROL_FOG){
    d3->fog=control->fog!=0;
    d3->fog_color=control->fog_color&UINT32_C(0xFFFFFF);
    d3->fog_start=control->fog_start;
    d3->fog_end=control->fog_end;
    if(d3->fog_end<d3->fog_start){
      double swap=d3->fog_start;
      d3->fog_start=d3->fog_end;
      d3->fog_end=swap;
    }
  }
  if(mask&GML_SOFTWARE3D_CONTROL_CULLING) d3->culling=control->culling!=0;
  if(mask&GML_SOFTWARE3D_CONTROL_LIGHTING) d3->lighting=control->lighting!=0;
  if(mask&GML_SOFTWARE3D_CONTROL_AMBIENT)
    d3->ambient_color=control->ambient_color&UINT32_C(0xFFFFFF);
  if(mask&GML_SOFTWARE3D_CONTROL_FOV){
    d3->fov=control->fov;
    if(d3->fov<1 || d3->fov>170) d3->fov=41.2;
  }
  if(mask&GML_SOFTWARE3D_CONTROL_CLIP){
    d3->aspect=control->aspect;
    if(d3->aspect<=0) d3->aspect=0;
    d3->near_clip=control->near_clip;
    if(d3->near_clip<=1e-6) d3->near_clip=1;
    d3->far_clip=control->far_clip;
    if(d3->far_clip<=d3->near_clip) d3->far_clip=32000;
  }
  if(mask&GML_SOFTWARE3D_CONTROL_ORTHOGRAPHIC){
    d3->ortho=1;
    d3->perspective=0;
    d3->ortho_x=control->ortho_x;
    d3->ortho_y=control->ortho_y;
    d3->ortho_w=control->ortho_w;
    d3->ortho_h=control->ortho_h;
    d3->ortho_angle=control->ortho_angle;
    if(fabs(d3->ortho_w)<1e-9) d3->ortho_w=1;
    if(fabs(d3->ortho_h)<1e-9) d3->ortho_h=1;
  }
  if(mask&GML_SOFTWARE3D_CONTROL_DEPTH) d3->draw_depth=control->draw_depth;
}

int gml_software3d_light_define_point(GmlSoftware3D *graphics,int id,
                                      double x,double y,double z,double range,
                                      uint32_t color){
  if(!graphics || id<0 || id>=8) return 0;
  graphics->d3.light[id].defined=1;
  graphics->d3.light[id].x=x;
  graphics->d3.light[id].y=y;
  graphics->d3.light[id].z=z;
  graphics->d3.light[id].range=fabs(range);
  graphics->d3.light[id].color=color&UINT32_C(0xFFFFFF);
  return 1;
}

int gml_software3d_light_define_direction(GmlSoftware3D *graphics,int id,
                                          double x,double y,double z,
                                          uint32_t color){
  if(!graphics || id<0 || id>=8) return 0;
  graphics->d3.light[id].defined=1;
  graphics->d3.light[id].x=x;
  graphics->d3.light[id].y=y;
  graphics->d3.light[id].z=z;
  graphics->d3.light[id].range=-1;
  graphics->d3.light[id].color=color&UINT32_C(0xFFFFFF);
  return 1;
}

int gml_software3d_light_enable(GmlSoftware3D *graphics,int id,int enabled){
  if(!graphics || id<0 || id>=8) return 0;
  graphics->d3.light[id].enabled=enabled!=0;
  return 1;
}

int gml_software3d_matrix_get(const GmlSoftware3D *graphics,int type,
                              double matrix[16]){
  if(!graphics || !matrix) return 0;
  const double *source;
  if(type==GML_SOFTWARE3D_MATRIX_VIEW) source=graphics->matrix_view;
  else if(type==GML_SOFTWARE3D_MATRIX_PROJECTION) source=graphics->matrix_projection;
  else if(type==GML_SOFTWARE3D_MATRIX_TRANSFORM) source=graphics->d3.transform;
  else return 0;
  memcpy(matrix,source,16*sizeof(*matrix));
  return 1;
}

void gml_software3d_vertex_format_begin(GmlSoftware3D *graphics){
  if(!graphics) return;
  memset(&graphics->vertex_builder,0,sizeof(graphics->vertex_builder));
  graphics->vertex_builder_active=1;
}

void gml_software3d_vertex_format_delete(GmlSoftware3D *graphics,int id){
  if(graphics && id>=0 && id<GML_VERTEX_FORMAT_MAX)
    memset(&graphics->vertex_format[id],0,sizeof(graphics->vertex_format[id]));
}

void gml_software3d_vertex_buffer_delete(GmlSoftware3D *graphics,int id){
  if(!graphics || id<0 || id>=GML_VERTEX_BUFFER_MAX ||
     !graphics->vertex_buffer[id].used) return;
  free(graphics->vertex_buffer[id].vertex);
  memset(&graphics->vertex_buffer[id],0,sizeof(graphics->vertex_buffer[id]));
  graphics->vertex_buffer[id].format=-1;
}

void gml_software3d_vertex_buffer_end(GmlSoftware3D *graphics,int id){
  if(graphics && id>=0 && id<GML_VERTEX_BUFFER_MAX &&
     graphics->vertex_buffer[id].used)
    vertex_partial_init(&graphics->vertex_buffer[id]);
}

int gml_software3d_vertex_buffer_attribute_ubyte4(
  GmlSoftware3D *graphics,int id,const double supplied[4]){
  if(!graphics || !supplied || id<0 || id>=GML_VERTEX_BUFFER_MAX ||
     !graphics->vertex_buffer[id].used) return 0;
  GmlVertexBuffer *buffer=&graphics->vertex_buffer[id];
  double value[4]={supplied[0],supplied[1],supplied[2],supplied[3]};
  int kind=GML_VERTEX_CUSTOM;
  if(buffer->format>=0 && buffer->format<GML_VERTEX_FORMAT_MAX &&
     buffer->attr_index>=0 &&
     buffer->attr_index<graphics->vertex_format[buffer->format].n_attr &&
     graphics->vertex_format[buffer->format].attr[buffer->attr_index].kind==
       GML_VERTEX_COLOR){
    uint32_t color=((uint32_t)value[0]&255)|
      (((uint32_t)value[1]&255)<<8)|(((uint32_t)value[2]&255)<<16);
    value[0]=color;
    value[1]=value[3]/255.0;
    kind=GML_VERTEX_COLOR;
  }
  return gml_software3d_vertex_buffer_attribute(graphics,id,kind,value,4);
}

int gml_software3d_vertex_buffer_freeze(GmlSoftware3D *graphics,int id){
  if(!graphics || id<0 || id>=GML_VERTEX_BUFFER_MAX ||
     !graphics->vertex_buffer[id].used) return 0;
  graphics->vertex_buffer[id].frozen=1;
  return 1;
}

int gml_software3d_vertex_buffer_number(const GmlSoftware3D *graphics,int id){
  if(!graphics || id<0 || id>=GML_VERTEX_BUFFER_MAX ||
     !graphics->vertex_buffer[id].used) return 0;
  return graphics->vertex_buffer[id].vertex_n;
}

size_t gml_software3d_vertex_buffer_size(const GmlSoftware3D *graphics,int id){
  if(!graphics || id<0 || id>=GML_VERTEX_BUFFER_MAX ||
     !graphics->vertex_buffer[id].used) return 0;
  const GmlVertexBuffer *buffer=&graphics->vertex_buffer[id];
  int stride=buffer->format>=0 && buffer->format<GML_VERTEX_FORMAT_MAX ?
    graphics->vertex_format[buffer->format].stride:0;
  return stride>0?(size_t)buffer->vertex_n*(size_t)stride:0;
}

void gml_software3d_primitive_2d_begin(GmlSoftware3D *graphics,
                                       int kind,int texture){
  if(!graphics) return;
  graphics->prim_kind=kind;
  graphics->prim_n=0;
  graphics->prim_texture=texture;
}

int gml_software3d_primitive_2d_append(GmlSoftware3D *graphics,
                                       double x,double y,double u,double v,
                                       uint32_t color,double alpha){
  if(!graphics || graphics->prim_n>=GML_PRIM_MAX) return 0;
  int index=graphics->prim_n++;
  graphics->prim_x[index]=x;
  graphics->prim_y[index]=y;
  graphics->prim_u[index]=u;
  graphics->prim_v[index]=v;
  graphics->prim_c[index]=color;
  graphics->prim_a[index]=alpha;
  return 1;
}

void gml_software3d_primitive_3d_begin(GmlSoftware3D *graphics,
                                       int kind,int texture){
  if(!graphics) return;
  graphics->d3_prim_kind=kind;
  graphics->d3_prim_n=0;
  graphics->d3_prim_texture=texture;
}

int gml_software3d_primitive_3d_append(GmlSoftware3D *graphics,
                                       GmlSoftware3DVertex vertex){
  if(!graphics || graphics->d3_prim_n>=GML_D3_PRIM_MAX) return 0;
  graphics->d3_prim[graphics->d3_prim_n++]=vertex;
  return 1;
}
