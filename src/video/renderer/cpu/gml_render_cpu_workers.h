/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_WORKERS_H
#define GML_RENDER_CPU_WORKERS_H

#include "gml_render.h"

/* Stop and join the instance-owned row pool before renderer workspaces are freed. */
void gml_row_pool_free(GmlRender *render);

#endif
