/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_LZMA_CONFIG_H
#define ANYGM_LZMA_CONFIG_H

/* Portable decoder closure: no host probes, threading, assembler, encoders or
 * runtime CPU dispatch. CRC tables are immutable upstream constants. */
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDBOOL_H 1
#define HAVE_DECODERS 1
#define HAVE_DECODER_LZMA1 1
#define HAVE_DECODER_LZMA2 1
#define HAVE_CHECK_CRC32 1
#define HAVE_CHECK_CRC64 1
#define HAVE_CHECK_SHA256 1
#define TUKLIB_SYMBOL_PREFIX lzma_
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define WORDS_BIGENDIAN 1
#elif !defined(__BYTE_ORDER__) && !defined(_WIN32)
#error "The target must declare its byte order"
#endif

#endif
