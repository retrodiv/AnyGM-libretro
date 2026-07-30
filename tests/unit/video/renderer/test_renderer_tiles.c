/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"

#include <stdint.h>
#include <stdio.h>
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
  render.target_id=-1;
  gml_draw_background_tile(&render,0,0,0,2,2,0,0,1,1,
                           mirror,flip,rotate,0xffffff,1.0);
  gml_render_flush_rotated_batch(&render);
  expect(!memcmp(framebuffer,expected,sizeof framebuffer),
         "tile mirror/flip/rotate combination produced shifted or reordered pixels");
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
  if(failures){
    fprintf(stderr,"renderer tiles: %d failure(s)\n",failures);
    return 1;
  }
  puts("renderer tiles: ok");
  return 0;
}
