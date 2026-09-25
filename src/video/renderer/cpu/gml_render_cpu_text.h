/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_TEXT_H
#define GML_RENDER_CPU_TEXT_H

/* Include after gml_render_internal.h. One coarse call per classic glyph. */
int gml_render_cpu_classic_info_subpixel_glyph(GmlRender *r,GmlFont *font,GmlGlyph *glyph,
                                                double x,double y,double xscale,double yscale,
                                                uint32_t color,double alpha);

#endif
