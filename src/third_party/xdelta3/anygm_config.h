/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_XDELTA_CONFIG_H
#define ANYGM_XDELTA_CONFIG_H

#include <stdint.h>
#include <limits.h>

/* One portable, decoder-only configuration shared by the library and its owner.
 * No command-line tool, automatic file decompressor or encoder is compiled. */
#define XD3_ENCODER 0
#define XD3_MAIN 0
#define VCDIFF_TOOLS 0
#define REGRESSION_TEST 0
#define XD3_DEBUG 0
#define SECONDARY_DJW 1
#define SECONDARY_FGK 1
#define SECONDARY_LZMA 1
#define XD3_USE_LARGESIZET 0
#define SIZEOF_UNSIGNED_INT 4
#define SIZEOF_UNSIGNED_LONG_LONG 8
#if SIZE_MAX == UINT64_MAX
#define SIZEOF_SIZE_T 8
#else
#define SIZEOF_SIZE_T 4
#endif
#if ULONG_MAX == UINT64_MAX
#define SIZEOF_UNSIGNED_LONG 8
#else
#define SIZEOF_UNSIGNED_LONG 4
#endif
#if defined(_WIN32) && !defined(WINVER)
#ifdef _WIN32_WINNT
#define WINVER _WIN32_WINNT
#else
#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#endif
#endif

#define XD3_LZMA_MEMLIMIT (UINT64_C(256) * 1024 * 1024)

#endif
