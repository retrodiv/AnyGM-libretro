/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#ifndef ANYGM_LIBM_COMPAT_H
#define ANYGM_LIBM_COMPAT_H

/* Version-pinned aliases of the two libm entry points a Linux/aarch64 cross build wraps. Each
 * name is bound to its GLIBC_2.17 revision by a .symver directive in the implementation, so it
 * exists only there; declaring the pair here keeps the wrappers' references owned by a header
 * rather than by a local extern. Nothing outside that translation unit calls them. */
float anygm_libm_sqrtf_v217(float x);
float anygm_libm_atan2f_v217(float y,float x);

/* The wrappers the aarch64 Linux/glibc link selects with --wrap=sqrtf,--wrap=atan2f. */
float __wrap_sqrtf(float x);
float __wrap_atan2f(float y,float x);

#endif
