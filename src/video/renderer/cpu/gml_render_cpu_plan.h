/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_PLAN_H
#define GML_RENDER_CPU_PLAN_H

#include "gml_render_plan.h"

/* Exact software execution into a caller-owned XRGB8888 target. Returns zero without writing
 * anything when the plan does not validate. */
int gml_render_cpu_plan_execute(const GmlRenderPlan *plan,uint32_t *target,
                                uint32_t target_pitch_pixels);

/* The same execution, with row-independent full-frame operations split across the renderer's
 * row-band pool when one is offered and the destination is large enough to pay for the fork.
 * The renderer supplies the pool and optional transient execution accounting; NULL executes on
 * the calling thread and is exactly the function above. Identical bytes either way, at any band
 * count. */
struct GmlRender;
int gml_render_cpu_plan_execute_pooled(const GmlRenderPlan *plan,uint32_t *target,
                                       uint32_t target_pitch_pixels,
                                       struct GmlRender *pool_owner);

#endif
