/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_WWISE_H
#define GML_WWISE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int gml_wwise_wem_to_ogg(const uint8_t *wem,size_t wem_size,
                         uint8_t **ogg,size_t *ogg_size);

#ifdef __cplusplus
}
#endif

#endif
