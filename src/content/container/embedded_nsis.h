/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_EMBEDDED_NSIS_H
#define ANYGM_EMBEDDED_NSIS_H

#include <stddef.h>
#include <stdint.h>

struct AnygmContentRouter;

typedef enum AnygmEmbeddedNsisStatus {
  ANYGM_EMBEDDED_NSIS_NOT_FOUND=0,
  ANYGM_EMBEDDED_NSIS_SUPPORTED=1,
  ANYGM_EMBEDDED_NSIS_UNSUPPORTED=2,
  ANYGM_EMBEDDED_NSIS_INVALID=3
} AnygmEmbeddedNsisStatus;

typedef struct AnygmEmbeddedNsis {
  uint64_t source_size;
  uint64_t source_hash;
  uint64_t header_offset;
  uint64_t archive_size;
  uint64_t data_offset;
  uint64_t data_size;
  uint32_t header_size;
  uint32_t header_packed_size;
  uint32_t flags;
} AnygmEmbeddedNsis;

AnygmEmbeddedNsisStatus anygm_embedded_nsis_probe(const struct AnygmContentRouter *router,
                                                   const char *path,
                                                   AnygmEmbeddedNsis *nsis);
int anygm_embedded_nsis_extract(const struct AnygmContentRouter *router,const char *source_path,
                                const AnygmEmbeddedNsis *nsis,char *payload_path,
                                size_t payload_path_size,char *asset_root,
                                size_t asset_root_size);

#endif
