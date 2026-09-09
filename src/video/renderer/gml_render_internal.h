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
  int project_authored_edges;                    /* a tilemap cell is one quad in an authored grid;
                                                   derive its far edge from its world coordinate */
  int alpha_scanned, ax0, ay0, ax1, ay1; /* nontransparent source bbox, cached after atlas decode */
  int alpha_max;                          /* max source alpha in the texture-page item */
  int alpha_partial;                      /* at least one source texel has alpha 1..254 */
  int black_scanned, all_black;           /* cached source-colour classification */
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
  int runtime_font_page;                                          /* reusable only by runtime fonts */
} GmlAtlas;
typedef struct {
  int atlas, sx, sy, sw, sh;
  int projected_x, projected_y, dest_x0, dest_y0;
  double edge_x0, edge_x1, edge_y0, edge_y1;
  uint32_t *phase[3];
} GmlInterpSubrectCache;
typedef struct {
  int tpag;
  const char *name;                                             /* BGND record name, for background_get_name */
  int transparent, smooth, preload;
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
  char *runtime_source_path;
  uint8_t runtime_source_sha256[32];
  int runtime_pixel_height, runtime_first, runtime_last;
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

/* Renderer-state reconstruction uses the ordinary font loader and glyph-cache owner. */
int gml_render_restore_runtime_font(GmlRender *render,int font,const char *path,
                                    const uint8_t sha256[32],int pixel_height,
                                    int first,int last,const uint32_t *characters,int count);
enum { GML_SHADER_GENERIC_UNIFORMS=32, GML_SHADER_GENERIC_SAMPLERS=4 };
/* Handles for the content's own uniforms and samplers live outside the recognized families' slot
 * space: bit 20 marks them, bits 8..19 carry the shader, bit 7 marks a sampler, bits 0..6 the
 * index. */
#define GML_RENDER_GENERIC_HANDLE_FLAG 0x100000
#define GML_RENDER_GENERIC_SAMPLER_FLAG 0x80
#define GML_RENDER_GENERIC_HANDLE(shader,index) \
  (GML_RENDER_GENERIC_HANDLE_FLAG|(((shader)&0xFFF)<<8)|((index)&0x7F))
#define GML_RENDER_GENERIC_SAMPLER_HANDLE(shader,index) \
  (GML_RENDER_GENERIC_HANDLE(shader,index)|GML_RENDER_GENERIC_SAMPLER_FLAG)

typedef struct { uint32_t *px; int w, h, live;
                 int dirty;                       /* px changed since the RLE cache was built */
                 int opaque_known, all_opaque, all_transparent;  /* conservative coverage metadata */
                 uint8_t *rle; size_t rle_len, rle_cap;  /* cached savestate RLE (u32 nrun + pairs) */
} GmlSurface;      /* XRGB8888 runtime surface */

/* Renderer implementation and the engine composition root may include this
 * header to obtain storage size. Subsystem callers remain on gml_render.h and
 * must not dereference these records. */

enum { GML_SHADED_FRAME_CACHE=32 };

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
  /* Set during Post-Draw after the automatic presentation decision. */
  int app_draw_presentation_settled;
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
  /* Coverage for the automatic first-generation screen composite. Explicit surface-0 draws read
   * the ordinary alpha stored in app_surface instead. */
  uint8_t *app_presentation_coverage;
  uint8_t *app_presentation_alpha_scratch;
  size_t app_presentation_coverage_capacity;
  int app_presentation_coverage_active;
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
  /* Fixed-function flat fog state for ordinary sprite drawing. */
  int       fog_flat; uint32_t fog_flat_rgb;
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
  int       layer_filter_active;
  /* GM palette-template shaders threshold a canonical render and remap it to palette colors.
   * Parsed data-driven from the SHDR chunk's GLSL at init; shader_set activates one and the
   * surface-composite blit applies the map per pixel. has==0 -> unknown shader, no-op. */
  struct GmlShaderPal {
    /* Ten-colour threshold palette. The fragment splits sampled pixels on red, then selects one
     * of five uniform colours from descending green thresholds in either branch. Names,
     * thresholds and values all come from the embedded shader and its uniform calls. */
    int threshold_palette;
    char threshold_palette_uniform[10][32];
    float threshold_palette_red;
    float threshold_palette_green[2][4];
    uint8_t threshold_palette_colour[10][3];
    unsigned threshold_palette_set;
    /* Indexed-palette brightness shift. The fragment classifies a sampled colour into one of ten
     * ordered ids, adds a scalar uniform, then selects a literal colour from separate ramps. */
    int indexed_brightness;
    char indexed_brightness_uniform[32];
    float indexed_brightness_red;
    float indexed_brightness_green[2][4];
    float indexed_brightness_family_cut;
    float indexed_brightness_negative_cut;
    float indexed_brightness_special_min, indexed_brightness_special_max;
    float indexed_brightness_special_threshold[2];
    uint8_t indexed_brightness_special_colour[3][3];
    float indexed_brightness_low_threshold[6];
    uint8_t indexed_brightness_low_colour[7][3];
    float indexed_brightness_high_threshold[7];
    uint8_t indexed_brightness_high_colour[8][3];
    float indexed_brightness_value;
    int indexed_brightness_set;
    /* Passthrough texture fragments that clear a subset of sampled RGB channels after vertex
     * colour modulation (for example `.bg = vec2(0)`). The parser retains the channels that the
     * fragment leaves enabled; alpha is unaffected. */
    int channel_mask, channel_mask_keep;
    /* Sampled colour-key alpha. The fragment preserves its modulated texture colour and clears
     * alpha when one RGB channel falls below a literal threshold. The channel, comparison and
     * threshold are parsed from SHDR rather than tied to an asset or shader identifier. */
    int channel_alpha_key, channel_alpha_key_channel, channel_alpha_key_inclusive;
    float channel_alpha_key_cutoff;
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
     * texture under the draw, is deliberately not in this class. */
    int   procedural;
    /* The fragment reads the pixel's place on the render target, so its answer depends on where
     * the draw lands and not only on the texels it samples: it cannot be evaluated once for a
     * texture-page rectangle and reused. */
    int   position_dependent;
    /* The content's own program, kept for the host's graphics context to execute. The four source
     * texts are lent from the SHDR chunk. What the game sets through shader_set_uniform_* and
     * texture_set_stage is kept by name: the program is the game's, so the runtime knows none of
     * its controls and forwards all of them. Not serialized: content sets them each frame or at
     * load, and a restored session re-runs whichever code did. */
    const char *source_vertex_es,*source_fragment_es,*source_vertex_gl,*source_fragment_gl;
    struct { char name[32]; float value[16]; int count; int integer; }
      generic_uniform[GML_SHADER_GENERIC_UNIFORMS];
    int generic_uniform_count;
    struct { char name[32]; int texture; } generic_sampler[GML_SHADER_GENERIC_SAMPLERS];
    int generic_sampler_count;
    /* The host's graphics context refused the program; shader_is_compiled answers no from then on,
     * exactly as a driver that rejects it would make the original answer. */
    int gpu_failed;
  } *shader_pal; int n_shader_pal;
  /* Scratch plane a sprite frame is expanded into when a content shader samples it. */
  uint32_t *content_sampler_plane; size_t content_sampler_plane_capacity;
  /* A content program executed in the middle of a frame by the host, and the plane its result
   * lands in, drawn as surface GML_RENDER_SHADED_SURFACE. */
  GmlRenderShaderExecutor shader_executor; void *shader_executor_context;
  /* Whether a graphics device is actually present now (adoption succeeded), refined per frame —
   * used to decide whether a program actually runs. And the finalized session policy, stable
   * across context resets — used to answer a shader-support query content may cache. */
  int shader_device_present;
  int shader_device_expected;
  uint32_t *shaded_plane; size_t shaded_plane_capacity;
  int shaded_plane_width,shaded_plane_height;
  /* A plane the composition reads instead of shaded_plane, so a cached result composes without
   * being copied first. */
  const uint32_t *shaded_plane_borrowed;
  /* The shaded plane in the byte order a decoded page uses, for the blits that read one. */
  uint8_t *shaded_bytes; size_t shaded_bytes_capacity;
  /* A target-sized plane a run of draws is put on plain, so one program pass covers all of them
   * instead of one per draw. Only for a program whose answer moves with the draw, which cannot be
   * kept for a rectangle and would otherwise pay a device round trip per glyph. */
  uint32_t *run_plane; size_t run_plane_capacity;
  /* The colour a gathered run hands the program, rather than having baked it in. */
  float shade_vertex_colour[4]; int shade_vertex_colour_valid;
  int run_plane_active;
  /* Shaded sprite frames. A program applied to a sprite transforms its texels, so its answer
   * depends on the frame, the program and the values set on it — not on where the frame is drawn.
   * Evaluating it once per frame and reusing it turns a per-draw device round trip into one per
   * distinct frame and avoids repeated device evaluations. */
  struct {
    int atlas,sx,sy,shader;
    uint64_t fingerprint;
    int w,h;
    uint32_t *px;
    uint32_t last_used;
  } shaded_frame[GML_SHADED_FRAME_CACHE];
  uint32_t shaded_frame_clock;
  uint32_t shaded_requests,shaded_draws;
  /* Per-shader, per-shape accounting: [shader][kind][executed]. Allocated only when asked for. */
  uint32_t *shader_account;
  int shader_account_shaders;
  int       lut_pal_sprite, lut_pal_frame;   /* texture_set_stage palette source (-1 = unset) */
  int       active_shader;   /* shader_set asset id, -1 = none. Reset per frame. */
  int       monitor_w;       /* virtual monitor width reported to content, or 0 for fallback. */
  int       monitor_h;       /* virtual monitor height reported to content, or 0 for fallback. */
  int       presentation_w;  /* effective final width after presentation policies. */
  int       presentation_h;  /* effective final height; recomputed by the host wrapper. */
  int       shader_report_all_compiled; /* non-zero (default): shader_is_compiled answers yes for
                                   * every payload shader, as a GPU would, and an unrecognized one
                                   * draws unshaded. 0: only recognized families answer yes, so
                                   * content carrying its own no-shader presentation selects it. */
  int       aspect_fullwidth;     /* a forced-wide aspect is active and the compositor should
                                   * span the whole frame: window_get_width/height report the widened
                                   * dimensions (below) so the content's compositing surface spans the full width
                                   * instead of a centered 4:3 sub-rect. 0 = normal. */
  int       aspect_wide_w, aspect_wide_h; /* the forced-wide base dimensions. */
  /* Frame-derived external canvas policy; rebuilt before drawing, never serialized. */
  int       surface_canvas, surface_canvas_width, surface_canvas_height;
  int       surface_canvas_world_width, surface_canvas_room_x, surface_canvas_room_width;
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
  int      lut_shader_log_count;
  int      stretched_shader_log_count;
  long     generated_sprite_log_count;
} GmlRender;

/* Preserve alpha on explicit surface targets and on a first-generation Studio
 * application surface sampled by later GUI draws. The second-generation
 * automatic application target is an opaque screen stage, so its fixed-function
 * draws must not reapply partial coverage during presentation. A classic direct
 * frame is not a sampled application surface; preserving its coverage would
 * let subtract blending remove visible frame coverage. */
static inline int gml_render_target_preserves_alpha(const GmlRender *render){
  if(!render) return 0;
  if(render->target_sp>0) return 1;
  return render->app_surface && render->fb==render->app_surface &&
         anygm_policy_uses_first_generation_studio(render->win);
}

/* Renderer-private cross-unit operations. These are implementation seams, not
 * subsystem-facing APIs; callers outside renderer .c owners must not use them. */
typedef void (*GmlRowBandFn)(void *ctx,int row_start,int row_end,int slot);

#define GML_ROW_THREADS_MAX 32
#define GML_ROW_THREADS_AUTO 16
/* How much of a textured draw has to be visible before its rows are worth splitting across the row
 * workers. Each of these sites hands the SAME callback either to the pool or to the calling thread,
 * so the choice moves no pixel; it only decides who writes them.
 *
 * Handover cost depends on whether workers are awake. Keep the higher threshold until
 * several dispatches in the same frame show that the workers remain active. */
#define GML_ROW_BAND_MIN_PIXELS_COLD 262144ull
#define GML_ROW_BAND_MIN_PIXELS_HOT 8192ull
#define GML_ROW_BAND_HOT_DISPATCHES 4
int gml_render_row_bands_profitable(GmlRender *r,unsigned long long visible_pixels);

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
int render_present_content_shader(GmlRender *r,int surf,const uint32_t *src,int sw,int sh,
                                  int x0,int y0,int x1,int y1,
                                  double dx,double dy,double dw,double dh,int shader);
int render_record_deferred_underlay(GmlRender *r,const uint32_t *src,int sw,int sh,
                                    int x0,int y0,int W,int H,int source_all_opaque);
void render_present_first_generation(GmlRender *r,int surf,const uint32_t *src,int sw,int sh,
                                     int x0,int y0,int x1,int y1,
                                     double dx,double dy,double dw,double dh);
int gml_render_shade_target_rect(GmlRender *r,int x1,int y1,int x2,int y2,uint32_t colour,double alpha);
int gml_render_shade_target_sprite(GmlRender *r,int sprite,int frame,
                                   double dx,double dy,double dw,double dh,uint32_t blend,double alpha);
uint64_t gml_render_shader_uniform_fingerprint(const GmlRender *r,int shader);
int gml_render_atlas_rect_plane(GmlRender *r,int atlas,int sx,int sy,int w,int h,
                                const uint32_t **pixels);
/* The program's answer for one atlas rectangle, evaluated on the device once and kept. Returns the
 * plane, or NULL when the device cannot produce it. */
const uint32_t *gml_render_shaded_atlas_rect(GmlRender *r,int atlas,int sx,int sy,int w,int h);
int gml_render_sprite_frame_rect(GmlRender *r,int sprite,int frame,int *atlas,int *sx,int *sy,
                                 int *w,int *h);
int gml_render_sprite_frame_rect_full(GmlRender *r,int sprite,int frame,int *atlas,int *sx,int *sy,
                                      int *w,int *h,int *tx,int *ty);
int gml_render_compose_shaded_rotated(GmlRender *r,GmlSprite *owner,const uint32_t *plane,
                                      int w,int h,double x,double y,double xs,double ys,
                                      double rot,int origin_x,int origin_y,
                                      uint32_t blend,double alpha);
/* Whether a run of draws should be gathered onto one plane and shaded in one pass: a program that
 * reads its place on the target, which cannot be kept, and is not already being gathered. */
int gml_render_shaded_run_wanted(GmlRender *r);
/* Begin gathering. Returns 0 when it cannot, and the caller draws as it would have. */
int gml_render_shaded_run_begin(GmlRender *r,uint32_t **saved_fb,int *saved_shader);
/* Shade what was gathered, over the given target rectangle, and compose it. */
void gml_render_shaded_run_end(GmlRender *r,uint32_t *saved_fb,int saved_shader,
                               int x0,int y0,int x1,int y1,uint32_t blend,double alpha);
int gml_render_shade_atlas_rect_at(GmlRender *r,int atlas,int sx,int sy,int w,int h,
                                   double dx,double dy,double dw,double dh,
                                   uint32_t blend,double alpha);
int gml_render_compose_shaded_plane(GmlRender *r,const uint32_t *plane,int w,int h,
                                    double rx,double ry,double rw,double rh,
                                    double dx,double dy,double dw,double dh,
                                    uint32_t blend,double alpha);
int gml_render_shade_target_sprite_part(GmlRender *r,int sprite,int frame,
                                        double rx,double ry,double rw,double rh,
                                        double dx,double dy,double dw,double dh,
                                        uint32_t blend,double alpha);
void draw_surface_region(GmlRender *r,int surface,double source_x,double source_y,
                         double source_width,double source_height,double destination_x,
                         double destination_y,double destination_width,
                         double destination_height,uint32_t blend,double alpha);
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
