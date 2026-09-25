/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SPRITES_INTERNAL_H
#define GML_RENDER_SPRITES_INTERNAL_H

#include "gml_render.h"

int gml_render_sprite_log_enabled(const GmlRender *render);
void gml_render_sprite_tiled_ext_unmasked(GmlRender *render, int sprite, int subimage,
                                           double x, double y, double xscale, double yscale,
                                           uint32_t blend, double alpha);

#endif
