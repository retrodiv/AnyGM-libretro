/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_CLASSIC_FIDELITY_H
#define GMLC_CLASSIC_FIDELITY_H

#include "anygm_host.h"
#include "gmlc_classic.h"

#define GMLC_CLASSIC_FIDELITY_SUFFIX ".fidelity.patch"
#define GMLC_CLASSIC_FIDELITY_FILE_LIMIT GMLC_CLASSIC_FILE_LIMIT

enum {
  GMLC_CLASSIC_FIDELITY_VERSION=2,
  GMLC_CLASSIC_FIDELITY_HEADER_SIZE=64,
  GMLC_CLASSIC_FIDELITY_EXECUTABLE_LAYOUT=1u,
  GMLC_CLASSIC_FIDELITY_SETTINGS=2u,
  GMLC_CLASSIC_FIDELITY_GAME_INFORMATION=4u,
  GMLC_CLASSIC_FIDELITY_REPLACE_PAYLOAD=1u,
  GMLC_CLASSIC_FIDELITY_REPLACE_SOURCE=2u,
  GMLC_CLASSIC_FIDELITY_SLOT_EXECUTABLE_LAYOUT=4u,
  GMLC_CLASSIC_FIDELITY_SLOT_LEGACY_LAYOUT=8u,
  GMLC_CLASSIC_FIDELITY_PAYLOAD_BZIP2=16u,
  GMLC_CLASSIC_FIDELITY_PAYLOAD_DELTA=32u
};

int gmlc_classic_fidelity_apply(GmlcClassicManifest *manifest,
                                const AnygmHostServices *host,
                                const char *project_path,
                                const void *project_data,size_t project_size,
                                char *err,size_t errcap);
int gmlc_classic_fidelity_apply_data(GmlcClassicManifest *manifest,
                                     const void *companion_data,size_t companion_size,
                                     const void *project_data,size_t project_size,
                                     char *err,size_t errcap);
int gmlc_classic_fidelity_dependency_hash(const AnygmHostServices *host,
                                          const char *project_path,
                                          uint64_t seed,uint64_t *hash_out);

#endif
