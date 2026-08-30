/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_render.h — software renderer: atlas/TPAG/sprite decode + blitter. */
#ifndef GML_RENDER_H
#define GML_RENDER_H
#include "gml_win.h"
#include "gml_software3d.h"

typedef struct GmlRender GmlRender;

/* bm_subtract is the fixed-function pair (bm_zero, bm_inv_src_colour), not the
 * arithmetic blend equation.  The destination component is therefore scaled
 * by the inverse source component.  Keep the integer rule shared by every
 * software drawing path so primitives, sprites and surfaces cannot drift. */
static inline unsigned gml_blend_inv_source_u8(unsigned dst, unsigned src){
  return (dst*(255u-src)+127u)/255u;
}

/* Stock room-layer filters are serialized as a type plus named properties. Decode the package
 * parameters, render the affected layer into an isolated software target, then evaluate the
 * corresponding operation family before compositing it. */
enum {
  GML_LAYER_FILTER_NONE=0,
  GML_LAYER_FILTER_RGB_NOISE,
  GML_LAYER_FILTER_TINT,
  GML_LAYER_FILTER_CLOUDS,
  GML_LAYER_FILTER_GLOW,
  GML_LAYER_FILTER_UNDERWATER,
  GML_LAYER_FILTER_ZOOM_BLUR,
  GML_LAYER_FILTER_LARGE_BLUR,
  GML_LAYER_FILTER_BOXES,
  GML_LAYER_FILTER_COLOURISE
};
typedef struct {
  int kind, sampler_sprite, affects_below;
  union {
    struct { double intensity, animation; uint32_t colour; uint32_t sampler_tpag_ptr; } noise;
    struct { uint32_t colour; } tint;
    struct {
      double scale, velocity[2], turbulence, level, waves, shape[2], density, fade;
      double shade_offset[2], shade_fade;
      uint32_t light_colour, shade_colour;
    } clouds;
    struct { double radius, quality, intensity, gamma, alpha; } glow;
    struct {
      double speed[2], scale[2][2], amount[2], chroma, camera_scale;
      uint32_t glint_colour, tint_colour, add_colour;
    } underwater;
    struct { double centre[2], intensity, focus_radius; } zoom_blur;
    struct { double radius; } large_blur;
    struct {
      double scale, size[2], displacement, speed, angle, rotation[2];
      double roundness, colour_speed, colours, sharpness;
    } boxes;
    struct { double intensity; uint32_t tint_colour; } colourise;
  } u;
} GmlLayerFilter;

/* Metadata-only sprite query. It never decodes or borrows texture pixels. The
 * optional name remains renderer-owned and is valid until the sprite changes. */
typedef struct {
  int width, height, origin_x, origin_y, frame_count;
  int collision_left, collision_top, collision_right, collision_bottom;
  double playback_speed;
  int playback_speed_type, playback_speed_valid;
  const char *name;
} GmlRenderSpriteMetrics;

/* Instance-owned skeletal state reaches the renderer through narrow callbacks.
 * The renderer does not borrow VM storage or language values. */
typedef const char *(*GmlRenderSkeletonAttachmentFn)(
  void *context, const char *slot);
typedef int (*GmlRenderSkeletonBoneFn)(
  void *context, const char *kind, const char *bone,
  const char *field, double *value);
typedef struct {
  void *context;
  const char *animation;
  const char *skin;
  double time;
  GmlRenderSkeletonAttachmentFn attachment;
  GmlRenderSkeletonBoneFn bone;
} GmlRenderSkeletonState;

typedef struct {
  int line_height;
  int ascender, ascender_offset, sdf_spread;
  int sprite, first, proportional, separation;
  int sprite_backed;
  int glyph_count;
} GmlRenderFontMetrics;

typedef struct {
  unsigned character;
  int x, y, width, height, shift, offset;
} GmlRenderFontGlyphMetrics;

typedef struct {
  int width, height;
  double camera_x, camera_y;
} GmlRenderTargetMetrics;

typedef struct {
  int texture_page, atlas;
  int packed_x, packed_y, packed_width, packed_height;
  int trim_x, trim_y, declared_width, declared_height;
  int logical_width, logical_height;
  int tile_width, tile_height, tile_border_x, tile_border_y;
  int tile_separation_x, tile_separation_y, tile_columns;
  int transparent, smooth, preload;
  const char *name;
} GmlRenderBackgroundMetrics;

enum {
  GML_RENDER_TEXTURE_NONE,
  GML_RENDER_TEXTURE_SPRITE,
  GML_RENDER_TEXTURE_SURFACE,
  GML_RENDER_TEXTURE_BACKGROUND,
  GML_RENDER_TEXTURE_FONT
};

typedef struct {
  int kind;
  int runtime;
  int atlas_backed;
  int logical_width, logical_height;
  int trim_x, trim_y;
  int width, height;
  int source_x, source_y;
  int full_width, full_height;
} GmlRenderTextureMetrics;

/* Coarse language-visible fixed-function state. The snapshot is a value copy:
 * it neither exposes renderer storage nor borrows pixel/resource records. */
typedef struct {
  uint32_t color;
  double alpha;
  int font, horizontal_alignment, vertical_alignment;
  int alpha_blend, circle_precision, interpolation;
  int blend_mode, blend_equation, blend_equation_alpha;
  int alpha_test_enable, alpha_test_reference;
  unsigned color_write_mask;
} GmlRenderDrawState;

enum {
  GML_RENDER_DRAW_STATE_COLOR                = 1u<<0,
  GML_RENDER_DRAW_STATE_ALPHA                = 1u<<1,
  GML_RENDER_DRAW_STATE_FONT                 = 1u<<2,
  GML_RENDER_DRAW_STATE_HORIZONTAL_ALIGNMENT = 1u<<3,
  GML_RENDER_DRAW_STATE_VERTICAL_ALIGNMENT   = 1u<<4,
  GML_RENDER_DRAW_STATE_ALPHA_BLEND          = 1u<<5,
  GML_RENDER_DRAW_STATE_CIRCLE_PRECISION     = 1u<<6,
  GML_RENDER_DRAW_STATE_INTERPOLATION        = 1u<<7,
  GML_RENDER_DRAW_STATE_BLEND_MODE           = 1u<<8,
  GML_RENDER_DRAW_STATE_BLEND_EQUATION       = 1u<<9,
  GML_RENDER_DRAW_STATE_BLEND_EQUATION_ALPHA = 1u<<10,
  GML_RENDER_DRAW_STATE_ALPHA_TEST_ENABLE    = 1u<<11,
  GML_RENDER_DRAW_STATE_ALPHA_TEST_REFERENCE = 1u<<12,
  GML_RENDER_DRAW_STATE_COLOR_WRITE_MASK     = 1u<<13
};

/* Presentation controls are queried together because window/display builtins
 * apply language-level fallbacks across several related values. */
typedef struct {
  int monitor_width, monitor_height;
  int effective_width, effective_height;
  int wide_aspect_active, wide_width, wide_height;
  int gui_pass_active, gui_base_width, gui_base_height;
  int application_width, application_height;
  int application_owned, application_draw_enabled;
  int interpolation, application_phase_active;
} GmlRenderPresentationMetrics;

/* Frame-scoped pixel-plane borrows are updated by family. The engine owns the
 * scratch allocations; renderer draw paths borrow them until the next update
 * or renderer teardown. */
typedef struct {
  uint32_t *application_vertical;
  uint32_t *application_interpolated[3];
  uint32_t *classic_vertical;
  uint32_t *classic_interpolated[3];
} GmlRenderSamplePlanes;

enum {
  GML_RENDER_SAMPLE_PLANES_APPLICATION = 1u<<0,
  GML_RENDER_SAMPLE_PLANES_CLASSIC     = 1u<<1
};

typedef struct {
  int opaque_known, all_opaque, all_transparent;
} GmlRenderTargetCoverage;

/* A terminal application-surface presentation the renderer has recorded but not yet written.
 *
 * It is deferred on exactly the discipline the renderer already uses for its other deferred
 * writes: any subsequent draw flushes it through the same kernel that would have produced it, so
 * the target is never observed half-written. Its purpose is to give the frame's last operation a
 * chance to happen somewhere other than the processor; when nothing takes it, it is written here
 * and nothing is different.
 *
 * The value record below describes that operation without exposing renderer storage. The sampling
 * rectangle is stated in destination pixels and is already camera-adjusted, because that is the
 * space the kernel samples in. */
/* Which sampling convention the recorded presentation follows. They are not interchangeable: at a
 * fractional magnification they disagree by one source texel at most boundaries. */
enum {
  /* Destination pixel centres over a fractional rectangle: a content-owned presentation. */
  GML_RENDER_PRESENTATION_PIXEL_CENTRE=0,
  /* Leading output edge in integer arithmetic: the automatic application-surface presentation. */
  GML_RENDER_PRESENTATION_LEADING_EDGE=1
};

typedef struct {
  int sampling_rule;
  /* The buffer the operation was recorded against. Writing it later has to reach that buffer and
   * not whichever one happens to be bound at the time. */
  uint32_t *target_pixels;
  int target_width, target_height;
  const uint32_t *source_pixels;
  int source_width, source_height, source_pitch;
  /* The destination rectangle, clipped to the target. */
  int destination_x, destination_y, destination_width, destination_height;
  /* The unclipped rectangle the sampling expression is defined over. The pixel-centre rule reads
   * the origin and the extent; the leading-edge rule reads the extent as an integer destination
   * width and counts from the local offset, because it is defined in destination-local indices. */
  double origin_x, origin_y, extent_x, extent_y;
  int local_offset_x, local_offset_y;
  /* A full-target fill the presentation was recorded together with. The two are one operation as
   * far as the target is concerned: the fill is what makes the margins around a narrower
   * presentation defined, and separating them would leave a frame half-described. */
  int has_fill;
  uint32_t fill_color;
  /* Stable identity of the source resource and a generation that advances when its pixels do. */
  uint32_t identity, generation;
  /* The content shader the presentation was drawn through, or -1 for the plain blit. The
   * software write of the record is always the plain blit: that is what the runtime draws when no
   * device executes the program. */
  int shader;
  /* Whether the content asked for interpolated sampling when it drew. */
  int linear;
} GmlRenderDeferredPresentation;

/* The content's own program and what the content set on it, as the graphics backend needs them.
 * Sources are lent from the content image and stay valid while the content is loaded. */
typedef struct {
  const char *vertex_es,*fragment_es,*vertex_gl,*fragment_gl;
} GmlRenderShaderSources;
typedef struct {
  char name[32];
  float value[16];
  uint32_t count;
  uint32_t integer;
} GmlRenderShaderUniform;
typedef struct {
  char name[32];
  /* The texture handle texture_set_stage bound, or 0 for none, decoded into what it names: a
   * sprite frame, or a surface. Whichever does not apply is -1. */
  int texture;
  int sprite,frame,surface;
} GmlRenderShaderSampler;

enum {
  GML_RENDER_COVERAGE_OPAQUE_KNOWN   = 1u<<0,
  GML_RENDER_COVERAGE_ALL_OPAQUE     = 1u<<1,
  GML_RENDER_COVERAGE_ALL_TRANSPARENT = 1u<<2,
  GML_RENDER_COVERAGE_ALL =
    GML_RENDER_COVERAGE_OPAQUE_KNOWN |
    GML_RENDER_COVERAGE_ALL_OPAQUE |
    GML_RENDER_COVERAGE_ALL_TRANSPARENT
};

/* The frame coordinator owns view-layout composition, so the renderer may
 * lend its owned application pixels for one bounded composition phase. The
 * allocation and dimensions remain renderer-owned; the borrow ends before
 * the next renderer operation which can resize or destroy surface zero. */
typedef struct {
  uint32_t *pixels;
  int width, height;
} GmlRenderApplicationWriteView;

/* Core resolves host policy; renderer owns the mutable controls. A masked
 * value-copy update keeps boot, live configuration, and per-frame hints coarse
 * without exposing renderer storage. */
typedef struct {
  int monitor_width, monitor_height;
  /* Non-zero: shader_is_compiled answers yes for every payload shader, as a GPU would.
   * Zero: only recognized shader families answer yes, so content that carries its own
   * no-shader presentation selects it. */
  int shader_report_all_compiled;
  int fast_forward, fast_alpha_cull;
  int wide_aspect_active, wide_width, wide_height;
} GmlRenderControl;

enum {
  GML_RENDER_CONTROL_MONITOR_SIZE   = 1u<<0,
  GML_RENDER_CONTROL_SHADERS            = 1u<<1,
  GML_RENDER_CONTROL_FAST_FORWARD   = 1u<<2,
  GML_RENDER_CONTROL_FAST_ALPHA     = 1u<<3,
  GML_RENDER_CONTROL_WIDE_ASPECT    = 1u<<4,
  GML_RENDER_CONTROL_HOST_OPTIONS =
    GML_RENDER_CONTROL_MONITOR_SIZE | GML_RENDER_CONTROL_SHADERS
};

typedef struct {
  int atlas_count, sprite_count, texture_page_count;
} GmlRenderResourceMetrics;

typedef struct {
  int active_shader, pending_fill, pending_underlay;
} GmlRenderDiagnosticMetrics;

enum {
  GML_RENDER_SHADER_TEXTURE_NONE,
  GML_RENDER_SHADER_TEXTURE_PALETTE,
  /* Bound to one of the content program's own samplers. */
  GML_RENDER_SHADER_TEXTURE_CONTENT,
  GML_RENDER_SHADER_TEXTURE_SURFACE
};

/* Value-copy description of a texture-stage mutation. It exists so the
 * language adapter can emit diagnostics without borrowing shader storage. */
typedef struct {
  int kind, sampler, sprite, frame, surface;
} GmlRenderShaderTextureBinding;

#define GML_MAX_FONTS 48
#define GML_MAX_SURFACES 64   /* allow many concurrent surface allocations */
#define GML_SURFACE_STACK 8
#include "gml_render_primitives.h"

int  gml_render_init(GmlRender *r, GmlWin *win);
void gml_render_free(GmlRender *r);
void gml_render_rebind_content(GmlRender *r,GmlWin *win);
void gml_render_bind_software3d(GmlRender *r,GmlSoftware3D *software3d);
GmlSoftware3D *gml_render_software3d(GmlRender *r);
void gml_render_set_frame(GmlRender *r,long frame);
void gml_render_control_update(GmlRender *r,const GmlRenderControl *control,
                               unsigned fields);
int gml_render_resource_metrics(const GmlRender *r,
                                GmlRenderResourceMetrics *metrics);
int gml_render_diagnostic_metrics(const GmlRender *r,
                                  GmlRenderDiagnosticMetrics *metrics);
/* Visible world rectangle in authored coordinates; see the definition for why the target
 * metrics cannot be used for world-space selection. */
int gml_render_world_view(const GmlRender *r,double *x,double *y,double *width,double *height);
int gml_render_target_metrics(const GmlRender *r,GmlRenderTargetMetrics *metrics);
void gml_render_target_metrics_update(GmlRender *r,
                                      const GmlRenderTargetMetrics *metrics,
                                      unsigned fields);
enum {
  GML_RENDER_TARGET_WIDTH  = 1u<<0,
  GML_RENDER_TARGET_HEIGHT = 1u<<1,
  GML_RENDER_TARGET_CAMERA = 1u<<2
};
int gml_render_presentation_metrics(const GmlRender *r,
                                    GmlRenderPresentationMetrics *metrics);
void gml_render_presentation_effective_set(GmlRender *r,int width,int height);
void gml_render_sample_planes_update(GmlRender *r,
                                     const GmlRenderSamplePlanes *planes,
                                     unsigned fields);
/* Allow the renderer to defer a terminal application-surface presentation instead of writing it.
 * Off by default: with nothing able to take the deferred operation, deferring it only moves the
 * same work later. */
void gml_render_set_deferred_presentation(GmlRender *r,int enabled);
/* Copy the deferred presentation out, leaving it deferred. Returns zero when there is none. */
int gml_render_deferred_presentation(const GmlRender *r,GmlRenderDeferredPresentation *out);
/* Write the deferred presentation through the exact kernel that recorded it. Idempotent. */
void gml_render_flush_deferred_presentation(GmlRender *r);
/* Forget it without writing. Only correct when the target it described is being rebuilt. */
void gml_render_discard_deferred_presentation(GmlRender *r);

int gml_render_target_coverage(const GmlRender *r,
                               GmlRenderTargetCoverage *coverage);
void gml_render_target_coverage_update(GmlRender *r,
                                       const GmlRenderTargetCoverage *coverage,
                                       unsigned fields);
void gml_render_application_surface_bind(GmlRender *r,uint32_t *pixels,
                                         int width,int height,int opaque);
int gml_render_application_surface_ensure_owned(GmlRender *r,int width,int height);
int gml_render_application_surface_owned_clear(
  GmlRender *r,uint32_t color,GmlRenderApplicationWriteView *view);
int gml_render_application_surface_owned_view(
  GmlRender *r,GmlRenderApplicationWriteView *view);
int gml_render_application_surface_select_owned(GmlRender *r,int opaque);
int gml_render_surface_mirror_pixels(GmlRender *r,int destination,
                                     uint32_t *pixels,int width,int height,
                                     int opaque);
void gml_render_set_flat_fog(GmlRender *r,int enabled,uint32_t colour);
int gml_render_draw_state_get(const GmlRender *r,GmlRenderDrawState *state);
void gml_render_draw_state_update(GmlRender *r,const GmlRenderDrawState *state,
                                  unsigned fields);
int gml_render_gpu_state_push(GmlRender *r);
int gml_render_gpu_state_pop(GmlRender *r);
void gml_render_application_surface_set_draw_enabled(GmlRender *r,int enabled);
/* Mark the Post-Draw interval after the automatic presentation decision. */
void gml_render_application_surface_presentation_settle(GmlRender *r,int settled);
int gml_render_application_surface_presentation_settled(const GmlRender *r);
int gml_render_target_pixel(GmlRender *r,int x,int y,uint32_t *pixel);
void gml_render_shader_set_current(GmlRender *r,int shader);
int gml_render_shader_current(const GmlRender *r);
int gml_render_is_classic(const GmlRender *r);
int gml_render_shader_is_compiled(const GmlRender *r,int shader);
int gml_render_shader_uniform_handle(GmlRender *r,int shader,const char *name);
int gml_render_shader_sampler_handle(GmlRender *r,int shader,const char *name);
void gml_render_shader_uniform_set(GmlRender *r,int handle,const double values[4]);
int gml_render_shader_texture_stage_set(
  GmlRender *r,int stage,int texture,GmlRenderShaderTextureBinding *binding);
/* Content shaders executed on the host's graphics context. A candidate is a program this renderer
 * recognizes no family in, that samples a picture, whose sources are present and that the device
 * has not refused. The state answers what the content set, by name. */
int gml_render_shader_content_candidate(const GmlRender *r,int shader);
int gml_render_shader_sources(const GmlRender *r,int shader,GmlRenderShaderSources *out);
uint32_t gml_render_shader_uniforms(const GmlRender *r,int shader,
                                    GmlRenderShaderUniform *out,uint32_t capacity);
uint32_t gml_render_shader_samplers(const GmlRender *r,int shader,
                                    GmlRenderShaderSampler *out,uint32_t capacity);
void gml_render_shader_mark_failed(GmlRender *r,int shader);
/* Multi-component setter for the content's own uniforms: up to sixteen values, integer or float. */
void gml_render_shader_uniform_set_values(GmlRender *r,int handle,const double *values,
                                          uint32_t count,int integer);
/* A content program run in the middle of a frame: the host executes the program over the source
 * at the destination's size and writes the result, rows from the top, into `output` (width*height
 * ARGB words). The renderer then composes that picture like any other surface. Returns 0 when the
 * host cannot (no context, or the program was refused), and the draw proceeds unshaded. */
typedef struct {
  int shader;
  const uint32_t *source;
  int source_width,source_height,source_pitch;
  /* A stable identity for the source picture and a serial the host may use to tell one request
   * from the next; the source may change between requests with the same identity. */
  uint32_t source_identity,serial;
  int width,height;
  int linear;
  uint32_t *output;
} GmlRenderShaderRequest;
typedef int (*GmlRenderShaderExecutor)(void *context,const GmlRenderShaderRequest *request);
void gml_render_set_shader_executor(GmlRender *r,GmlRenderShaderExecutor executor,void *context);
/* The surface id that names the renderer's plane holding the last executed program's result. */
enum { GML_RENDER_SHADED_SURFACE=-2 };
/* A sprite frame as one ARGB plane in renderer-owned scratch, for a sampler upload. Valid until the
 * next call. */
int gml_render_sprite_frame_plane(GmlRender *r,int sprite,int frame,int *width,int *height,
                                  const uint32_t **pixels);
/* A runtime surface's pixels, for a sampler upload. */
int gml_render_surface_plane(GmlRender *r,int surface,int *width,int *height,
                             const uint32_t **pixels);

/* Convert an instance/layer image_speed multiplier into subimages per runtime step. Modern sprites
 * serialize their own rate as either frames/second or frames/game-frame; older/runtime sprites
 * retain the legacy one-subimage-per-step multiplier. */
double gml_sprite_animation_delta(GmlRender *r, int sprite, double image_speed, double game_fps);
/* queue background decodes (worker threads; safe no-ops when disabled or already decoded) */
void gml_render_prefetch_atlas(GmlRender *r, int idx);
void gml_render_prefetch_sprite(GmlRender *r, int sprite);
void gml_render_prefetch_bg(GmlRender *r, int bg);
/* synchronously decode directly referenced pages; used at room boundaries to avoid first-draw hitches */
void gml_render_warm_sprite(GmlRender *r, int sprite);
void gml_render_warm_bg(GmlRender *r, int bg);
int gml_render_content_composited_screen(GmlRender *r);
void gml_render_clear_content_composited_screen(GmlRender *r);
void gml_render_begin(GmlRender *r, uint32_t *fb, int w, int h, double camx, double camy);
void gml_render_begin_retaining_target(GmlRender *r, uint32_t *fb, int w, int h,
                                       double camx, double camy);
void gml_render_world_set_logical_extent(GmlRender *r,int width,int height);
void gml_render_gui_begin(GmlRender *r, int logical_w, int logical_h);
void gml_render_gui_set_size(GmlRender *r, int logical_w, int logical_h);
void gml_render_gui_set_maximise(GmlRender *r, int active, double xscale, double yscale,
                                 double xoffset, double yoffset,
                                 int window_w, int window_h);
void gml_render_gui_end(GmlRender *r);
void gml_render_draw_map_point(const GmlRender *r,double *x,double *y);
void gml_render_draw_map_scale(const GmlRender *r,double *xscale,double *yscale);
int gml_render_gui_transform_active(const GmlRender *r);
void gml_render_gui_map_point(const GmlRender *r, double *x, double *y);
void gml_render_gui_map_scale(const GmlRender *r, double *xscale, double *yscale);
double gml_render_gui_logical_width(const GmlRender *r);
double gml_render_gui_logical_height(const GmlRender *r);
double gml_render_gui_logical_x(const GmlRender *r, double physical_x);
double gml_render_gui_logical_y(const GmlRender *r, double physical_y);
void gml_render_set_pending_underlay(GmlRender *r, int x, int y, int w, int h);
void gml_render_flush_pending_underlay(GmlRender *r);
void gml_render_set_pending_fill(GmlRender *r, uint32_t color);
void gml_render_flush_pending_fill(GmlRender *r);
void gml_render_cancel_pending_fill(GmlRender *r);
void gml_render_clear(GmlRender *r,uint32_t color,double alpha);
void gml_render_cancel_pending_underlay(GmlRender *r);
void gml_render_prepare_draw(GmlRender *r);
void gml_render_prepare_opaque_rect(GmlRender *r, int x0, int y0, int x1, int y1);
/* Apply a recognized untextured procedural fragment to a filled screen-space rectangle.  Returns
 * nonzero only when the active shader handled the draw. */
void gml_render_maybe_prepare_draw(GmlRender *r);
void gml_render_maybe_prepare_opaque_rect(GmlRender *r, int x0, int y0, int x1, int y1);

void gml_draw_sprite_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                         double xs, double ys, double rot, uint32_t blend, double alpha);
void gml_render_skeleton_state_set(GmlRender *r,
                                   const GmlRenderSkeletonState *state);
int  gml_render_sprite_is_skeleton(const GmlRender *r, int sprite);
double gml_render_skeleton_animation_duration(const GmlRender *r, int sprite,
                                              const char *animation);
int  gml_render_skeleton_bone_setup(const GmlRender *r, int sprite,
                                    const char *bone, const char *field,
                                    double *value);
void gml_draw_sprite_pos(GmlRender *r, int sprite, int subimg,
                         const double x[4], const double y[4], double alpha);
int  gml_d3_draw_sprite_2d(GmlRender *r, int sprite, int subimg, double x, double y,
                           double xs, double ys, double rot, uint32_t blend, double alpha);
int  gml_d3_draw_sprite_pos_2d(GmlRender *r, int sprite, int subimg,
                               const double x[4], const double y[4], double alpha);
int  gml_d3_draw_background_2d(GmlRender *r, int background, double x, double y,
                               double xs, double ys, uint32_t blend, double alpha);
int  gml_d3_draw_sprite_part_2d(GmlRender *r, int sprite, int subimg,
                                double sx, double sy, double sw, double sh,
                                double x, double y, double xs, double ys,
                                uint32_t blend, double alpha);
int  gml_d3_draw_background_part_2d(GmlRender *r, int background,
                                    double sx, double sy, double sw, double sh,
                                    double x, double y, double xs, double ys,
                                    uint32_t blend, double alpha);
int  gml_d3_draw_atlas_part_2d(GmlRender *r, int atlas, int sx, int sy, int w, int h,
                               double x, double y, double xs, double ys,
                               uint32_t blend, double alpha);
int  gml_d3_draw_surface_part_2d(GmlRender *r, int surface,
                                 double sx, double sy, double sw, double sh,
                                 double x, double y, double xs, double ys,
                                 uint32_t blend, double alpha);
void gml_d3_set_draw_depth(GmlRender *render,double depth);
int  gml_d3_is_active(GmlRender *render);
void gml_d3_sync_render_camera(GmlRender *render);
int  gml_d3_draw_rectangle_2d(GmlRender *r,double x1,double y1,double x2,double y2,
                               uint32_t color,double alpha,int outline);
int  gml_render_warm_atlas(GmlRender *r, int atlas);
uint32_t gml_render_named_tpag_ptr(GmlRender *r, const char *name);
int  gml_render_named_sprite(GmlRender *r, const char *name);
void gml_draw_sprite(GmlRender *r, int sprite, int subimg, double x, double y);
void gml_draw_sprite_tiled_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                               double xs, double ys, uint32_t blend, double alpha);
/* GMS background-layer sprites use the layer coordinate as the logical cell's top-left (sprite
 * origins are instance metadata) and tile only on the axes selected by the layer. */
void gml_draw_layer_background_sprite(GmlRender *r, int sprite, int subimg, double x, double y,
                                      double xs, double ys, uint32_t blend, double alpha,
                                      int htiled, int vtiled);
/* Stock GMS2 effect-layer filters, evaluated against the current software target in layer order. */
void gml_render_layer_rgb_noise(GmlRender *r, uint32_t sampler_tpag_ptr,
                                double intensity, double animation, uint32_t rgb);
void gml_render_layer_tint(GmlRender *r, uint32_t rgba);
int  gml_render_layer_filter_begin(GmlRender *r, const GmlLayerFilter *filter);
void gml_render_layer_filter_end(GmlRender *r, const GmlLayerFilter *filter, double time_seconds);
void gml_draw_sprite_part_ext(GmlRender *r, int sprite, int subimg, double sx, double sy,
                              double sw, double sh, double x, double y,
                              double xs, double ys, uint32_t blend, double alpha);
void gml_draw_background(GmlRender *r, int bg, double x, double y);
void gml_draw_background_part_ext(GmlRender *r, int bg, double sx, double sy, double sw, double sh,
                                  double x, double y, double xs, double ys, uint32_t color, double alpha);
void gml_draw_background_tile(GmlRender *r, int bg,
                              double sx, double sy, double sw, double sh,
                              double x, double y, double xs, double ys,
                              int mirror, int flip, int rotate,
                              uint32_t color, double alpha);
void gml_draw_background_stretched(GmlRender *r, int bg, double x, double y, double w, double h, uint32_t color, double alpha);
void gml_draw_background_tiled(GmlRender *r, int bg, double x, double y, int htiled, int vtiled);
void gml_draw_background_ext(GmlRender *r, int bg, double x, double y, double xs, double ys, uint32_t color, double alpha);
void gml_draw_background_tiled_ext(GmlRender *r, int bg, double x, double y, double xs, double ys, uint32_t color, double alpha, int htiled, int vtiled);
void gml_draw_room_backgrounds(GmlRender *r, uint32_t bg_ptr, int want_fg);
void gml_draw_room_tiles(GmlRender *r, uint32_t tile_ptr);
void gml_draw_tile(GmlRender *r, int def, int sx, int sy, int w, int h, double x, double y);
/* GML draw commands for stretching the application_surface and sprites in
 * screen-space without a camera. The engine executes the runtime draw calls. */
int  gml_surface_create(GmlRender *r, int w, int h);
void gml_surface_free(GmlRender *r, int id);
int  gml_surface_exists(GmlRender *r, int id);
void gml_surface_resize(GmlRender *r, int id, int w, int h);
int  gml_surface_width(GmlRender *r, int id);
int  gml_surface_height(GmlRender *r, int id);
const uint32_t *gml_surface_pixels_read(GmlRender *r, int id, int *w, int *h);
void gml_surface_copy(GmlRender *r, int dst, int x, int y, int src);
void gml_surface_copy_part(GmlRender *r, int dst, int x, int y, int src,
                           int source_x, int source_y, int width, int height);
int  gml_surface_set_target(GmlRender *r, int id);
void gml_surface_reset_target(GmlRender *r);
int  gml_surface_get_target(GmlRender *r);
int  gml_surface_target_lit(GmlRender *r);
void gml_draw_surface_stretched(GmlRender *r, int surf, double x, double y, double w, double h, uint32_t blend, double alpha);
void gml_draw_surface_ext(GmlRender *r, int surf, double x, double y,
                          double xs, double ys, double rot, uint32_t blend, double alpha);
void gml_draw_surface_part_ext(GmlRender *r, int surf, double sx, double sy, double sw, double sh,
                               double x, double y, double xs, double ys, uint32_t blend, double alpha);
int  gml_sprite_create_from_surface(GmlRender *r, int surf, int x, int y, int w, int h,
                                    int removeback, int smooth, int xorig, int yorig);
int  gml_sprite_replace_from_file(GmlRender *r, int sprite, const char *path, int imgnumb,
                                  int removeback, int smooth, int xorig, int yorig);
int  gml_background_replace_from_file(GmlRender *r, int background, const char *path,
                                      int removeback, int smooth);
int  gml_background_replace_from_rgba(GmlRender *r, int background,
                                      uint8_t *rgba, int width, int height);
int  gml_sprite_replace_from_rgba(GmlRender *r, int sprite, uint8_t *rgba, int w, int h, int xorig, int yorig);
int  gml_sprite_append_from_rgba(GmlRender *r, uint8_t *rgba, int w, int h, int xorig, int yorig, const char *name);
int  gml_sprite_replace_from_rgba_frames(GmlRender *r, int sprite, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig);
int  gml_sprite_append_from_rgba_frames(GmlRender *r, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig, const char *name);
int  gml_sprite_assign(GmlRender *r, int destination, int source);
void gml_render_clear_runtime_sprites(GmlRender *r);
int  gml_classic_present_explicit_port(const GmlWin *win, int explicit_window,
                                       int canvas_w, int canvas_h,
                                       int port_x, int port_y, int port_w, int port_h,
                                       int *target_x, int *target_y,
                                       int *target_w, int *target_h);
void gml_classic_room_window_size(int room_w, int room_h,
                                  int fixed_scale_pct,
                                  int configured_w, int configured_h,
                                  const int visible[8],
                                  const int xport[8], const int yport[8],
                                  const int wport[8], const int hport[8],
                                  int *window_w, int *window_h);
void gml_classic_present_adjust(const GmlWin *win, int source_w, int source_h,
                                int target_w, int target_h, int interpolated,
                                int *target_x, int *target_y);
void gml_draw_sprite_stretched(GmlRender *r, int sprite, int frame, double x, double y, double w, double h, uint32_t blend, double alpha);
int  gml_sprite_exists(GmlRender *r, int sprite);
int  gml_render_sprite_metrics(const GmlRender *r, int sprite,
                               GmlRenderSpriteMetrics *metrics);
int  gml_render_sprite_set_playback(GmlRender *r, int sprite,
                                    double speed, int speed_type);
int  gml_render_sprite_texture_handle(int sprite, int image);
int  gml_render_surface_texture_handle(int surface);
int  gml_render_texture_metrics(GmlRender *r,int texture,
                                GmlRenderTextureMetrics *metrics);
int  gml_render_background_metrics(const GmlRender *r, int background,
                                   GmlRenderBackgroundMetrics *metrics);
int  gml_render_background_texture_handle(const GmlRender *r, int background);
int  gml_render_background_tile_animation_frame(const GmlRender *r, int background,
                                                 double elapsed_seconds);
int  gml_render_background_tile_source_index(const GmlRender *r, int background,
                                              int tile_index, int animation_frame);
int  gml_render_font_metrics(const GmlRender *r, int font,
                             GmlRenderFontMetrics *metrics);
int  gml_render_font_glyph_metrics(const GmlRender *r, int font, int glyph,
                                   GmlRenderFontGlyphMetrics *metrics);
int  gml_render_font_texture_handle(const GmlRender *r, int font);
int  gml_render_font_exists(const GmlRender *r, int font);
int  gml_sprite_duplicate(GmlRender *r, int sprite);
void gml_sprite_set_offset(GmlRender *r, int sprite, int xorig, int yorig);
void gml_sprite_delete(GmlRender *r, int sprite);
int  gml_sprite_collision_mask(GmlRender *r, int sprite, int sepmasks, int bboxmode,
                               int bbleft, int bbtop, int bbright, int bbbottom,
                               int kind, int tolerance);
int  gml_sprite_frames(GmlRender *r, int sprite);
int  gml_sprite_alpha(GmlRender *r, int sprite, int frame, int lx, int ly);
int  gml_sprite_set_alpha_from_sprite(GmlRender *r, int sprite, int alpha_sprite);
int  gml_sprite_collision(GmlRender *r, int sprite, int frame, int lx, int ly);
int  gml_font_add_sprite(GmlRender *r, int sprite, int first, int prop, int sep);
int  gml_font_add_sprite_ext(GmlRender *r, int sprite, const char *map, int prop, int sep);
int  gml_font_add_file(GmlRender *r, const char *path, double point_size,
                       int first, int last);                     /* runtime TTF (font_add) */
void gml_font_delete(GmlRender *r, int font);
int  gml_sprite_add_file(GmlRender *r, const char *path, int imgnum, int removeback, int xorig, int yorig);
void gml_render_rebuild_font_maps(GmlRender *r);
void gml_draw_text(GmlRender *r, double x, double y, const char *str);
void gml_draw_text_ext(GmlRender *r, double x, double y, const char *str, double sep, double w);
void gml_draw_text_sprite(GmlRender *r, double x, double y, const char *str,
                          double sep, double w, int sprite, int first, double scale);
void gml_draw_text_ext_transformed(GmlRender *r, double x, double y, const char *str,
                                   double sep, double w, double xs, double ys, double rot,
                                   uint32_t blend, double alpha);
void gml_draw_text_transformed(GmlRender *r, double x, double y, const char *str,
                               double xs, double ys, double rot, uint32_t blend, double alpha);
int  gml_text_width(GmlRender *r, const char *str);
int  gml_text_height(GmlRender *r, const char *str);
double gml_text_width_ext(GmlRender *r, const char *str, double sep, double width);
double gml_text_height_ext(GmlRender *r, const char *str, double sep, double width);
void gml_draw_classic_game_information(GmlRender *r, uint32_t *framebuffer,
                                       int width, int height,
                                       const uint8_t *record, size_t record_size);

#endif
