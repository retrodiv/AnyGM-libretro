/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_OPENGL_INTERNAL_H
#define GML_GPU_OPENGL_INTERNAL_H

#include "gml_gpu_internal.h"
#include "gml_gpu_opengl_api.h"
#include "gml_gpu_opengl_variants.h"

enum {
  GL_PLAN_MAP_SLOTS=GML_PLAN_MAX_OPERATIONS*2,
  GL_MAP_SLOTS=GL_PLAN_MAP_SLOTS+2,
  GL_INFO_LOG_LIMIT=512,
  GL_CONTENT_PROGRAMS=16,
  GL_CONTENT_UNIFORMS=64,
  GL_READBACK_SLOTS=8,
  GL_SCENE_MAX_IMAGES=64,
  /* Two fixed frame-target baselines, then numbered surfaces sharing one byte budget. */
  GL_SCENE_FRAME_CHECKPOINTS=2,
  GL_SCENE_CHECKPOINTS=8,
  GL_SCENE_SURFACE_CHECKPOINT_BYTES=32*1024*1024,
  /* Below this a direct pixel read costs less than a device copy and a buffer map. */
  GL_SCENE_LINEAR_READ_MIN_BYTES=256*1024,
  GL_SCENE_MAX_QUADS=2048,
  GL_SCENE_WHITE_TEXELS=16384,
  GL_SCENE_STAGING_MIN_BYTES=64*1024,
  GL_SCENE_STAGING_MAX_BYTES=16*1024*1024,
  GL_SCENE_NATIVE_POINT_FILTER=171,
  GL_SCENE_NATIVE_POINT_OPAQUE_FILTER=172,
  GL_SCENE_NATIVE_POINT_STAMP_FILTER=173,
  GL_SCENE_VIEW_SDF_FILTER=174,
  GL_SCENE_VIEW_SDF_OPAQUE_FILTER=175,
  GL_SCENE_VIEW_SDF_STAMP_FILTER=176,
  GL_SCENE_NATIVE_TRIANGLE_SOURCE_ALPHA_FILTER=177,
  GL_SCENE_VIEW_BILINEAR_FILTER=178,
  GL_SCENE_VIEW_BILINEAR_SOURCE_ALPHA_FILTER=179,
  GL_SCENE_NATIVE_TEXTURE_NEAREST_FILTER=180,
  GL_SCENE_NATIVE_TEXTURE_LINEAR_FILTER=181,
  GL_SCENE_NATIVE_TEXTURE_STAMP_NEAREST_FILTER=182,
  GL_SCENE_NATIVE_TEXTURE_STAMP_LINEAR_FILTER=183,
  GL_SCENE_NATIVE_TEXTURE_SURFACE_NEAREST_FILTER=184,
  GL_SCENE_NATIVE_TEXTURE_SURFACE_LINEAR_FILTER=185,
  GL_SCENE_AFFINE_SURFACE_POINT_FILTER=186,
  GL_SCENE_AFFINE_SURFACE_POINT_OPAQUE_FILTER=187,
  GL_SCENE_AFFINE_SURFACE_POINT_STAMP_FILTER=188,
  GL_SCENE_BYTE_POSITIVE_FILTER_FIRST=189,
  GL_SCENE_BYTE_POSITIVE_FILTER_LAST=200,
  GL_SCENE_BYTE_POSITIVE_FILTER_OFFSET=42,
  GL_SCENE_RAW_POSITIVE_FILTER_FIRST=201,
  GL_SCENE_RAW_POSITIVE_FILTER_LAST=204,
  GL_SCENE_RAW_POSITIVE_FILTER_OFFSET=122,
  GL_SCENE_UNIT_BACKGROUND_FILTER_FIRST=205,
  GL_SCENE_UNIT_BACKGROUND_FILTER_LAST=214,
  GL_SCENE_UNIT_BACKGROUND_FILTER_OFFSET=140,
  GL_SCENE_SURFACE_CUTOFF_FILTER=215,
  GL_SCENE_SURFACE_CUTOFF_STAMP_FILTER=216,
  GL_SCENE_SURFACE_CUTOFF_PRESERVING_FILTER=217,
  GL_SCENE_VERTEX_FLOATS=28,
  GL_SCENE_INSTANCE_FLOATS=52,
  GL_SCENE_INSTANCE_SPANS=128,
  GL_SCENE_STREAM_BYTES=20*GL_SCENE_MAX_QUADS*GL_SCENE_INSTANCE_FLOATS*sizeof(float)
};

_Static_assert(GL_SCENE_BYTE_POSITIVE_FILTER_FIRST==GL_SCENE_AFFINE_SURFACE_POINT_STAMP_FILTER+1 &&
               GL_SCENE_BYTE_POSITIVE_FILTER_FIRST-147==GL_SCENE_BYTE_POSITIVE_FILTER_OFFSET &&
               GL_SCENE_BYTE_POSITIVE_FILTER_LAST-158==GL_SCENE_BYTE_POSITIVE_FILTER_OFFSET,
               "Byte-positive selectors append a separate hardware byte range");
_Static_assert(GL_SCENE_RAW_POSITIVE_FILTER_FIRST==GL_SCENE_BYTE_POSITIVE_FILTER_LAST+1 &&
               GL_SCENE_RAW_POSITIVE_FILTER_FIRST-79==GL_SCENE_RAW_POSITIVE_FILTER_OFFSET &&
               GL_SCENE_RAW_POSITIVE_FILTER_LAST-82==GL_SCENE_RAW_POSITIVE_FILTER_OFFSET,
               "Raw-positive selectors append a continuous hardware range");
_Static_assert(GL_SCENE_UNIT_BACKGROUND_FILTER_FIRST==GL_SCENE_RAW_POSITIVE_FILTER_LAST+1 &&
               GL_SCENE_UNIT_BACKGROUND_FILTER_FIRST-65==GL_SCENE_UNIT_BACKGROUND_FILTER_OFFSET &&
               GL_SCENE_UNIT_BACKGROUND_FILTER_LAST-74==GL_SCENE_UNIT_BACKGROUND_FILTER_OFFSET,
               "Unit background selectors preserve the established colour programs");

_Static_assert(GL_SCENE_NATIVE_POINT_STAMP_FILTER<GL_SCENE_VIEW_SDF_FILTER &&
               GL_SCENE_VIEW_SDF_FILTER==174 && GL_SCENE_VIEW_SDF_OPAQUE_FILTER==175 &&
               GL_SCENE_VIEW_SDF_STAMP_FILTER==176 &&
               GL_SCENE_VIEW_SDF_STAMP_FILTER<GL_SCENE_NATIVE_TEXTURE_NEAREST_FILTER,
               "Selected-view distance selectors must be disjoint and match the shader range");
_Static_assert(GL_SCENE_VIEW_SDF_STAMP_FILTER<GL_SCENE_NATIVE_TRIANGLE_SOURCE_ALPHA_FILTER &&
               GL_SCENE_NATIVE_TRIANGLE_SOURCE_ALPHA_FILTER==177 &&
               GL_SCENE_NATIVE_TRIANGLE_SOURCE_ALPHA_FILTER<GL_SCENE_NATIVE_TEXTURE_NEAREST_FILTER,
               "Native triangle alpha selector must be disjoint and match the shader");
_Static_assert(GL_SCENE_NATIVE_POINT_STAMP_FILTER<GL_SCENE_NATIVE_TEXTURE_NEAREST_FILTER,
               "Native texture selectors must not alias the point sampler");
_Static_assert(GL_SCENE_NATIVE_TRIANGLE_SOURCE_ALPHA_FILTER<GL_SCENE_VIEW_BILINEAR_FILTER &&
               GL_SCENE_VIEW_BILINEAR_FILTER==178 &&
               GL_SCENE_VIEW_BILINEAR_SOURCE_ALPHA_FILTER==179 &&
               GL_SCENE_VIEW_BILINEAR_SOURCE_ALPHA_FILTER<GL_SCENE_NATIVE_TEXTURE_NEAREST_FILTER,
               "Selected-view bilinear selectors must be disjoint and match the shader");
_Static_assert(GL_SCENE_NATIVE_TEXTURE_NEAREST_FILTER==180 &&
               GL_SCENE_NATIVE_TEXTURE_SURFACE_LINEAR_FILTER==185,
               "Native texture selectors must match the scene shader range");

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
 * previous matching draw left, never another picture drawn with the same program. A slot is
 * primed by one ordinary read the first time it is used. */
typedef struct GlReadbackSlot {
  uint64_t draw_key;
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
  uint32_t intermediate_extent;
  int valid;
} GlAxisMap;

typedef struct GlSceneImage {
  GmlRenderImageRef ref;
  GLuint texture,framebuffer;
  GLuint depth_renderbuffer;
  GLuint triangle_texture,triangle_framebuffer;
  /* Front is published; the other plane is disposable submission staging.
   * Both names share this image's storage and context lifetime. */
  GLuint raw_depth_texture[2],raw_depth_framebuffer[2];
  uint64_t raw_depth_epoch,raw_depth_version;
  unsigned raw_depth_front;
  GLuint content_source_texture,content_source_framebuffer;
  uint32_t content_source_width,content_source_height;
  uint64_t content_source_version;
  uint32_t width,height,pixel_format;
  uint64_t last_used,context_epoch;
  int valid,target,rendered_origin,retired;
  int framebuffer_complete;
  int alpha_opaque; /* derived proof that every stored alpha byte is 255 */
  int alpha_nonzero; /* derived proof that no stored alpha byte is 0; implied by alpha_opaque */
  /* Derived proof that every stored pixel holds uniform_colour (host ARGB words), valid only
   * while the image still carries uniform_ref: any later write advances the reference. */
  int uniform_known;
  uint32_t uniform_colour;
  GmlRenderImageRef uniform_ref;
  /* Conservative top-origin bounds of every possibly nonzero alpha byte.
   * This context-local certificate is discarded by untracked image writers. */
  int alpha_bounds_known;
  int32_t alpha_bounds[4];
  int alpha_tiles_full;
  uint32_t alpha_tiles[32];
  int opaque_white_known;
  uint8_t opaque_white[GL_SCENE_WHITE_TEXELS/8];
  int source_partial;
  GmlRenderRect source_region;
  uint64_t source_upload_bytes;
  /* Complete storage for a partial atlas page, defined a band of rows at a time while the
   * partial texture serves draws. It replaces texture once every row is defined. */
  GLuint completion_texture;
  uint32_t completion_rows;
} GlSceneImage;

void gml_gpu_opengl_alpha_cover(GlSceneImage *image,double left,double top,
                               double right,double bottom);
int gml_gpu_opengl_surface_alpha_scissor(GmlGpuBackend *backend,GlSceneImage *target,
  const GlSceneImage *source,uint32_t filter,uint32_t count,uint32_t source_alpha_key,
  const GmlRenderSourcePalette *palette);
unsigned gml_gpu_opengl_alpha_regions(const GlSceneImage *source,GmlRenderRect regions[32]);

typedef struct GlSceneCheckpoint {
  GmlRenderImageRef image;
  GLuint texture;
  uint32_t width,height,pixel_format;
  uint64_t last_used;
  /* The baseline was copied from an image proved uniform in this colour. */
  int uniform_known;
  uint32_t uniform_colour;
} GlSceneCheckpoint;

/* Uniform locations belong to their program, not to the current draw. GLES normally shares
 * one combined exact-font program and retains the split generation programs as a compile fallback. */
typedef struct GlSceneExactProgram {
  GLuint exact_program;
  GLint exact_a_position,exact_a_texcoord,exact_a_colour;
  GLint exact_u_extent,exact_u_image,exact_u_previous,exact_u_region_origin;
  GLint exact_u_repetitions;
  GLint exact_u_blend_rule,exact_u_additive_alpha;
  GLint exact_u_effect_colour,exact_u_effect_intensity;
  GLint exact_u_effect_zoom,exact_u_effect_linear,exact_u_effect_seed;
  GLint exact_u_effect_sampler,exact_u_effect_parameters,exact_u_effect_shade,exact_u_effect_add;
  GLint exact_u_font_origin,exact_u_font_scale,exact_u_font_alpha;
  GLint exact_u_font_crop,exact_u_font_tint,exact_u_font_mode,exact_u_font_preserve_alpha;
  GLint exact_u_font_channel,exact_u_font_margin;
  GLint exact_u_underlay_size,exact_u_underlay_origin,exact_u_underlay_flags;
  GLint exact_u_surface_rule,exact_u_surface_tint,exact_u_surface_alpha;
} GlSceneExactProgram;

typedef struct GlSceneProgram {
  GLuint program;
  GLint a_position01,a_position23,a_texcoord01,a_texcoord23;
  GLint a_colour[4],a_clamp,a_map,a_step_margin_active,a_span_x,a_span_y;
  GLint u_extent,u_image,u_filter,u_additive_bytes,u_alpha_test,u_source_alpha_key,u_previous,u_region_origin;
  GLint u_source_palette_enabled,u_source_palette_red,u_source_palette_green[2],u_source_palette_colour;
  GLint u_rotated_alpha16;
  GLint u_point_map_x,u_point_map_y,u_point_origin;
  GLint u_recovery_screen,u_recovery_application,u_recovery_rect;
  double rotated_alpha16_value;
  int rotated_alpha16_valid;
  int rotated_alpha16_modern_nearest;
  int identity_surface_body;
} GlSceneProgram;

typedef struct GlDepthTriangleProgram {
  GLuint program;
  GLint extent,depth,colour,flags,policy,source,flat_uv,input_alpha,shade,fog_colour,fog_range;
  GLint alpha_coordinates,alpha_fraction,alpha_weights;
} GlDepthTriangleProgram;

typedef struct GlEffectSampleImage {
  GLuint texture,framebuffer,read_framebuffer;
  uint32_t width,height;
  GLenum format;
  GmlRenderImageRef source;
  GmlRenderLayerSampler layout;
  int valid;
} GlEffectSampleImage;

/* Disposable whole-image neighbourhood-pass resources, shared by layer filters. */
typedef struct GlLayerPass {
  GLuint program;
  GLint source,prefix,size,axis,mode,step,radius,flip;
  int failed;
  GlEffectSampleImage images[6];
} GlLayerPass;

/* Private glow accumulator and paired maximum-reduction images. */
typedef struct GlLayerGlow {
  GLuint program;
  GLint source,halo,parent,peaks,size,mode,source_flip,halo_flip;
  GLint alpha,gamma,scale,preserve;
  int failed,active;
  GlEffectSampleImage images[4];
} GlLayerGlow;

typedef struct GlScene {
  GlSceneImage images[GL_SCENE_MAX_IMAGES];
  /* One bounded baseline for each owned frame-target class, then numbered surfaces, least
   * recently checkpointed first out, within GL_SCENE_SURFACE_CHECKPOINT_BYTES. */
  GlSceneCheckpoint checkpoints[GL_SCENE_CHECKPOINTS];
  GLuint checkpoint_program,checkpoint_mask,checkpoint_framebuffer;
  /* The first comparison pass: one texel for every 32 pixels of a row. */
  GLuint checkpoint_rows,checkpoint_rows_framebuffer;
  GLint checkpoint_u_image,checkpoint_u_previous,checkpoint_u_alpha;
  GLint checkpoint_u_pass,checkpoint_u_extent;
  uint32_t checkpoint_mask_width,checkpoint_mask_height;
  uint32_t checkpoint_rows_width,checkpoint_rows_height;
  int checkpoint_program_failed;
  /* Device-written linear pixel words, for reads a tiled device would otherwise untile on the
   * host. Refused once, it stays refused until the context is replaced. */
  GLuint linear_program,linear_buffer;
  GLint linear_u_image,linear_u_region,linear_u_format;
  size_t linear_capacity;
  int linear_failed;
  GlSceneProgram main_program,filtered_program,compact_programs[GL_SCENE_COMPACT_PROGRAM_COUNT];
  GLuint vertex_buffer,vertex_array,instance_buffer,instance_array;
  GlDepthTriangleProgram depth_triangle_program[2];
  GLuint depth_triangle_buffer,depth_triangle_array;
  GLuint upload_buffer;
  size_t upload_capacity;
  /* Zero is unmeasured, positive retains staging, negative selects client bytes. */
  int upload_policy;
  GmlGpuTimeFn upload_clock;
  void *upload_clock_userdata;
  uint64_t upload_client_ns,upload_staged_ns;
  /* One completion band per offered frame; completed bytes await the counters. */
  int completion_budget;
  uint64_t completion_uploads,completion_bytes;
  size_t instance_bytes_used;
  int instance_storage_ready;
  /* Byte-identical instance ranges share only their immutable stream storage, not draw state. */
  struct { size_t offset,bytes; uint64_t hash; } instance_spans[GL_SCENE_INSTANCE_SPANS];
  unsigned instance_span_count,instance_span_next;
  uint8_t *instance_mirror;
  int instance_reuse_attempted;
  GlSceneExactProgram exact_programs[3];
  GLuint snapshot_texture,copy_framebuffer,rgb_query;
  /* Disposable sampling images; public resources keep their actual byte format. */
  GlEffectSampleImage effect_sampler,effect_capture;
  GlLayerPass layer_pass;
  GlLayerGlow layer_glow;
  /* Two reusable reduction planes and two independent final predicates. No host observation. */
  GLuint rgb_reduce_program,rgb_reduce_framebuffer,rgb_reduce_texture[4];
  uint32_t rgb_reduce_width[4],rgb_reduce_height[4];
  GLint rgb_reduce_u_image,rgb_reduce_u_extent;
  GLuint recovery_indirect_program,recovery_indirect_buffer;
  GLint recovery_indirect_u_screen,recovery_indirect_u_application;
  int supports_recovery_indirect,recovery_indirect_active,recovery_indirect_failed;
  uint32_t snapshot_width,snapshot_height;
  GLint max_texture_units;
  float *vertices,*instances;
  uint8_t *scratch;
  size_t scratch_capacity;
  uint64_t use_clock,context_epoch;
  GLint max_texture_size;
  int supports_sample_mask;
  int exact_font_split;
  int font_write_flush;
  int available;
} GlScene;

enum { GL_SCENE_NATIVE_PLAIN=1,GL_SCENE_NATIVE_WHITE_TINT=2,GL_SCENE_NATIVE_ZERO_ALPHA_BLEND=4,
       GL_SCENE_NATIVE_EQUAL_EXTENT=8 };

typedef struct GlFrameSnapshot {
  GLuint framebuffer,texture;
  uint32_t width,height;
  uint64_t generation;
  int valid,attached;
} GlFrameSnapshot;

struct GmlGpuBackend {
  uint64_t target_attachments,target_reuses,white_unit_clears,white_tint_quads,zero_alpha_blend_quads,identity_map_draws,bounded_surface_draws;
  uint64_t host_extent_passes;
  GmlGlApi gl;
  int loaded;
  GmlGpuApi api;
  int little_endian;
  GLint max_texture_size;
  GLuint program;
  GLuint vertex_array;
  int presentation_prepared;
  GLint u_source;
  GLint u_map_x;
  GLint u_map_y;
  GLint u_dest_x;
  GLint u_dest_y;
  GLint u_target_height;
  GLint u_source_height,u_resident_source,u_rgb_query;
  GLint u_filter_destination;
  GLint u_sharp_scale,u_area_filter,u_rotation;
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
  int readback_present_valid;
  uint8_t *readback_scratch;
  size_t readback_scratch_capacity;
  GlReadbackSlot readback_slots[GL_READBACK_SLOTS];
  int readback_pipelined;
  /* Completed transport pixels must outlive the borrowed frontend target and ordinary
   * content readback plans. Only metadata is retained on CPU until observation. */
  GlFrameSnapshot completed;
  /* An adapter's failure frame is the final image already delivered to its frontend. */
  GlFrameSnapshot delivered;
  /* One unused R8 attachment bounds tiled presentation work; never content or state. */
  GLuint host_extent_renderbuffer;
  uint32_t host_extent_width,host_extent_height;
  GlScene scene;
};

int gml_gpu_opengl_upload_source(GmlGpuBackend *backend,const GmlPlanImage *image,
                                  GmlGpuCounters *counters,GLuint *out_name,int linear);
int gml_gpu_opengl_upload_axis(GmlGpuBackend *backend,uint32_t slot_index,
                                const GmlPlanAxis *axis,uint32_t count,
                                GmlGpuCounters *counters,GLuint *out_name);
int gml_gpu_opengl_upload_point_axis(GmlGpuBackend *backend,unsigned axis,
                                    double anchor,double phase,double scale,
                                    int first,uint32_t count,uint32_t source_extent,
                                    GLuint *out_name);
void gml_gpu_opengl_record_error(char *error,size_t capacity,const char *format,...);
void gml_gpu_opengl_set_rect(GmlGpuBackend *backend,const GmlPlanRect *rect,
                              uint32_t target_height);
int gml_gpu_opengl_build_program(GmlGpuBackend *backend,char *error,size_t error_capacity);
GlSceneProgram *gml_gpu_opengl_scene_draw_program(GmlGpuBackend *backend,uint32_t filter,int native_flags,
  char *error,size_t error_capacity);
int gml_gpu_opengl_scene_program(GmlGpuBackend *backend,char *error,size_t error_capacity);
/* Shared device mappings for captured independent blend state. Callers
 * validate factors/equations before preparing any destination writes. */
static inline GLenum gml_gpu_opengl_native_factor(unsigned factor){
  static const GLenum values[]={0,GL_ZERO,GL_ONE,GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,
    GL_SRC_COLOR,GL_DST_COLOR,GL_DST_ALPHA,GL_ONE_MINUS_SRC_COLOR,
    GL_ONE_MINUS_DST_ALPHA,GL_ONE_MINUS_DST_COLOR,0x0308u};
  return factor>=1u && factor<=11u?values[factor]:0xffffffffu;
}
static inline GLenum gml_gpu_opengl_native_equation(unsigned equation){
  static const GLenum values[]={GL_FUNC_ADD,GL_MAX,GL_FUNC_REVERSE_SUBTRACT,GL_FUNC_SUBTRACT,GL_MIN};
  return equation<5u?values[equation]:0xffffffffu;
}

int gml_gpu_opengl_depth_triangle_programs(GmlGpuBackend *backend,char *error,size_t error_capacity);
int gml_gpu_opengl_depth_triangle_valid(GmlGpuBackend *backend,const GmlRenderDrawBatch *batch);
int gml_render_gpu_opengl_depth_triangle(GmlGpuBackend *backend,const GmlRenderDrawBatch *batch,
                                   uintptr_t frontend_framebuffer,char *error,size_t error_capacity);
unsigned gml_gpu_opengl_prepare_fixed_scene_draws(GmlGpuBackend *backend,GLuint source_texture);
/* Zero leaves the driver-clear path available; negative means an emitted draw failed. */
int gml_gpu_opengl_fill_region_quad(GmlGpuBackend *backend,GlSceneImage *target,
                                   const GmlRenderRect *region,uint32_t colour);
int gml_gpu_opengl_prepare_automatic_draw(GmlGpuBackend *backend,GLuint source_texture,
                                          char *error,size_t error_capacity);
GlSceneExactProgram *gml_gpu_opengl_scene_exact_program(GmlGpuBackend *backend,int font_mode,
                                                       char *error,size_t error_capacity);
int gml_gpu_opengl_checkpoint_program(GmlGpuBackend *backend);
int gml_gpu_opengl_linear_read_program(GmlGpuBackend *backend);
int gml_gpu_opengl_rgb_reduce_program(GmlGpuBackend *backend);
int gml_gpu_opengl_recovery_indirect_program(GmlGpuBackend *backend);
int gml_gpu_opengl_scene_reduce_rgb(GmlGpuBackend *backend,GlSceneImage *source,
                                    unsigned predicate);
uint64_t gml_gpu_opengl_content_source_key(const GmlPlanShader *shader);
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
int gml_gpu_opengl_compose_shader_output(GmlGpuBackend *backend,GlSceneImage *target,
                                         GmlRenderRect region);
int gml_gpu_opengl_bind_readback_target(GmlGpuBackend *backend,uint32_t width,uint32_t height,
                                         char *error,size_t error_capacity);
int gml_render_gpu_opengl_present_transform(GmlGpuBackend *backend,uintptr_t framebuffer,
  uint32_t width,uint32_t height,uint64_t generation,int dx,int dy,unsigned rotation,int scratch,
  GmlGpuCounters *counters,char *error,size_t error_capacity);
int gml_gpu_opengl_read_back_target(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                     char *error,size_t error_capacity);
int gml_gpu_opengl_read_back_target_pipelined(GmlGpuBackend *backend,GmlRenderPlan *plan,
                                               char *error,size_t error_capacity);
void gml_gpu_opengl_scene_forget(GmlGpuBackend *backend);
void gml_gpu_opengl_scene_destroy(GmlGpuBackend *backend,int context_is_current);
int gml_gpu_opengl_layer_glow_program(GmlGpuBackend *backend,char *error,size_t capacity);
int gml_gpu_opengl_layer_pass_program(GmlGpuBackend *backend,char *error,size_t capacity);
void gml_gpu_opengl_effect_images_release(GmlGpuBackend *backend,GlEffectSampleImage *images,
                                         unsigned count);

void gml_gpu_opengl_scene_reset(GmlGpuBackend *backend,const GmlGpuContext *context);
void gml_gpu_opengl_scene_prepare_uploads(GmlGpuBackend *backend);
int gml_gpu_opengl_scene_capabilities(const GmlGpuBackend *backend,GmlRenderDeviceCaps *caps);
int gml_gpu_opengl_scene_upload_source_region(GmlGpuBackend *backend,
  const GmlRenderImageUpload *full,GmlRenderRect region,uint64_t *uploaded_bytes);
/* Grants the next offered frame its completion band and returns the completion uploads and
 * bytes defined since the previous call. */
void gml_gpu_opengl_scene_begin_frame(GmlGpuBackend *backend,uint64_t *completion_uploads,
                                      uint64_t *completion_bytes);
int gml_gpu_opengl_scene_source_region_ready(const GlSceneImage *image,
  const GmlRenderDrawOp *operation);
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
int gml_gpu_opengl_clamped_background_valid(const GmlRenderDrawOp *op,
                                        const GmlRenderDrawStateSnapshot *state,
                                        uint32_t width,uint32_t height);
int gml_gpu_opengl_clamped_atlas_valid(const GmlRenderDrawOp *op,
                                      uint32_t source_width,uint32_t source_height);
int gml_gpu_opengl_half_gui_point_valid(const GmlRenderDrawOp *op,
                                        const GmlRenderDrawStateSnapshot *state,
                                        uint32_t source_width,uint32_t source_height);
int gml_render_gpu_opengl_apply_effect(GmlGpuBackend *backend,
                                       const GmlRenderDrawOp *operation,
                                       const GmlRenderDrawStateSnapshot *state,
                                       GlSceneImage *target,GlSceneImage *source,
                                       char *error,size_t error_capacity);
GlSceneImage *gml_gpu_opengl_scene_image(GmlGpuBackend *backend,GmlRenderImageRef image);
GlSceneImage *gml_gpu_opengl_scene_target(GmlGpuBackend *backend,const GmlRenderTargetRef *target);
GLuint gml_gpu_opengl_scene_content_source(GmlGpuBackend *backend,GlSceneImage *source);
int gml_render_gpu_opengl_copy_image(GmlGpuBackend *backend,const GmlRenderDrawOp *operation,
                                      GlSceneImage *target,GlSceneImage *source);
GmlRenderExecutionResult gml_gpu_opengl_scene_submit(GmlGpuBackend *backend,
                                                       const GmlRenderDrawBatch *batch,
                                                       uintptr_t frontend_framebuffer,
                                                       char *error,size_t error_capacity);

#endif
