/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"

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

  expect(gml_background_replace_from_rgba(&render,0,rgba,2,2),
         "runtime background replacement rejected valid RGBA pixels");
  expect(render.n_atlas==2 && render.bg[0].tpag==0 &&
         render.tpag[0].atlas==1 && render.tpag[0].sw==2 &&
         render.tpag[0].sh==2 && render.tpag[0].bw==2 &&
         render.tpag[0].bh==2,
         "runtime background replacement did not publish its new texture page");
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
  if(failures){
    fprintf(stderr,"renderer tiles: %d failure(s)\n",failures);
    return 1;
  }
  puts("renderer tiles: ok");
  return 0;
}
