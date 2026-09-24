/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "anygm_compatibility.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(int condition,const char *message){
  if(condition) return;
  fprintf(stderr,"renderer tiles: %s\n",message);
  failures++;
}

static void write_u32(uint8_t *bytes,size_t offset,uint32_t value){
  bytes[offset+0]=(uint8_t)value;
  bytes[offset+1]=(uint8_t)(value>>8);
  bytes[offset+2]=(uint8_t)(value>>16);
  bytes[offset+3]=(uint8_t)(value>>24);
}

static void write_u64(uint8_t *bytes,size_t offset,uint64_t value){
  write_u32(bytes,offset,(uint32_t)value);
  write_u32(bytes,offset+4,(uint32_t)(value>>32));
}

static void check_tileset_layout(int separated){
  uint8_t data[512];
  GmlWin content;
  GmlRender render;
  const size_t record=32;
  const unsigned tile_count=separated?8:2;
  const size_t table=record+(separated?72:64);

  memset(data,0,sizeof data);
  memset(&content,0,sizeof content);
  write_u32(data,0,1);
  write_u32(data,4,(uint32_t)record);
  write_u32(data,record+20,1);
  write_u32(data,record+24,2);
  write_u32(data,record+28,2);
  if(separated){
    write_u32(data,record+40,1);
    write_u32(data,record+44,1);
    write_u32(data,record+48,3);
    write_u32(data,record+52,4);
    write_u32(data,record+56,tile_count);
    write_u64(data,record+64,100000);
  } else {
    write_u32(data,record+32,1);
    write_u32(data,record+36,1);
    write_u32(data,record+40,3);
    write_u32(data,record+44,4);
    write_u32(data,record+48,tile_count);
    write_u64(data,record+56,100000);
  }
  for(unsigned tile=0;tile<tile_count;tile++) for(unsigned frame=0;frame<4;frame++)
    write_u32(data,table+(tile*4+frame)*4,tile*100+frame);

  content.data=data;
  content.size=sizeof data;
  content.bytecode=17;
  content.n_chunks=1;
  memcpy(content.chunks[0].name,"BGND",4);
  content.chunks[0].off=0;
  content.chunks[0].size=sizeof data;
  expect(gml_render_init(&render,&content)==0,
         "synthetic tileset renderer initialization failed");
  expect(render.n_bg==1 && render.bg &&
         render.bg[0].tile_items_per_tile==4 &&
         render.bg[0].tile_count==(int)tile_count &&
         render.bg[0].tile_frame_length_us==100000,
         separated?"separated tileset layout was not parsed":
                   "compact tileset layout was not parsed");
  expect(gml_render_background_tile_animation_frame(&render,0,0.099)==0 &&
         gml_render_background_tile_animation_frame(&render,0,0.100)==1 &&
         gml_render_background_tile_animation_frame(&render,0,0.399)==3 &&
         gml_render_background_tile_animation_frame(&render,0,0.400)==0,
         "tileset animation cadence did not wrap at the declared interval");
  expect(gml_render_background_tile_source_index(&render,0,1,2)==102,
         "animated tile frame did not select the corresponding source index");
  gml_render_free(&render);
}

static void check_transform(int mirror,int flip,int rotate,
                            const uint32_t expected[4]){
  static const uint8_t rgba[16]={
    255,0,0,255, 0,255,0,255,
    0,0,255,255, 255,255,255,255
  };
  uint32_t framebuffer[4]={0,0,0,0};
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&render,0,sizeof render);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=2;
  page.atlas=0;
  page.sw=page.sh=page.bw=page.bh=2;
  page.alpha_scanned=1;
  page.alpha_max=255;
  page.ax1=page.ay1=1;
  background.tpag=0;
  render.fb=render.base_fb=framebuffer;
  render.fbw=render.base_fbw=2;
  render.fbh=render.base_fbh=2;
  render.atlas=&atlas;
  render.n_atlas=1;
  render.tpag=&page;
  render.n_tpag=1;
  render.bg=&background;
  render.n_bg=1;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;
  render.target_id=-1;
  gml_draw_background_tile(&render,0,0,0,2,2,0,0,1,1,
                           mirror,flip,rotate,0xffffff,1.0);
  gml_render_flush_rotated_batch(&render);
  expect(!memcmp(framebuffer,expected,sizeof framebuffer),
         "tile mirror/flip/rotate combination produced shifted or reordered pixels");
  free(page.argb_cache);
}

static void check_fractional_transform_equivalence(int mirror,int flip){
  static const uint8_t rgba[16]={
    255,0,0,128, 0,255,0,255,
    0,0,255,64, 255,255,255,192
  };
  uint32_t fast_framebuffer[25];
  uint32_t general_framebuffer[25];
  GmlRender fast;
  GmlRender general;
  GmlAtlas atlas;
  GmlTpag fast_page;
  GmlTpag general_page;
  GmlBg background;

  for(int i=0;i<25;i++)
    fast_framebuffer[i]=general_framebuffer[i]=
      0xff102030u+(uint32_t)(i*0x00010101u);
  memset(&fast,0,sizeof fast);
  memset(&general,0,sizeof general);
  memset(&atlas,0,sizeof atlas);
  memset(&fast_page,0,sizeof fast_page);
  memset(&background,0,sizeof background);
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=2;
  fast_page.atlas=0;
  fast_page.sw=fast_page.sh=fast_page.bw=fast_page.bh=2;
  fast_page.alpha_scanned=1;
  fast_page.alpha_max=255;
  fast_page.ax1=fast_page.ay1=1;
  general_page=fast_page;
  background.tpag=0;
  fast.fb=fast.base_fb=fast_framebuffer;
  fast.fbw=fast.base_fbw=5;
  fast.fbh=fast.base_fbh=5;
  fast.atlas=&atlas;
  fast.n_atlas=1;
  fast.tpag=&fast_page;
  fast.n_tpag=1;
  fast.bg=&background;
  fast.n_bg=1;
  fast.cam_x=0.2;
  fast.cam_y=0.8;
  fast.alpha=1.0;
  fast.alphablend=1;
  fast.color_write_mask=0x0f;
  fast.active_shader=-1;
  fast.target_id=-1;
  general=fast;
  general.fb=general.base_fb=general_framebuffer;
  general.tpag=&general_page;
  general.active_shader=0;

  gml_draw_background_tile(&fast,0,0,0,2,2,1,1,1,1,
                           mirror,flip,1,0xffffff,1.0);
  gml_draw_background_tile(&general,0,0,0,2,2,1,1,1,1,
                           mirror,flip,1,0xffffff,1.0);
  gml_render_flush_rotated_batch(&fast);
  gml_render_flush_rotated_batch(&general);
  expect(!memcmp(fast_framebuffer,general_framebuffer,sizeof fast_framebuffer),
         "fractional cardinal tile path diverged from the general rotation kernel");
  free(fast_page.argb_cache);
  free(general_page.argb_cache);
  free(fast.rotated_batch);
  free(general.rotated_batch);
}

static void check_runtime_background_replacement(void){
  static const uint8_t replacement[16]={
    255,0,0,255, 0,255,0,255,
    0,0,255,255, 255,255,255,255
  };
  static const uint32_t expected[4]={
    0xffff0000,0xff00ff00,0xff0000ff,0xffffffff
  };
  GmlRender render;
  uint32_t framebuffer[4]={0};
  uint8_t *rgba=malloc(sizeof(replacement));

  memset(&render,0,sizeof render);
  render.atlas=calloc(1,sizeof(*render.atlas));
  render.tpag=calloc(1,sizeof(*render.tpag));
  render.tpag_ptr=calloc(1,sizeof(*render.tpag_ptr));
  render.bg=calloc(1,sizeof(*render.bg));
  if(render.atlas) render.atlas[0].px=calloc(4,1);
  if(!rgba || !render.atlas || !render.tpag || !render.tpag_ptr ||
     !render.bg || !render.atlas[0].px){
    free(rgba);
    gml_render_free(&render);
    expect(0,"runtime background fixture allocation failed");
    return;
  }
  memcpy(rgba,replacement,sizeof(replacement));
  render.n_atlas=render.n_tpag=render.n_bg=1;
  render.atlas[0].w=render.atlas[0].h=1;
  render.tpag[0].atlas=0;
  render.tpag[0].sw=render.tpag[0].sh=1;
  render.tpag[0].bw=render.tpag[0].bh=1;
  render.bg[0].tpag=0;
  render.fb=render.base_fb=framebuffer;
  render.fbw=render.base_fbw=2;
  render.fbh=render.base_fbh=2;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;
  render.target_id=-1;
  render.tpag[0].interp_draw_cache=malloc(sizeof(uint32_t));
  render.tpag[0].interp_draw_runs=malloc(sizeof(GmlTpagInterpRun));
  if(!render.tpag[0].interp_draw_cache || !render.tpag[0].interp_draw_runs){
    free(rgba);
    gml_render_free(&render);
    expect(0,"runtime background cache fixture allocation failed");
    return;
  }
  render.tpag[0].interp_draw_cache_valid=1;
  render.tpag[0].interp_draw_cache_bytes=
    sizeof(uint32_t)+sizeof(GmlTpagInterpRun);
  render.interp_draw_cache_bytes=render.tpag[0].interp_draw_cache_bytes;

  expect(gml_background_replace_from_rgba(&render,0,rgba,2,2),
         "runtime background replacement rejected valid RGBA pixels");
  expect(render.n_atlas==2 && render.bg[0].tpag==0 &&
         render.tpag[0].atlas==1 && render.tpag[0].sw==2 &&
         render.tpag[0].sh==2 && render.tpag[0].bw==2 &&
         render.tpag[0].bh==2,
         "runtime background replacement did not publish its new texture page");
  expect(!render.tpag[0].interp_draw_cache &&
         !render.tpag[0].interp_draw_runs &&
         render.interp_draw_cache_bytes==0,
         "runtime background replacement retained filtered samples from the old image");
  gml_draw_background(&render,0,0,0);
  expect(!memcmp(framebuffer,expected,sizeof expected),
         "runtime background replacement did not affect background drawing");
  gml_render_free(&render);
}

static void check_trimmed_background_tiling_period(void){
  static const uint8_t rgba[4]={255,0,0,255};
  uint32_t framebuffer[8*6]={0};
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&render,0,sizeof render);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=1;
  page.atlas=0;
  page.sw=page.sh=1;
  page.bw=3;
  page.bh=4;
  page.tx=1;
  page.ty=2;
  page.alpha_scanned=1;
  page.alpha_max=255;
  background.tpag=0;
  render.fb=render.base_fb=framebuffer;
  render.fbw=render.base_fbw=8;
  render.fbh=render.base_fbh=6;
  render.atlas=&atlas;
  render.n_atlas=1;
  render.tpag=&page;
  render.n_tpag=1;
  render.bg=&background;
  render.n_bg=1;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;
  render.target_id=-1;

  gml_draw_background_tiled(&render,0,0,0,1,1);
  for(int y=0;y<6;y++) for(int x=0;x<8;x++){
    int expected=y==2 && (x==1 || x==4 || x==7);
    expect((framebuffer[y*8+x]!=0)==expected,
           "trimmed background repeated its crop instead of its logical cell");
  }
  free(page.argb_cache);
}

/* A parallax layer is positioned from the view, and a view that drifts by a fraction of a pixel
 * puts that position on an integer boundary. Whatever the layer does there, it must do what every
 * other draw does: round to the nearest pixel. Snapping the anchor down first answers a different
 * rule, and the layer then jumps a whole pixel back and forth every frame while the world it
 * belongs to holds still. Two anchors a fraction apart around the same boundary must draw the same
 * pixels. */
static void check_background_anchor_holds_across_subpixel_drift(void){
  static const uint8_t rgba[4]={255,0,0,255};
  uint32_t first[8*6]={0},second[8*6]={0};
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&render,0,sizeof render);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=1;
  page.atlas=0;
  page.sw=page.sh=1;
  page.bw=page.bh=4;
  page.alpha_scanned=1;
  page.alpha_max=255;
  background.tpag=0;
  render.classic=1;
  render.fbw=render.base_fbw=8;
  render.fbh=render.base_fbh=6;
  render.atlas=&atlas;
  render.n_atlas=1;
  render.tpag=&page;
  render.n_tpag=1;
  render.bg=&background;
  render.n_bg=1;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;
  render.target_id=-1;

  /* The same layer at the same place, seen by a view that moved an eighth of a pixel: half the
   * drift lands either side of an integer because the layer scrolls at half the view's rate. */
  render.fb=render.base_fb=first;
  render.cam_x=1133.875;
  gml_draw_background_tiled(&render,0,566.9375,0,1,1);
  render.fb=render.base_fb=second;
  render.cam_x=1134.125;
  gml_draw_background_tiled(&render,0,567.0625,0,1,1);

  expect(!memcmp(first,second,sizeof first),
         "a background layer moved a whole pixel for a sub-pixel view drift");
  free(page.argb_cache);
}

static void check_removeback_uses_bottom_left(void){
  uint8_t rgba[16]={
    255,0,0,255, 0,0,255,255,
    0,0,255,17, 255,0,0,255
  };
  gml_render_apply_removeback_rgba(rgba,2,2,1);
  expect(rgba[3]==255 && rgba[7]==0 && rgba[11]==0 && rgba[15]==255,
         "removeback did not key RGB from the bottom-left pixel");
}

/* A sprite part may select a fractional number of source rows. Snapping that extent up to a whole
 * texel before scaling makes the part cover the destination height of a full row, which at a
 * magnifying scale is a visible band of extra pixels. The destination extent must follow the
 * continuous source intersection, so the covered rows are those whose centres fall inside it. */
static void check_fractional_sprite_part_extent(double source_height,int expected_rows){
  enum { WIDTH=4,HEIGHT=8 };
  static const uint8_t rgba[4]={255,255,255,255};
  uint32_t framebuffer[WIDTH*HEIGHT];
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlSprite sprite;
  int frame=0;
  int rows=0;

  memset(framebuffer,0,sizeof framebuffer);
  memset(&render,0,sizeof render);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&sprite,0,sizeof sprite);
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=1;
  page.atlas=0;
  page.sw=page.sh=page.bw=page.bh=1;
  page.alpha_scanned=1;
  page.alpha_max=255;
  page.ax1=page.ay1=0;
  sprite.n_frames=1;
  sprite.frame=&frame;
  sprite.w=sprite.h=1;
  render.fb=render.base_fb=framebuffer;
  render.fbw=render.base_fbw=WIDTH;
  render.fbh=render.base_fbh=HEIGHT;
  render.atlas=&atlas;
  render.n_atlas=1;
  render.tpag=&page;
  render.n_tpag=1;
  render.spr=&sprite;
  render.n_spr=1;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;
  render.target_id=-1;

  gml_draw_sprite_part_ext(&render,0,0,0.0,0.0,1.0,source_height,
                           0.0,0.0,2.0,2.0,0xFFFFFF,1.0);

  for(int y=0;y<HEIGHT;y++){
    int filled=0;
    for(int x=0;x<WIDTH;x++)
      if(framebuffer[y*WIDTH+x]&0x00FFFFFFu) filled=1;
    rows+=filled;
  }
  if(rows!=expected_rows)
    fprintf(stderr,"renderer tiles: sprite part of source height %.2f at yscale 2 covered %d "
                   "destination rows instead of %d\n",source_height,rows,expected_rows);
  expect(rows==expected_rows,
         "fractional sprite part covered the wrong destination height");
  free(page.argb_cache);
}

static void check_early_second_generation_camera_phase(void){
  uint32_t framebuffer=0;
  GmlWin content;
  AnygmCompatibilityProfile compatibility;
  GmlRender render;
  memset(&content,0,sizeof content);
  memset(&compatibility,0,sizeof compatibility);
  memset(&render,0,sizeof render);
  compatibility.diagnostic_family=ANYGM_FAMILY_STUDIO_FIRST;
  compatibility.has_modern_layer_semantics=1;
  content.compatibility=&compatibility;
  render.win=&content;

  gml_render_begin(&render,&framebuffer,1024,768,0.0,0.25);
  gml_render_world_set_logical_extent(&render,480,360);
  double x=0.0,y=0.0;
  gml_render_draw_map_point(&render,&x,&y);
  expect(x==0.0 && y==0.0 && render.cam_y==0.0,
         "early Studio 2 kept a fractional scaled vertical camera origin");

  gml_render_begin(&render,&framebuffer,480,360,0.0,0.0);
  gml_render_world_set_logical_extent(&render,480,360);
  x=y=0.0;
  gml_render_draw_map_point(&render,&x,&y);
  expect(x==0.0 && y==0.0,
         "early Studio 2 altered a 1:1 world target");

  compatibility.diagnostic_family=ANYGM_FAMILY_STUDIO_SECOND;
  gml_render_begin(&render,&framebuffer,1024,768,0.0,0.25);
  gml_render_world_set_logical_extent(&render,480,360);
  x=y=0.0;
  gml_render_draw_map_point(&render,&x,&y);
  expect(x==0.0 && y==0.0 && render.cam_y>0.5,
         "later Studio 2 inherited the revision-15 integer camera projection");
}

static void check_first_generation_default_font_metrics(void){
  GmlWin content;
  AnygmCompatibilityProfile compatibility;
  GmlRender render;
  memset(&content,0,sizeof content);
  memset(&compatibility,0,sizeof compatibility);
  memset(&render,0,sizeof render);
  compatibility.diagnostic_family=ANYGM_FAMILY_STUDIO_FIRST;
  content.compatibility=&compatibility;
  render.win=&content;
  expect(build_default_font(&render),
         "first-generation default font did not build");
  GmlFont *font=&render.default_font;
  int h=font->glyph_by_char['H'];
  int i=font->glyph_by_char['I'];
  int s=font->glyph_by_char['S'];
  int colon=font->glyph_by_char[':'];
  int space=font->glyph_by_char[' '];
  expect(font->line_height==15 && h>=0 && i>=0 && s>=0 && colon>=0 && space>=0 &&
         font->glyphs[h].shift==11 && font->glyphs[i].shift==5 &&
         font->glyphs[s].shift==10 && font->glyphs[colon].shift==6 &&
         font->glyphs[space].shift==7,
         "first-generation default-font metrics do not match the selected profile");
  gml_render_free(&render);
}

static void check_render_pass_restores_normal_blending(void){
  uint32_t framebuffer=0;
  GmlRender render;
  memset(&render,0,sizeof render);
  render.alphablend=0;
  render.blendmode=3;
  render.blend_equation=2;
  render.blend_equation_alpha=2;
  gml_render_begin(&render,&framebuffer,1,1,0.0,0.0);
  expect(render.alphablend==1 && render.blendmode==0 &&
         render.blend_equation==1 && render.blend_equation_alpha==1,
         "a new render-target pass retained disabled or non-normal blending");
}

/* Independently projected tile quads retain their fractional position.
 * Centre sampling keeps the selected rows inside the subrectangle and
 * prevents an unfilled transparent line at its edge. */
static void check_first_generation_fractional_tile_projection(void){
  enum { LOGICAL_HEIGHT=26, SURFACE_HEIGHT=30, WIDTH=1 };
  uint8_t rgba[LOGICAL_HEIGHT*4];
  uint32_t application[SURFACE_HEIGHT];
  GmlWin content;
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&content,0,sizeof content);
  memset(&render,0,sizeof render);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  memset(application,0,sizeof application);
  for(int y=0;y<LOGICAL_HEIGHT;y++){
    rgba[y*4+0]=(uint8_t)(y+1);
    rgba[y*4+1]=(uint8_t)(255-y);
    rgba[y*4+2]=(uint8_t)(y*7);
    rgba[y*4+3]=255;
  }
  content.bytecode=15;
  atlas.px=rgba;
  atlas.w=1; atlas.h=LOGICAL_HEIGHT;
  page.atlas=0;
  page.sw=page.bw=1;
  page.sh=page.bh=LOGICAL_HEIGHT;
  page.alpha_scanned=1;
  page.alpha_max=255;
  page.ax1=0; page.ay1=LOGICAL_HEIGHT-1;
  background.tpag=0;
  render.win=&content;
  render.atlas=&atlas; render.n_atlas=1;
  render.tpag=&page; render.n_tpag=1;
  render.bg=&background; render.n_bg=1;
  render.app_surface=application;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;
  gml_render_begin(&render,application,WIDTH,SURFACE_HEIGHT,0.0,0.0);
  gml_render_world_set_logical_extent(&render,WIDTH,LOGICAL_HEIGHT);
  gml_draw_background_tile(&render,0,0,1,1,3,0,1,1,1,0,0,0,0xffffff,1.0);
  static const int expected_source[4]={1,2,3,3};
  for(int y=1;y<=4;y++){
    int source_y=expected_source[y-1];
    uint32_t expected=0xff000000u|((uint32_t)rgba[source_y*4]<<16)|
      ((uint32_t)rgba[source_y*4+1]<<8)|rgba[source_y*4+2];
    if(application[y]!=expected){
      fprintf(stderr,"renderer tiles: fractional application projection row %d was %08x, expected %08x\n",
              y,application[y],expected);
      failures++;
      break;
    }
  }
  free(page.argb_cache);
}

/* Three adjacent 32-pixel cells projected at 5/3 begin at output columns 0, 53 and 107. A fixed
 * rounded width of 53 leaves column 106 untouched; projecting each quad's far authored edge makes
 * the cells cover the target continuously. */
static void check_modern_fractional_tile_grid_has_no_cracks(void){
  enum { SOURCE=32, CELLS=3, WIDTH=160, HEIGHT=2 };
  uint8_t rgba[SOURCE*SOURCE*4];
  uint32_t application[WIDTH*HEIGHT];
  GmlWin content;
  AnygmCompatibilityProfile compatibility;
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&content,0,sizeof content);
  memset(&compatibility,0,sizeof compatibility);
  memset(&render,0,sizeof render);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  memset(rgba,255,sizeof rgba);
  compatibility.has_modern_layer_semantics=1;
  content.bytecode=17;
  content.compatibility=&compatibility;
  atlas.px=rgba; atlas.w=atlas.h=SOURCE;
  page.atlas=0; page.sw=page.sh=page.bw=page.bh=SOURCE;
  page.alpha_scanned=1; page.alpha_max=255; page.ax1=page.ay1=SOURCE-1;
  background.tpag=0;
  render.win=&content;
  render.atlas=&atlas; render.n_atlas=1;
  render.tpag=&page; render.n_tpag=1;
  render.bg=&background; render.n_bg=1;
  render.app_surface=application;
  render.alpha=1.0; render.alphablend=1;
  render.color_write_mask=0x0f; render.active_shader=-1;

  for(int mirror=0;mirror<=1;mirror++) for(int phase=0;phase<32;phase++){
    for(size_t i=0;i<sizeof application/sizeof application[0];i++)
      application[i]=0xff999999u;
    gml_render_begin(&render,application,WIDTH,HEIGHT,(double)phase/32.0,0.0);
    gml_render_world_set_logical_extent(&render,SOURCE*CELLS,HEIGHT*3.0/5.0);
    /* One cell beyond each side keeps the viewport covered while the camera moves. The assertion
     * then measures only seams between projected authored edges, never an intentional world edge. */
    for(int cell=-1;cell<=CELLS;cell++)
      gml_draw_background_tile(&render,0,0,0,SOURCE,SOURCE,
                               cell*SOURCE,0,1,1,mirror,0,0,0xffffff,1.0);
    for(int x=0;x<WIDTH;x++) if(application[x]!=0xffffffffu){
      fprintf(stderr,
              "renderer tiles: fractional %s tile grid phase %d column %d retained %08x\n",
              mirror?"mirrored":"ordinary",phase,x,application[x]);
      expect(0,"fractionally projected moving tile grid exposed a crack between cells");
      break;
    }
  }
  free(page.argb_cache);
}

/* At exact 2x the odd output rows come from a second sample plane, and a layer that covers the
 * whole logical raster has to cover the whole of both. It used to lose its last row there — the
 * quad was replayed one output row above itself and its bottom row dropped — so whatever another
 * layer had drawn underneath stayed visible along the bottom edge of the screen. */
static void check_classic_double_scale_layer_covers_its_last_row(void){
  enum { WIDTH=1, HEIGHT=4, UNDER_HEIGHT=8 };
  static const uint8_t under_rgba[UNDER_HEIGHT*4]={
    255,0,0,255, 255,0,0,255, 255,0,0,255, 255,0,0,255,
    255,0,0,255, 255,0,0,255, 255,0,0,255, 255,0,0,255,
  };
  static const uint8_t over_rgba[HEIGHT*4]={
    0,255,0,255, 0,255,0,255, 0,255,0,255, 0,255,0,255,
  };
  uint32_t base[WIDTH*HEIGHT]={0},phase[WIDTH*HEIGHT]={0};
  GmlWin content;
  GmlRender render;
  GmlAtlas atlas[2];
  GmlTpag page[2];
  GmlBg background[2];

  memset(&content,0,sizeof content);
  memset(&render,0,sizeof render);
  memset(atlas,0,sizeof atlas);
  memset(page,0,sizeof page);
  memset(background,0,sizeof background);
  atlas[0].px=(uint8_t*)under_rgba; atlas[0].w=WIDTH; atlas[0].h=UNDER_HEIGHT;
  atlas[1].px=(uint8_t*)over_rgba;  atlas[1].w=WIDTH; atlas[1].h=HEIGHT;
  page[0].atlas=0; page[0].sw=page[0].bw=WIDTH; page[0].sh=page[0].bh=UNDER_HEIGHT;
  page[1].atlas=1; page[1].sw=page[1].bw=WIDTH; page[1].sh=page[1].bh=HEIGHT;
  for(int index=0;index<2;index++){
    page[index].alpha_scanned=1;
    page[index].alpha_max=255;
    page[index].ay1=page[index].sh-1;
    background[index].tpag=index;
  }
  render.win=&content;
  render.classic=1;
  render.atlas=atlas; render.n_atlas=2;
  render.tpag=page; render.n_tpag=2;
  render.bg=background; render.n_bg=2;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;
  render.target_id=-1;
  render.classic_phase_y=phase;
  gml_render_begin(&render,base,WIDTH,HEIGHT,0.0,0.0);
  render.classic_phase_y=phase;
  /* The taller layer first, then the one that covers the raster exactly. */
  gml_draw_background_tiled(&render,0,0.0,0.0,0,0);
  gml_draw_background_tiled(&render,1,0.0,0.0,0,0);
  for(int row=0;row<HEIGHT;row++){
    if((base[row]&0x00FFFFFFu)!=0x0000FF00u){
      fprintf(stderr,"renderer tiles: 2x base row %d is %08x, expected the covering layer\n",
              row,base[row]);
      failures++;
    }
    if((phase[row]&0x00FFFFFFu)!=0x0000FF00u){
      fprintf(stderr,"renderer tiles: 2x odd-sample row %d is %08x, expected the covering layer\n",
              row,phase[row]);
      failures++;
    }
  }
}

static void check_modern_fractional_camera_tie(void){
  static const uint8_t rgba[4]={255,255,255,255};
  uint32_t application[4]={0,0,0,0};
  GmlWin content;
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&content,0,sizeof content);
  memset(&render,0,sizeof render);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  content.bytecode=17;
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=1;
  page.atlas=0;
  page.sw=page.sh=page.bw=page.bh=1;
  page.alpha_scanned=1;
  page.alpha_max=255;
  background.tpag=0;
  render.win=&content;
  render.atlas=&atlas; render.n_atlas=1;
  render.tpag=&page; render.n_tpag=1;
  render.bg=&background; render.n_bg=1;
  render.app_surface=application;
  render.app_w=4; render.app_h=1;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;

  gml_render_begin(&render,application,4,1,0.5,0.0);
  gml_draw_background(&render,0,2.0,0.0);
  expect(application[1]==0xffffffffu && application[2]==0,
         "a fractional modern camera resolved a half-pixel edge forward");

  memset(application,0,sizeof application);
  gml_render_begin(&render,application,4,1,0.5,0.0);
  gml_draw_background(&render,0,2.5,0.0);
  expect(application[2]==0xffffffffu && application[1]==0,
         "a fractional modern camera moved an integer projected edge");

  memset(application,0,sizeof application);
  gml_render_begin(&render,application,4,1,0.25,0.0);
  gml_draw_background(&render,0,2.75,0.0);
  expect(application[3]==0xffffffffu && application[2]==0,
         "a non-half fractional camera changed ordinary half-up rounding");
  free(page.argb_cache);
}

/* Under nearest-neighbour magnification, every source row must reach
 * at least one destination pixel. Sweep every synthetic row position
 * because a trailing-edge phase can fail only at some positions. */
static void check_first_generation_magnification_keeps_every_source_row(void){
  enum { LOGICAL_HEIGHT=208, SURFACE_HEIGHT=240, WIDTH=1, GLYPH=9 };
  uint8_t rgba[LOGICAL_HEIGHT*4];
  uint32_t application[SURFACE_HEIGHT];
  GmlWin content;
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&content,0,sizeof content);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  for(int y=0;y<LOGICAL_HEIGHT;y++){
    rgba[y*4+0]=(uint8_t)(1+y);
    rgba[y*4+1]=(uint8_t)(255-y);
    rgba[y*4+2]=(uint8_t)(3+y*3);
    rgba[y*4+3]=255;
  }
  content.bytecode=15;
  atlas.px=rgba;
  atlas.w=1; atlas.h=LOGICAL_HEIGHT;
  page.atlas=0;
  page.sw=page.bw=1;
  page.sh=page.bh=LOGICAL_HEIGHT;
  page.alpha_scanned=1;
  page.alpha_max=255;
  page.ax1=0; page.ay1=LOGICAL_HEIGHT-1;
  background.tpag=0;

  for(int top=0;top<=LOGICAL_HEIGHT-GLYPH;top++){
    memset(&render,0,sizeof render);
    memset(application,0,sizeof application);
    render.win=&content;
    render.atlas=&atlas; render.n_atlas=1;
    render.tpag=&page; render.n_tpag=1;
    render.bg=&background; render.n_bg=1;
    render.app_surface=application;
    render.alpha=1.0;
    render.alphablend=1;
    render.color_write_mask=0x0f;
    render.active_shader=-1;
    gml_render_begin(&render,application,WIDTH,SURFACE_HEIGHT,0.0,0.0);
    gml_render_world_set_logical_extent(&render,WIDTH,LOGICAL_HEIGHT);
    gml_draw_background_tile(&render,0,0,top,1,GLYPH,0,top,1,1,0,0,0,0xffffff,1.0);
    int missing=-1;
    for(int row=0;row<GLYPH && missing<0;row++){
      int source_y=top+row;
      uint32_t wanted=0xff000000u|((uint32_t)rgba[source_y*4]<<16)|
        ((uint32_t)rgba[source_y*4+1]<<8)|rgba[source_y*4+2];
      int seen=0;
      for(int y=0;y<SURFACE_HEIGHT && !seen;y++) if(application[y]==wanted) seen=1;
      if(!seen) missing=source_y;
    }
    if(missing>=0){
      fprintf(stderr,"renderer tiles: magnifying rows %d..%d of a %d-row camera into %d never "
              "rasterized source row %d\n",top,top+GLYPH-1,LOGICAL_HEIGHT,SURFACE_HEIGHT,missing);
      failures++;
      break;
    }
    free(page.argb_cache);
    page.argb_cache=NULL;
  }
  free(page.argb_cache);
}

/* A fractionally positioned quad at integer viewport magnification can put a destination
 * sample exactly on a source boundary. Keep that trailing-edge tie distinct from the
 * pixel-centre phase used at non-integer magnification. */
static void check_first_generation_integer_magnification_tie(void){
  static const uint8_t rgba[3*4]={
    255,0,0,255, 0,255,0,255, 0,0,255,255
  };
  uint32_t application[6]={0};
  GmlWin content;
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&content,0,sizeof content);
  memset(&render,0,sizeof render);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  content.bytecode=15;
  atlas.px=(uint8_t*)rgba;
  atlas.w=3; atlas.h=1;
  page.atlas=0;
  page.sw=page.bw=2;
  page.sh=page.bh=1;
  page.alpha_scanned=1;
  page.alpha_max=255;
  page.ax1=1; page.ay1=0;
  background.tpag=0;
  render.win=&content;
  render.atlas=&atlas; render.n_atlas=1;
  render.tpag=&page; render.n_tpag=1;
  render.bg=&background; render.n_bg=1;
  render.app_surface=application;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;

  gml_render_begin(&render,application,6,1,0.0,0.0);
  gml_render_world_set_logical_extent(&render,3,1);
  gml_draw_background_tile(&render,0,0,0,2,1,5.0/16.0,0,1,1,0,0,0,0xffffff,1.0);
  static const uint32_t expected[4]={
    0xffff0000u,0xff00ff00u,0xff00ff00u,0xff00ff00u
  };
  for(int x=0;x<4;x++) if(application[x+1]!=expected[x]){
    fprintf(stderr,"renderer tiles: integer magnification tie pixel %d was %08x, expected %08x\n",
            x,application[x+1],expected[x]);
    failures++;
    break;
  }
  free(page.argb_cache);
}

static void check_first_generation_application_surface_partial_alpha_coverage(void){
  static const uint8_t rgba[4]={255,255,255,128};
  uint32_t application=0xff000000u;
  uint32_t presentation=0xff000000u;
  GmlRender render;
  GmlWin content;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&render,0,sizeof render);
  memset(&content,0,sizeof content);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  content.bytecode=14;
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=1;
  page.atlas=0;
  page.sw=page.sh=page.bw=page.bh=1;
  background.tpag=0;
  render.win=&content;
  render.atlas=&atlas; render.n_atlas=1;
  render.tpag=&page; render.n_tpag=1;
  render.bg=&background; render.n_bg=1;
  render.app_surface=&application;
  render.app_w=render.app_h=1;
  render.app_surface_opaque=1;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;

  gml_render_begin(&render,&application,1,1,0.0,0.0);
  gml_render_clear(&render,0,1.0);
  gml_draw_background(&render,0,0.0,0.0);
  expect((application>>24)<255u,
         "a partial atlas texel did not change application-surface alpha");
  /* First-generation surface sampling retains partial coverage while automatic
   * screen presentation remains opaque and does not apply it again. */
  expect(render.app_surface_opaque && render.fb_opaque_known && render.fb_all_opaque,
         "first-generation partial coverage invalidated opaque screen presentation");

  gml_render_begin(&render,&presentation,1,1,0.0,0.0);
  gml_draw_surface_stretched(&render,0,0.0,0.0,1.0,1.0,0xffffffu,1.0);
  expect((presentation&0x00ffffffu)==(application&0x00ffffffu),
         "first-generation application-surface presentation applied coverage twice");
  gml_render_texture_page_cache_clear(&render,&page);
}

static void check_first_generation_automatic_presentation_ignores_retained_alpha(void){
  static const uint8_t rgba[4]={0,0,0,255};
  uint32_t application[2]={0xffc08040u,0xff604020u};
  uint32_t automatic[2]={0xff000000u,0xff000000u};
  uint32_t explicit_draw[2]={0xff000000u,0xff000000u};
  GmlRender render;
  GmlWin content;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&render,0,sizeof render);
  memset(&content,0,sizeof content);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  content.bytecode=14;
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=1;
  page.atlas=0;
  page.sw=page.sh=page.bw=page.bh=1;
  background.tpag=0;
  render.win=&content;
  render.atlas=&atlas; render.n_atlas=1;
  render.tpag=&page; render.n_tpag=1;
  render.bg=&background; render.n_bg=1;
  render.app_surface=application;
  render.app_w=2; render.app_h=1;
  render.app_surface_opaque=1;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;

  gml_render_begin(&render,application,2,1,0.0,0.0);
  gml_draw_background_ext(&render,0,0.0,0.0,1.0,1.0,0xffffffu,0.5);
  expect(!render.app_surface_opaque && (application[0]>>24)<255u,
         "a translucent application-surface draw did not retain partial coverage");
  uint32_t resolved_application[2]={application[0],application[1]};

  gml_render_begin(&render,automatic,2,1,0.0,0.0);
  gml_render_set_pending_underlay(&render,0,0,2,1);
  gml_render_flush_pending_underlay(&render);
  expect((automatic[0]&0x00ffffffu)==(resolved_application[0]&0x00ffffffu) &&
         (automatic[1]&0x00ffffffu)==(resolved_application[1]&0x00ffffffu),
         "first-generation automatic presentation applied retained coverage twice");
  expect(!render.app_surface_opaque && application[0]==resolved_application[0] &&
         application[1]==resolved_application[1],
         "automatic presentation changed application-surface pixels or coverage metadata");

  gml_render_begin(&render,explicit_draw,2,1,0.0,0.0);
  gml_draw_surface_stretched(&render,0,0.0,0.0,2.0,1.0,0xffffffu,1.0);
  expect((explicit_draw[0]&0x00ffffffu)!=(resolved_application[0]&0x00ffffffu),
         "an explicit surface-0 draw ignored the retained application-surface coverage");
  gml_render_texture_page_cache_clear(&render,&page);
}

static void check_first_generation_complete_black_mask_reaches_presentation(void){
  static const uint8_t rgba[4]={0,0,0,128};
  uint32_t application=0xffc08040u;
  uint32_t automatic=0xff000000u;
  GmlRender render;
  GmlWin content;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&render,0,sizeof render);
  memset(&content,0,sizeof content);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  content.bytecode=14;
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=1;
  page.atlas=0;
  page.sw=page.sh=page.bw=page.bh=1;
  background.tpag=0;
  render.win=&content;
  render.atlas=&atlas; render.n_atlas=1;
  render.tpag=&page; render.n_tpag=1;
  render.bg=&background; render.n_bg=1;
  render.app_surface=&application;
  render.app_w=render.app_h=1;
  render.app_surface_opaque=1;
  render.alpha=1.0;
  render.alphablend=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;

  gml_render_begin(&render,&application,1,1,0.0,0.0);
  gml_draw_background(&render,0,0.0,0.0);
  uint32_t resolved_application=application;
  expect((resolved_application&0x00ffffffu)==0x00604020u,
         "a complete black mask truncated first-generation target colour");
  expect(render.app_presentation_coverage_active,
         "a complete translucent black mask did not record presentation coverage");
  gml_render_application_surface_bind(&render,&application,1,1,render.app_surface_opaque);
  expect(render.app_presentation_coverage_active,
         "rebinding the same application surface discarded presentation coverage");

  gml_render_begin(&render,&automatic,1,1,0.0,0.0);
  gml_render_set_pending_underlay(&render,0,0,1,1);
  gml_render_flush_pending_underlay(&render);
  for(int shift=0;shift<=16;shift+=8){
    int resolved=(int)((resolved_application>>shift)&0xffu);
    int presented=(int)((automatic>>shift)&0xffu);
    expect(abs(presented*255-resolved*127)<=255,
           "a complete black mask did not carry its exact coverage to presentation");
  }
  expect(application==resolved_application,
         "masked automatic presentation changed stored surface-0 pixels");
  gml_render_begin(&render,&application,1,1,0.0,0.0);
  gml_draw_background(&render,0,0.0,0.0);
  expect(render.app_presentation_coverage_active,
         "a replacement mask did not record presentation coverage");
  gml_render_clear(&render,0,1.0);
  expect(!render.app_presentation_coverage_active,
         "a whole-target clear retained superseded presentation coverage");
  free(render.app_presentation_coverage);
  free(render.app_presentation_alpha_scratch);
  gml_render_texture_page_cache_clear(&render,&page);
}

static void check_first_generation_filtered_minification(void){
  static const uint8_t rgba[16]={
    255,0,0,255, 0,255,0,255,
    0,0,255,255, 255,255,255,255
  };
  uint32_t framebuffer=0xff000000u;
  GmlWin content;
  GmlRender render;
  GmlAtlas atlas;
  GmlTpag page;
  GmlBg background;

  memset(&content,0,sizeof content);
  memset(&render,0,sizeof render);
  memset(&atlas,0,sizeof atlas);
  memset(&page,0,sizeof page);
  memset(&background,0,sizeof background);
  content.bytecode=14;
  atlas.px=(uint8_t*)rgba;
  atlas.w=atlas.h=2;
  page.atlas=0;
  page.sw=page.sh=page.bw=page.bh=2;
  page.alpha_scanned=1;
  page.alpha_max=255;
  background.tpag=0;
  render.win=&content;
  render.fb=render.base_fb=&framebuffer;
  render.fbw=render.fbh=render.base_fbw=render.base_fbh=1;
  render.atlas=&atlas; render.n_atlas=1;
  render.tpag=&page; render.n_tpag=1;
  render.bg=&background; render.n_bg=1;
  render.alpha=1.0;
  render.alphablend=1;
  render.interp=1;
  render.color_write_mask=0x0f;
  render.active_shader=-1;
  render.target_id=-1;

  gml_draw_background_ext(&render,0,0.0,0.0,0.5,0.5,0xffffffu,1.0);
  expect(framebuffer==0xff808080u,
         "first-generation filtered minification did not sample all four neighbouring texels");
  free(page.argb_cache);
}

static void fill_cache_test_target(uint32_t *pixels,int width,int height,unsigned seed){
  for(int y=0;y<height;y++) for(int x=0;x<width;x++){
    unsigned red=(unsigned)(x*13+y*3+seed*17)&255u;
    unsigned green=(unsigned)(x*5+y*11+seed*29)&255u;
    unsigned blue=(unsigned)(x*7+y*19+seed*31)&255u;
    pixels[(size_t)y*width+x]=UINT32_C(0xff000000)|(red<<16)|(green<<8)|blue;
  }
}

static void init_first_generation_cache_renderer(
    GmlRender *render,GmlWin *content,GmlAtlas *atlas,GmlTpag *page,
    GmlBg *background,uint32_t *framebuffer,int width,int height){
  memset(render,0,sizeof(*render));
  render->win=content;
  render->fb=render->base_fb=framebuffer;
  render->fbw=render->base_fbw=width;
  render->fbh=render->base_fbh=height;
  render->atlas=atlas;
  render->n_atlas=1;
  render->tpag=page;
  render->n_tpag=1;
  render->bg=background;
  render->n_bg=1;
  render->alpha=1.0;
  render->alphablend=1;
  render->interp=1;
  render->color_write_mask=0x0f;
  render->active_shader=-1;
  render->target_id=-1;
}

static void check_repeated_filtered_draw_cache(void){
  enum { SOURCE_WIDTH=80,SOURCE_HEIGHT=80,ATLAS_WIDTH=82,ATLAS_HEIGHT=82,
         TARGET_WIDTH=200,TARGET_HEIGHT=180 };
  size_t atlas_bytes=(size_t)ATLAS_WIDTH*ATLAS_HEIGHT*4;
  size_t target_bytes=(size_t)TARGET_WIDTH*TARGET_HEIGHT*sizeof(uint32_t);
  uint8_t *rgba=calloc(atlas_bytes,1);
  uint32_t *reference_pixels=malloc(target_bytes);
  uint32_t *cached_pixels=malloc(target_bytes);
  GmlWin content;
  GmlAtlas atlas;
  GmlTpag reference_page,cached_page;
  GmlBg background;
  GmlRender reference,cached;
  if(!rgba || !reference_pixels || !cached_pixels){
    free(rgba);
    free(reference_pixels);
    free(cached_pixels);
    expect(0,"filtered draw cache fixture allocation failed");
    return;
  }
  memset(&content,0,sizeof content);
  memset(&atlas,0,sizeof atlas);
  memset(&reference_page,0,sizeof reference_page);
  memset(&background,0,sizeof background);
  content.bytecode=14;
  atlas.px=rgba;
  atlas.w=ATLAS_WIDTH;
  atlas.h=ATLAS_HEIGHT;
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++){
    uint8_t *pixel=rgba+((size_t)(y+1)*ATLAS_WIDTH+x+1)*4;
    pixel[0]=(uint8_t)(x*17+y*3);
    pixel[1]=(uint8_t)(x*5+y*23);
    pixel[2]=(uint8_t)(x*11+y*7);
    switch((x/7+y/5)%5){
      case 0: pixel[3]=0; break;
      case 1: pixel[3]=47; break;
      case 2: pixel[3]=128; break;
      case 3: pixel[3]=219; break;
      default: pixel[3]=255; break;
    }
  }
  reference_page.sx=reference_page.sy=1;
  reference_page.sw=reference_page.tw=SOURCE_WIDTH;
  reference_page.sh=reference_page.th=SOURCE_HEIGHT;
  reference_page.tx=reference_page.ty=1;
  reference_page.bw=ATLAS_WIDTH;
  reference_page.bh=ATLAS_HEIGHT;
  reference_page.atlas=0;
  cached_page=reference_page;
  background.tpag=0;
  init_first_generation_cache_renderer(
    &reference,&content,&atlas,&reference_page,&background,
    reference_pixels,TARGET_WIDTH,TARGET_HEIGHT);
  init_first_generation_cache_renderer(
    &cached,&content,&atlas,&cached_page,&background,
    cached_pixels,TARGET_WIDTH,TARGET_HEIGHT);

  fill_cache_test_target(reference_pixels,TARGET_WIDTH,TARGET_HEIGHT,1);
  memcpy(cached_pixels,reference_pixels,target_bytes);
  gml_draw_background_ext(&reference,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
  gml_draw_background_ext(&cached,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
  expect(!memcmp(reference_pixels,cached_pixels,target_bytes),
         "the filtered cache warm-up draw changed a destination pixel");
  expect(!cached_page.interp_draw_cache_valid,
         "a one-off filtered draw populated the repeat cache");

  fill_cache_test_target(reference_pixels,TARGET_WIDTH,TARGET_HEIGHT,2);
  memcpy(cached_pixels,reference_pixels,target_bytes);
  memset(&reference_page.interp_draw_pending_key,0,
         sizeof(reference_page.interp_draw_pending_key));
  reference_page.interp_draw_pending_count=0;
  cached.frame=reference.frame=2;
  gml_draw_background_ext(&reference,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
  gml_draw_background_ext(&cached,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
  expect(cached_page.interp_draw_cache_valid && cached.interp_draw_cache_bytes>0,
         "a repeated filtered draw did not populate the repeat cache");
  expect(!memcmp(reference_pixels,cached_pixels,target_bytes),
         "building the filtered repeat cache changed a destination pixel");

  fill_cache_test_target(reference_pixels,TARGET_WIDTH,TARGET_HEIGHT,3);
  memcpy(cached_pixels,reference_pixels,target_bytes);
  memset(&reference_page.interp_draw_pending_key,0,
         sizeof(reference_page.interp_draw_pending_key));
  reference_page.interp_draw_pending_count=0;
  cached.frame=reference.frame=3;
  gml_draw_background_ext(&reference,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
  gml_draw_background_ext(&cached,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
  expect(!memcmp(reference_pixels,cached_pixels,target_bytes),
         "replaying filtered opaque and partial-alpha spans changed a destination pixel");

  fill_cache_test_target(reference_pixels,TARGET_WIDTH,TARGET_HEIGHT,4);
  memcpy(cached_pixels,reference_pixels,target_bytes);
  memset(&reference_page.interp_draw_pending_key,0,
         sizeof(reference_page.interp_draw_pending_key));
  reference_page.interp_draw_pending_count=0;
  cached.frame=reference.frame=4;
  gml_draw_background_ext(&reference,0,9.0,7.0,2.0,1.875,0xffffffu,1.0);
  gml_draw_background_ext(&cached,0,9.0,7.0,2.0,1.875,0xffffffu,1.0);
  expect(!memcmp(reference_pixels,cached_pixels,target_bytes),
         "a changed filtered geometry reused stale cached samples");

  fill_cache_test_target(reference_pixels,TARGET_WIDTH,TARGET_HEIGHT,5);
  memcpy(cached_pixels,reference_pixels,target_bytes);
  memset(&reference_page.interp_draw_pending_key,0,
         sizeof(reference_page.interp_draw_pending_key));
  reference_page.interp_draw_pending_count=0;
  cached.frame=reference.frame=5;
  gml_draw_background_ext(&reference,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
  gml_draw_background_ext(&cached,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
  expect(!memcmp(reference_pixels,cached_pixels,target_bytes),
         "returning to a cached filtered geometry changed a destination pixel");

  gml_render_texture_page_cache_clear(&reference,&reference_page);
  gml_render_texture_page_cache_clear(&cached,&cached_page);
  reference.app_surface=reference_pixels;
  cached.app_surface=cached_pixels;
  reference.app_w=cached.app_w=TARGET_WIDTH;
  reference.app_h=cached.app_h=TARGET_HEIGHT;
  for(unsigned pass=0;pass<3;pass++){
    fill_cache_test_target(reference_pixels,TARGET_WIDTH,TARGET_HEIGHT,pass+6);
    for(size_t pixel=0;pixel<(size_t)TARGET_WIDTH*TARGET_HEIGHT;pixel++)
      reference_pixels[pixel]=(reference_pixels[pixel]&UINT32_C(0x00ffffff))|
        (((unsigned)(pixel*37u+pass*53u)&255u)<<24);
    memcpy(cached_pixels,reference_pixels,target_bytes);
    memset(&reference_page.interp_draw_pending_key,0,
           sizeof(reference_page.interp_draw_pending_key));
    reference_page.interp_draw_pending_count=0;
    cached.frame=reference.frame=6+pass;
    gml_draw_background_ext(&reference,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
    gml_draw_background_ext(&cached,0,8.25,6.75,2.125,1.9375,0xffffffu,1.0);
    if(pass==1)
      expect(cached_page.interp_draw_cache_valid,
             "a repeated filtered application-surface draw did not populate the cache");
    expect(!memcmp(reference_pixels,cached_pixels,target_bytes),
           "cached filtered application-surface draw changed destination ARGB pixels");
  }
  gml_render_texture_page_cache_clear(&reference,&reference_page);
  gml_render_texture_page_cache_clear(&cached,&cached_page);
  expect(cached.interp_draw_cache_bytes==0,
         "clearing a texture page left filtered cache memory accounted");
  free(rgba);
  free(reference_pixels);
  free(cached_pixels);
}

static void init_modern_point_renderer(
    GmlRender *render,GmlWin *content,GmlAtlas *atlas,GmlTpag *page,
    GmlBg *background,uint32_t *framebuffer,int width,int height,int active_shader){
  memset(render,0,sizeof(*render));
  render->win=content;
  render->fb=render->base_fb=framebuffer;
  render->fbw=render->base_fbw=width;
  render->fbh=render->base_fbh=height;
  render->app_surface=framebuffer;
  render->app_w=width;
  render->app_h=height;
  render->atlas=atlas;
  render->n_atlas=1;
  render->tpag=page;
  render->n_tpag=1;
  render->bg=background;
  render->n_bg=1;
  render->alpha=1.0;
  render->alphablend=1;
  render->blend_equation=1;
  render->blend_equation_alpha=1;
  render->color_write_mask=0x0f;
  render->active_shader=active_shader;
  render->target_id=-1;
}

static void fill_alpha_target(uint32_t *pixels,int width,int height,unsigned seed){
  for(int y=0;y<height;y++) for(int x=0;x<width;x++){
    unsigned alpha=(unsigned)(x*23+y*41+seed*13)&255u;
    unsigned red=(unsigned)(x*13+y*3+seed*17)&255u;
    unsigned green=(unsigned)(x*5+y*11+seed*29)&255u;
    unsigned blue=(unsigned)(x*7+y*19+seed*31)&255u;
    pixels[(size_t)y*width+x]=(alpha<<24)|(red<<16)|(green<<8)|blue;
  }
}

static void check_modern_opaque_scaled_partial_alpha(void){
  enum { SOURCE_WIDTH=80,SOURCE_HEIGHT=60,ATLAS_WIDTH=82,ATLAS_HEIGHT=62,
         TARGET_WIDTH=640,TARGET_HEIGHT=480 };
  static const double scale[4][2]={{4.25,4.25},{-4.25,4.25},
                                    {-4.25,-4.25},{4.25,-4.25}};
  static const double draw_alpha[3]={0.05,0.2,0.375};
  size_t atlas_bytes=(size_t)ATLAS_WIDTH*ATLAS_HEIGHT*4u;
  size_t target_bytes=(size_t)TARGET_WIDTH*TARGET_HEIGHT*sizeof(uint32_t);
  uint8_t *rgba=calloc(atlas_bytes,1);
  uint32_t *reference_pixels=malloc(target_bytes);
  uint32_t *fast_pixels=malloc(target_bytes);
  GmlWin content;
  AnygmCompatibilityProfile compatibility;
  GmlAtlas atlas;
  GmlTpag reference_page,fast_page;
  GmlBg background;
  GmlRender reference,fast;
  if(!rgba || !reference_pixels || !fast_pixels){
    free(rgba);
    free(reference_pixels);
    free(fast_pixels);
    expect(0,"modern opaque scaled-alpha fixture allocation failed");
    return;
  }
  memset(&content,0,sizeof content);
  memset(&compatibility,0,sizeof compatibility);
  memset(&atlas,0,sizeof atlas);
  memset(&reference_page,0,sizeof reference_page);
  memset(&background,0,sizeof background);
  compatibility.has_modern_layer_semantics=1;
  compatibility.blend=ANYGM_BLEND_STUDIO_SECOND;
  content.bytecode=17;
  content.compatibility=&compatibility;
  atlas.px=rgba;
  atlas.w=ATLAS_WIDTH;
  atlas.h=ATLAS_HEIGHT;
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++){
    uint8_t *pixel=rgba+((size_t)(y+1)*ATLAS_WIDTH+x+1)*4u;
    pixel[0]=(uint8_t)(x*29+y*7);
    pixel[1]=(uint8_t)(x*11+y*31);
    pixel[2]=(uint8_t)(x*17+y*13);
    pixel[3]=255;
  }
  reference_page.sx=reference_page.sy=1;
  reference_page.sw=reference_page.tw=SOURCE_WIDTH;
  reference_page.sh=reference_page.th=SOURCE_HEIGHT;
  reference_page.bw=SOURCE_WIDTH;
  reference_page.bh=SOURCE_HEIGHT;
  reference_page.atlas=0;
  fast_page=reference_page;
  background.tpag=0;
  /* An out-of-range active shader leaves the general renderer unchanged while intentionally
   * making it ineligible for the no-shader fast path. */
  init_modern_point_renderer(
    &reference,&content,&atlas,&reference_page,&background,
    reference_pixels,TARGET_WIDTH,TARGET_HEIGHT,0);
  init_modern_point_renderer(
    &fast,&content,&atlas,&fast_page,&background,
    fast_pixels,TARGET_WIDTH,TARGET_HEIGHT,-1);
  for(int pass=0;pass<3;pass++){
    fill_alpha_target(reference_pixels,TARGET_WIDTH,TARGET_HEIGHT,(unsigned)pass+1u);
    memcpy(fast_pixels,reference_pixels,target_bytes);
    for(int draw=0;draw<4;draw++){
      gml_draw_background_ext(
        &reference,0,320.0,240.0,scale[draw][0],scale[draw][1],
        0xffffffu,draw_alpha[pass]);
      gml_draw_background_ext(
        &fast,0,320.0,240.0,scale[draw][0],scale[draw][1],
        0xffffffu,draw_alpha[pass]);
    }
    expect(!memcmp(reference_pixels,fast_pixels,target_bytes),
           "modern opaque scaled partial-alpha fast path changed a destination pixel");
  }
  expect(fast_page.alpha_runs_built && fast_page.alpha_run_count==SOURCE_HEIGHT,
         "modern opaque scaled partial-alpha draw did not certify its texture coverage");
  gml_render_texture_page_cache_clear(&reference,&reference_page);
  gml_render_texture_page_cache_clear(&fast,&fast_page);
  free(rgba);
  free(reference_pixels);
  free(fast_pixels);
}

int main(void){
  static const uint32_t transformed[8][4]={
    {0xffff0000,0xff00ff00,0xff0000ff,0xffffffff},
    {0xff00ff00,0xffff0000,0xffffffff,0xff0000ff},
    {0xff0000ff,0xffffffff,0xffff0000,0xff00ff00},
    {0xffffffff,0xff0000ff,0xff00ff00,0xffff0000},
    {0xff0000ff,0xffff0000,0xffffffff,0xff00ff00},
    {0xffffffff,0xff00ff00,0xff0000ff,0xffff0000},
    {0xffff0000,0xff0000ff,0xff00ff00,0xffffffff},
    {0xff00ff00,0xffffffff,0xffff0000,0xff0000ff}
  };
  check_tileset_layout(0);
  check_tileset_layout(1);
  for(int bits=0;bits<8;bits++)
    check_transform(bits&1,(bits>>1)&1,(bits>>2)&1,transformed[bits]);
  for(int bits=0;bits<4;bits++)
    check_fractional_transform_equivalence(bits&1,(bits>>1)&1);
  check_runtime_background_replacement();
  check_trimmed_background_tiling_period();
  check_background_anchor_holds_across_subpixel_drift();
  check_removeback_uses_bottom_left();
  /* Destination extent is source_height*2; the covered rows are those whose centres fall in it. */
  check_fractional_sprite_part_extent(0.6,1);
  check_fractional_sprite_part_extent(0.8,2);
  check_fractional_sprite_part_extent(0.2,0);
  check_early_second_generation_camera_phase();
  check_first_generation_default_font_metrics();
  check_render_pass_restores_normal_blending();
  check_first_generation_fractional_tile_projection();
  check_modern_fractional_tile_grid_has_no_cracks();
  check_first_generation_magnification_keeps_every_source_row();
  check_first_generation_integer_magnification_tie();
  check_classic_double_scale_layer_covers_its_last_row();
  check_modern_fractional_camera_tie();
  check_first_generation_application_surface_partial_alpha_coverage();
  check_first_generation_automatic_presentation_ignores_retained_alpha();
  check_first_generation_complete_black_mask_reaches_presentation();
  check_first_generation_filtered_minification();
  check_repeated_filtered_draw_cache();
  check_modern_opaque_scaled_partial_alpha();
  if(failures){
    fprintf(stderr,"renderer tiles: %d failure(s)\n",failures);
    return 1;
  }
  puts("renderer tiles: ok");
  return 0;
}
