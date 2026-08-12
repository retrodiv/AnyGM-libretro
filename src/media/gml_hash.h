/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_HASH_H
#define GML_HASH_H

#include <stddef.h>
#include <stdint.h>

void gml_sha256(const void *data,size_t size,uint8_t digest[32]);

#endif
