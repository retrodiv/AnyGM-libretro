/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_BLIT_H
#define GML_RENDER_CPU_BLIT_H

#include "gml_render.h"

typedef struct GmlTpag GmlTpag;

/* One resolved background/tile source emitted into the active CPU target. */
void gml_render_cpu_blit_background_phase(GmlRender *r, GmlTpag *t, double dx, double dy,
                                          double xs, double ys, uint32_t blend, double alpha);

#endif
