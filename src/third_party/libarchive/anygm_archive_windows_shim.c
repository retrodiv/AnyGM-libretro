/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Minimal native-Windows boundary for the retained libarchive CAB reader.
 * archive_windows.c is intentionally not retained because its pathname and
 * process helpers are outside the callback-only read closure.
 */
#if defined(_WIN32) && !defined(__CYGWIN__)

#include <io.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

typedef intptr_t anygm_archive_ssize_t;

anygm_archive_ssize_t
__la_write(int fd, const void *buffer, size_t size)
{
	unsigned int bounded = size > UINT_MAX ? UINT_MAX : (unsigned int)size;
	return (anygm_archive_ssize_t)_write(fd, buffer, bounded);
}

#endif
