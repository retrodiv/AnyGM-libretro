/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Private renderer storage and implementation records. */
#ifndef GML_RENDER_INTERNAL_H
#define GML_RENDER_INTERNAL_H

#include "gml_render.h"

#include "anygm_compatibility.h"

typedef struct { uint16_t y, x, len; uint8_t alpha; } GmlTpagAlphaRun;
typedef struct {
  int framebuffer_width, framebuffer_height;
  int destination_x, destination_y, width, height;
  int source_x, source_y;
  double draw_x, draw_y, scale_x, scale_y;
} GmlTpagInterpKey;
typedef struct {
  int y, x, len;
  uint8_t opaque;
} GmlTpagInterpRun;
typedef struct {
  int sx,sy,sw,sh, tx,ty, tw,th, bw,bh, atlas;  /* texture page item; tw/th = logical extent of the
                                                   stored rectangle (differs when the page is scaled) */
  int alpha_scanned, ax0, ay0, ax1, ay1; /* nontransparent source bbox, cached after atlas decode */
  int alpha_max;                          /* max source alpha in the texture-page item */
  int alpha_partial;                      /* at least one source texel has alpha 1..254 */
  int *alpha_row_min, *alpha_row_max;     /* per-source-row nontransparent span, optional */
  uint16_t *alpha_qrow_min, *alpha_qrow_max; /* per-alpha-threshold row spans, built lazily */
  uint8_t *alpha_qrow_built;
  GmlTpagAlphaRun *alpha_runs; int alpha_run_count, alpha_runs_built;
  uint8_t *alpha8_cache;                  /* compact alpha source for constant-colour masks */
  uint32_t *argb_cache;                  /* compact ARGB source pixels for hot rotated draws */
  uint32_t *solid_blur_alpha_cache;      /* recognized constant-colour convolution alpha */
  int solid_blur_alpha_shader, solid_blur_alpha_interp;
  uint32_t *interp_phase_cache[3];       /* lazy x/y/xy half-sample ARGB for exact-2x ports */
  uint32_t *fast8_draw_cache;            /* RGB plus draw-alpha for repeated large fast8 draws */
  double fast8_draw_alpha_key;
  double fast8_draw_pending_alpha_key;
  uint32_t fast8_draw_blend_key;
  uint32_t fast8_draw_pending_blend_key;
  int fast8_draw_alpha_floor_key;
  int fast8_draw_pending_alpha_floor_key;
  int fast8_draw_cache_valid, fast8_draw_cache_copy_255, fast8_draw_pending_count;
  /* A repeated, axis-aligned filtered draw of immutable atlas content can retain the filtered
   * source sample.  Replaying opaque spans and blending only coverage edges avoids evaluating the
   * same four texture taps at every output pixel on every frame. */
  uint32_t *interp_draw_cache;
  GmlTpagInterpRun *interp_draw_runs;
  int interp_draw_run_count;
  GmlTpagInterpKey interp_draw_key, interp_draw_pending_key;
  size_t interp_draw_cache_bytes;
  long interp_draw_last_frame;
  int interp_draw_cache_valid, interp_draw_pending_count;
} GmlTpag;
void gml_render_texture_page_cache_clear(GmlRender *render, GmlTpag *page);
void gml_render_interpolated_subrect_cache_clear(GmlRender *render);
void gml_render_apply_removeback_rgba(uint8_t *rgba, int width, int height,
                                      int removeback);
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
typedef struct {
  char *name;
  int x, y, width, height;
  int offset_x, offset_y, original_width, original_height;
  int rotate;
} GmlSpineRegion;
typedef struct {
  void *json;
  char *json_text, *atlas_text;
  GmlSpineRegion *region;
  int region_count;
  int texture_page;
  const char *default_animation;
} GmlSpine;
typedef struct {
  int texture_page;
  double x[4], y[4], u[4], v[4];
  uint32_t colour;
  double alpha;
} GmlSpineDrawItem;
typedef struct {
  const char *name;
  int parent;
  double x, y, rotation, scale_x, scale_y;
  double world_x, world_y, a, b, c, d;
} GmlSpinePoseBone;
typedef struct { const char *name; int originx, originy, w, h, n_frames; int *frame;
                 int ml, mr, mt, mb;                /* collision bbox coordinates: left,right,top,bottom */
                 const uint8_t *mask; int mask_rowb, mask_count;  /* SPRT collision mask: 1bpp */
                 uint8_t *runtime_mask;                           /* owned copy for runtime assets */
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
                 GmlSpine *spine;
                 /* GMS2.3+ nine-slice: draw scaled with fixed-size borders (corners never scale;
                  * edges/center follow their tile mode: 0=stretch 1=repeat 2=mirror 3=blankrepeat 4=hide) */
                 int ns_enabled, ns_l, ns_t, ns_r, ns_b, ns_tile[5]; } GmlSprite;
typedef struct {
  uint8_t *px; int w, h;                                          /* RGBA8, decoded lazily */
  uint32_t blob; size_t avail, chunk_end; int decode_attempted;    /* offsets into data.win, never addresses */
  uint8_t *external_blob; size_t external_size;                    /* encoded VFS sidecar, if any */
  int debug_dumped;                                                /* one-shot opt-in atlas diagnostic */
  int no_source_reported;                                          /* one-shot: page carries no blob */
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
  uint64_t tile_frame_length_us;
  const uint8_t *tile_ids;                                      /* GMS2 BGND tileset id table (little-endian u32s) */
} GmlBg;                                                        /* background/tileset -> texture page */
typedef struct { int32_t sx, sy, w, h; int16_t shift, offset; uint16_t ch; } GmlGlyph;
/* A signed pen adjustment for one left/right glyph pair. */
typedef struct { uint16_t left, right; int16_t amount; } GmlFontKern;
typedef struct {
  int sprite, first, prop, sep;
  uint32_t *map; int map_len;                                    /* font_add_sprite_ext explicit map */
  int map_fast[256];
  /* real FONT-chunk font: glyph sub-rects blitted from the data.win atlas at runtime
   * (no glyph data is bundled; parsed from the user-supplied data.win like sprites). */
  int real, atlas, line_height;                                  /* real=1; atlas index; line advance */
  int ascender, ascender_offset;                                 /* authored FONT vertical metrics */
  int sdf_spread;                                                /* zero when distance-field rendering is off */
  int align_height;                                              /* visible cell extent for valign */
  int runtime_owned;                                             /* font + atlas created after load */
  int subpixel;                                                   /* per-channel GDI coverage for classic info */
  GmlGlyph *glyphs; int n_glyphs, glyphs_sorted;
  GmlFontKern *kerning; int n_kerning;                           /* sorted by (left,right) */
  /* Runtime TTF fonts retain their rasterizer face so missing glyphs outside
   * the initial font_add range can be appended to the private atlas on demand. */
  struct GmlFontRasterFace *runtime_face;
  float runtime_scale; int runtime_ascent;
  int runtime_pen_x, runtime_pen_y, runtime_row_h, runtime_glyph_cap;
  int glyph_by_char[256];                                        /* fast ASCII lookup, -1 = none */
} GmlFont;                                                        /* sprite font or real FONT-chunk font */
typedef struct { uint32_t *px; int w, h, live;
                 int dirty;                       /* px changed since the RLE cache was built */
                 int opaque_known, all_opaque, all_transparent;  /* conservative coverage metadata */
                 uint8_t *rle; size_t rle_len, rle_cap;  /* cached savestate RLE (u32 nrun + pairs) */
} GmlSurface;

/* Renderer implementation and the engine composition root may include this
 * header to obtain storage size. Subsystem callers remain on gml_render.h and
 * must not dereference these records. */

typedef struct GmlRender {
  GmlWin   *win;
  /* Borrowed engine-owned fixed-function context. */
  GmlSoftware3D *software3d;
  long frame;                     /* simulation frame supplied by the owning VM */
  int classic;                    /* GM6/7/8 pixel rules that differ from Studio */
  GmlAtlas *atlas; int n_atlas;
  GmlTpag  *tpag; int n_tpag;
  uint32_t *tpag_ptr;                 /* source offsets parallel to tpag, owned by this renderer */
  GmlInterpSubrectCache *interp_subrect_cache;
  int interp_subrect_count, interp_subrect_capacity;
  size_t interp_subrect_bytes;
  size_t interp_draw_cache_bytes;
  GmlSprite *spr; int n_spr, base_n_spr, spr_cap, spr_has_free;
  /* content-hash index over spr[].name (lazy; for O(1) gml_render_named_sprite lookups).
   * spr_name_gen bumps on every append/delete so the index rebuilds after a runtime sprite's
   * name is added or a slot's name is cleared; base (SPRT) sprites never rename in place. */
  unsigned spr_name_gen;
  int32_t *spr_name_hix; uint32_t spr_name_hix_cap; unsigned spr_name_hix_built_gen;
  GmlRenderSkeletonState skeleton_state;
  int skeleton_state_active;
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
  /* An independently resized application surface changes the physical world raster without
   * changing room, camera, or instance coordinates. This frame-local transform keeps the
   * software raster at that physical resolution; it is reset by gml_render_begin and is not
   * part of canonical renderer state. */
  int       world_transform_active;
  double    world_scale_x, world_scale_y;
  /* Draw-GUI can change its logical coordinate space in the middle of an event with
   * display_set_gui_size().  The framebuffer does not change size at that point: subsequent
   * draws are transformed to the same physical GUI target immediately.  Keep this transform in
   * the renderer so sprites, surfaces, text and primitives all share the same semantics without
   * allocating a temporary full-resolution framebuffer every frame. */
  int       gui_pass_active;
  int       gui_base_logical_w, gui_base_logical_h;
  int       gui_logical_w, gui_logical_h;
  double    gui_scale_x, gui_scale_y;
  int       gui_maximise_active;
  double    gui_maximise_xscale, gui_maximise_yscale;
  double    gui_maximise_xoffset, gui_maximise_yoffset;
  /* the application_surface: the buffer the game is rendered into and later
   * readable by draw_surface_* calls. Set by the host; same w/h as fbw/fbh. */
  uint32_t *app_surface; int app_draw_enable;
  int content_composited_screen;       /* a surface was drawn onto the base canvas */
  uint64_t content_authored_surfaces;  /* frame-local authored surfaces, including cleared ones */
  /* GM application_surface_draw_enable, default 1 */
  /* surface_resize(application_surface, ...) changes the application surface independently of
   * the active camera/view. The host normally lends its world framebuffer through
   * app_surface; once GML explicitly resizes surface 0 this owned buffer persists across room and
   * view-size changes. */
  uint32_t *app_surface_owned;
  /* Optional classic 2x vertical coverage plane.  The game pass replays texture draws at the
   * half-row samples as well as at logical pixel centres; the presentation pass then interleaves
   * them without a GPU.  This is a borrowed scratch buffer owned by the host. */
  uint32_t *classic_phase_y;
  uint32_t *app_phase_y;
  /* Optional exact-2x interpolation samples.  Each logical-size plane stores the result of
   * composing textured draws at the horizontal, vertical or diagonal half-pixel sample.  This
   * reproduces a scaled classic viewport without requiring a GPU-sized render target. */
  uint32_t *classic_interp_phase[3];
  uint32_t *app_interp_phase[3];
  int       interp;   /* texture_set_interpolation state: 0 nearest (GM default), 1 bilinear. */
  int       font_sdf_active;
  double    font_sdf_width;
  int       composites_app;  /* set by a draw when the game blits the application_surface stretched in
                              * the GUI/post pass. Read
                              * next frame to supersample that pass so its bilinear bloom renders. */
  int app_w, app_h;                             /* app_surface dims (the view render size) */
  int app_surface_opaque;                       /* host/render metadata: every app pixel has alpha 255 */
  int pending_underlay, underlay_x, underlay_y, underlay_w, underlay_h;  /* deferred default app-surface blit */
  int pending_fill; uint32_t pending_fill_color; /* deferred full-target overwrite */
  /* Deferred terminal application-surface presentation. Recorded only when it covers the whole
   * target, so whatever it would have overwritten cannot matter, and flushed by the same hooks
   * that flush the two records above. */
  int pending_presentation;
  int presentation_deferral_enabled;
  GmlRenderDeferredPresentation presentation;
  GmlSurface surface[GML_MAX_SURFACES]; int next_surface_id;
  /* draw state */
  uint32_t  color;  double alpha; int halign, valign, font, alphablend, circle_precision;
  int       alpha_test_enable;      /* fixed-function alpha-test state (disabled by default) */
  uint8_t   alpha_test_ref;         /* inclusive 0..255 reference set by gpu_set_alphatestref */
  uint8_t   color_write_mask; /* gpu_set_colorwriteenable RGBA bits 0..3; defaults to all enabled */
  /* A reusable target-sized snapshot lets masked sprite draws merge enabled channels after
   * the ordinary rasterizer has produced its exact blend result. */
  uint32_t *color_write_scratch;
  size_t    color_write_scratch_capacity;
  int       software_overlay; /* bypass world-space D3 projection for a final 2D modal pass */
  int       blendmode;   /* 0=normal, 1=add, 2=(zero, inverse-source-colour), 3=multiply, 4=max preset,
                          * 5=(source colour, one). */
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
  /* GM palette-template shaders threshold a canonical render and remap it to palette colors.
   * Parsed data-driven from the SHDR chunk's GLSL at init; shader_set activates one and the
   * surface-composite blit applies the map per pixel. has==0 -> unknown shader, no-op. */
  struct GmlShaderPal { int has; uint8_t L[3],M[3],D[3],S[3];
    /* Passthrough texture fragments that clear a subset of sampled RGB channels after vertex
     * colour modulation (for example `.bg = vec2(0)`). The parser retains the channels that the
     * fragment leaves enabled; alpha is unaffected. */
    int channel_mask, channel_mask_keep;
    /* Literal alpha-discard pass-through fragment. The threshold and comparison are parsed from
     * the embedded GLSL, so texture draws can preserve hard sprite edges without a GPU. */
    int alpha_discard, alpha_discard_inclusive;
    float alpha_discard_cutoff;
    /* Ordered-dither cutout family. The fragment samples gm_BaseTexture once, multiplies it by the
     * vertex colour, then replaces the alpha with one cell of a 4x4 pattern selected by the
     * interpolated object-space position. The pattern is built from a single float uniform
     * quantised to seventeen levels, so the result is an exact per-pixel keep-or-drop decision
     * with no filtering: a fade that a software renderer can reproduce rather than approximate.
     * Drawing such a pass unshaded paints the source at full coverage, which is the opposite of
     * what a low uniform asks for. */
    int ordered_dither;
    char ordered_dither_uniform[32];
    float ordered_dither_alpha;
    /* Four-band intensity quantiser. Stored thresholds select one of four
     * configured RGB triplets from the mean input-channel intensity. */
    int quantise4;
    char quantise4_uniform[4][32];
    float quantise4_colour[4][3];
    int quantise4_set;
    float quantise4_threshold[3];
    /* Constant-colour alpha-mask family. The fragment samples the base texture once, clears alpha
     * below a parsed literal threshold, and emits a uniform RGB with the remaining source alpha.
     * The parser derives the uniform and comparison from the complete fragment operation graph. */
    int solid_alpha_mask, solid_alpha_mask_inclusive;
    char solid_alpha_mask_uniform[32];
    float solid_alpha_mask_cutoff, solid_alpha_mask_colour[3];
    int solid_alpha_mask_cutoff_step;
    uint32_t solid_alpha_mask_rgb;
    /* Constant-colour alpha convolution. The horizontal pass is a weighted sample sum; the
     * vertical pass multiplies that accumulator by parsed weighted source-alpha terms. Both tap
     * graphs and normalized texture steps are retained from the complete fragment. */
    int solid_blur_alpha;
    char solid_blur_alpha_uniform[32];
    float solid_blur_alpha_colour[3];
    uint32_t solid_blur_alpha_rgb;
    float solid_blur_alpha_step_x, solid_blur_alpha_step_y;
    int solid_blur_alpha_x_count, solid_blur_alpha_y_count;
    float solid_blur_alpha_x_offset[16], solid_blur_alpha_x_weight[16];
    float solid_blur_alpha_y_offset[16], solid_blur_alpha_y_weight[16];
    /* Vertex-stage colour modulation multiplies a colour varying by a uniform vec4 before
     * the fragment stage samples the texture. */
    int vertex_colour_blend, vertex_colour_blend_alpha_only, vertex_colour_blend_set;
    char vertex_colour_blend_uniform[64];
    float vertex_colour_blend_value[4];
    int lut;                    /* palette-LUT shader: out = palette[(src.r, row)] */
    char lut_row_uniform[32];   /* uniform float selecting the palette row */
    char lut_sampler[32];       /* sampler2D holding the palette texture */
    float lut_row;              /* current row (normalized v), set by shader_set_uniform_f */
    /* Indexed grayscale palette family. A source gray level selects a palette row while a
     * normalized float selects its column. The parser derives every handle from the fragment
     * operation graph; the renderer samples the staged sprite locally, independent of atlas UVs. */
    int lut_indexed, lut_has_colorise, lut_has_bounds;
    char lut_uvs_uniform[32], lut_offset_uniform[32], lut_colors_uniform[32];
    char lut_colorise_uniform[32], lut_bounds_uniform[32];
    float lut_uvs[4], lut_offset, lut_colors, lut_colorise[4], lut_bounds[4];
    /* Palette-grid shader: find the source color in palette column 0, then sample the selected
     * column (with fractional interpolation). Uniform names/configuration are parsed from GLSL. */
    int grid;
    char grid_sampler[32], grid_uvs_uniform[32], grid_id_uniform[32], grid_pixel_uniform[32];
    float grid_uvs[4], grid_id, grid_pixel[2];
    /* CRT-geom post-process template (scanline + aperture-mask + gamma + optional radial warp and
     * corner vignette). Detected structurally from the SHDR GLSL; tunable constants parsed from it
     * so it stays data-driven. The full-
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
    /* All-in-one sampled CRT family: quintic source reconstruction, channel convergence,
     * phosphor texture, periodic scanlines, glow, optional reflection/interlace/overlay. The
     * operation graph and uniforms are discovered from GLSL; values and staged textures remain
     * per-shader runtime state. Indices 0..19 have stable semantic meanings in gml_builtin.c and
     * gml_render.c, while sampler slots 0..2 are mask/noise/backdrop. */
    int   sampled_crt;
    char  sampled_crt_uniform[20][32];
    float sampled_crt_value[20][4];
    char  sampled_crt_sampler[3][32];
    int   sampled_crt_sprite[3], sampled_crt_frame[3];
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
    /* Three-channel HSV scan post-process. Each RGB channel comes from an independently offset
     * base-texture lookup; value is shaped by a separable vignette and a periodic row term, then
     * saturation compensates for the value loss. The complete graph and every literal below are
     * parsed structurally from GLSL, keeping the software implementation asset-independent. */
    int   hsv_scan;
    float hsv_scan_channel_offset;
    float hsv_scan_vignette_base, hsv_scan_vignette_gain, hsv_scan_vignette_scale;
    float hsv_scan_row_base, hsv_scan_row_value_gain, hsv_scan_row_sine_gain;
    float hsv_scan_row_frequency, hsv_scan_saturation_gain;
    /* Binary-palette HSV variant. It samples an inset UV, maps the sampled red channel through a
     * literal threshold into two parsed colours, applies the HSV value/saturation graph above,
     * then mixes toward an unwarped sample at the horizontal edges. */
    int   hsv_scan_binary_palette;
    float hsv_scan_uv_scale, hsv_scan_binary_threshold;
    float hsv_scan_binary_high[3], hsv_scan_binary_low[3];
    float hsv_scan_mix_center, hsv_scan_mix_min, hsv_scan_mix_max;
    /* Multi-stage noise/jumble post-process. The fragment combines line displacement, block
     * displacement, channel-separated samples, and seeded multiplicative noise. Recognition
     * retains the complete custom-uniform interface in semantic declaration order; runtime
     * values stay per shader and the software display pass evaluates the parsed graph. */
    int   noise_jumble;
    char  noise_jumble_uniform[GML_NOISE_JUMBLE_UNIFORM_COUNT][32];
    float noise_jumble_value[GML_NOISE_JUMBLE_UNIFORM_COUNT][2];
    /* Radial sine displacement: samples the base texture at uv + direction*wave(distance,time).
     * The six controls are discovered from the fragment declarations/operation graph and remain
     * generic runtime state: time, centre vec2, resolution vec2, amount, divisor and speed. */
    int   radial_wave;
    char  radial_wave_uniform[6][32];
    float radial_wave_value[6][2];
    /* Single-sample UV displacement fragments.  Mode 1 offsets texture x with a cosine of source
     * v and time; mode 2 uses a sine of vertex y and time, tapered by source u/v.  Constants and
     * the time handle are parsed from the fragment assignment, so layer shaders can run in the
     * software atlas path without depending on shader or asset names. */
    int   uv_wave_mode;
    char  uv_wave_uniform[32];
    float uv_wave_time;
    float uv_wave_uv_factor, uv_wave_time_factor, uv_wave_divisor;
    float uv_wave_size_x, uv_wave_spatial, uv_wave_amplitude, uv_wave_taper;
    /* Quantized swirling-paint procedural fragment family.  This is recognized from the GLSL's
     * operations and its constants/uniforms are read from SHDR; filled primitives can therefore
     * execute it in the software renderer without baking an asset or shader name into the core. */
    int   paint, paint_opaque, paint_resolution_mediump;
    char  paint_time_uniform[32], paint_resolution_uniform[32];
    float paint_time, paint_resolution[3];
    float paint_pixel_factor, paint_spin_ease, paint_spin_amount, paint_contrast;
    float paint_color[3][4];
    /* Luminance shader family: RGB becomes a parsed weighted dot product; an optional user float
     * scales source alpha (used by cross-fading variants of the same fragment). */
    int   grayscale, grayscale_has_alpha_uniform;
    char  grayscale_alpha_uniform[32];
    float grayscale_weight[3], grayscale_alpha;
    /* A fragment that calls no sampling function reads no pixels at all: it computes its colour
     * from coordinates, time and its own uniforms. Such a program has no representation as an
     * operation on anything that was drawn, so drawing it unshaded is not an approximation of it —
     * it paints the primitive's own colour, which the shader would have thrown away. A fragment
     * that samples anything, including a surface the content bound to a stage rather than the
     * texture under the draw, is deliberately not in this class. Recognition still wins over the
     * flag: the recognized paint family is procedural too and this renderer executes it. */
    int   procedural;
  } *shader_pal; int n_shader_pal;
  int       lut_pal_sprite, lut_pal_frame;   /* texture_set_stage palette source (-1 = unset) */
  int       active_shader;   /* shader_set asset id, -1 = none. Reset per frame. */
  int       monitor_w;       /* virtual monitor width reported to content, or 0 for fallback. */
  int       monitor_h;       /* virtual monitor height reported to content, or 0 for fallback. */
  int       presentation_w;  /* effective final width after presentation policies. */
  int       presentation_h;  /* effective final height; recomputed by the host wrapper. */
  int       crt_shader_enable; /* run recognized embedded CRT post-process shaders (default 1). 0 =
                              * report them not-compiled and never execute them, so content falls back
                              * to their no-shader video modes (pre-emulation behavior). Palette/LUT
                              * shaders are not gated because they are integral to rendering. */
  int       crt_shader_present; /* set at init when the SHDR chunk contains a recognized CRT-geom
                              * fragment. */
  /* Individually toggleable components of the recognized CRT-geom fragment (the 5 features intrinsic
   * to that shader family). Each defaults to reproducing the shader as shipped.
   * crt_curvature/crt_vignette are -1=auto (follow the content-provided distort/border uniforms),
   * 0=force off, 1=force on; the others are 0/1 with default 1. */
  int       crt_mask_enable;      /* aperture (dot) mask (the aperture mask). Off -> flat 0.9 average:
                                   * same brightness, no chroma, so non-1:1 scaling shows no bands. */
  int       crt_scanlines_enable; /* scanline beam profile (the scanline profile). Off -> flat vertical. */
  int       crt_gamma_enable;     /* input/output gamma curve. Off -> linear (no CRT gamma). */
  int       crt_curvature;        /* radial warp (distort uniform): -1 auto / 0 off / 1 on. */
  int       crt_vignette;         /* corner darkening (border uniform): -1 auto / 0 off / 1 on. */
  int       shader_report_all_compiled; /* non-zero (default): shader_is_compiled answers yes for
                                   * every payload shader, as a GPU would, and an unrecognized one
                                   * draws unshaded. 0: only recognized families answer yes, so
                                   * content carrying its own no-shader presentation selects it. */
  int       crt_ff;               /* host is fast-forwarding. Presentation resolution and CRT
                                   * state remain unchanged; this is only an optimization hint. */
  int       aspect_fullwidth;     /* a forced-wide aspect is active and the compositor should
                                   * span the whole frame: window_get_width/height report the widened
                                   * dimensions (below) so the CRT surface spans the full width
                                   * instead of a centered 4:3 sub-rect. 0 = normal. */
  int       aspect_wide_w, aspect_wide_h; /* the forced-wide base dimensions. */
  /* Reusable software-CRT workspaces. High-resolution compositing used to allocate tens of
   * megabytes plus two convolution rows per worker every frame. These grow on demand and live
   * with the renderer. */
  void     *crt_gamma_scratch; size_t crt_gamma_scratch_cap;
  void     *crt_cols_scratch;  size_t crt_cols_scratch_cap;
  void     *crt_conv_scratch;  size_t crt_conv_scratch_cap;
  void     *crt_tables;        /* per-renderer lookup tables for the software CRT path */
  void     *crt_warp_geometry_cache; /* invariant curved-CRT sampling geometry */
  void     *hsv_binary_lut_cache;       /* derived binary-palette post-process colours */
  /* async atlas prefetch pool (opaque; see gml_render_atlas.c). Decodes atlases on worker threads so
   * first-use of a texture page does not stall a frame for a full BZ2+QOI atlas decode. */
  void     *prefetch; int prefetch_checked;
  void     *row_pool;                 /* persistent compositor workers, owned by this renderer */
  /* Consecutive large rotated source-over draws can share one row-pool dispatch while retaining
   * their order independently inside every framebuffer row. The opaque command storage is owned
   * by the blitter; all other renderer operations flush it before observing the target. */
  void     *rotated_batch;
  int       rotated_batch_count, rotated_batch_capacity;
  int       rotated_batch_building;
  size_t   atlas_decoded_bytes;
  size_t   atlas_prefetch_budget;
  int      axis_cache_log_count;
  int      sprite_position_log_count;
  int      surface_draw_logging;
  int      surface_draw_log_count;
  int      text_width_log_count;
  int      dual_shader_fast_log_count;
  int      noise_jumble_log_count;
  int      hsv_shader_log_count;
  int      hsv_shader_fast_log_count;
  int      sampled_crt_log_count;
  int      lut_shader_log_count;
  int      stretched_shader_log_count;
  long     generated_sprite_log_count;
} GmlRender;

/* Preserve alpha on explicit surface targets and on a sampled application surface.
 * A classic direct frame is not a sampled application surface; preserving its
 * coverage would let subtract blending remove visible frame coverage. */
static inline int gml_render_target_preserves_alpha(const GmlRender *render){
  if(!render) return 0;
  if(render->target_sp>0) return 1;
  return render->app_surface && render->fb==render->app_surface &&
         !anygm_policy_uses_classic_runtime(render->win);
}

/* Renderer-private cross-unit operations. These are implementation seams, not
 * subsystem-facing APIs; callers outside renderer .c owners must not use them. */
typedef void (*GmlRowBandFn)(void *ctx,int row_start,int row_end,int slot);

#define GML_ROW_THREADS_MAX 32
#define GML_ROW_THREADS_AUTO 16

const char *render_setting(const GmlRender *r,const char *name);
int rprof_enabled(void);
double rprof_now(void);
void rprof_add(const char *label,GmlRender *r,GmlTpag *tpag,double milliseconds,
               unsigned long long pixels);
int rprof_tpag_id(GmlRender *r,GmlTpag *t);
int tpag_index_for_ptr(GmlRender *r,uint32_t pointer);
uint32_t u32(const uint8_t *data,uint32_t offset);
uint16_t u16(const uint8_t *data,uint32_t offset);
uint32_t be32(const uint8_t *data);

void atlas_pool_free(GmlRender *r);
uint8_t *atlas_pixels(GmlRender *r,int index);
void gml_render_materialize_scaled_pages(GmlRender *r);
void parse_txtr(GmlRender *r);
void parse_shader_palettes(GmlRender *r);
int gml_render_parse_spine(GmlRender *r,GmlSprite *sprite,uint32_t record,
                           uint32_t header);
void gml_render_free_spine(GmlSpine *spine);
int gml_render_spine_build_items(
  GmlRender *r,const GmlSprite *sprite,double x,double y,double xscale,
  double yscale,double rotation,GmlSpineDrawItem *items,int capacity);
void gml_run_row_bands_n(GmlRender *r,int height,int thread_count,
                         GmlRowBandFn function,void *context);
void gml_run_row_bands(GmlRender *r,int height,GmlRowBandFn function,void *context);
void gml_render_flush_rotated_batch(GmlRender *r);
uint32_t *surface_pixels(GmlRender *r,int surface,int *width,int *height);
int surface_known_opaque(GmlRender *r,int surface);
int surface_known_transparent(GmlRender *r,int surface);
int rect_covers_target(GmlRender *r,int x0,int y0,int x1,int y1);
/* Keep deferred terminal presentation recording outside the composition kernels. */
void render_write_deferred_presentation(GmlRender *r);
int render_record_deferred_underlay(GmlRender *r,const uint32_t *src,int sw,int sh,
                                    int x0,int y0,int W,int H,int source_all_opaque);
void render_present_first_generation(GmlRender *r,int surf,const uint32_t *src,int sw,int sh,
                                     int x0,int y0,int x1,int y1,
                                     double dx,double dy,double dw,double dh);
void draw_surface_region(GmlRender *r,int surface,double source_x,double source_y,
                         double source_width,double source_height,double destination_x,
                         double destination_y,double destination_width,
                         double destination_height,uint32_t blend,double alpha);
void crt_tables_free(GmlRender *r);
void crt_warp_geometry_cache_free(GmlRender *r);
void hsv_binary_lut_cache_free(GmlRender *r);

void draw_surface_dual_sample(GmlRender *r,const struct GmlShaderPal *shader,int surface,
                              double source_x,double source_y,double source_width,
                              double source_height,double destination_x,double destination_y,
                              double destination_width,double destination_height,
                              uint32_t blend,double alpha);




void parse_font(GmlRender *r);
int build_default_font(GmlRender *r);
void render_rotation_sincos(double degrees,double *cosine,double *sine);
const uint8_t *runtime_frame_rgba(GmlSprite *sprite,int frame);
void gml_render_sprite_cache_free(GmlSprite *sprite);
void blit(GmlRender *r,GmlTpag *tpag,double x,double y,double xscale,double yscale,
          uint32_t blend,double alpha);
void blit_rgba_sprite(GmlRender *r,GmlSprite *owner,const uint8_t *source,
                      int source_width,int source_height,double x,double y,
                      double xscale,double yscale,double rotation,int origin_x,
                      int origin_y,uint32_t blend,double alpha,int apply_camera,
                      const int *row_min,const int *row_max,int source_opaque);
void blit_rotated(GmlRender *r,GmlSprite *sprite,GmlTpag *tpag,double x,double y,
                  double xscale,double yscale,double rotation,uint32_t blend,double alpha);

#endif
