/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_render.h — software renderer: atlas/TPAG/sprite decode + blitter. */
#ifndef GML_RENDER_H
#define GML_RENDER_H
#include "gml_win.h"

typedef struct { int sx,sy,sw,sh, tx,ty, bw,bh, atlas; } GmlTpag;  /* texture page item */
typedef struct { const char *name; int originx, originy, w, h, n_frames; int *frame;
                 int ml, mr, mt, mb;                /* mask margins: left,right,top,bottom */
                 const uint8_t *mask; int mask_rowb, mask_count; } GmlSprite; /* SPRT collision mask: 1bpp */
typedef struct { uint8_t *px; int w, h; } GmlAtlas;                /* RGBA8 */
typedef struct { int tpag; } GmlBg;                               /* background -> texture page */
typedef struct { int sprite, first, prop, sep; } GmlFont;         /* sprite font */

#define GML_MAX_FONTS 32
typedef struct {
  GmlWin   *win;
  GmlAtlas  atlas[16]; int n_atlas;
  GmlTpag  *tpag; int n_tpag;
  GmlSprite *spr; int n_spr;
  GmlBg    *bg; int n_bg;
  GmlFont   fonts[GML_MAX_FONTS]; int n_fonts;
  /* current target framebuffer (borrowed) + camera */
  uint32_t *fb; int fbw, fbh;
  double    cam_x, cam_y;
  /* the application_surface: the buffer the game is rendered into (read by draw_surface_* in
   * an overlay object's Draw GUI). Set by the frontend; same w/h as fbw/fbh. */
  uint32_t *app_surface; int app_draw_enable;   /* GM application_surface_draw_enable, default 1 */
  /* draw state */
  uint32_t  color;  double alpha; int halign, valign, font;
} GmlRender;

int  gml_render_init(GmlRender *r, GmlWin *win);
void gml_render_free(GmlRender *r);
void gml_render_begin(GmlRender *r, uint32_t *fb, int w, int h, double camx, double camy);

void gml_draw_sprite_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                         double xs, double ys, double rot, uint32_t blend, double alpha);
void gml_draw_sprite(GmlRender *r, int sprite, int subimg, double x, double y);
void gml_draw_sprite_tiled_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                               double xs, double ys, uint32_t blend, double alpha);
void gml_draw_background(GmlRender *r, int bg, double x, double y);
void gml_draw_background_tiled(GmlRender *r, int bg, double x, double y, int htiled, int vtiled);
void gml_draw_background_ext(GmlRender *r, int bg, double x, double y, double xs, double ys, uint32_t color, double alpha);
void gml_draw_background_tiled_ext(GmlRender *r, int bg, double x, double y, double xs, double ys, uint32_t color, double alpha, int htiled, int vtiled);
void gml_draw_room_backgrounds(GmlRender *r, uint32_t bg_ptr, int want_fg);
void gml_draw_room_tiles(GmlRender *r, uint32_t tile_ptr);
void gml_draw_tile(GmlRender *r, int def, int sx, int sy, int w, int h, double x, double y);
/* Stretch the application surface or a sprite in screen space, without camera offsets. */
void gml_draw_surface_stretched(GmlRender *r, double x, double y, double w, double h, uint32_t blend, double alpha);
void gml_draw_sprite_stretched(GmlRender *r, int sprite, int frame, double x, double y, double w, double h, uint32_t blend, double alpha);
int  gml_sprite_frames(GmlRender *r, int sprite);
int  gml_sprite_alpha(GmlRender *r, int sprite, int frame, int lx, int ly);
int  gml_sprite_collision(GmlRender *r, int sprite, int frame, int lx, int ly);
int  gml_font_add_sprite(GmlRender *r, int sprite, int first, int prop, int sep);
void gml_draw_text(GmlRender *r, double x, double y, const char *str);

#endif
