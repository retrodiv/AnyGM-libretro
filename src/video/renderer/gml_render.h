/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_render.h — software renderer: atlas/TPAG/sprite decode + blitter. */
#ifndef GML_RENDER_H
#define GML_RENDER_H
#include "gml_win.h"

/* bm_subtract is the fixed-function pair (bm_zero, bm_inv_src_colour), not the
 * arithmetic blend equation.  The destination component is therefore scaled
 * by the inverse source component.  Keep the integer rule shared by every
 * software drawing path so primitives, sprites and surfaces cannot drift. */
static inline unsigned gml_blend_inv_source_u8(unsigned dst, unsigned src){
  return (dst*(255u-src)+127u)/255u;
}

typedef struct { uint16_t y, x, len; uint8_t alpha; } GmlTpagAlphaRun;
typedef struct {
  int sx,sy,sw,sh, tx,ty, bw,bh, atlas;  /* texture page item */
  int alpha_scanned, ax0, ay0, ax1, ay1; /* nontransparent source bbox, cached after atlas decode */
  int alpha_max;                          /* max source alpha in the texture-page item */
  int *alpha_row_min, *alpha_row_max;     /* per-source-row nontransparent span, optional */
  uint16_t *alpha_qrow_min, *alpha_qrow_max; /* per-alpha-threshold row spans, built lazily */
  uint8_t *alpha_qrow_built;
  GmlTpagAlphaRun *alpha_runs; int alpha_run_count, alpha_runs_built;
  uint32_t *argb_cache;                  /* compact ARGB source pixels for hot rotated draws */
  uint32_t *interp_phase_cache[3];       /* lazy x/y/xy half-sample ARGB for exact-2x ports */
  uint32_t *fast8_draw_cache;            /* RGB plus draw-alpha for repeated large fast8 draws */
  double fast8_draw_alpha_key;
  double fast8_draw_pending_alpha_key;
  uint32_t fast8_draw_blend_key;
  uint32_t fast8_draw_pending_blend_key;
  int fast8_draw_alpha_floor_key;
  int fast8_draw_pending_alpha_floor_key;
  int fast8_draw_cache_valid, fast8_draw_cache_copy_255, fast8_draw_pending_count;
} GmlTpag;
typedef struct {
  const uint8_t *src;
  int sw, sh, fbw, fbh, x0, y0, w, h, originx, originy;
  double ax, ay, xs, ys, alpha;
  uint32_t blend;
} GmlRuntimeAxisKey;
typedef struct {
  int y, x, len;
  uint8_t alpha;
} GmlRuntimeAxisRun;
typedef struct { const char *name; int originx, originy, w, h, n_frames; int *frame;
                 int ml, mr, mt, mb;                /* collision bbox coordinates: left,right,top,bottom */
                 const uint8_t *mask; int mask_rowb, mask_count;  /* SPRT collision mask: 1bpp */
                 int collision_kind, collision_tolerance;
                 float playback_speed; int playback_speed_type, playback_speed_valid;
                 uint8_t *runtime_rgba; int runtime_owned, runtime_extra, runtime_opaque; char *owned_name;
                 int *runtime_row_min, *runtime_row_max;
                 uint32_t *runtime_axis_cache_px; uint8_t *runtime_axis_cache_alpha;
                 int *runtime_axis_cache_row_min, *runtime_axis_cache_row_max;
                 GmlRuntimeAxisRun *runtime_axis_cache_runs; int runtime_axis_cache_run_count;
                 GmlRuntimeAxisKey runtime_axis_cache_key, runtime_axis_pending_key;
                 int runtime_axis_cache_valid, runtime_axis_cache_copy_255, runtime_axis_cache_uniform_alpha, runtime_axis_cache_full_rect, runtime_axis_pending_count;
                 char *runtime_source_path; int runtime_source_imgnum, runtime_source_removeback;
                 int base_valid, base_originx, base_originy, base_w, base_h, base_n_frames;
                 int base_ml, base_mr, base_mt, base_mb, base_mask_rowb, base_mask_count;
                 int base_collision_kind, base_collision_tolerance;
                 const uint8_t *base_mask;
                 /* GMS2.3+ nine-slice: draw scaled with fixed-size borders (corners never scale;
                  * edges/center follow their tile mode: 0=stretch 1=repeat 2=mirror 3=blankrepeat 4=hide) */
                 int ns_enabled, ns_l, ns_t, ns_r, ns_b, ns_tile[5]; } GmlSprite;
typedef struct {
  uint8_t *px; int w, h;                                          /* RGBA8, decoded lazily */
  uint32_t blob; size_t avail, chunk_end; int decode_attempted;    /* source blob in data.win */
  int debug_dumped;                                                /* one-shot opt-in atlas diagnostic */
} GmlAtlas;
typedef struct {
  int atlas, sx, sy, sw, sh;
  int projected_x, projected_y, dest_x0, dest_y0;
  double edge_x0, edge_x1, edge_y0, edge_y1;
  uint32_t *phase[3];
} GmlInterpSubrectCache;
typedef struct {
  int tpag;
  int tile_w, tile_h, tile_border_x, tile_border_y, tile_separation_x, tile_separation_y;
  int tile_columns, tile_items_per_tile, tile_count;
  const uint8_t *tile_ids;                                      /* GMS2 BGND tileset id table (little-endian u32s) */
} GmlBg;                                                        /* background/tileset -> texture page */
typedef struct { int32_t sx, sy, w, h; int16_t shift, offset; uint16_t ch; } GmlGlyph;
typedef struct {
  int sprite, first, prop, sep;
  uint32_t *map; int map_len;                                    /* font_add_sprite_ext explicit map */
  int map_fast[256];
  /* real FONT-chunk font: glyph sub-rects blitted from the data.win atlas at runtime
   * (no glyph data is bundled; parsed from the user-supplied data.win like sprites). */
  int real, atlas, line_height;                                  /* real=1; atlas index; line advance */
  int ascender_offset;                                           /* FONT vertical origin above the glyph cell */
  int align_height;                                              /* visible cell extent for valign */
  int runtime_owned;                                             /* font + atlas created after load */
  int subpixel;                                                   /* per-channel GDI coverage for classic info */
  GmlGlyph *glyphs; int n_glyphs, glyphs_sorted;
  int glyph_by_char[256];                                        /* fast ASCII lookup, -1 = none */
} GmlFont;                                                        /* sprite font or real FONT-chunk font */
typedef struct { uint32_t *px; int w, h, live;
                 int dirty;                       /* px changed since the RLE cache was built */
                 int opaque_known, all_opaque, all_transparent;  /* conservative coverage metadata */
                 uint8_t *rle; size_t rle_len, rle_cap;  /* cached savestate RLE (u32 nrun + pairs) */
} GmlSurface;      /* XRGB8888 runtime surface */

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

#define GML_MAX_FONTS 48
#define GML_MAX_SURFACES 64   /* allow many concurrent surface allocations */
#define GML_SURFACE_STACK 8
typedef struct {
  GmlWin   *win;
  int classic;                    /* GM6/7/8 pixel rules that differ from Studio */
  GmlAtlas *atlas; int n_atlas;
  GmlTpag  *tpag; int n_tpag;
  GmlInterpSubrectCache *interp_subrect_cache;
  int interp_subrect_count, interp_subrect_capacity;
  size_t interp_subrect_bytes;
  GmlSprite *spr; int n_spr, base_n_spr, spr_cap, spr_has_free;
  GmlBg    *bg; int n_bg;
  GmlFont   fonts[GML_MAX_FONTS]; int n_fonts;
  GmlFont   default_font;                                          /* built-in font selected by id -1 */
  struct {
    int source_index, font_id;
  } classic_info_font_cache[32]; int classic_info_font_cache_count;
  /* A platform-native renderer may cache the complete information passage.  It
   * is an optional accelerator/fidelity path; the bundled portable renderer is
   * still used whenever the host cannot provide the project's requested fonts. */
  uint32_t *classic_info_native_pixels;
  const uint8_t *classic_info_native_record;
  size_t classic_info_native_record_size;
  int classic_info_native_w, classic_info_native_h;
  int classic_info_native_attempted;
  /* current target framebuffer (borrowed) + camera */
  uint32_t *fb; int fbw, fbh;
  uint32_t *base_fb; int base_fbw, base_fbh;
  struct {
    uint32_t *fb; int w, h; double cx, cy, projection_cx, projection_cy;
    int target_id, opaque_known, all_opaque, all_transparent;
    int pending_underlay, underlay_x, underlay_y, underlay_w, underlay_h;
    int pending_fill; uint32_t fill_color;
  } target_stack[GML_SURFACE_STACK]; int target_sp;
  int target_id;
  double    cam_x, cam_y;
  /* Camera before the world matrix. A translation-only matrix is represented as an
   * equivalent camera delta for portable software draws; the full matrix remains in the D3 path. */
  double    projection_cam_x, projection_cam_y;
  /* Draw-GUI can change its logical coordinate space in the middle of an event with
   * display_set_gui_size().  The framebuffer does not change size at that point: subsequent
   * draws are transformed to the same physical GUI target immediately.  Keep this transform in
   * the renderer so sprites, surfaces, text and primitives all share the same semantics without
   * allocating a temporary full-resolution framebuffer every frame. */
  int       gui_pass_active;
  int       gui_base_logical_w, gui_base_logical_h;
  int       gui_logical_w, gui_logical_h;
  double    gui_scale_x, gui_scale_y;
  /* the application_surface: the buffer the game is rendered into and later
   * readable by draw_surface_* calls. Set by the frontend; same w/h as fbw/fbh. */
  uint32_t *app_surface; int app_draw_enable;   /* GM application_surface_draw_enable, default 1 */
  /* surface_resize(application_surface, ...) changes the application surface independently of
   * the active camera/view. The frontend normally lends its world framebuffer through
   * app_surface; once GML explicitly resizes surface 0 this owned buffer persists across room and
   * view-size changes. */
  uint32_t *app_surface_owned;
  /* Optional classic 2x vertical coverage plane.  The game pass replays texture draws at the
   * half-row samples as well as at logical pixel centres; the presentation pass then interleaves
   * them without a GPU.  This is a borrowed scratch buffer owned by the frontend. */
  uint32_t *classic_phase_y;
  uint32_t *app_phase_y;
  /* Optional exact-2x interpolation samples.  Each logical-size plane stores the result of
   * composing textured draws at the horizontal, vertical or diagonal half-pixel sample.  This
   * reproduces a scaled classic viewport without requiring a GPU-sized render target. */
  uint32_t *classic_interp_phase[3];
  uint32_t *app_interp_phase[3];
  int       interp;   /* texture_set_interpolation state: 0 nearest (GM default), 1 bilinear. Only
                       * upscaling surface/sprite blits honor it (nearest is exact for pixel art). */
  int       composites_app;  /* set by a draw when the game blits the application_surface stretched in
                              * the GUI/post pass. Read
                              * next frame to supersample that pass so its bilinear bloom renders. */
  int app_w, app_h;                             /* app_surface dims (the view render size) */
  int app_surface_opaque;                       /* frontend/render metadata: every app pixel has alpha 255 */
  int pending_underlay, underlay_x, underlay_y, underlay_w, underlay_h;  /* deferred default app-surface blit */
  int pending_fill; uint32_t pending_fill_color; /* deferred full-target overwrite */
  GmlSurface surface[GML_MAX_SURFACES]; int next_surface_id;
  /* draw state */
  uint32_t  color;  double alpha; int halign, valign, font, alphablend, circle_precision;
  int       alpha_test_enable;      /* fixed-function alpha-test state (disabled by default) */
  uint8_t   alpha_test_ref;         /* inclusive 0..255 reference set by gpu_set_alphatestref */
  uint8_t   color_write_mask; /* gpu_set_colorwriteenable RGBA bits 0..3; defaults to all enabled */
  int       software_overlay; /* bypass world-space D3 projection for a final 2D modal pass */
  int       blendmode;   /* 0=normal, 1=add, 2=(zero, inverse-source-colour), 3=source*destination. */
  int       blend_equation, blend_equation_alpha; /* 1 add, 2 max, 3 subtract, 4 reverse-subtract, 5 min */
  struct GmlGpuState {
    int alphablend, alpha_test_enable, blendmode, blend_equation, blend_equation_alpha, interp;
    uint8_t alpha_test_ref, color_write_mask;
  } gpu_state_stack[16];
  int       gpu_state_sp;
  int       fast_alpha_cull;  /* optional fast path: drop alpha contributions <= this 8-bit step */
  int       fb_opaque_known, fb_all_opaque, fb_all_transparent;  /* current target coverage metadata */
  /* GMS2 effect-layer RGB-noise seed cache. The stock filter is static for a given sampler,
   * surface size, animation parameter and colour, so evaluating its three sine hashes once avoids
   * turning a portable software post-process into the dominant per-frame cost. */
  uint32_t *layer_noise_rgb;
  int       layer_noise_w, layer_noise_h;
  uint32_t  layer_noise_tpag_ptr, layer_noise_colour;
  float     layer_noise_animation;
  /* One layer is filtered at a time. These reusable buffers avoid per-frame surface allocation;
   * the source target keeps alpha through blur and glow compositing. */
  uint32_t *layer_filter_src, *layer_filter_work, *layer_filter_aux;
  size_t    layer_filter_capacity;
  void     *layer_blur_taps;
  size_t    layer_blur_tap_capacity;
  int       layer_blur_sampler, layer_blur_noise_w, layer_blur_noise_h, layer_blur_interp;
  double    layer_blur_radius;
  int       layer_filter_active;
  /* Palette and lookup-texture state declarations. */
  struct GmlShaderPal { int has; uint8_t L[3],M[3],D[3],S[3];
    /* Literal alpha-discard pass-through fragment. The threshold and comparison are parsed from
     * the embedded GLSL, so texture draws can preserve hard sprite edges without a GPU. */
    int alpha_discard, alpha_discard_inclusive;
    float alpha_discard_cutoff;
    int lut;                    /* palette-LUT shader: out = palette[(src.r, row)] */
    char lut_row_uniform[32];   /* uniform float selecting the palette row */
    char lut_sampler[32];       /* sampler2D holding the palette texture */
    float lut_row;              /* current row (normalized v), set by shader_set_uniform_f */
    /* Palette-grid shader: find the source color in palette column 0, then sample the selected
     * column (with fractional interpolation). Uniform names/configuration are parsed from GLSL. */
    int grid;
    char grid_sampler[32], grid_uvs_uniform[32], grid_id_uniform[32], grid_pixel_uniform[32];
    float grid_uvs[4], grid_id, grid_pixel[2];
    /* CRT-geom post-process template (scanline + aperture-mask + gamma + optional radial warp and
     * corner vignette). Detected structurally from the SHDR GLSL; tunable constants parsed from it
     * so it stays data-driven (any GameMaker game shipping this shader family gets it). The full-
     * screen fragment runs per OUTPUT pixel in draw_surface_* when this shader is active. */
    int   crt;                  /* 1 = recognized CRT-geom fragment */
    float crt_input_gamma;      /* GLSL inputGamma  (e.g. 2.8) */
    float crt_output_gamma;     /* GLSL outputGamma (e.g. 3.2) */
    float crt_overscan_x, crt_overscan_y; /* GLSL overscan (e.g. 0.99,0.99) */
    float crt_cornersize;       /* GLSL cornersize   (e.g. 0.03) */
    float crt_cornersmooth;     /* GLSL cornersmooth (e.g. 80.0) */
    char  crt_sizes_uniform[32];      /* vec4 (src_w,src_h,out_w,out_h) uniform name */
    char  crt_distortion_uniform[32]; /* float distortion-amount uniform name */
    char  crt_distort_uniform[32];    /* bool  enable-radial-warp uniform name */
    char  crt_border_uniform[32];     /* bool  enable-corner-vignette uniform name */
    float crt_sizes[4];         /* current uniform value: (src_w,src_h,out_w,out_h) */
    float crt_distortion;       /* current distortion amount */
    int   crt_distort;          /* current bool: radial warp on */
    int   crt_border;           /* current bool: corner vignette on */
    /* Two-sample channel-offset post-process. The fragment samples the base texture twice, shifts
     * the second lookup along one texture axis by the product of two float uniforms, scales the
     * samples per channel, then adds them. The parser derives identifiers and coefficients from
     * GLSL, so this models the shader family rather than any asset or game. */
    int   dual_sample;
    int   dual_axis;            /* 0 = texture x, 1 = texture y */
    int   dual_sign;            /* +1 for +=, -1 for -= */
    float dual_base_gain[4];    /* RGBA multipliers for the unshifted lookup */
    float dual_shift_gain[4];   /* RGBA multipliers for the shifted lookup */
    char  dual_uniform[2][32];  /* the two float factors in the normalized-coordinate shift */
    float dual_value[2];        /* values supplied through shader_set_uniform_f */
    /* Quantized swirling-paint procedural fragment family.  This is recognized from the GLSL's
     * operations and its constants/uniforms are read from SHDR; filled primitives can therefore
     * execute it in the software renderer without baking an asset or shader name into the core. */
    int   paint, paint_opaque;
    char  paint_time_uniform[32], paint_resolution_uniform[32];
    float paint_time, paint_resolution[3];
    float paint_pixel_factor, paint_spin_ease, paint_spin_amount, paint_contrast;
    float paint_color[3][4];
    /* Luminance shader family: RGB becomes a parsed weighted dot product; an optional user float
     * scales source alpha (used by cross-fading variants of the same fragment). */
    int   grayscale, grayscale_has_alpha_uniform;
    char  grayscale_alpha_uniform[32];
    float grayscale_weight[3], grayscale_alpha;
  } *shader_pal; int n_shader_pal;
  int       lut_pal_sprite, lut_pal_frame;   /* texture_set_stage palette source (-1 = unset) */
  int       active_shader;   /* shader_set asset id, -1 = none. Reset per frame. */
  int       resolution_w;    /* requested final presentation width, or 0 for the game's base size.
                              * Window/display getters expose the same value so window-sized render
                              * surfaces and the frontend framebuffer stay in one coordinate space. */
  int       resolution_h;    /* requested final presentation height, or 0 for the game's base size. */
  int       presentation_w;  /* effective final width after presentation policies (for example an
                              * aspect override derived from the requested height). The requested
                              * core-option axes above stay unchanged. */
  int       presentation_h;  /* effective final height; recomputed by the libretro wrapper. */
  int       crt_shader_enable; /* run recognized embedded CRT post-process shaders (default 1). 0 =
                              * report them not-compiled and never execute them, so games fall back
                              * to their no-shader video modes (pre-emulation behavior). Palette/LUT
                              * shaders are NOT gated: they are integral to those games' rendering. */
  int       crt_shader_present; /* set at init when the SHDR chunk contains a recognized CRT-geom
                              * fragment. */
  /* Individually toggleable components of the recognized CRT-geom fragment (the 5 features intrinsic
   * to that shader family; generic, no game names). Each defaults to reproducing the shader as
   * shipped. crt_curvature/crt_vignette are -1=auto (follow the game's distort/border uniforms),
   * 0=force off, 1=force on; the others are 0/1 with default 1. */
  int       crt_mask_enable;      /* aperture (dot) mask (the aperture mask). Off -> flat 0.9 average:
                                   * same brightness, no chroma, so non-1:1 scaling shows no bands. */
  int       crt_scanlines_enable; /* scanline beam profile (the scanline profile). Off -> flat vertical. */
  int       crt_gamma_enable;     /* input/output gamma curve. Off -> linear (no CRT gamma). */
  int       crt_curvature;        /* radial warp (distort uniform): -1 auto / 0 off / 1 on. */
  int       crt_vignette;         /* corner darkening (border uniform): -1 auto / 0 off / 1 on. */
  int       crt_ff;               /* frontend is fast-forwarding. Presentation resolution and CRT
                                   * state remain unchanged; this is only an optimization hint. */
  int       aspect_fullwidth;     /* a forced-wide aspect is active AND this game's compositor should
                                   * span the whole frame: window_get_width/height report the widened
                                   * dimensions (below) so the game sizes its CRT surface to full width
                                   * instead of a centered 4:3 sub-rect. 0 = normal. */
  int       aspect_wide_w, aspect_wide_h; /* the forced-wide base dimensions. */
  /* Reusable software-CRT workspaces. High-resolution compositing used to allocate tens of
   * megabytes plus two convolution rows per worker every frame. These grow on demand and live
   * with the renderer. */
  void     *crt_gamma_scratch; size_t crt_gamma_scratch_cap;
  void     *crt_cols_scratch;  size_t crt_cols_scratch_cap;
  void     *crt_conv_scratch;  size_t crt_conv_scratch_cap;
  /* async atlas prefetch pool (opaque; see gml_render.c). Decodes atlases on worker threads so
   * first-use of a texture page does not stall a frame for a full BZ2+QOI atlas decode. */
  void     *prefetch; int prefetch_checked;
  size_t   atlas_decoded_bytes;
} GmlRender;

int  gml_render_init(GmlRender *r, GmlWin *win);
void gml_render_free(GmlRender *r);
/* Convert an instance or layer image_speed multiplier into subimages per step. Modern sprites
 * serialize their own rate as either frames per second or frames per step; runtime sprites
 * retain the legacy one-subimage-per-step multiplier. */
double gml_sprite_animation_delta(GmlRender *r, int sprite, double image_speed, double game_fps);
/* queue background decodes (worker threads; safe no-ops when disabled or already decoded) */
void gml_render_prefetch_atlas(GmlRender *r, int idx);
void gml_render_prefetch_sprite(GmlRender *r, int sprite);
void gml_render_prefetch_bg(GmlRender *r, int bg);
/* synchronously decode directly referenced pages; used at room boundaries to avoid first-draw hitches */
void gml_render_warm_sprite(GmlRender *r, int sprite);
void gml_render_warm_bg(GmlRender *r, int bg);
void gml_render_begin(GmlRender *r, uint32_t *fb, int w, int h, double camx, double camy);
void gml_render_gui_begin(GmlRender *r, int logical_w, int logical_h);
void gml_render_gui_set_size(GmlRender *r, int logical_w, int logical_h);
void gml_render_gui_end(GmlRender *r);
static inline int gml_render_gui_transform_active(const GmlRender *r){
  return r && r->gui_pass_active && r->target_sp==0 && r->target_id<0;
}
static inline void gml_render_gui_map_point(const GmlRender *r, double *x, double *y){
  if(!gml_render_gui_transform_active(r)) return;
  if(x) *x *= r->gui_scale_x;
  if(y) *y *= r->gui_scale_y;
}
static inline void gml_render_gui_map_scale(const GmlRender *r, double *xscale, double *yscale){
  if(!gml_render_gui_transform_active(r)) return;
  if(xscale) *xscale *= r->gui_scale_x;
  if(yscale) *yscale *= r->gui_scale_y;
}
static inline double gml_render_gui_logical_width(const GmlRender *r){
  return gml_render_gui_transform_active(r) && r->gui_scale_x>0.0
       ? (double)r->fbw/r->gui_scale_x : (double)(r?r->fbw:0);
}
static inline double gml_render_gui_logical_height(const GmlRender *r){
  return gml_render_gui_transform_active(r) && r->gui_scale_y>0.0
       ? (double)r->fbh/r->gui_scale_y : (double)(r?r->fbh:0);
}
static inline double gml_render_gui_logical_x(const GmlRender *r, double physical_x){
  return gml_render_gui_transform_active(r) && r->gui_scale_x>0.0
       ? physical_x/r->gui_scale_x : physical_x;
}
static inline double gml_render_gui_logical_y(const GmlRender *r, double physical_y){
  return gml_render_gui_transform_active(r) && r->gui_scale_y>0.0
       ? physical_y/r->gui_scale_y : physical_y;
}
void gml_render_set_pending_underlay(GmlRender *r, int x, int y, int w, int h);
void gml_render_flush_pending_underlay(GmlRender *r);
void gml_render_set_pending_fill(GmlRender *r, uint32_t color);
void gml_render_flush_pending_fill(GmlRender *r);
void gml_render_cancel_pending_fill(GmlRender *r);
void gml_render_cancel_pending_underlay(GmlRender *r);
void gml_render_prepare_draw(GmlRender *r);
void gml_render_prepare_opaque_rect(GmlRender *r, int x0, int y0, int x1, int y1);
/* Apply a recognized untextured procedural fragment to a filled screen-space rectangle.  Returns
 * nonzero only when the active shader handled the draw. */
int gml_render_shader_fill_rect(GmlRender *r, int x0, int y0, int x1, int y1);
static inline void gml_render_maybe_prepare_draw(GmlRender *r){
  if(r){
    if(r->pending_underlay || r->pending_fill) gml_render_prepare_draw(r);
    r->fb_all_transparent=0;
  }
}
static inline void gml_render_maybe_prepare_opaque_rect(GmlRender *r, int x0, int y0, int x1, int y1){
  if(r){
    if(r->pending_underlay || r->pending_fill) gml_render_prepare_opaque_rect(r,x0,y0,x1,y1);
    r->fb_all_transparent=0;
  }
}

void gml_draw_sprite_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                         double xs, double ys, double rot, uint32_t blend, double alpha);
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
void gml_d3_set_draw_depth(double depth);
int  gml_d3_is_active(void);
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
void gml_draw_layer_color_fill(GmlRender *r, uint32_t gmcol, double alpha); /* spriteless GMS2 bg layer */
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
void gml_draw_background_stretched(GmlRender *r, int bg, double x, double y, double w, double h, uint32_t color, double alpha);
void gml_draw_background_tiled(GmlRender *r, int bg, double x, double y, int htiled, int vtiled);
void gml_draw_background_ext(GmlRender *r, int bg, double x, double y, double xs, double ys, uint32_t color, double alpha);
void gml_draw_background_tiled_ext(GmlRender *r, int bg, double x, double y, double xs, double ys, uint32_t color, double alpha, int htiled, int vtiled);
void gml_draw_room_backgrounds(GmlRender *r, uint32_t bg_ptr, int want_fg);
void gml_draw_room_tiles(GmlRender *r, uint32_t tile_ptr);
void gml_draw_tile(GmlRender *r, int def, int sx, int sy, int w, int h, double x, double y);
/* Stretch the application surface and sprites in screen space, without camera offsets. */
int  gml_surface_create(GmlRender *r, int w, int h);
void gml_surface_free(GmlRender *r, int id);
int  gml_surface_exists(GmlRender *r, int id);
void gml_surface_resize(GmlRender *r, int id, int w, int h);
int  gml_surface_width(GmlRender *r, int id);
int  gml_surface_height(GmlRender *r, int id);
const uint32_t *gml_surface_pixels_read(GmlRender *r, int id, int *w, int *h);
void gml_surface_copy(GmlRender *r, int dst, int x, int y, int src);
int  gml_surface_set_target(GmlRender *r, int id);
void gml_surface_reset_target(GmlRender *r);
int  gml_surface_get_target(GmlRender *r);
void gml_draw_surface_stretched(GmlRender *r, int surf, double x, double y, double w, double h, uint32_t blend, double alpha);
void gml_draw_surface_ext(GmlRender *r, int surf, double x, double y,
                          double xs, double ys, double rot, uint32_t blend, double alpha);
void gml_draw_surface_part_ext(GmlRender *r, int surf, double sx, double sy, double sw, double sh,
                               double x, double y, double xs, double ys, uint32_t blend, double alpha);
int  gml_sprite_create_from_surface(GmlRender *r, int surf, int x, int y, int w, int h,
                                    int removeback, int smooth, int xorig, int yorig);
int  gml_sprite_replace_from_file(GmlRender *r, int sprite, const char *path, int imgnumb,
                                  int removeback, int smooth, int xorig, int yorig);
int  gml_sprite_replace_from_rgba(GmlRender *r, int sprite, uint8_t *rgba, int w, int h, int xorig, int yorig);
int  gml_sprite_append_from_rgba(GmlRender *r, uint8_t *rgba, int w, int h, int xorig, int yorig, const char *name);
int  gml_sprite_replace_from_rgba_frames(GmlRender *r, int sprite, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig);
int  gml_sprite_append_from_rgba_frames(GmlRender *r, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig, const char *name);
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
void gml_draw_text_ext_transformed(GmlRender *r, double x, double y, const char *str,
                                   double sep, double w, double xs, double ys, double rot,
                                   uint32_t blend, double alpha);
void gml_draw_text_transformed(GmlRender *r, double x, double y, const char *str,
                               double xs, double ys, double rot, uint32_t blend, double alpha);
int  gml_text_width(GmlRender *r, const char *str);
int  gml_text_height(GmlRender *r, const char *str);
void gml_draw_classic_game_information(GmlRender *r, uint32_t *framebuffer,
                                       int width, int height,
                                       const uint8_t *record, size_t record_size);

#endif
