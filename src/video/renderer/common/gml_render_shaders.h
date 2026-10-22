/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SHADERS_H
#define GML_RENDER_SHADERS_H

#include "gml_render.h"
#include "gml_render_plan.h"
#include "gml_render_resources.h"

/* Snapshot draw-time content-program inputs; sampler image references are resolved by the caller
 * against its own resource owner before submission. */
int gml_render_shader_plan_snapshot(const GmlRender *render,int shader_id,
                                    uint32_t content_generation,const float colour[4],
                                    GmlPlanShader *out);

/* Recognized threshold/indexed byte palettes, without executing content GLSL. */
int gml_render_shader_threshold_palette_snapshot(const GmlRender *render,int shader_id,
                                                GmlPlanShader *out);
/* Same finite evaluator for batched textured checkpoints. Source alpha and
 * geometry are unchanged;no source or destination pixels are observed. */
int gml_render_shader_source_palette_snapshot(const GmlRender *render,int shader_id,
                                              GmlRenderSourcePalette *out);
/* Resolve the recognized red-index palette against its existing immutable texture owner. */
int gml_render_shader_lut_snapshot(GmlRender *render,int shader_id,GmlPlanShader *out,
                                    GmlRenderResourceSource *palette);

/* Whether the selected program reaches the CPU's unchanged ordinary draw.
 * This is not evidence that the authored program itself has no effect. */
int gml_render_shader_cpu_passthrough(const GmlRender *render);

/* Snapshot the sole recognized per-source-channel alpha predicate as a byte
 * partition. This does not evaluate pixels or change readable shader state. */
int gml_render_shader_channel_alpha_key(const GmlRender *render,uint32_t *key);

/* Emit the existing optional per-shader accounting during renderer teardown. */
void gml_render_shader_account_report(GmlRender *render);

#endif
