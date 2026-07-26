/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Software fixed-function model resources and their canonical payload. */
#include "gml_software3d_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define g_d3 (gml_render_backend_software3d(R)->d3)
#define d3_texture gml_software3d_texture
#define d3_depth_prepare gml_software3d_depth_prepare
#define d3_emit_point gml_software3d_emit_point
#define d3_emit_line gml_software3d_emit_line
#define d3_emit_triangle gml_software3d_emit_triangle
static int d3_model_grow_vertices(GmlD3Model *model,int needed){
  if(!model || needed<0 || needed>GML_D3_MODEL_VERTEX_MAX) return 0;
  if(needed<=model->vertex_cap) return 1;
  int capacity=model->vertex_cap?model->vertex_cap:64;
  while(capacity<needed){
    if(capacity>GML_D3_MODEL_VERTEX_MAX/2){ capacity=GML_D3_MODEL_VERTEX_MAX; break; }
    capacity*=2;
  }
  GmlD3Vertex *grown=realloc(model->vertex,(size_t)capacity*sizeof(*grown));
  if(!grown) return 0;
  model->vertex=grown; model->vertex_cap=capacity;
  return 1;
}
static int d3_model_grow_batches(GmlD3Model *model,int needed){
  if(!model || needed<0 || needed>GML_D3_MODEL_VERTEX_MAX) return 0;
  if(needed<=model->batch_cap) return 1;
  int capacity=model->batch_cap?model->batch_cap:16;
  while(capacity<needed){
    if(capacity>GML_D3_MODEL_VERTEX_MAX/2){ capacity=GML_D3_MODEL_VERTEX_MAX; break; }
    capacity*=2;
  }
  GmlD3Batch *grown=realloc(model->batch,(size_t)capacity*sizeof(*grown));
  if(!grown) return 0;
  model->batch=grown; model->batch_cap=capacity;
  return 1;
}
static int d3_model_begin_batch(GmlD3Model *model,int kind){
  if(!model || !model->used || kind<1 || kind>6 ||
     !d3_model_grow_batches(model,model->batch_n+1)) return 0;
  model->building=model->batch_n;
  model->batch[model->batch_n++]=(GmlD3Batch){kind,model->vertex_n,0};
  return 1;
}
static int d3_model_append_vertex(GmlD3Model *model,GmlD3Vertex vertex){
  if(!model || !model->used || model->building<0 || model->building>=model->batch_n ||
     !d3_model_grow_vertices(model,model->vertex_n+1)) return 0;
  model->vertex[model->vertex_n++]=vertex;
  model->batch[model->building].count++;
  return 1;
}
static int d3_model_append_quad(GmlD3Model *model,const double point[4][3],double hrepeat,double vrepeat){
  double edge_a[3]={point[1][0]-point[0][0],point[1][1]-point[0][1],point[1][2]-point[0][2]};
  double edge_b[3]={point[2][0]-point[0][0],point[2][1]-point[0][1],point[2][2]-point[0][2]};
  double normal[3]; gml_software3d_cross(edge_a,edge_b,normal); int has_normal=gml_software3d_normalize(normal);
  static const int corner[6]={0,1,2,0,2,3};
  static const double uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  for(int i=0;i<6;i++){
    int c=corner[i]; GmlD3Vertex vertex={0};
    vertex.x=point[c][0]; vertex.y=point[c][1]; vertex.z=point[c][2];
    vertex.u=uv[c][0]*hrepeat; vertex.v=uv[c][1]*vrepeat;
    vertex.r=vertex.g=vertex.b=255; vertex.alpha=1;
    vertex.nx=normal[0]; vertex.ny=normal[1]; vertex.nz=normal[2]; vertex.has_normal=has_normal;
    if(!d3_model_append_vertex(model,vertex)) return 0;
  }
  return 1;
}
static int d3_model_add_shape(GmlD3Model *model,int shape,double x1,double y1,double z1,
                              double x2,double y2,double z2,double hrepeat,double vrepeat,
                              int closed,int requested_steps){
  if(!d3_model_begin_batch(model,4)) return 0;
  int ok=1;
  if(shape==14 || shape==15){
    double point[4][3];
    if(shape==15){
      double vertices[4][3]={{x1,y1,z1},{x2,y1,z1},{x2,y2,z2},{x1,y2,z2}};
      memcpy(point,vertices,sizeof(point));
    } else {
      double vertices[4][3]={{x1,y1,z1},{x2,y2,z1},{x2,y2,z2},{x1,y1,z2}};
      memcpy(point,vertices,sizeof(point));
    }
    ok=d3_model_append_quad(model,point,hrepeat,vrepeat);
  } else if(shape==10){
    double face[6][4][3]={
      {{x1,y1,z1},{x2,y1,z1},{x2,y2,z1},{x1,y2,z1}},
      {{x1,y2,z2},{x2,y2,z2},{x2,y1,z2},{x1,y1,z2}},
      {{x1,y1,z2},{x2,y1,z2},{x2,y1,z1},{x1,y1,z1}},
      {{x1,y2,z1},{x2,y2,z1},{x2,y2,z2},{x1,y2,z2}},
      {{x1,y1,z1},{x1,y2,z1},{x1,y2,z2},{x1,y1,z2}},
      {{x2,y1,z2},{x2,y2,z2},{x2,y2,z1},{x2,y1,z1}}};
    for(int i=0;i<6&&ok;i++) ok=d3_model_append_quad(model,face[i],hrepeat,vrepeat);
  } else if(shape==11 || shape==12){
    double cx=(x1+x2)*.5,cy=(y1+y2)*.5,rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5;
    int steps=requested_steps; if(steps<3) steps=3; if(steps>128) steps=128;
    int cone=shape==12;
    for(int i=0;i<steps&&ok;i++){
      double q0=2*M_PI*i/steps,q1=2*M_PI*(i+1)/steps;
      double top0x=cone?cx:cx+cos(q0)*rx,top0y=cone?cy:cy+sin(q0)*ry;
      double top1x=cone?cx:cx+cos(q1)*rx,top1y=cone?cy:cy+sin(q1)*ry;
      double side[4][3]={{cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                         {top1x,top1y,z2},{top0x,top0y,z2}};
      ok=d3_model_append_quad(model,side,hrepeat/steps,vrepeat);
      if(closed&&ok){
        double cap0[4][3]={{cx,cy,z1},{cx+cos(q1)*rx,cy+sin(q1)*ry,z1},
                            {cx+cos(q0)*rx,cy+sin(q0)*ry,z1},{cx,cy,z1}};
        ok=d3_model_append_quad(model,cap0,hrepeat,vrepeat);
        if(!cone&&ok){
          double cap1[4][3]={{cx,cy,z2},{cx+cos(q0)*rx,cy+sin(q0)*ry,z2},
                              {cx+cos(q1)*rx,cy+sin(q1)*ry,z2},{cx,cy,z2}};
          ok=d3_model_append_quad(model,cap1,hrepeat,vrepeat);
        }
      }
    }
  } else if(shape==13){
    double cx=(x1+x2)*.5,cy=(y1+y2)*.5,cz=(z1+z2)*.5;
    double rx=fabs(x2-x1)*.5,ry=fabs(y2-y1)*.5,rz=fabs(z2-z1)*.5;
    int steps=requested_steps; if(steps<4) steps=4; if(steps>64) steps=64;
    for(int lat=0;lat<steps&&ok;lat++) for(int lon=0;lon<steps*2&&ok;lon++){
      double latitude0=-M_PI*.5+M_PI*lat/steps,latitude1=-M_PI*.5+M_PI*(lat+1)/steps;
      double longitude0=M_PI*lon/steps,longitude1=M_PI*(lon+1)/steps;
      double point[4][3]={{cx+rx*cos(latitude0)*cos(longitude0),cy+ry*cos(latitude0)*sin(longitude0),cz+rz*sin(latitude0)},
                          {cx+rx*cos(latitude0)*cos(longitude1),cy+ry*cos(latitude0)*sin(longitude1),cz+rz*sin(latitude0)},
                          {cx+rx*cos(latitude1)*cos(longitude1),cy+ry*cos(latitude1)*sin(longitude1),cz+rz*sin(latitude1)},
                          {cx+rx*cos(latitude1)*cos(longitude0),cy+ry*cos(latitude1)*sin(longitude0),cz+rz*sin(latitude1)}};
      ok=d3_model_append_quad(model,point,hrepeat/steps,vrepeat/steps);
    }
  } else ok=0;
  model->building=-1;
  if(!ok){
    GmlD3Batch *batch=&model->batch[model->batch_n-1];
    model->vertex_n=batch->first; model->batch_n--;
  }
  return ok;
}
static void d3_model_draw(GmlRender *R,const GmlD3Model *model,double x,double y,double z,int texture_handle){
  if(!R || !model || !model->used) return;
  GmlD3Texture texture={0};
  if(texture_handle!=-1) d3_texture(R,texture_handle,&texture);
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_backend_prepare_draw(R);
  if(!d3_depth_prepare(R)) return;
  for(int b=0;b<model->batch_n;b++){
    const GmlD3Batch *batch=&model->batch[b];
    if(batch->first<0 || batch->count<0 || batch->first+batch->count>model->vertex_n) continue;
#define D3_MODEL_VERTEX(index) ({ GmlD3Vertex _v=model->vertex[batch->first+(index)]; _v.x+=x; _v.y+=y; _v.z+=z; _v; })
    if(batch->kind==1){
      for(int i=0;i<batch->count;i++) d3_emit_point(R,D3_MODEL_VERTEX(i),&texture);
    } else if(batch->kind==2){
      for(int i=0;i+1<batch->count;i+=2) d3_emit_line(R,D3_MODEL_VERTEX(i),D3_MODEL_VERTEX(i+1),&texture);
    } else if(batch->kind==3){
      for(int i=0;i+1<batch->count;i++) d3_emit_line(R,D3_MODEL_VERTEX(i),D3_MODEL_VERTEX(i+1),&texture);
    } else if(batch->kind==4){
      for(int i=0;i+2<batch->count;i+=3){
        GmlD3Vertex triangle[3]={D3_MODEL_VERTEX(i),D3_MODEL_VERTEX(i+1),D3_MODEL_VERTEX(i+2)};
        d3_emit_triangle(R,triangle,&texture);
      }
    } else if(batch->kind==5){
      for(int i=2;i<batch->count;i++){
        GmlD3Vertex triangle[3];
        if(i&1){ triangle[0]=D3_MODEL_VERTEX(i-1); triangle[1]=D3_MODEL_VERTEX(i-2); }
        else { triangle[0]=D3_MODEL_VERTEX(i-2); triangle[1]=D3_MODEL_VERTEX(i-1); }
        triangle[2]=D3_MODEL_VERTEX(i); d3_emit_triangle(R,triangle,&texture);
      }
    } else if(batch->kind==6){
      for(int i=1;i+1<batch->count;i++){
        GmlD3Vertex triangle[3]={D3_MODEL_VERTEX(0),D3_MODEL_VERTEX(i),D3_MODEL_VERTEX(i+1)};
        d3_emit_triangle(R,triangle,&texture);
      }
    }
#undef D3_MODEL_VERTEX
  }
}

static GmlD3Model *d3_model_find(GmlSoftware3D *graphics,int id){
  if(!graphics || id<0 || id>=GML_D3_MODEL_MAX ||
     !graphics->d3_model[id].used) return NULL;
  return &graphics->d3_model[id];
}
static const GmlD3Model *d3_model_find_const(const GmlSoftware3D *graphics,
                                             int id){
  if(!graphics || id<0 || id>=GML_D3_MODEL_MAX ||
     !graphics->d3_model[id].used) return NULL;
  return &graphics->d3_model[id];
}
int gml_software3d_model_create(GmlSoftware3D *graphics){
  if(!graphics) return -1;
  for(int id=0;id<GML_D3_MODEL_MAX;id++) if(!graphics->d3_model[id].used){
    gml_software3d_model_clear_data(&graphics->d3_model[id]);
    graphics->d3_model[id].used=1;
    graphics->d3_model[id].building=-1;
    return id;
  }
  return -1;
}
int gml_software3d_model_destroy(GmlSoftware3D *graphics,int id){
  GmlD3Model *model=d3_model_find(graphics,id);
  if(!model) return 0;
  gml_software3d_model_clear_data(model);
  model->used=0;
  return 1;
}
int gml_software3d_model_clear(GmlSoftware3D *graphics,int id){
  GmlD3Model *model=d3_model_find(graphics,id);
  if(!model) return 0;
  gml_software3d_model_clear_data(model);
  return 1;
}
int gml_software3d_model_is_live(const GmlSoftware3D *graphics,int id){
  return d3_model_find_const(graphics,id)!=NULL;
}
int gml_software3d_model_begin(GmlSoftware3D *graphics,int id,int kind){
  return d3_model_begin_batch(d3_model_find(graphics,id),kind);
}
int gml_software3d_model_end(GmlSoftware3D *graphics,int id){
  GmlD3Model *model=d3_model_find(graphics,id);
  if(!model) return 0;
  model->building=-1;
  return 1;
}
int gml_software3d_model_append_vertex(GmlSoftware3D *graphics,int id,
                                       GmlSoftware3DVertex vertex){
  return d3_model_append_vertex(d3_model_find(graphics,id),vertex);
}
int gml_software3d_model_append_quad(GmlSoftware3D *graphics,int id,
                                     const double points[4][3],
                                     double hrepeat,double vrepeat){
  return d3_model_append_quad(d3_model_find(graphics,id),points,hrepeat,vrepeat);
}
int gml_software3d_model_add_shape(GmlSoftware3D *graphics,int id,int shape,
                                   double x1,double y1,double z1,
                                   double x2,double y2,double z2,
                                   double hrepeat,double vrepeat,
                                   int closed,int requested_steps){
  return d3_model_add_shape(d3_model_find(graphics,id),shape,
                            x1,y1,z1,x2,y2,z2,hrepeat,vrepeat,
                            closed,requested_steps);
}
void gml_software3d_model_draw(GmlRender *render,int id,
                               double x,double y,double z,int texture_handle){
  GmlSoftware3D *graphics=gml_render_backend_software3d(render);
  d3_model_draw(render,d3_model_find_const(graphics,id),
                x,y,z,texture_handle);
}
int gml_software3d_model_info(const GmlSoftware3D *graphics,int id,
                              int *vertex_count,int *batch_count){
  const GmlD3Model *model=d3_model_find_const(graphics,id);
  if(vertex_count) *vertex_count=0;
  if(batch_count) *batch_count=0;
  if(!model) return 0;
  if(vertex_count) *vertex_count=model->vertex_n;
  if(batch_count) *batch_count=model->batch_n;
  return 1;
}
int gml_software3d_model_batch_get(const GmlSoftware3D *graphics,int id,
                                   int index,GmlSoftware3DBatch *batch){
  const GmlD3Model *model=d3_model_find_const(graphics,id);
  if(batch) memset(batch,0,sizeof(*batch));
  if(!model || !batch || index<0 || index>=model->batch_n) return 0;
  batch->kind=model->batch[index].kind;
  batch->first=model->batch[index].first;
  batch->count=model->batch[index].count;
  return 1;
}
int gml_software3d_model_vertex_get(const GmlSoftware3D *graphics,int id,
                                    int index,GmlSoftware3DVertex *vertex){
  const GmlD3Model *model=d3_model_find_const(graphics,id);
  if(vertex) memset(vertex,0,sizeof(*vertex));
  if(!model || !vertex || index<0 || index>=model->vertex_n) return 0;
  *vertex=model->vertex[index];
  return 1;
}

enum { GML_MODEL_STATE_SCHEMA=1 };
#define GML_MODEL_STATE_MAGIC UINT32_C(0x534D4441)
typedef struct { unsigned char *data; size_t cap,pos; int ok; } D3BlobW;
typedef struct { const unsigned char *data; size_t cap,pos; int ok; } D3BlobR;
static void d3_blob_write(D3BlobW *writer,const void *data,size_t size){
  if(size>SIZE_MAX-writer->pos){ writer->ok=0; writer->pos=SIZE_MAX; return; }
  if(writer->data){
    if(writer->pos<=writer->cap && size<=writer->cap-writer->pos) memcpy(writer->data+writer->pos,data,size);
    else writer->ok=0;
  }
  if(size>SIZE_MAX-writer->pos){ writer->ok=0; return; }
  writer->pos+=size;
}
static void d3_blob_read(D3BlobR *reader,void *data,size_t size){
  if(size>SIZE_MAX-reader->pos){ memset(data,0,size); reader->ok=0; reader->pos=SIZE_MAX; return; }
  if(reader->pos<=reader->cap && size<=reader->cap-reader->pos) memcpy(data,reader->data+reader->pos,size);
  else { memset(data,0,size); reader->ok=0; }
  if(size>SIZE_MAX-reader->pos){ reader->ok=0; return; }
  reader->pos+=size;
}
static void d3_blob_u32(D3BlobW *writer,uint32_t value){
  uint8_t b[4]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),(uint8_t)(value>>24)};
  d3_blob_write(writer,b,sizeof b);
}
static uint32_t d3_blob_read_u32(D3BlobR *reader){
  uint8_t b[4]={0}; d3_blob_read(reader,b,sizeof b);
  return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static void d3_blob_double(D3BlobW *writer,double value){
  uint64_t bits=0; uint8_t b[8]; memcpy(&bits,&value,sizeof bits);
  for(unsigned i=0;i<8;i++) b[i]=(uint8_t)(bits>>(i*8));
  d3_blob_write(writer,b,sizeof b);
}
static double d3_blob_read_double(D3BlobR *reader){
  uint8_t b[8]={0}; uint64_t bits=0; d3_blob_read(reader,b,sizeof b);
  for(unsigned i=0;i<8;i++) bits|=(uint64_t)b[i]<<(i*8);
  double value=0; memcpy(&value,&bits,sizeof value); return value;
}
static void d3_blob_write_model(D3BlobW *writer,const GmlD3Model *model){
  d3_blob_u32(writer,(uint32_t)model->vertex_n); d3_blob_u32(writer,(uint32_t)model->batch_n);
  for(int i=0;i<model->vertex_n;i++){
    const GmlD3Vertex *vertex=&model->vertex[i];
    const double values[12]={vertex->x,vertex->y,vertex->z,vertex->u,vertex->v,
      vertex->r,vertex->g,vertex->b,vertex->alpha,vertex->nx,vertex->ny,vertex->nz};
    for(unsigned value=0;value<12;value++) d3_blob_double(writer,values[value]);
    d3_blob_u32(writer,(uint32_t)vertex->has_normal);
  }
  for(int i=0;i<model->batch_n;i++){
    d3_blob_u32(writer,(uint32_t)model->batch[i].kind);
    d3_blob_u32(writer,(uint32_t)model->batch[i].first);
    d3_blob_u32(writer,(uint32_t)model->batch[i].count);
  }
}
static int d3_blob_read_model(D3BlobR *reader,GmlD3Model *model){
  uint32_t vertex_n=d3_blob_read_u32(reader),batch_n=d3_blob_read_u32(reader);
  if(!reader->ok || vertex_n>GML_D3_MODEL_VERTEX_MAX || batch_n>GML_D3_MODEL_VERTEX_MAX ||
     !d3_model_grow_vertices(model,(int)vertex_n) || !d3_model_grow_batches(model,(int)batch_n)) return 0;
  model->vertex_n=(int)vertex_n; model->batch_n=(int)batch_n; model->building=-1;
  for(int i=0;i<model->vertex_n;i++){
    double values[12]; for(unsigned value=0;value<12;value++) values[value]=d3_blob_read_double(reader);
    GmlD3Vertex *vertex=&model->vertex[i];
    vertex->x=values[0]; vertex->y=values[1]; vertex->z=values[2];
    vertex->u=values[3]; vertex->v=values[4]; vertex->r=values[5]; vertex->g=values[6];
    vertex->b=values[7]; vertex->alpha=values[8]; vertex->nx=values[9]; vertex->ny=values[10]; vertex->nz=values[11];
    vertex->has_normal=(int)d3_blob_read_u32(reader);
  }
  for(int i=0;i<model->batch_n;i++){
    GmlD3Batch *batch=&model->batch[i];
    batch->kind=(int)d3_blob_read_u32(reader); batch->first=(int)d3_blob_read_u32(reader);
    batch->count=(int)d3_blob_read_u32(reader);
    if(batch->kind<1 || batch->kind>6 || batch->first<0 || batch->count<0 ||
       batch->first>model->vertex_n-batch->count) reader->ok=0;
  }
  return reader->ok;
}
size_t gml_d3_models_state_size(GmlSoftware3D *graphics){
  if(!graphics) return 0;
  D3BlobW writer={.ok=1};
  d3_blob_u32(&writer,GML_MODEL_STATE_MAGIC); /* ADMS */
  d3_blob_u32(&writer,GML_MODEL_STATE_SCHEMA);
  int count=0; for(int i=0;i<GML_D3_MODEL_MAX;i++) if(graphics->d3_model[i].used) count++;
  d3_blob_u32(&writer,(uint32_t)count);
  for(int i=0;i<GML_D3_MODEL_MAX;i++) if(graphics->d3_model[i].used){
    d3_blob_u32(&writer,(uint32_t)i); d3_blob_write_model(&writer,&graphics->d3_model[i]);
  }
  return writer.ok?writer.pos:0;
}
int gml_d3_models_state_save(GmlSoftware3D *graphics,void *data,size_t capacity){
  if(!graphics) return 0;
  D3BlobW writer={.data=data,.cap=capacity,.ok=1};
  d3_blob_u32(&writer,GML_MODEL_STATE_MAGIC); /* ADMS */
  d3_blob_u32(&writer,GML_MODEL_STATE_SCHEMA);
  int count=0; for(int i=0;i<GML_D3_MODEL_MAX;i++) if(graphics->d3_model[i].used) count++;
  d3_blob_u32(&writer,(uint32_t)count);
  for(int i=0;i<GML_D3_MODEL_MAX;i++) if(graphics->d3_model[i].used){
    d3_blob_u32(&writer,(uint32_t)i); d3_blob_write_model(&writer,&graphics->d3_model[i]);
  }
  return writer.ok&&writer.pos==capacity;
}
int gml_d3_models_state_load(GmlSoftware3D *graphics,const void *data,size_t size){
  if(!graphics) return 0;
  gml_software3d_models_clear_all(graphics);
  if(!data || size<12) return size==0;
  D3BlobR reader={.data=data,.cap=size,.ok=1};
  uint32_t magic=d3_blob_read_u32(&reader),schema=d3_blob_read_u32(&reader);
  uint32_t count=d3_blob_read_u32(&reader);
  if(magic!=GML_MODEL_STATE_MAGIC || schema!=GML_MODEL_STATE_SCHEMA ||
     count>GML_D3_MODEL_MAX) reader.ok=0;
  for(uint32_t i=0;i<count&&reader.ok;i++){
    uint32_t id=d3_blob_read_u32(&reader);
    if(id>=GML_D3_MODEL_MAX || graphics->d3_model[id].used){ reader.ok=0; break; }
    graphics->d3_model[id].used=1; graphics->d3_model[id].building=-1;
    if(!d3_blob_read_model(&reader,&graphics->d3_model[id])) break;
  }
  if(!reader.ok || reader.pos!=size){ gml_software3d_models_clear_all(graphics); return 0; }
  return 1;
}
