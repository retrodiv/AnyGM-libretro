/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_PIPELINE_H
#define ANYGM_CONTENT_PIPELINE_H

#include <stddef.h>
#include <stdint.h>

/* Policy-free, explicitly selected buffer operations. No source probing or I/O.
 * Inflaters require an explicit output capacity in the step declaration. */
int anygm_content_pipeline_builtin_valid(const char *step);
int anygm_content_pipeline_builtin_run(const char *step,const void *input,size_t size,
                                        uint8_t **output,size_t *output_size);

#endif
