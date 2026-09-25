/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SURFACES_INTERNAL_H
#define GML_RENDER_SURFACES_INTERNAL_H

#include "gml_render.h"

/* Stable bounded surface ID to pool slot; used only at coarse API boundaries. */
int surface_slot(int id);
int gml_render_surface_draw_targets_screen_raster(const GmlRender *render, int surface,
                                                   double width, double height);

#endif
