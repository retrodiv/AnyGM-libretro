/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Portable configuration boundary for the retained libarchive CAB reader.
 * It deliberately exposes only C library facilities used by the retained
 * read-only closure and enables no optional compression or crypto library.
 */
#ifndef ANYGM_LIBARCHIVE_CONFIG_H
#define ANYGM_LIBARCHIVE_CONFIG_H

#define __LIBARCHIVE_CONFIG_H_INCLUDED 1

#define HAVE_CTYPE_H 1
#define HAVE_ERRNO_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1

#define HAVE_DECL_INT32_MAX 1
#define HAVE_DECL_INT32_MIN 1
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_INTMAX_MAX 1
#define HAVE_DECL_INTMAX_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_SSIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_EILSEQ 1
#define HAVE_MBRTOWC 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRNLEN 1
#define HAVE_STRRCHR 1
#define HAVE_WCRTOMB 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WMEMCMP 1
#define HAVE_WMEMCPY 1
#define HAVE_WMEMMOVE 1

#if defined(_WIN32)
#define gid_t short
#define uid_t short
#define SIZEOF_WCHAR_T 2
#else
#define HAVE_LANGINFO_H 1
#define HAVE_NL_LANGINFO 1
#define HAVE_UNISTD_H 1
#define SIZEOF_WCHAR_T 4
#endif

#endif
