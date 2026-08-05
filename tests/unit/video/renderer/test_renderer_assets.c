/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "stdio_vfs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"renderer assets failed: %s\n",label); \
    return 1; \
  } \
}while(0)

/* One pixel per subimage, two subimages, so the decoded frame count is unambiguous and cannot be
 * confused with a strip that a requested count would cut. */
static const uint8_t animated_two_frame_gif[]={
  0x47,0x49,0x46,0x38,0x39,0x61,0x01,0x00,0x01,0x00,0x80,0x00,0x00,
  0xFF,0x00,0x00,0x00,0x00,0xFF,
  0x21,0xF9,0x04,0x00,0x00,0x00,0x00,0x00,
  0x2C,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x00,
  0x02,0x02,0x44,0x01,0x00,
  0x21,0xF9,0x04,0x00,0x00,0x00,0x00,0x00,
  0x2C,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x00,
  0x02,0x02,0x4C,0x01,0x00,
  0x3B
};

/* A four-pixel row that a requested count of four cuts into four single-pixel subimages. */
static const uint8_t strip_gif[]={
  0x47,0x49,0x46,0x38,0x39,0x61,0x04,0x00,0x01,0x00,0x80,0x00,0x00,
  0xFF,0x00,0x00,0x00,0x00,0xFF,
  0x2C,0x00,0x00,0x00,0x00,0x04,0x00,0x01,0x00,0x00,
  0x02,0x03,0x44,0x82,0x05,0x00,
  0x3B
};

static int write_file(const char *path,const uint8_t *data,size_t size){
  FILE *file=fopen(path,"wb");
  if(!file) return 0;
  size_t written=fwrite(data,1,size,file);
  fclose(file);
  return written==size;
}

int main(void){
  char root[]="build/renderer-assets-XXXXXX";
  REQUIRE(mkdtemp(root)!=NULL,"temporary root");
  char animated[256],strip[256];
  REQUIRE(snprintf(animated,sizeof animated,"%s/animated.gif",root)<(int)sizeof animated &&
          snprintf(strip,sizeof strip,"%s/strip.gif",root)<(int)sizeof strip,"fixture paths");
  REQUIRE(write_file(animated,animated_two_frame_gif,sizeof animated_two_frame_gif),
          "animated fixture");
  REQUIRE(write_file(strip,strip_gif,sizeof strip_gif),"strip fixture");

  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  GmlWin win;
  memset(&win,0,sizeof win);
  win.host=&services;
  GmlRender render;
  REQUIRE(gml_render_init(&render,&win)==0,"renderer init");

  /* An animated image states its own subimage count. Applying a one-image request to it would
   * collapse the animation to its first frame. */
  int animated_sprite=gml_sprite_add_file(&render,animated,1,0,0,0);
  REQUIRE(animated_sprite>=0,"animated sprite was not added");
  REQUIRE(gml_sprite_frames(&render,animated_sprite)==2,
          "animated subimage count did not come from the file");

  int all_frames=gml_sprite_add_file(&render,animated,-1,0,0,0);
  REQUIRE(all_frames>=0 && gml_sprite_frames(&render,all_frames)==2,
          "an unrestricted request changed the animated subimage count");

  /* A single-image strip keeps taking its subimage count from the request, which is the only
   * information that says where to cut it. */
  int strip_sprite=gml_sprite_add_file(&render,strip,4,0,0,0);
  REQUIRE(strip_sprite>=0 && gml_sprite_frames(&render,strip_sprite)==4,
          "strip subimage count did not follow the request");
  int whole_strip=gml_sprite_add_file(&render,strip,1,0,0,0);
  REQUIRE(whole_strip>=0 && gml_sprite_frames(&render,whole_strip)==1,
          "an uncut strip gained subimages");

  /* Replacement follows the same rule, since content commonly swaps a placeholder for a file. */
  REQUIRE(gml_sprite_replace_from_file(&render,strip_sprite,animated,1,0,0,0,0),
          "animated replacement failed");
  REQUIRE(gml_sprite_frames(&render,strip_sprite)==2,
          "animated replacement did not take its subimage count from the file");

  gml_render_free(&render);
  puts("renderer assets: ok");
  return 0;
}
