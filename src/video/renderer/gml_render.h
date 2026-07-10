/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_render.h — software renderer: atlas/TPAG/sprite decode + blitter. */
#ifndef GML_RENDER_H
#define GML_RENDER_H
#include "gml_win.h"

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
} GmlAtlas;
typedef struct {
  int tpag;
  int tile_w, tile_h, tile_border_x, tile_border_y, tile_columns, tile_items_per_tile, tile_count;
  const uint8_t *tile_ids;                                      /* GMS2 BGND tileset id table (little-endian u32s) */
} GmlBg;                                                        /* background/tileset -> texture page */
typedef struct { int32_t sx, sy, w, h; int16_t shift, offset; uint16_t ch; } GmlGlyph;
typedef struct {
  int sprite, first, prop, sep;
  uint32_t *map; int map_len;                                    /* font_add_sprite_ext explicit map */
  int map_fast[256];
  /* real FONT-chunk font: glyph sub-rects blitted from the data.win atlas at runtime
   * (no glyph data is bundled; parsed from the user-supplied data.win like sprites). */
  int real, atlas, line_height;                                  /* real=1; atlas index; em/line height */
  GmlGlyph *glyphs; int n_glyphs;
  int glyph_by_char[256];                                        /* fast ASCII lookup, -1 = none */
} GmlFont;                                                        /* sprite font or real FONT-chunk font */
typedef struct { uint32_t *px; int w, h, live;
                 int dirty;                       /* px changed since the RLE cache was built */
                 int opaque_known, all_opaque, all_transparent;  /* conservative coverage metadata */
                 uint8_t *rle; size_t rle_len, rle_cap;  /* cached savestate RLE (u32 nrun + pairs) */
} GmlSurface;      /* XRGB8888 runtime surface */

#define GML_MAX_FONTS 48
#define GML_MAX_SURFACES 64   /* allow many concurrent surface allocations */
#define GML_SURFACE_STACK 8
typedef struct {
  GmlWin   *win;
  GmlAtlas *atlas; int n_atlas;
  GmlTpag  *tpag; int n_tpag;
  GmlSprite *spr; int n_spr, base_n_spr, spr_cap, spr_has_free;
  GmlBg    *bg; int n_bg;
  GmlFont   fonts[GML_MAX_FONTS]; int n_fonts;
  /* current target framebuffer (borrowed) + camera */
  uint32_t *fb; int fbw, fbh;
  uint32_t *base_fb; int base_fbw, base_fbh;
  struct {
    uint32_t *fb; int w, h; double cx, cy; int target_id, opaque_known, all_opaque, all_transparent;
    int pending_underlay, underlay_x, underlay_y, underlay_w, underlay_h;
    int pending_fill; uint32_t fill_color;
  } target_stack[GML_SURFACE_STACK]; int target_sp;
  int target_id;
  double    cam_x, cam_y;
  /* the application_surface: the buffer the game is rendered into and later
   * readable by draw_surface_* calls. Set by the frontend; same w/h as fbw/fbh. */
  uint32_t *app_surface; int app_draw_enable;   /* GM application_surface_draw_enable, default 1 */
  int app_w, app_h;                             /* app_surface dims (the view render size) */
  int app_surface_opaque;                       /* frontend/render metadata: every app pixel has alpha 255 */
  int pending_underlay, underlay_x, underlay_y, underlay_w, underlay_h;  /* deferred default app-surface blit */
  int pending_fill; uint32_t pending_fill_color; /* deferred full-target overwrite */
  GmlSurface surface[GML_MAX_SURFACES]; int next_surface_id;
  /* draw state */
  uint32_t  color;  double alpha; int halign, valign, font, alphablend, circle_precision;
  int       blendmode;   /* gpu_set_blendmode: 0=normal, 1=add (others fall back to normal). Reset per frame. */
  int       fast_alpha_cull;  /* optional fast path: drop alpha contributions <= this 8-bit step */
  int       fb_opaque_known, fb_all_opaque, fb_all_transparent;  /* current target coverage metadata */
  /* Palette and lookup-texture state declarations. */
  struct GmlShaderPal { int has; uint8_t L[3],M[3],D[3],S[3];
    int lut;                    /* palette-LUT shader: out = palette[(src.r, row)] */
    char lut_row_uniform[32];   /* uniform float selecting the palette row */
    char lut_sampler[32];       /* sampler2D holding the palette texture */
    float lut_row;              /* current row (normalized v), set by shader_set_uniform_f */
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
  } *shader_pal; int n_shader_pal;
  int       lut_pal_sprite, lut_pal_frame;   /* texture_set_stage palette source (-1 = unset) */
  int       active_shader;   /* shader_set asset id, -1 = none. Reset per frame. */
  int       crt_scale;       /* virtual-window supersample factor (>=1) for shader-CRT games: the
                              * game reads window_get_width/height * crt_scale so its window-scaled
                              * CRT surface renders at that multiple; the GUI/present canvas is sized
                              * to match. 1 = native (no supersample). Set from the gml_crt_scale core
                              * option / GML_CRT_SCALE env by the frontend. */
  int       crt_shader_enable; /* run recognized embedded CRT post-process shaders (default 1). 0 =
                              * report them not-compiled and never execute them, so games fall back
                              * to their no-shader video modes (pre-emulation behavior). Palette/LUT
                              * shaders are NOT gated: they are integral to those games' rendering. */
  int       crt_shader_present; /* set at init when the SHDR chunk contains a recognized CRT-geom
                              * fragment. Gates crt_scale's virtual window/supersample so the option
                              * has zero effect on games without an embedded CRT shader. */
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
  /* async atlas prefetch pool (opaque; see gml_render.c). Decodes atlases on worker threads so
   * first-use of a texture page does not stall a frame for a full BZ2+QOI atlas decode. */
  void     *prefetch; int prefetch_checked;
  size_t   atlas_decoded_bytes;
} GmlRender;

int  gml_render_init(GmlRender *r, GmlWin *win);
void gml_render_free(GmlRender *r);
/* queue background decodes (worker threads; safe no-ops when disabled or already decoded) */
void gml_render_prefetch_atlas(GmlRender *r, int idx);
void gml_render_prefetch_sprite(GmlRender *r, int sprite);
void gml_render_prefetch_bg(GmlRender *r, int bg);
/* synchronously decode directly referenced pages; used at room boundaries to avoid first-draw hitches */
void gml_render_warm_sprite(GmlRender *r, int sprite);
void gml_render_warm_bg(GmlRender *r, int bg);
void gml_render_begin(GmlRender *r, uint32_t *fb, int w, int h, double camx, double camy);
void gml_render_set_pending_underlay(GmlRender *r, int x, int y, int w, int h);
void gml_render_flush_pending_underlay(GmlRender *r);
void gml_render_set_pending_fill(GmlRender *r, uint32_t color);
void gml_render_flush_pending_fill(GmlRender *r);
void gml_render_cancel_pending_fill(GmlRender *r);
void gml_render_cancel_pending_underlay(GmlRender *r);
void gml_render_prepare_draw(GmlRender *r);
void gml_render_prepare_opaque_rect(GmlRender *r, int x0, int y0, int x1, int y1);
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
void gml_draw_sprite(GmlRender *r, int sprite, int subimg, double x, double y);
void gml_draw_sprite_tiled_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                               double xs, double ys, uint32_t blend, double alpha);
void gml_draw_layer_color_fill(GmlRender *r, uint32_t gmcol, double alpha); /* spriteless GMS2 bg layer */
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
void gml_surface_copy(GmlRender *r, int dst, int x, int y, int src);
int  gml_surface_set_target(GmlRender *r, int id);
void gml_surface_reset_target(GmlRender *r);
int  gml_surface_get_target(GmlRender *r);
void gml_draw_surface_stretched(GmlRender *r, int surf, double x, double y, double w, double h, uint32_t blend, double alpha);
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
int  gml_sprite_collision(GmlRender *r, int sprite, int frame, int lx, int ly);
int  gml_font_add_sprite(GmlRender *r, int sprite, int first, int prop, int sep);
int  gml_font_add_sprite_ext(GmlRender *r, int sprite, const char *map, int prop, int sep);
int  gml_font_add_file(GmlRender *r, const char *path, double point_size);   /* runtime TTF (font_add) */
int  gml_sprite_add_file(GmlRender *r, const char *path, int imgnum, int removeback, int xorig, int yorig);
void gml_render_rebuild_font_maps(GmlRender *r);
void gml_draw_text(GmlRender *r, double x, double y, const char *str);
void gml_draw_text_ext(GmlRender *r, double x, double y, const char *str, double sep, double w);
void gml_draw_text_transformed(GmlRender *r, double x, double y, const char *str,
                               double xs, double ys, double rot, uint32_t blend, double alpha);
int  gml_text_width(GmlRender *r, const char *str);
int  gml_text_height(GmlRender *r, const char *str);

#endif
