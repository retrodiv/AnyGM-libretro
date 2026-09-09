/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_font_raster.h"
#include "font_test_fixture.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int condition, const char *message){
  if(condition) return;
  fprintf(stderr,"font raster test failed: %s\n",message);
  failures++;
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
  size_t font_size=font_fixture_build(font);
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
