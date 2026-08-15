/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
// A Linux/aarch64 cross toolchain built against a newer glibc can bind
// sqrtf/atan2f to a symbol version the target device's older glibc does not
// carry, even though the target's own glibc implements both functions fine.
// Pin both references to the oldest widely available version (present since
// aarch64's glibc baseline) so a core built on a newer host still loads on
// an older aarch64 target. Paired with the --wrap=sqrtf,--wrap=atan2f link
// flags applied only for aarch64-linux cross builds; unused and inert
// everywhere else.

#if defined(__linux__) && defined(__aarch64__) && !defined(__ANDROID__)

#include "anygm_libm_compat.h"

__asm__(".symver anygm_libm_sqrtf_v217,sqrtf@GLIBC_2.17");

float __wrap_sqrtf(float x) {
    return anygm_libm_sqrtf_v217(x);
}

__asm__(".symver anygm_libm_atan2f_v217,atan2f@GLIBC_2.17");

float __wrap_atan2f(float y, float x) {
    return anygm_libm_atan2f_v217(y, x);
}

#endif
