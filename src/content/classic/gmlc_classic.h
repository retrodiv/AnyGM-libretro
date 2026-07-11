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

typedef enum {
  GMLC_CLASSIC_SOUND = 0,
  GMLC_CLASSIC_SPRITE,
  GMLC_CLASSIC_BACKGROUND,
  GMLC_CLASSIC_PATH,
  GMLC_CLASSIC_SCRIPT,
  GMLC_CLASSIC_FONT,
  GMLC_CLASSIC_TIMELINE,
  GMLC_CLASSIC_OBJECT,
  GMLC_CLASSIC_ROOM,
  GMLC_CLASSIC_RESOURCE_TYPES
} GmlcClassicResourceType;

typedef struct {
  GmlcClassicHeader header;
  uint32_t settings_version;
  uint32_t trigger_slots;
  uint32_t constants;
  uint32_t resource_slots[GMLC_CLASSIC_RESOURCE_TYPES];
  uint32_t last_instance_id;
  uint32_t last_tile_id;
  size_t resource_section_offsets[GMLC_CLASSIC_RESOURCE_TYPES];
  size_t payload_end;
} GmlcClassicInventory;

typedef struct {
  int exists;
  uint32_t version;
  char *name;
  /* Populated for script resources; NULL for other resource kinds. */
  char *source;
} GmlcClassicResourceSlot;

typedef struct {
  GmlcClassicInventory inventory;
  GmlcClassicResourceSlot *slots[GMLC_CLASSIC_RESOURCE_TYPES];
  uint32_t existing[GMLC_CLASSIC_RESOURCE_TYPES];
} GmlcClassicManifest;

/* Inspect only the unencrypted common project header. This deliberately does
 * not claim that the rest of the project is loadable. */
int gmlc_classic_probe(const void *data, size_t size, GmlcClassicHeader *out,
                       char *err, size_t errcap);
int gmlc_classic_probe_file(const char *path, GmlcClassicHeader *out,
                            char *err, size_t errcap);
/* Parse the length-delimited top-level inventory used by GM8/8.1 projects.
 * Individual resource payloads are not interpreted by this function. */
int gmlc_classic_inventory(const void *data, size_t size,
                           GmlcClassicInventory *out, char *err, size_t errcap);
int gmlc_classic_inventory_file(const char *path, GmlcClassicInventory *out,
                                char *err, size_t errcap);
/* Inflate enough of each GM8/8.1 resource block to validate its envelope and
 * read its existence flag, name, and resource-format version. */
int gmlc_classic_manifest(const void *data, size_t size,
                          GmlcClassicManifest *out, char *err, size_t errcap);
int gmlc_classic_manifest_file(const char *path, GmlcClassicManifest *out,
                               char *err, size_t errcap);
void gmlc_classic_manifest_free(GmlcClassicManifest *manifest);
const char *gmlc_classic_version_name(GmlcClassicVersion version);
const char *gmlc_classic_resource_name(GmlcClassicResourceType type);

#endif
