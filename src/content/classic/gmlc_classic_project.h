/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_CLASSIC_PROJECT_H
#define GMLC_CLASSIC_PROJECT_H

#include "gmlc_project.h"
#include "gmlc_classic.h"
#include <stddef.h>

/* Import an already normalized file. Source adapters belong to the caller's
 * preparation stage; this importer uses transforms only for nested records. */
int gmlc_classic_project_load(const AnygmContentTransforms *transforms,GmlcProject *project,const AnygmHostServices *host,
                              const char *project_path,
                              const char *cache_dir, char *err, size_t errcap);
/* Parse a supplied normalized image while retaining the original path, bytes
 * and sidecars as the source identity and resource root. NULL image uses the
 * original file bytes. Neither input nor original file is modified. */
int gmlc_classic_project_load_image(const AnygmContentTransforms *transforms,GmlcProject *project,
                                    const AnygmHostServices *host,const char *project_path,
                                    const void *image,size_t image_size,const char *cache_dir,
                                    char *err,size_t errcap);
int gmlc_classic_included_dependency_hash(const AnygmContentTransforms *transforms,const AnygmHostServices *host,
                                          const char *project_path,
                                          uint64_t seed,uint64_t *hash_out);

#endif
