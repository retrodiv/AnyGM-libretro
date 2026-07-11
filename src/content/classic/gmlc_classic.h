/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#ifndef GMLC_CLASSIC_H
#define GMLC_CLASSIC_H

#include <stddef.h>
#include <stdint.h>

#define GMLC_CLASSIC_MAGIC 1234321u

typedef enum {
  GMLC_CLASSIC_UNKNOWN = 0,
  GMLC_CLASSIC_GM6 = 600,
  GMLC_CLASSIC_GM7 = 701,
  GMLC_CLASSIC_GM7_ALT = 702,
  GMLC_CLASSIC_GM8 = 800,
  GMLC_CLASSIC_GM81 = 810
} GmlcClassicVersion;

typedef struct {
  GmlcClassicVersion version;
  uint32_t game_id;
  uint8_t guid[16];
} GmlcClassicHeader;

/* Inspect only the unencrypted common project header. This deliberately does
 * not claim that the rest of the project is loadable. */
int gmlc_classic_probe(const void *data, size_t size, GmlcClassicHeader *out,
                       char *err, size_t errcap);
int gmlc_classic_probe_file(const char *path, GmlcClassicHeader *out,
                            char *err, size_t errcap);
const char *gmlc_classic_version_name(GmlcClassicVersion version);

#endif
