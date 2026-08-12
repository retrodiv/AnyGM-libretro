/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_PXTONE_H
#define GML_PXTONE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode one bounded PTCOP/PTTUNE image to 44.1 kHz stereo PCM. The caller owns *pcm. */
int gml_pxtone_render(const uint8_t *data,size_t size,int16_t **pcm,
                      uint32_t *frames,uint32_t *loop_start_frame);

#ifdef __cplusplus
}
#endif
#endif
