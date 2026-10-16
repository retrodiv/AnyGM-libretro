/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SHADERS_H
#define GML_RENDER_SHADERS_H

#include "gml_render.h"
#include "gml_render_plan.h"

/* Snapshot draw-time content-program inputs; sampler image references are resolved by the caller
 * against its own resource owner before submission. */
int gml_render_shader_plan_snapshot(const GmlRender *render,int shader_id,
                                    uint32_t content_generation,const float colour[4],
                                    GmlPlanShader *out);

/* The existing recognized byte-colour evaluator, without executing content GLSL. */
int gml_render_shader_threshold_palette_snapshot(const GmlRender *render,int shader_id,
                                                GmlPlanShader *out);

/* Emit the existing optional per-shader accounting during renderer teardown. */
void gml_render_shader_account_report(GmlRender *render);

#endif
