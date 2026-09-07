/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_TEST_VCDIFF_FIXTURE_H
#define ANYGM_TEST_VCDIFF_FIXTURE_H
#include <stddef.h>
#include <stdint.h>

/* Authored RFC 3284 ADD-only window for small synthetic payloads. */
uint8_t *anygm_test_vcdiff_literal(const void *target,size_t size,size_t *patch_size);
void anygm_test_sha256_hex(const void *data,size_t size,char output[65]);
#endif
