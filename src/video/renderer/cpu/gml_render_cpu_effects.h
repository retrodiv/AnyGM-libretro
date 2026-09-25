/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_EFFECTS_H
#define GML_RENDER_CPU_EFFECTS_H

#include "gml_render.h"

/* One pass-level boundary; pixel helpers remain private to the CPU owner. */
void gml_render_cpu_layer_filter_end(GmlRender *render,
                                     const GmlLayerFilter *filter,double time_seconds);

#endif
