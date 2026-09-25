/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_REPLAY_H
#define GML_RENDER_CPU_REPLAY_H

#include "gml_render_resources.h"

/* A deliberately narrow borrowed view of resolved work. The resource owner retains the records
 * and source versions; this CPU executor owns no logical resource, device object or journal. */
int gml_render_cpu_replay_eligible(const GmlRenderDrawOp *operation,
                                    const GmlRenderDrawStateSnapshot *state,
                                    const GmlRenderResourceSource *source);
int gml_render_cpu_replay_one(uint32_t *target,uint32_t width,uint32_t height,
                               const GmlRenderDrawOp *operation,
                               const GmlRenderDrawStateSnapshot *state,
                               const GmlRenderResourceSource *source);

#endif
