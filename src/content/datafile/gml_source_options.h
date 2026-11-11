/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_SOURCE_OPTIONS_H
#define GML_SOURCE_OPTIONS_H

/* Explicit source settings are independent of the normalized instruction encoding. */
typedef enum GmlSourceArrayCopy {
  GML_SOURCE_ARRAY_COPY_UNSPECIFIED=0,
  GML_SOURCE_ARRAY_COPY_ON_WRITE=1,
  GML_SOURCE_ARRAY_COPY_REFERENCE=2
} GmlSourceArrayCopy;

#endif
