/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_font_raster.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message){
  if(condition) return;
  fprintf(stderr,"font raster test failed: %s\n",message);
  failures++;
}

static void put_u16(uint8_t *data, size_t offset, unsigned value){
  data[offset]=(uint8_t)(value>>8);
  data[offset+1]=(uint8_t)value;
}

static void put_u32(uint8_t *data, size_t offset, uint32_t value){
  data[offset]=(uint8_t)(value>>24);
  data[offset+1]=(uint8_t)(value>>16);
  data[offset+2]=(uint8_t)(value>>8);
  data[offset+3]=(uint8_t)value;
}

static void put_table(uint8_t *data, size_t directory, const char tag[4],
                      uint32_t offset, uint32_t length){
  memcpy(data+directory,tag,4);
  put_u32(data,directory+8,offset);
  put_u32(data,directory+12,length);
}

static size_t build_synthetic_font(uint8_t data[552]){
  enum {
    DIRECTORY=12,
    CMAP=124,
    GLYF=400,
    HEAD=436,
    HHEA=492,
    HMTX=528,
    LOCA=536,
    MAXP=544
  };
  memset(data,0,552);
  put_u32(data,0,0x00010000u);
  put_u16(data,4,7);
  put_u16(data,6,64);
  put_u16(data,8,2);
  put_u16(data,10,48);
  put_table(data,DIRECTORY+0u*16u,"cmap",CMAP,274);
  put_table(data,DIRECTORY+1u*16u,"glyf",GLYF,34);
  put_table(data,DIRECTORY+2u*16u,"head",HEAD,54);
  put_table(data,DIRECTORY+3u*16u,"hhea",HHEA,36);
  put_table(data,DIRECTORY+4u*16u,"hmtx",HMTX,8);
  put_table(data,DIRECTORY+5u*16u,"loca",LOCA,6);
  put_table(data,DIRECTORY+6u*16u,"maxp",MAXP,6);

  put_u16(data,CMAP,0);
  put_u16(data,CMAP+2,1);
  put_u16(data,CMAP+4,3);
  put_u16(data,CMAP+6,1);
  put_u32(data,CMAP+8,12);
  put_u16(data,CMAP+12,0);
  put_u16(data,CMAP+14,262);
  put_u16(data,CMAP+16,0);
  data[CMAP+18+65]=1;

  put_u16(data,GLYF,1);
  put_u16(data,GLYF+2,0);
  put_u16(data,GLYF+4,0);
  put_u16(data,GLYF+6,500);
  put_u16(data,GLYF+8,700);
  put_u16(data,GLYF+10,3);
  put_u16(data,GLYF+12,0);
  data[GLYF+14]=1;
  data[GLYF+15]=1;
  data[GLYF+16]=1;
  data[GLYF+17]=1;
  put_u16(data,GLYF+18,0);
  put_u16(data,GLYF+20,500);
  put_u16(data,GLYF+22,0);
  put_u16(data,GLYF+24,(unsigned)-500);
  put_u16(data,GLYF+26,0);
  put_u16(data,GLYF+28,0);
  put_u16(data,GLYF+30,700);
  put_u16(data,GLYF+32,0);

  put_u32(data,HEAD,0x00010000u);
  put_u32(data,HEAD+4,0x00010000u);
  put_u32(data,HEAD+12,0x5f0f3cf5u);
  put_u16(data,HEAD+18,1000);
  put_u16(data,HEAD+36,0);
  put_u16(data,HEAD+38,0);
  put_u16(data,HEAD+40,500);
  put_u16(data,HEAD+42,700);
  put_u16(data,HEAD+46,8);
  put_u16(data,HEAD+48,2);
  put_u16(data,HEAD+50,0);
  put_u16(data,HEAD+52,0);

  put_u32(data,HHEA,0x00010000u);
  put_u16(data,HHEA+4,800);
  put_u16(data,HHEA+6,(unsigned)-200);
  put_u16(data,HHEA+8,200);
  put_u16(data,HHEA+10,600);
  put_u16(data,HHEA+16,500);
  put_u16(data,HHEA+18,1);
  put_u16(data,HHEA+34,2);

  put_u16(data,HMTX,500);
  put_u16(data,HMTX+2,0);
  put_u16(data,HMTX+4,600);
  put_u16(data,HMTX+6,0);
  put_u16(data,LOCA,0);
  put_u16(data,LOCA+2,0);
  put_u16(data,LOCA+4,17);
  put_u32(data,MAXP,0x00010000u);
  put_u16(data,MAXP+4,2);
  return 552;
}

static uint64_t fnv64(const uint8_t *data, size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  for(size_t i=0;i<size;i++){
    hash^=data[i];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

int main(void){
  uint8_t font[552];
  size_t font_size=build_synthetic_font(font);
  GmlFontRasterFace *face=NULL;
  expect(gml_font_raster_face_open(font,font_size,"missing family",&face),
         "open synthetic face with first-face fallback");
  expect(face!=NULL,"open returns an owned face");
  memset(font,0,sizeof(font));

  float scale=gml_font_raster_scale_for_em(face,100.0f);
  expect(scale>0.0999f && scale<0.1001f,"em scale");
  GmlFontRasterMetrics metrics={0};
  expect(gml_font_raster_metrics(face,&metrics),"font metrics query");
  expect(metrics.ascent==800 && metrics.descent==-200 && metrics.line_gap==200,
         "font metrics values");
  expect(gml_font_raster_has_glyph(face,65),"mapped glyph");
  expect(!gml_font_raster_has_glyph(face,66),"unmapped glyph");

  GmlFontRasterGlyphMetrics glyph={0};
  expect(gml_font_raster_glyph_metrics(face,65,scale,&glyph),
         "glyph metrics query");
  expect(glyph.advance==600 && glyph.left_bearing==0 &&
         glyph.x0==0 && glyph.y0==-70 && glyph.x1==50 && glyph.y1==0,
         "glyph metrics values");

  uint8_t coverage[50u*70u];
  memset(coverage,0,sizeof(coverage));
  expect(!gml_font_raster_render_glyph(face,65,scale,coverage,
                                        sizeof(coverage)-1,50,70,50),
         "coverage size bound");
  expect(!gml_font_raster_render_glyph(face,65,scale,coverage,
                                        sizeof(coverage),50,70,49),
         "coverage stride bound");
  expect(gml_font_raster_render_glyph(face,65,scale,coverage,
                                       sizeof(coverage),50,70,50),
         "glyph rasterization");
  uint64_t hash=fnv64(coverage,sizeof(coverage));
  expect(hash==UINT64_C(0x8783b5265d0c96c7),
         "exact synthetic glyph coverage");

  gml_font_raster_face_close(face);
  face=(GmlFontRasterFace*)(uintptr_t)1;
  expect(!gml_font_raster_face_open(font,sizeof(font),NULL,&face) && face==NULL,
         "malformed face is rejected transactionally");
  expect(!gml_font_raster_face_open(NULL,0,NULL,&face),
         "empty face is rejected");
  gml_font_raster_face_close(NULL);

  if(failures) return 1;
  puts("font raster tests: ok");
  return 0;
}
