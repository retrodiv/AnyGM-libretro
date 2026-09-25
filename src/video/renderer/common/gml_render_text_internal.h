/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_TEXT_INTERNAL_H
#define GML_RENDER_TEXT_INTERNAL_H

/* Include after gml_render_internal.h. These helpers retain one implementation. */
int font_kerning(const GmlFont *font,unsigned left,unsigned right);
int glyph_w(GmlRender *render,GmlFont *font,int frame,unsigned codepoint);
int glyph_frame(GmlFont *font,unsigned codepoint);
unsigned utf8_next(const char **text);
GmlGlyph *real_glyph(GmlFont *font,unsigned codepoint);
GmlGlyph *real_glyph_demand(GmlRender *render,GmlFont *font,unsigned codepoint);

#endif
