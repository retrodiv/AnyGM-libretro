/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_TRANSFORM_SOURCE_H
#define ANYGM_CONTENT_TRANSFORM_SOURCE_H

#include <stddef.h>
#include <stdint.h>

/* Compile one bounded C-like function into the existing buffer instruction set.
 * consumed ends immediately after its closing brace. No host services are used. */
int anygm_content_transform_compile(const char *source,size_t size,size_t *consumed,
    uint8_t **program,size_t *program_size,uint8_t **parameters,size_t *parameter_size,
    char *error,size_t error_size);

#endif
