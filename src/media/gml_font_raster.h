/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_FONT_RASTER_H
#define GML_FONT_RASTER_H

#include <stddef.h>
#include <stdint.h>

typedef struct GmlFontRasterFace GmlFontRasterFace;

typedef struct {
  int ascent;
  int descent;
  int line_gap;
} GmlFontRasterMetrics;

typedef struct {
  int advance;
  int left_bearing;
  int x0;
  int y0;
  int x1;
  int y1;
} GmlFontRasterGlyphMetrics;

/* The face owns an internal copy of font_bytes. A non-empty family_name asks
 * a collection for that family and falls back to its first face. */
int gml_font_raster_face_open(const uint8_t *font_bytes, size_t font_size,
                              const char *family_name,
                              GmlFontRasterFace **out_face);
void gml_font_raster_face_close(GmlFontRasterFace *face);
/* Retain the immutable face for another owner in the same serialized engine operation. */
int gml_font_raster_face_retain(GmlFontRasterFace *face);

float gml_font_raster_scale_for_em(const GmlFontRasterFace *face,
                                   float pixel_height);
int gml_font_raster_metrics(const GmlFontRasterFace *face,
                            GmlFontRasterMetrics *out_metrics);
int gml_font_raster_has_glyph(const GmlFontRasterFace *face, int codepoint);
int gml_font_raster_glyph_metrics(const GmlFontRasterFace *face,
                                  int codepoint, float scale,
                                  GmlFontRasterGlyphMetrics *out_metrics);
int gml_font_raster_render_glyph(const GmlFontRasterFace *face,
                                 int codepoint, float scale,
                                 uint8_t *coverage, size_t coverage_size,
                                 int width, int height, int stride);

#endif
