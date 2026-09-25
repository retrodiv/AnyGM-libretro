/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_OPENGL_INTERNAL_H
#define GML_GPU_OPENGL_INTERNAL_H

#include "gml_gpu_internal.h"
#include "gml_gpu_opengl_api.h"

enum {
  GL_MAP_SLOTS=GML_PLAN_MAX_OPERATIONS*2,
  GL_INFO_LOG_LIMIT=512,
  GL_CONTENT_PROGRAMS=16,
  GL_CONTENT_UNIFORMS=64,
  GL_READBACK_SLOTS=8
};

/* One uniform the content's program declares, as the context reports it after linking. A value
 * the content sets is delivered through the setter this type calls for, whatever width the content
 * supplied: sending a vector to a scalar, or a matrix through a vector setter, is an error the
 * context refuses the whole draw over. */
typedef struct GlContentUniform {
  char name[48];
  GLint location;
  GLint size;
  GLenum type;
} GlContentUniform;

/* One pipelined read-back: two buffer objects the device fills in turn. A pass reads what the
 * previous pass of the same program left — one frame old — and issues the current one without
 * waiting, which is what removes the stall. A slot is primed by one ordinary read the first time
 * it is used, so the first frame is correct rather than empty. */
typedef struct GlReadbackSlot {
  uint32_t identity;
  uint32_t width,height;
  GLuint buffer[2];
  int cursor;
  int primed;
  uint32_t last_used;
} GlReadbackSlot;

/* A content program compiled for this context, keyed by the shader's identity and the text it was
 * compiled from. `failed` retires a program the context refused, so a frame does not pay the
 * refusal again and the runtime can answer that the shader did not compile. */
typedef struct GlContentProgram {
  GLuint program;
  uint32_t identity;
  const char *fragment_key;
  int failed;
  uint32_t last_used;
  GLint a_position,a_colour,a_texcoord;
  GLint u_matrices,u_base_texture,u_alpha_test,u_fog_ps,u_fog_vs;
  GlContentUniform uniforms[GL_CONTENT_UNIFORMS];
  uint32_t uniform_count;
} GlContentProgram;

typedef struct GlSourceTexture {
  GLuint name;
  int linear;
  uint32_t image_class;
  uint32_t identity;
  uint32_t content_generation;
  uint32_t pixel_generation;
  uint32_t width;
  uint32_t height;
  uint32_t last_used;
  int uploaded;
} GlSourceTexture;

/* An axis map is a pure function of the axis record, so the record is also its cache key. Keeping
 * the key instead of the contents makes the comparison a handful of scalars rather than several
 * kilobytes, and it cannot disagree with the map it describes. */
typedef struct GlAxisMap {
  GLuint name;
  uint32_t capacity;
  uint32_t count;
  uint32_t rule;
  uint32_t source_extent;
  uint32_t destination_extent;
  int32_t destination_first;
  double origin;
  double extent;
  int valid;
} GlAxisMap;

struct GmlGpuBackend {
  GmlGlApi gl;
  int loaded;
  GmlGpuApi api;
  int little_endian;
  GLint max_texture_size;
  GLuint program;
  GLuint vertex_array;
  GLint u_source;
  GLint u_map_x;
  GLint u_map_y;
  GLint u_dest_x;
  GLint u_dest_y;
  GLint u_target_height;
  GlSourceTexture sources[GML_PLAN_MAX_IMAGES];
  GlAxisMap maps[GL_MAP_SLOTS];
  uint16_t *map_scratch;
  uint32_t map_scratch_capacity;
  uint32_t use_clock;
  GlContentProgram content[GL_CONTENT_PROGRAMS];
  GLuint content_buffer;
  /* The off-screen target a read-back plan executes into, kept at the last size asked for. */
  GLuint readback_framebuffer,readback_texture,readback_attached;
  uint32_t readback_width,readback_height;
  uint8_t *readback_scratch;
  size_t readback_scratch_capacity;
  GlReadbackSlot readback_slots[GL_READBACK_SLOTS];
  int readback_pipelined;
};

int gml_gpu_opengl_upload_source(GmlGpuBackend *backend,const GmlPlanImage *image,
                                  GmlGpuCounters *counters,GLuint *out_name,int linear);
int gml_gpu_opengl_upload_axis(GmlGpuBackend *backend,uint32_t slot_index,
                                const GmlPlanAxis *axis,uint32_t count,
                                GmlGpuCounters *counters,GLuint *out_name);
void gml_gpu_opengl_record_error(char *error,size_t capacity,const char *format,...);
void gml_gpu_opengl_set_rect(GmlGpuBackend *backend,const GmlPlanRect *rect,
                              uint32_t target_height);
int gml_gpu_opengl_build_program(GmlGpuBackend *backend,char *error,size_t error_capacity);
int gml_gpu_opengl_execute_shader_draw(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                        const GmlPlanOp *op,GmlGpuCounters *counters,
                                        char *error,size_t error_capacity);

#endif
