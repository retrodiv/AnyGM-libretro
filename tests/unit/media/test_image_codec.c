/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_image_codec.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static int large_png_bytes(void){
  enum { WIDTH=257, HEIGHT=129, STRIDE=1037 };
  static const size_t expected_size[]={1877u,132829u};
  static const uint64_t expected_hash[]={
    UINT64_C(0x4d8aae5336368498),UINT64_C(0x9686cd0ff957a69c)
  };
  static const size_t expected_deflate_size[]={4201u,133804u};
  static const uint64_t expected_deflate_hash[]={
    UINT64_C(0x597a583ddde301c1),UINT64_C(0x94874aea9667112f)
  };
  uint8_t pixels[STRIDE*HEIGHT];
  int ok=1;
  for(unsigned pattern=0;pattern<2u;pattern++){
    memset(pixels,0xa5,sizeof pixels);
    uint32_t random=UINT32_C(0x12345678);
    for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH*4;x++){
      random=random*UINT32_C(1664525)+UINT32_C(1013904223);
      pixels[y*STRIDE+x]=pattern?(uint8_t)(random>>24):(uint8_t)(x/4+y*3);
    }
    GmlMediaBuffer png={0},decoded={0};
    if(!expect(gml_image_encode_png(pixels,WIDTH,HEIGHT,4,STRIDE,&png),
               "large padded PNG encode")) return 0;
    /* Preserve the established encoder bytes across compiler optimization,
     * including repeated rows and incompressible input with growing tables. */
    ok&=expect(png.size==expected_size[pattern] &&
               fnv64(png.data,png.size)==expected_hash[pattern],
               "large PNG retains established compressed bytes");
    int width=0,height=0,components=0;
    int decoded_ok=gml_image_decode_rgba(png.data,png.size,&decoded,
                                        &width,&height,&components);
    ok&=expect(decoded_ok && width==WIDTH && height==HEIGHT && components==4 &&
               decoded.size==(size_t)WIDTH*HEIGHT*4u,
               "large PNG decode extent");
    if(decoded_ok && decoded.size==(size_t)WIDTH*HEIGHT*4u)
      for(int y=0;y<HEIGHT;y++)
        ok&=expect(!memcmp(decoded.data+(size_t)y*WIDTH*4u,
                           pixels+(size_t)y*STRIDE,WIDTH*4u),
                   "large PNG exact pixels exclude stride padding");
    gml_media_buffer_release(&decoded);
    gml_media_buffer_release(&png);
    /* The buffer encoder shares the compressor but uses a different quality. */
    int deflated=gml_deflate_encode_zlib(pixels,sizeof pixels,&png);
    ok&=expect(deflated && png.size==expected_deflate_size[pattern] &&
               fnv64(png.data,png.size)==expected_deflate_hash[pattern],
               "large zlib retains established compressed bytes");
    if(deflated){
      decoded_ok=gml_deflate_decode(png.data,png.size,GML_DEFLATE_ZLIB,&decoded);
      ok&=expect(decoded_ok && decoded.size==sizeof pixels &&
                 !memcmp(decoded.data,pixels,sizeof pixels),
                 "large zlib exact round trip includes padding bytes");
    }
    gml_media_buffer_release(&decoded);
    gml_media_buffer_release(&png);
  }
  return ok;
}

/* Exercises the direct 8-bit RGBA decode against the defining transform: each image is filtered
 * row by row with all five PNG filters, compressed, split across several IDAT chunks and decoded
 * back. Streams outside that form must still decode through the generic loader. */
static uint32_t png_test_crc(const uint8_t *data,size_t size){
  uint32_t crc=0xffffffffu;
  for(size_t index=0;index<size;index++){
    crc^=data[index];
    for(int bit=0;bit<8;bit++) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));
  }
  return crc^0xffffffffu;
}

static uint8_t *png_test_chunk(uint8_t *at,const char *type,const uint8_t *data,uint32_t size){
  at[0]=(uint8_t)(size>>24);at[1]=(uint8_t)(size>>16);at[2]=(uint8_t)(size>>8);at[3]=(uint8_t)size;
  memcpy(at+4,type,4);
  if(size) memcpy(at+8,data,size);
  uint32_t crc=png_test_crc(at+4,size+4u);
  at[8+size]=(uint8_t)(crc>>24);at[9+size]=(uint8_t)(crc>>16);
  at[10+size]=(uint8_t)(crc>>8);at[11+size]=(uint8_t)crc;
  return at+12+size;
}

static uint8_t png_test_paeth(int a,int b,int c){
  int p=a+b-c,pa=abs(p-a),pb=abs(p-b),pc=abs(p-c);
  return (uint8_t)(pa<=pb && pa<=pc?a:pb<=pc?b:c);
}

/* Writes a PNG of the given colour type. Row y uses filter (y+first_filter)%5, and the
 * compressed stream is cut into pieces of at most split bytes. */
static int png_test_build(const uint8_t *pixels,uint32_t width,uint32_t height,int colour,
                          int first_filter,size_t split,const char *extra_chunk,
                          size_t trailing,GmlMediaBuffer *out){
  uint32_t channels=colour==6?4u:3u;
  size_t row=(size_t)width*channels,raw_size=(row+1u)*height+trailing;
  uint8_t *raw=calloc(raw_size,1);
  GmlMediaBuffer zlib={0};
  if(!raw) return 0;
  for(uint32_t y=0;y<height;y++){
    uint8_t filter=(uint8_t)((y+(uint32_t)first_filter)%5u);
    uint8_t *line=raw+(size_t)y*(row+1u);
    line[0]=filter;
    for(size_t i=0;i<row;i++){
      size_t x=i/channels,c=i%channels;
      const uint8_t *source=pixels+((size_t)y*width+x)*4u+c;
      int value=*source;
      int left=i>=channels?source[-4]:0;
      int up=y?source[-(ptrdiff_t)width*4]:0;
      int corner=y && i>=channels?source[-(ptrdiff_t)width*4-4]:0;
      int predicted=filter==1?left:filter==2?up:filter==3?(left+up)>>1:
                    filter==4?png_test_paeth(left,up,corner):0;
      line[1+i]=(uint8_t)(value-predicted);
    }
  }
  int ok=gml_deflate_encode_zlib(raw,raw_size,&zlib);
  free(raw);
  if(!ok) return 0;
  size_t pieces=split?(zlib.size+split-1u)/split:1u;
  size_t capacity=8u+25u+(extra_chunk?24u:0u)+zlib.size+12u*pieces+12u;
  out->data=malloc(capacity);
  if(!out->data){gml_media_buffer_release(&zlib);return 0;}
  uint8_t *at=out->data;
  memcpy(at,"\x89PNG\r\n\x1a\n",8);at+=8;
  uint8_t header[13]={(uint8_t)(width>>24),(uint8_t)(width>>16),(uint8_t)(width>>8),(uint8_t)width,
                      (uint8_t)(height>>24),(uint8_t)(height>>16),(uint8_t)(height>>8),(uint8_t)height,
                      8,(uint8_t)colour,0,0,0};
  at=png_test_chunk(at,"IHDR",header,13);
  if(extra_chunk){
    static const uint8_t payload[12]={0,0,0,0,0,0,0,0,0,0,0,0};
    at=png_test_chunk(at,extra_chunk,payload,colour==6 && !strcmp(extra_chunk,"tRNS")?6u:12u);
  }
  for(size_t offset=0;offset<zlib.size;){
    size_t length=split && zlib.size-offset>split?split:zlib.size-offset;
    at=png_test_chunk(at,"IDAT",zlib.data+offset,(uint32_t)length);
    offset+=length;
  }
  at=png_test_chunk(at,"IEND",NULL,0);
  out->size=(size_t)(at-out->data);
  gml_media_buffer_release(&zlib);
  return 1;
}

static int png_test_decodes_to(const GmlMediaBuffer *png,const uint8_t *pixels,uint32_t width,
                               uint32_t height,int opaque){
  GmlMediaBuffer decoded={0};
  int decoded_width=0,decoded_height=0,components=0;
  int ok=gml_image_decode_rgba(png->data,png->size,&decoded,&decoded_width,&decoded_height,
                               &components) &&
    decoded_width==(int)width && decoded_height==(int)height &&
    components==(opaque?3:4) && decoded.size==(size_t)width*height*4u;
  for(size_t index=0;ok && index<decoded.size;index++)
    ok=decoded.data[index]==(opaque && index%4u==3u?255u:pixels[index]);
  gml_media_buffer_release(&decoded);
  return ok;
}

static int png_direct_decode(void){
  static const uint32_t sizes[][2]={{67,41},{1,9},{9,1},{300,64}};
  int ok=1;
  for(size_t shape=0;shape<sizeof sizes/sizeof sizes[0];shape++){
    uint32_t width=sizes[shape][0],height=sizes[shape][1];
    size_t bytes=(size_t)width*height*4u;
    uint8_t *pixels=malloc(bytes);
    if(!expect(pixels!=NULL,"PNG test pixels")) return 0;
    /* Bands repeat with byte periods of one to eight, then a pseudo-random band, so the
     * compressed stream carries literals and matches at short and long distances. */
    uint32_t random=UINT32_C(0x9e3779b9);
    for(uint32_t y=0;y<height;y++) for(size_t i=0;i<(size_t)width*4u;i++){
      uint32_t band=(y/3u)%10u;
      random=random*UINT32_C(1664525)+UINT32_C(1013904223);
      pixels[(size_t)y*width*4u+i]=band<8u?(uint8_t)(17u*(i%(band+1u))+y/30u):
                                   band==8u?(uint8_t)(random>>24):(uint8_t)(i/4u+y);
    }
    GmlMediaBuffer png={0};
    for(int first=0;first<5;first++) for(int split=0;split<2;split++){
      ok&=expect(png_test_build(pixels,width,height,6,first,split?7u:0u,
                                split?"tEXt":NULL,0,&png) &&
                 png_test_decodes_to(&png,pixels,width,height,0),
                 "RGBA PNG decodes exactly under every row filter and IDAT split");
      gml_media_buffer_release(&png);
    }
    /* The generic loader accepts surplus image data after the last row. */
    ok&=expect(png_test_build(pixels,width,height,6,3,5u,NULL,16u,&png) &&
               png_test_decodes_to(&png,pixels,width,height,0),
               "RGBA PNG with surplus image data decodes exactly");
    gml_media_buffer_release(&png);
    ok&=expect(png_test_build(pixels,width,height,2,1,0u,NULL,0,&png) &&
               png_test_decodes_to(&png,pixels,width,height,1),
               "RGB PNG still decodes opaque");
    gml_media_buffer_release(&png);
    /* Transparency is invalid with an alpha channel, and a cut stream is incomplete. */
    ok&=expect(png_test_build(pixels,width,height,6,0,0u,"tRNS",0,&png) &&
               !png_test_decodes_to(&png,pixels,width,height,0),
               "RGBA PNG with transparency chunk is refused");
    gml_media_buffer_release(&png);
    if(png_test_build(pixels,width,height,6,4,0u,NULL,0,&png)){
      GmlMediaBuffer decoded={0};
      int w=0,h=0,c=0;
      ok&=expect(!gml_image_decode_rgba(png.data,png.size/2u,&decoded,&w,&h,&c) &&
                 !decoded.data && !w && !h && !c,
                 "truncated RGBA PNG is refused transactionally");
    } else ok&=expect(0,"truncated PNG build");
    gml_media_buffer_release(&png);
    free(pixels);
  }
  return ok;
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
  ok&=large_png_bytes();
  ok&=png_direct_decode();

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
  GmlMediaBuffer compressed={0},compressed_again={0};
  ok&=expect(gml_deflate_encode_zlib(plain,sizeof(plain)-1,&compressed),
             "zlib deflate");
  ok&=expect(gml_deflate_encode_zlib(plain,sizeof(plain)-1,&compressed_again),
             "repeat zlib deflate");
  ok&=expect(compressed.size==compressed_again.size &&
             !memcmp(compressed.data,compressed_again.data,compressed.size),
             "deterministic zlib bytes");
  ok&=expect(compressed.size==34 &&
             fnv64(compressed.data,compressed.size)==UINT64_C(0x8f2552ae81a379fd),
             "pinned zlib encoder output");
  ok&=expect(gml_deflate_decode(compressed.data,compressed.size,
                                GML_DEFLATE_ZLIB,&inflated),"encoded zlib inflate");
  ok&=expect(inflated.size==sizeof(plain)-1 &&
             !memcmp(inflated.data,plain,sizeof(plain)-1),"encoded zlib bytes");
  gml_media_buffer_release(&inflated);
  gml_media_buffer_release(&compressed_again);
  gml_media_buffer_release(&compressed);

  ok&=expect(gml_deflate_encode_zlib(NULL,0,&compressed),"empty zlib deflate");
  ok&=expect(compressed.size==11 &&
             !memcmp(compressed.data,"\x78\x01\x01\x00\x00\xff\xff\x00\x00\x00\x01",11),
             "canonical empty zlib bytes");
  ok&=expect(gml_deflate_decode(compressed.data,compressed.size,
                                GML_DEFLATE_ZLIB,&inflated) && inflated.size==0,
             "empty zlib inflate");
  gml_media_buffer_release(&inflated);
  gml_media_buffer_release(&compressed);

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
