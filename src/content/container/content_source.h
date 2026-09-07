/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_SOURCE_H
#define ANYGM_CONTENT_SOURCE_H

#include "content_transform.h"

#define ANYGM_SOURCE_MAX_CANDIDATES 64u
#define ANYGM_SOURCE_CANDIDATE_BYTES 80u

/* A structural validator borrows a complete candidate, never publishes it, and
 * returns positive for valid, zero for invalid, negative for a fatal failure.
 * This callback is supplied by compiled readers, never exposed to byte programs. */
typedef int (*AnygmContentSourceValidator)(void *context,const void *data,size_t size,
                                           char *error,size_t error_size);

int anygm_content_source_configured(const AnygmContentTransforms *transforms);
/* Prepare a single explicitly selected input, or enumerate ranges with input.probe
 * and select exactly one completely valid input result. The probe returns at most
 * 64 records: little-endian u64 offset, u64 length, then a zero-padded 64-byte
 * operation name (empty selects input). Offsets must not decrease. Repeated ranges
 * with different operations are alternatives; exact duplicate records reject. The
 * entire table is checked before normalization/validation; overlapping ranges are
 * independent immutable inputs. A zero-record probe declines selection.
 * An optional input.final pipeline then consumes the selected representation, or
 * the original bytes after an identity/zero-record decline. It runs exactly once
 * after selection, independently of candidate operation names. Its result still
 * needs the caller's ordinary structural parser; a failed final stage publishes
 * no selected or intermediate bytes.
 *
 * With no probe, input has its ordinary single-result contract; validation belongs
 * to the caller's next reader. With a probe, a validator is required and every
 * selected operation must resolve before any candidate executes.
 * A sole whole-source identity result declines adaptation before validation so
 * ordinary container/member selection can proceed. No second input pass occurs.
 * Success with NULL/0 means borrow the original. Other success returns owned bytes.
 * Every failure clears output to NULL/0, including ambiguous candidate results. */
int anygm_content_source_prepare(const AnygmContentTransforms *transforms,
                                  const void *data,size_t size,
                                  AnygmContentSourceValidator validate,void *context,
                                  uint8_t **output,size_t *output_size,
                                  char *error,size_t error_size);

#endif
