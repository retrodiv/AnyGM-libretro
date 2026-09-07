/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_DELTA_H
#define ANYGM_CONTENT_DELTA_H

#include <stddef.h>
#include <stdint.h>

#define ANYGM_CONTENT_DELTA_MAX_BYTES ((size_t)1024 * 1024 * 1024)
#define ANYGM_CONTENT_DELTA_MAX_WORKSPACE ((size_t)256 * 1024 * 1024)

/* Decode an xdelta3 VCDIFF patch against an immutable source. The caller owns
 * identity checks and supplies the exact expected output size. All decoder
 * allocations, including secondary compression, share one workspace budget;
 * the explicit input and result buffers are separate from that budget.
 * Success transfers one malloc-owned result. Failure publishes no bytes.
 * This boundary has no host, path, process or network capability. */
int anygm_content_delta_apply(const uint8_t *source, size_t source_size,
                              const uint8_t *patch, size_t patch_size,
                              size_t result_size, size_t workspace_limit,
                              uint8_t **result, char *error, size_t error_size);

#endif
