/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_EMBEDDED_CAB_H
#define ANYGM_EMBEDDED_CAB_H

#include <stddef.h>
#include <stdint.h>

#define ANYGM_EMBEDDED_CAB_MARKER_MAX_BYTES (20u*1024u*1024u)
#define ANYGM_EMBEDDED_CAB_NAME_MAX_PROBES 64u

struct AnygmContentRouter;

typedef enum AnygmEmbeddedCabStatus {
  ANYGM_EMBEDDED_CAB_NOT_FOUND=0,
  ANYGM_EMBEDDED_CAB_SUPPORTED=1,
  ANYGM_EMBEDDED_CAB_UNSUPPORTED=2,
  ANYGM_EMBEDDED_CAB_INVALID=3
} AnygmEmbeddedCabStatus;

typedef struct AnygmEmbeddedCab {
  uint64_t source_size;
  uint64_t source_hash;
  uint64_t offset;
  uint64_t size;
  uint32_t profile;
} AnygmEmbeddedCab;

typedef enum AnygmEmbeddedCabEntryKind {
  ANYGM_EMBEDDED_CAB_ENTRY_REGULAR=1,
  ANYGM_EMBEDDED_CAB_ENTRY_DIRECTORY=2,
  ANYGM_EMBEDDED_CAB_ENTRY_SYMLINK=3,
  ANYGM_EMBEDDED_CAB_ENTRY_SPECIAL=4
} AnygmEmbeddedCabEntryKind;

typedef struct AnygmEmbeddedCabNameMetrics {
  size_t inserted;
  size_t hits;
  uint64_t probes;
  uint64_t equality_bytes;
  int rejected;
  int work_exhausted;
} AnygmEmbeddedCabNameMetrics;

AnygmEmbeddedCabStatus anygm_embedded_cab_probe(const struct AnygmContentRouter *router,
                                                 const char *path,
                                                 AnygmEmbeddedCab *cab);
int anygm_embedded_cab_extract(const struct AnygmContentRouter *router,const char *source_path,
                               const AnygmEmbeddedCab *cab,char *payload_path,
                               size_t payload_path_size,char *asset_root,size_t asset_root_size);
/* Shared metadata policy seam for synthetic link/special-file injection. */
int anygm_embedded_cab_entry_allowed(AnygmEmbeddedCabEntryKind kind,
                                     int has_hardlink,int has_symlink);
int anygm_embedded_cab_limits_allowed(uint64_t cabinet_size,unsigned entries,
                                      uint64_t member_size,uint64_t total_size);
/* Checked serialization budget shared by the marker writer and synthetic boundary tests. */
int anygm_embedded_cab_marker_budget_allowed(size_t payload_path_size,unsigned entries,
                                              size_t member_path_bytes,size_t *budget_size);
/* Narrow production-operation seam for deterministic bounded-work security tests. */
int anygm_embedded_cab_name_index_measure(const char *const *insert_paths,size_t insert_count,
                                           const char *const *lookup_paths,size_t lookup_count,
                                           int force_collisions,
                                           AnygmEmbeddedCabNameMetrics *metrics);

#endif
