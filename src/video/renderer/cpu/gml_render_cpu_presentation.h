/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_PRESENTATION_H
#define GML_RENDER_CPU_PRESENTATION_H

#include "gml_render.h"

/* Pass-level materialization of the recorded presentation; pixel mapping stays inline. */
void gml_render_cpu_present_image(GmlRender *r);

#endif
