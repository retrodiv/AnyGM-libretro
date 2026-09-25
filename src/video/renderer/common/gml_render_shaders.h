/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SHADERS_H
#define GML_RENDER_SHADERS_H

#include "gml_render.h"

/* Emit the existing optional per-shader accounting during renderer teardown. */
void gml_render_shader_account_report(GmlRender *render);

#endif
