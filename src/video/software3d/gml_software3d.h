/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Typed interface to the single engine-owned software fixed-function pipeline. */
#ifndef GML_SOFTWARE3D_H
#define GML_SOFTWARE3D_H

#include <stddef.h>
#include <stdint.h>

typedef struct GmlSoftware3D GmlSoftware3D;
struct GmlRender;

typedef struct {
  int active;
} GmlSoftware3DStatus;

typedef struct {
  int hidden;
  int zwrite;
  int smooth;
  int fog;
  int culling;
  int lighting;
  int ortho;
  int perspective;
  double fov;
  double aspect;
  double near_clip;
  double far_clip;
  double draw_depth;
  double fog_start;
  double fog_end;
  double ortho_x;
  double ortho_y;
  double ortho_w;
  double ortho_h;
  double ortho_angle;
  uint32_t fog_color;
  uint32_t ambient_color;
} GmlSoftware3DControl;

enum {
  GML_SOFTWARE3D_CONTROL_HIDDEN=UINT32_C(1)<<0,
  GML_SOFTWARE3D_CONTROL_ZWRITE=UINT32_C(1)<<1,
  GML_SOFTWARE3D_CONTROL_SMOOTH=UINT32_C(1)<<2,
  GML_SOFTWARE3D_CONTROL_FOG=UINT32_C(1)<<3,
  GML_SOFTWARE3D_CONTROL_CULLING=UINT32_C(1)<<4,
  GML_SOFTWARE3D_CONTROL_LIGHTING=UINT32_C(1)<<5,
  GML_SOFTWARE3D_CONTROL_AMBIENT=UINT32_C(1)<<6,
  GML_SOFTWARE3D_CONTROL_FOV=UINT32_C(1)<<7,
  GML_SOFTWARE3D_CONTROL_CLIP=UINT32_C(1)<<8,
  GML_SOFTWARE3D_CONTROL_ORTHOGRAPHIC=UINT32_C(1)<<9,
  GML_SOFTWARE3D_CONTROL_DEPTH=UINT32_C(1)<<10
};

enum {
  GML_SOFTWARE3D_MATRIX_VIEW=0,
  GML_SOFTWARE3D_MATRIX_PROJECTION=1,
  GML_SOFTWARE3D_MATRIX_TRANSFORM=2
};

enum {
  GML_SOFTWARE3D_STACK_CLEAR,
  GML_SOFTWARE3D_STACK_EMPTY,
  GML_SOFTWARE3D_STACK_PUSH,
  GML_SOFTWARE3D_STACK_POP,
  GML_SOFTWARE3D_STACK_TOP,
  GML_SOFTWARE3D_STACK_DISCARD
};

enum {
  GML_SOFTWARE3D_VERTEX_POSITION2=1,
  GML_SOFTWARE3D_VERTEX_POSITION3,
  GML_SOFTWARE3D_VERTEX_COLOR,
  GML_SOFTWARE3D_VERTEX_NORMAL,
  GML_SOFTWARE3D_VERTEX_TEXCOORD,
  GML_SOFTWARE3D_VERTEX_CUSTOM
};

typedef struct {
  double x,y,z,u,v;
  double r,g,b,alpha;
  double nx,ny,nz;
  int has_normal;
} GmlSoftware3DVertex;

typedef struct {
  int kind;
  int first;
  int count;
} GmlSoftware3DBatch;

#define GML_SOFTWARE3D_STATE_FLAG_COUNT 26
#define GML_SOFTWARE3D_STATE_VALUE_COUNT 616
#define GML_SOFTWARE3D_STATE_COLOR_COUNT 10
#define GML_SOFTWARE3D_MODEL_VERTEX_MAX 1048576

GmlSoftware3D *gml_software3d_create(void);
void gml_software3d_destroy(GmlSoftware3D *software3d);
void gml_software3d_reset(GmlSoftware3D *software3d);
int gml_software3d_status_get(const GmlSoftware3D *software3d,
                              GmlSoftware3DStatus *status);
void gml_software3d_set_classic(GmlSoftware3D *software3d,int classic);
void gml_software3d_begin(struct GmlRender *render,double x,double y,
                          double width,double height,double angle);
void gml_software3d_end(GmlSoftware3D *software3d);
void gml_software3d_control_update(GmlSoftware3D *software3d,
                                   const GmlSoftware3DControl *control,
                                   uint32_t mask);
int gml_software3d_light_define_point(GmlSoftware3D *software3d,int id,
                                      double x,double y,double z,double range,
                                      uint32_t color);
int gml_software3d_light_define_direction(GmlSoftware3D *software3d,int id,
                                          double x,double y,double z,
                                          uint32_t color);
int gml_software3d_light_enable(GmlSoftware3D *software3d,int id,int enabled);

void gml_software3d_matrix_identity(double matrix[16]);
double gml_software3d_dot(const double first[3],const double second[3]);
int gml_software3d_normalize(double vector[3]);
void gml_software3d_cross(const double first[3],const double second[3],
                          double output[3]);
void gml_software3d_matrix_multiply(const double left[16],
                                    const double right[16],double output[16]);
void gml_software3d_matrix_translation(double output[16],
                                       double x,double y,double z);
void gml_software3d_matrix_scaling(double output[16],
                                   double x,double y,double z);
void gml_software3d_matrix_rotation_axis(double output[16],
                                         double x,double y,double z,
                                         double degrees);
int gml_software3d_matrix_get(const GmlSoftware3D *software3d,int type,
                              double matrix[16]);
int gml_software3d_matrix_set(struct GmlRender *render,int type,
                              const double matrix[16]);
int gml_software3d_matrix_prepend(struct GmlRender *render,
                                  const double matrix[16]);
int gml_software3d_transform_stack(struct GmlRender *render,int action);

void gml_software3d_vertex_format_begin(GmlSoftware3D *software3d);
int gml_software3d_vertex_format_add(GmlSoftware3D *software3d,
                                     int kind,int type,int usage,
                                     int count,int bytes);
int gml_software3d_vertex_format_finish(GmlSoftware3D *software3d);
void gml_software3d_vertex_format_delete(GmlSoftware3D *software3d,int id);
int gml_software3d_vertex_buffer_create(GmlSoftware3D *software3d,
                                        int reserve_bytes);
void gml_software3d_vertex_buffer_delete(GmlSoftware3D *software3d,int id);
int gml_software3d_vertex_buffer_begin(GmlSoftware3D *software3d,
                                       int id,int format);
void gml_software3d_vertex_buffer_end(GmlSoftware3D *software3d,int id);
int gml_software3d_vertex_buffer_attribute(GmlSoftware3D *software3d,int id,
                                           int supplied_kind,
                                           const double value[4],int count);
int gml_software3d_vertex_buffer_attribute_ubyte4(
  GmlSoftware3D *software3d,int id,const double value[4]);
int gml_software3d_vertex_buffer_freeze(GmlSoftware3D *software3d,int id);
int gml_software3d_vertex_buffer_number(const GmlSoftware3D *software3d,int id);
size_t gml_software3d_vertex_buffer_size(const GmlSoftware3D *software3d,int id);
void gml_software3d_vertex_submit_buffer(struct GmlRender *render,int id,
                                         int primitive,int texture_handle,
                                         int first,int number);

void gml_software3d_primitive_2d_begin(GmlSoftware3D *software3d,
                                       int kind,int texture);
int gml_software3d_primitive_2d_append(GmlSoftware3D *software3d,
                                       double x,double y,double u,double v,
                                       uint32_t color,double alpha);
void gml_software3d_primitive_2d_end(struct GmlRender *render);
void gml_software3d_primitive_3d_begin(GmlSoftware3D *software3d,
                                       int kind,int texture);
int gml_software3d_primitive_3d_append(GmlSoftware3D *software3d,
                                       GmlSoftware3DVertex vertex);
void gml_software3d_primitive_3d_end(struct GmlRender *render);

void gml_software3d_draw_point_2d(struct GmlRender *render,double x,double y,
                                  uint32_t color,double alpha);
void gml_software3d_draw_line_2d(struct GmlRender *render,
                                 double x1,double y1,double x2,double y2,
                                 uint32_t color1,uint32_t color2,
                                 double alpha,double width);
void gml_software3d_draw_triangle_2d(struct GmlRender *render,
                                     const double points[3][2],
                                     const uint32_t colors[3],
                                     double alpha,int outline);
void gml_software3d_draw_rectangle_2d(struct GmlRender *render,
                                      double x1,double y1,double x2,double y2,
                                      const uint32_t colors[4],
                                      double alpha,int outline);
void gml_software3d_draw_ellipse_2d(struct GmlRender *render,
                                    double center_x,double center_y,
                                    double radius_x,double radius_y,
                                    uint32_t inner,uint32_t outer,
                                    double alpha,int outline);
void gml_software3d_draw_quad(struct GmlRender *render,
                              const double points[4][3],int texture,
                              double hrepeat,double vrepeat,uint32_t color,
                              int reverse_normal,int uv_mode);
void gml_software3d_set_camera(struct GmlRender *render,
                               double xfrom,double yfrom,double zfrom,
                               double xto,double yto,double zto,
                               double xup,double yup,double zup);
void gml_software3d_set_default_projection(struct GmlRender *render,
                                           double x,double y,
                                           double width,double height,
                                           double angle);
void gml_software3d_draw_ellipsoid(struct GmlRender *render,
                                   double x1,double y1,double z1,
                                   double x2,double y2,double z2,int texture,
                                   double hrepeat,double vrepeat,int steps,
                                   uint32_t color);

int gml_software3d_model_create(GmlSoftware3D *software3d);
int gml_software3d_model_destroy(GmlSoftware3D *software3d,int id);
int gml_software3d_model_clear(GmlSoftware3D *software3d,int id);
int gml_software3d_model_is_live(const GmlSoftware3D *software3d,int id);
int gml_software3d_model_begin(GmlSoftware3D *software3d,int id,int kind);
int gml_software3d_model_end(GmlSoftware3D *software3d,int id);
int gml_software3d_model_append_vertex(GmlSoftware3D *software3d,int id,
                                       GmlSoftware3DVertex vertex);
int gml_software3d_model_append_quad(GmlSoftware3D *software3d,int id,
                                     const double points[4][3],
                                     double hrepeat,double vrepeat);
int gml_software3d_model_add_shape(GmlSoftware3D *software3d,int id,int shape,
                                   double x1,double y1,double z1,
                                   double x2,double y2,double z2,
                                   double hrepeat,double vrepeat,
                                   int closed,int requested_steps);
void gml_software3d_model_draw(struct GmlRender *render,int id,
                               double x,double y,double z,int texture_handle);
int gml_software3d_model_info(const GmlSoftware3D *software3d,int id,
                              int *vertex_count,int *batch_count);
int gml_software3d_model_batch_get(const GmlSoftware3D *software3d,int id,
                                   int index,GmlSoftware3DBatch *batch);
int gml_software3d_model_vertex_get(const GmlSoftware3D *software3d,int id,
                                    int index,GmlSoftware3DVertex *vertex);

void gml_software3d_state_get(
  GmlSoftware3D *software3d,
  int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT],
  double values[GML_SOFTWARE3D_STATE_VALUE_COUNT],
  uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]);
void gml_software3d_state_set(
  GmlSoftware3D *software3d,
  const int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT],
  const double values[GML_SOFTWARE3D_STATE_VALUE_COUNT],
  const uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]);
size_t gml_d3_models_state_size(GmlSoftware3D *software3d);
int gml_d3_models_state_save(GmlSoftware3D *software3d,void *data,size_t capacity);
int gml_d3_models_state_load(GmlSoftware3D *software3d,const void *data,size_t size);

#endif
