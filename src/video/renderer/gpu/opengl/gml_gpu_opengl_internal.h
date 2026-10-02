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
  GL_READBACK_SLOTS=8,
  GL_SCENE_MAX_IMAGES=64,
  GL_SCENE_MAX_QUADS=2048,
  GL_SCENE_VERTEX_FLOATS=28
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
  uint32_t content_generation;
  uint64_t source_key;
  int scene_rgba;
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

typedef struct GlSceneImage {
  GmlRenderImageRef ref;
  GLuint texture,framebuffer;
  GLuint content_source_texture,content_source_framebuffer;
  uint32_t content_source_width,content_source_height;
  uint64_t content_source_version;
  uint32_t width,height,pixel_format;
  uint64_t last_used,context_epoch;
  int valid,target,rendered_origin,retired;
} GlSceneImage;

typedef struct GlSceneCheckpoint {
  GmlRenderImageRef image;
  GLuint texture;
  uint32_t width,height,pixel_format;
} GlSceneCheckpoint;

/* Ordinary exact composition and the two GLES font generation policies have distinct
 * programs. Uniform locations belong to their program, not to the current draw. */
typedef struct GlSceneExactProgram {
  GLuint exact_program;
  GLint exact_a_position,exact_a_texcoord,exact_a_colour;
  GLint exact_u_extent,exact_u_image,exact_u_previous,exact_u_region_origin;
  GLint exact_u_repetitions;
  GLint exact_u_blend_rule,exact_u_additive_alpha;
  GLint exact_u_effect_colour,exact_u_effect_intensity;
  GLint exact_u_effect_zoom,exact_u_effect_linear,exact_u_effect_seed;
  GLint exact_u_font_origin,exact_u_font_scale,exact_u_font_alpha;
  GLint exact_u_font_crop,exact_u_font_tint,exact_u_font_mode,exact_u_font_preserve_alpha;
  GLint exact_u_font_channel;
  GLint exact_u_underlay_size,exact_u_underlay_origin,exact_u_underlay_flags;
} GlSceneExactProgram;

typedef struct GlScene {
  GlSceneImage images[GL_SCENE_MAX_IMAGES];
  /* Exactly one bounded baseline for each owned frame-target class, never per surface. */
  GlSceneCheckpoint checkpoints[2];
  GLuint checkpoint_program,checkpoint_mask,checkpoint_framebuffer;
  GLint checkpoint_u_image,checkpoint_u_previous,checkpoint_u_alpha;
  uint32_t checkpoint_mask_width,checkpoint_mask_height;
  int checkpoint_program_failed;
  GLuint program,vertex_buffer,vertex_array;
  GLint a_position,a_texcoord,a_colour,a_clamp,a_map,a_step,a_margin;
  GLint a_span_x,a_span_y,a_span_active;
  GLint u_extent,u_image,u_filter;
  GlSceneExactProgram exact_programs[3];
  GLuint snapshot_texture,rgb_query;
  uint32_t snapshot_width,snapshot_height;
  GLint max_texture_units;
  float *vertices;
  uint8_t *scratch;
  size_t scratch_capacity;
  uint64_t use_clock,context_epoch;
  GLint max_texture_size;
  int supports_sample_mask;
  int available;
} GlScene;

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
  GLint u_source_height,u_resident_source,u_rgb_query;
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
  GlScene scene;
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
int gml_gpu_opengl_scene_program(GmlGpuBackend *backend,char *error,size_t error_capacity);
GlSceneExactProgram *gml_gpu_opengl_scene_exact_program(GmlGpuBackend *backend,int font_mode,
                                                       char *error,size_t error_capacity);
int gml_gpu_opengl_checkpoint_program(GmlGpuBackend *backend);
int gml_gpu_opengl_execute_shader_draw(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                        const GmlPlanOp *op,GmlGpuCounters *counters,
                                       char *error,size_t error_capacity);
int gml_gpu_opengl_scene_shader_draw(GmlGpuBackend *backend,const GmlPlanShader *shader,
                                      const GmlRenderDrawStateSnapshot *state,
                                      GlSceneImage *target,GmlRenderRect region,
                                      GlSceneImage *source,
                                      GlSceneImage *const *samplers,uint32_t sampler_count,
                                      int source_over,
                                      char *error,size_t error_capacity);
int gml_gpu_opengl_bind_readback_target(GmlGpuBackend *backend,uint32_t width,uint32_t height,
                                         char *error,size_t error_capacity);
int gml_gpu_opengl_read_back_target(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                     char *error,size_t error_capacity);
int gml_gpu_opengl_read_back_target_pipelined(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                               char *error,size_t error_capacity);
void gml_gpu_opengl_scene_forget(GmlGpuBackend *backend);
void gml_gpu_opengl_scene_destroy(GmlGpuBackend *backend,int context_is_current);
void gml_gpu_opengl_scene_reset(GmlGpuBackend *backend,const GmlGpuContext *context);
int gml_gpu_opengl_scene_capabilities(const GmlGpuBackend *backend,GmlRenderDeviceCaps *caps);
int gml_gpu_opengl_scene_upload(GmlGpuBackend *backend,const GmlRenderImageUpload *transfer,
                                int *did_upload);
int gml_gpu_opengl_scene_readback(GmlGpuBackend *backend,const GmlRenderImageReadback *transfer);
void gml_gpu_opengl_scene_retire(GmlGpuBackend *backend,GmlRenderImageRef image);
int gml_gpu_opengl_scene_flush(GmlGpuBackend *backend);
int gml_gpu_opengl_scene_wait(GmlGpuBackend *backend);
int gml_render_gpu_opengl_clear(GmlGpuBackend *backend,const GmlRenderDrawOp *op,
                                GlSceneImage *target);
int gml_render_gpu_opengl_draw_primitive(GmlGpuBackend *backend,
                                         const GmlRenderDrawOp *op,GlSceneImage *target);
int gml_render_gpu_opengl_draw_textured_quad(const GmlRenderDrawOp *op,
                                             const GmlRenderDrawStateSnapshot *state,
                                             uint32_t source_width,uint32_t source_height,
                                             int rendered_source_origin,
                                             float *vertices);
int gml_render_gpu_opengl_apply_effect(GmlGpuBackend *backend,
                                       const GmlRenderDrawOp *operation,
                                       const GmlRenderDrawStateSnapshot *state,
                                       GlSceneImage *target,GlSceneImage *source,
                                       char *error,size_t error_capacity);
GlSceneImage *gml_gpu_opengl_scene_image(GmlGpuBackend *backend,GmlRenderImageRef image);
GlSceneImage *gml_gpu_opengl_scene_target(GmlGpuBackend *backend,const GmlRenderTargetRef *target);
GLuint gml_gpu_opengl_scene_content_source(GmlGpuBackend *backend,GlSceneImage *source);
GmlRenderExecutionResult gml_gpu_opengl_scene_submit(GmlGpuBackend *backend,
                                                       const GmlRenderDrawBatch *batch,
                                                       uintptr_t frontend_framebuffer,
                                                       char *error,size_t error_capacity);

#endif
