/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_image_codec.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int expect(int condition,const char *what){
  if(!condition) fprintf(stderr,"image codec assertion failed: %s\n",what);
  return condition;
}

static uint64_t fnv64(const uint8_t *data,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  for(size_t index=0;index<size;index++){
    hash^=data[index];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

int main(void){
  static const uint8_t rgba[]={
    0xff,0x00,0x00,0xff, 0x00,0xff,0x00,0x80,
    0x00,0x00,0xff,0x40, 0xff,0xff,0xff,0x00
  };
  static const uint8_t plain[]="deterministic codec probe\n";
  static const uint8_t zlib_stream[]={
    0x78,0xda,0x4b,0x49,0x2d,0x49,0x2d,0xca,0xcd,0xcc,0xcb,0x2c,
    0x2e,0xc9,0x4c,0x56,0x48,0xce,0x4f,0x49,0x4d,0x56,0x28,0x28,
    0xca,0x4f,0x4a,0xe5,0x02,0x00,0x8c,0x09,0x09,0xd5
  };
  static const uint8_t raw_stream[]={
    0x4b,0x49,0x2d,0x49,0x2d,0xca,0xcd,0xcc,0xcb,0x2c,0x2e,0xc9,
    0x4c,0x56,0x48,0xce,0x4f,0x49,0x4d,0x56,0x28,0x28,0xca,0x4f,
    0x4a,0xe5,0x02,0x00
  };
  static const uint8_t animated_gif[]={
    'G','I','F','8','9','a',1,0,1,0,0x80,0,0,
    255,0,0, 0,255,0,
    0x21,0xf9,0x04,0x00,0x0a,0x00,0x00,0x00,
    0x2c,0,0,0,0,1,0,1,0,0, 0x02,0x02,0x44,0x01,0x00,
    0x21,0xf9,0x04,0x00,0x0a,0x00,0x00,0x00,
    0x2c,0,0,0,0,1,0,1,0,0, 0x02,0x02,0x4c,0x01,0x00,
    0x3b
  };
  int ok=1;

  GmlMediaBuffer png={0};
  ok&=expect(gml_image_encode_png(rgba,2,2,4,8,&png),"encode PNG");
  ok&=expect(png.size>=8 && !memcmp(png.data,"\x89PNG\r\n\x1a\n",8),
             "PNG signature");
  ok&=expect(png.size==80 &&
             fnv64(png.data,png.size)==UINT64_C(0xcc2a0b573b15c2ac),
             "byte-identical established package encoder output");
  int width=0,height=0,components=0;
  GmlMediaBuffer decoded={0};
  ok&=expect(gml_image_decode_rgba(png.data,png.size,&decoded,
                                  &width,&height,&components),"decode PNG");
  ok&=expect(width==2 && height==2 && components==4 &&
             decoded.size==sizeof(rgba) &&
             !memcmp(decoded.data,rgba,sizeof(rgba)),"exact RGBA round trip");
  gml_media_buffer_release(&decoded);
  gml_media_buffer_release(&png);
  ok&=expect(!decoded.data && decoded.size==0 && !png.data && png.size==0,
             "release clears ownership records");

  int frames=0;
  ok&=expect(gml_image_decode_rgba_frames(
      animated_gif,sizeof(animated_gif),&decoded,
      &width,&height,&frames,&components),"decode animated GIF");
  static const uint8_t gif_rgba[]={
    255,0,0,255, 0,255,0,255
  };
  ok&=expect(width==1 && height==1 && frames==2 &&
             decoded.size==sizeof(gif_rgba) &&
             !memcmp(decoded.data,gif_rgba,sizeof(gif_rgba)),
             "exact animated GIF frame planes");
  gml_media_buffer_release(&decoded);

  GmlMediaBuffer inflated={0};
  ok&=expect(gml_deflate_decode(zlib_stream,sizeof(zlib_stream),
                                GML_DEFLATE_ZLIB,&inflated),"zlib inflate");
  ok&=expect(inflated.size==sizeof(plain)-1 &&
             !memcmp(inflated.data,plain,sizeof(plain)-1),"zlib bytes");
  gml_media_buffer_release(&inflated);
  ok&=expect(gml_deflate_decode(raw_stream,sizeof(raw_stream),
                                GML_DEFLATE_RAW,&inflated),"raw inflate");
  ok&=expect(inflated.size==sizeof(plain)-1 &&
             !memcmp(inflated.data,plain,sizeof(plain)-1),"raw bytes");
  gml_media_buffer_release(&inflated);

  uint8_t fixed[sizeof(plain)-1];
  size_t fixed_size=0;
  ok&=expect(gml_deflate_decode_to_buffer(raw_stream,sizeof(raw_stream),
                                         GML_DEFLATE_RAW,fixed,sizeof(fixed),
                                         &fixed_size),"raw fixed-buffer inflate");
  ok&=expect(fixed_size==sizeof(fixed) && !memcmp(fixed,plain,sizeof(fixed)),
             "fixed-buffer bytes");
  ok&=expect(!gml_deflate_decode_to_buffer(raw_stream,sizeof(raw_stream),
                                          GML_DEFLATE_RAW,fixed,sizeof(fixed)-1,
                                          &fixed_size) && fixed_size==0,
             "bounded output rejection");

  static const uint8_t malformed[]={0x89,'P','N','G'};
  width=height=components=7;
  decoded.data=(uint8_t*)(uintptr_t)1;
  decoded.size=9;
  ok&=expect(!gml_image_decode_rgba(malformed,sizeof(malformed),&decoded,
                                   &width,&height,&components) &&
             !decoded.data && decoded.size==0 &&
             width==0 && height==0 && components==0,
             "transactional malformed image rejection");
  ok&=expect(!gml_deflate_decode(malformed,(size_t)INT_MAX+1u,
                                GML_DEFLATE_ZLIB,&inflated) &&
             !inflated.data && inflated.size==0,
             "oversized encoded range rejection");

  if(!ok) return 1;
  printf("image codec tests: ok\n");
  return 0;
}
