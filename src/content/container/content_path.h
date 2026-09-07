/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CONTENT_PATH_H
#define ANYGM_CONTENT_PATH_H

#include <stddef.h>

#define ANYGM_CONTENT_MAX_MEMBER_PATH 511u

/* Normalize an explicitly sized, writable relative member name in place.
 * The caller supplies raw_size + 1 bytes. No absolute path, drive, control
 * character, empty interior segment, dot or parent segment is accepted.
 * A trailing slash is preserved; a file-only caller must reject it. */
int anygm_content_member_normalize(char *name, size_t raw_size);

#endif
