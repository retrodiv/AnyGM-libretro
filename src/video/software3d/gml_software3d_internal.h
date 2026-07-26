/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Private software-3D model and raster state. */
#ifndef GML_SOFTWARE3D_INTERNAL_H
#define GML_SOFTWARE3D_INTERNAL_H

#include "gml_software3d.h"
#include "gml_render_backend.h"

typedef GmlSoftware3DVertex GmlD3Vertex;

/* Vertex formats and buffers are runtime resources independent of
 * the legacy d3d_model API.  The software backend stores a canonical vertex
 * rather than mirroring a host GPU layout; format stride is still tracked so
 * the public size/query functions retain their documented byte semantics. */
#define GML_VERTEX_FORMAT_MAX 128
#define GML_VERTEX_BUFFER_MAX 256
#define GML_VERTEX_ATTR_MAX 16
#define GML_VERTEX_MAX 1048576
#define GML_VERTEX_POSITION2 GML_SOFTWARE3D_VERTEX_POSITION2
#define GML_VERTEX_POSITION3 GML_SOFTWARE3D_VERTEX_POSITION3
#define GML_VERTEX_COLOR GML_SOFTWARE3D_VERTEX_COLOR
#define GML_VERTEX_NORMAL GML_SOFTWARE3D_VERTEX_NORMAL
#define GML_VERTEX_TEXCOORD GML_SOFTWARE3D_VERTEX_TEXCOORD
#define GML_VERTEX_CUSTOM GML_SOFTWARE3D_VERTEX_CUSTOM
typedef struct {
  unsigned char kind,type,usage,count;
  unsigned short bytes;
} GmlVertexAttr;
typedef struct {
  int used,n_attr,stride;
  GmlVertexAttr attr[GML_VERTEX_ATTR_MAX];
} GmlVertexFormat;
typedef struct {
  int used,frozen,format,attr_index,reserve_bytes;
  GmlD3Vertex partial;
  GmlD3Vertex *vertex;
  int vertex_n,vertex_cap;
} GmlVertexBuffer;
typedef struct {
  int active, hidden, culling, ortho, classic;
  double eye[3], right[3], up[3], forward[3];
  double ortho_x, ortho_y, ortho_w, ortho_h, ortho_angle;
  int zwrite, smooth, fog, perspective;
  double fov, aspect, near_clip, far_clip, draw_depth;
  double fog_start, fog_end;
  uint32_t fog_color, ambient_color;
  double transform[16], transform_stack[32][16];
  int transform_stack_n;
  int lighting;
  struct { int defined, enabled; double x,y,z,range; uint32_t color; } light[8];
  double shade_r, shade_g, shade_b;
  float *depth; size_t depth_cap;
  int depth_w, depth_h;
  long depth_frame;
} GmlD3State;
#define GML_D3_PRIM_MAX 4096
#define GML_D3_MODEL_MAX 256
#define GML_D3_MODEL_VERTEX_MAX GML_SOFTWARE3D_MODEL_VERTEX_MAX
#define GML_PRIM_MAX 256
typedef struct { int kind,first,count; } GmlD3Batch;
typedef struct {
  int used,building;
  GmlD3Vertex *vertex;
  int vertex_n,vertex_cap;
  GmlD3Batch *batch;
  int batch_n,batch_cap;
} GmlD3Model;
struct GmlSoftware3D {
  GmlVertexFormat vertex_format[GML_VERTEX_FORMAT_MAX];
  GmlVertexFormat vertex_builder;
  int vertex_builder_active;
  GmlVertexBuffer vertex_buffer[GML_VERTEX_BUFFER_MAX];
  GmlD3State d3;
  double matrix_view[16],matrix_projection[16];
  GmlD3Vertex d3_prim[GML_D3_PRIM_MAX];
  int d3_prim_kind,d3_prim_texture,d3_prim_n;
  GmlD3Model d3_model[GML_D3_MODEL_MAX];
  int prim_kind,prim_n,prim_texture;
  double prim_x[GML_PRIM_MAX],prim_y[GML_PRIM_MAX];
  double prim_u[GML_PRIM_MAX],prim_v[GML_PRIM_MAX],prim_a[GML_PRIM_MAX];
  uint32_t prim_c[GML_PRIM_MAX];
  int d3_texture_log_count;
};

void gml_software3d_model_clear_data(GmlD3Model *model);
void gml_software3d_models_clear_all(GmlSoftware3D *software3d);
typedef struct {
  int kind,w,h,stride,offset_x,offset_y;
  const uint8_t *rgba;
  const uint32_t *xrgb;
} GmlD3Texture;

/* Software-3D-private raster operations shared by the fixed-function owners. */
void gml_software3d_transform_point(GmlRender *render,double *x,double *y,double *z);
int gml_software3d_depth_prepare(GmlRender *render);
int gml_software3d_texture(GmlRender *render,int handle,GmlD3Texture *texture);
void gml_software3d_emit_triangle(GmlRender *render,const GmlD3Vertex vertices[3],
                                  const GmlD3Texture *texture);
void gml_software3d_emit_point(GmlRender *render,GmlD3Vertex vertex,
                               const GmlD3Texture *texture);
void gml_software3d_emit_line(GmlRender *render,GmlD3Vertex first,GmlD3Vertex second,
                              const GmlD3Texture *texture);

#endif
