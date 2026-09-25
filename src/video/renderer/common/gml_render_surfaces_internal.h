/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_SURFACES_INTERNAL_H
#define GML_RENDER_SURFACES_INTERNAL_H

/* Stable bounded surface ID to pool slot; used only at coarse API boundaries. */
int surface_slot(int id);

#endif
