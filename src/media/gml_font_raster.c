/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_font_raster.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct GmlFontRasterFace {
  uint8_t *bytes;
  size_t size;
  stbtt_fontinfo info;
};

static int gml_font_raster_sfnt_magic(const uint8_t *data, size_t size,
                                      int offset){
  if(!data || offset<0 || (size_t)offset>size || size-(size_t)offset<12) return 0;
  data+=(size_t)offset;
  return (data[0]==0 && data[1]==1 && data[2]==0 && data[3]==0) ||
         !memcmp(data,"true",4) || !memcmp(data,"typ1",4) ||
         !memcmp(data,"OTTO",4);
}

int gml_font_raster_face_open(const uint8_t *font_bytes, size_t font_size,
                              const char *family_name,
                              GmlFontRasterFace **out_face){
  enum { FONT_GUARD_BYTES=16 };
  if(out_face) *out_face=NULL;
  if(!out_face || !font_bytes || font_size<12 || font_size>(size_t)INT_MAX ||
     font_size>SIZE_MAX-FONT_GUARD_BYTES) return 0;

  GmlFontRasterFace *face=(GmlFontRasterFace*)calloc(1,sizeof(*face));
  if(!face) return 0;
  face->bytes=(uint8_t*)calloc(font_size+FONT_GUARD_BYTES,1);
  if(!face->bytes){
    free(face);
    return 0;
  }
  memcpy(face->bytes,font_bytes,font_size);
  face->size=font_size;

  int offset=-1;
  if(family_name && *family_name)
    offset=stbtt_FindMatchingFont(face->bytes,family_name,STBTT_MACSTYLE_DONTCARE);
  if(offset<0) offset=stbtt_GetFontOffsetForIndex(face->bytes,0);
  if(!gml_font_raster_sfnt_magic(face->bytes,font_size,offset) ||
     !stbtt_InitFont(&face->info,face->bytes,offset)){
    gml_font_raster_face_close(face);
    return 0;
  }

  *out_face=face;
  return 1;
}

void gml_font_raster_face_close(GmlFontRasterFace *face){
  if(!face) return;
  free(face->bytes);
  face->bytes=NULL;
  face->size=0;
  free(face);
}

float gml_font_raster_scale_for_em(const GmlFontRasterFace *face,
                                   float pixel_height){
  if(!face || !(pixel_height>0.0f)) return 0.0f;
  return stbtt_ScaleForMappingEmToPixels(&face->info,pixel_height);
}

int gml_font_raster_metrics(const GmlFontRasterFace *face,
                            GmlFontRasterMetrics *out_metrics){
  if(!face || !out_metrics) return 0;
  stbtt_GetFontVMetrics(&face->info,&out_metrics->ascent,
                        &out_metrics->descent,&out_metrics->line_gap);
  return 1;
}

int gml_font_raster_has_glyph(const GmlFontRasterFace *face, int codepoint){
  return face && codepoint>=0 &&
         stbtt_FindGlyphIndex(&face->info,codepoint)!=0;
}

int gml_font_raster_glyph_metrics(const GmlFontRasterFace *face,
                                  int codepoint, float scale,
                                  GmlFontRasterGlyphMetrics *out_metrics){
  if(!face || codepoint<0 || !(scale>0.0f) || !out_metrics) return 0;
  stbtt_GetCodepointHMetrics(&face->info,codepoint,&out_metrics->advance,
                             &out_metrics->left_bearing);
  stbtt_GetCodepointBitmapBox(&face->info,codepoint,scale,scale,
                              &out_metrics->x0,&out_metrics->y0,
                              &out_metrics->x1,&out_metrics->y1);
  return 1;
}

int gml_font_raster_render_glyph(const GmlFontRasterFace *face,
                                 int codepoint, float scale,
                                 uint8_t *coverage, size_t coverage_size,
                                 int width, int height, int stride){
  if(!face || codepoint<0 || !(scale>0.0f) || !coverage ||
     width<=0 || height<=0 || stride<width ||
     (size_t)height>SIZE_MAX/(size_t)stride ||
     coverage_size<(size_t)height*(size_t)stride) return 0;
  stbtt_MakeCodepointBitmap(&face->info,coverage,width,height,stride,
                            scale,scale,codepoint);
  return 1;
}
