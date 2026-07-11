/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#ifndef GMLC_CLASSIC_IMPORT_H
#define GMLC_CLASSIC_IMPORT_H

#include "gmlc_classic.h"
#include "gmlc_project.h"

#include <stddef.h>

/* Normalize classic script slots into the existing source-project model.
 * Slot positions are retained so numeric classic script ids remain stable. */
int gmlc_classic_import_scripts(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap);
int gmlc_classic_import_sprites(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap);
int gmlc_classic_import_backgrounds(const GmlcClassicManifest *classic,
                                    GmlcProject *project, const char *cache_dir,
                                    char *err, size_t errcap);
int gmlc_classic_import_fonts(const GmlcClassicManifest *classic,
                              GmlcProject *project, const char *cache_dir,
                              char *err, size_t errcap);
int gmlc_classic_import_timelines(const GmlcClassicManifest *classic,
                                  GmlcProject *project, const char *cache_dir,
                                  char *err, size_t errcap);
int gmlc_classic_import_sounds(const GmlcClassicManifest *classic,
                               GmlcProject *project, const char *cache_dir,
                               char *err, size_t errcap);
int gmlc_classic_import_paths(const GmlcClassicManifest *classic,
                              GmlcProject *project, char *err, size_t errcap);
int gmlc_classic_import_objects(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap);
int gmlc_classic_import_rooms(const GmlcClassicManifest *classic,
                              GmlcProject *project, const char *cache_dir,
                              char *err, size_t errcap);

#endif
