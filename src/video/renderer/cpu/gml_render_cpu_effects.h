/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_EFFECTS_H
#define GML_RENDER_CPU_EFFECTS_H

#include "gml_render.h"

/* The shared pass chooses its executor after restoring the captured target.
 * CPU pixel kernels remain private to this owner. */
void gml_render_cpu_layer_filter_restore_target(GmlRender *render);
void gml_render_cpu_layer_filter_end(GmlRender *render,
                                     const GmlLayerFilter *filter,double time_seconds,
                                     int width,int height,double camera_x,double camera_y,
                                     int source_empty);

#endif
