/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_CLASSIC_IMPORT_H
#define GMLC_CLASSIC_IMPORT_H

#include "gmlc_classic.h"
#include "gmlc_project.h"

#include <stddef.h>
#include <stdint.h>

/* Normalize classic script slots into the existing source-project model.
 * Slot positions are retained so numeric classic script ids remain stable. */
int gmlc_classic_import_scripts(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap);
/* Load GML scripts and public-to-internal function mappings from extension
 * packages next to a classic project. Project scripts take priority; missing,
 * unrelated or malformed packages are a non-fatal compatibility fallback. */
int gmlc_classic_import_extension_aliases(const GmlcClassicManifest *classic,
                                          GmlcProject *project,
                                          const char *project_dir,
                                          char *err, size_t errcap);
/* Fold every sibling extension payload visible to the importer into an
 * existing cache hash. Enumeration is deterministic on every platform. */
int gmlc_classic_extension_dependency_hash(const AnygmHostServices *host,
                                           const char *project_dir,
                                           uint64_t seed, uint64_t *hash_out);
int gmlc_classic_import_sprites(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap);
int gmlc_classic_import_backgrounds(const GmlcClassicManifest *classic,
                                    GmlcProject *project, const char *cache_dir,
                                    char *err, size_t errcap);
/* Only existing classic font slots are emitted, in slot order. Named GML references bind to the
 * resulting compact FONT ids, so deleted/empty slots do not consume the fixed runtime capacity.
 * Limitation: a numeric literal containing an original sparse font slot id has no type information
 * at compile time and cannot be remapped; source should refer to the font resource by name. */
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

/* Copy and validate the explicit room execution order retained by the classic
 * container parser. */
int gmlc_classic_import_room_order(const GmlcClassicManifest *classic,
                                   GmlcProject *project,
                                   char *err, size_t errcap);

#endif
