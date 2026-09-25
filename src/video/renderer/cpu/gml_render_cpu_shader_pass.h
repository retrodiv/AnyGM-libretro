/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_CPU_SHADER_PASS_H
#define GML_RENDER_CPU_SHADER_PASS_H

#include <stdint.h>

typedef struct GmlRender GmlRender;

/* One coarse host-program request and composition, never a per-pixel executor entry. */
int gml_render_cpu_shader_pass_surface(GmlRender *r,int surf,const uint32_t *spx,int sw,int sh,
                                       double rx,double ry,double rw,double rh,
                                       double dx,double dy,double dw,double dh,
                                       uint32_t blend,double alpha);

#endif
