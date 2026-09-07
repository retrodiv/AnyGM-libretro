/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_CLASSIC_H
#define GMLC_CLASSIC_H

#include <stddef.h>
#include <stdint.h>

struct AnygmHostServices;
typedef struct AnygmContentTransforms AnygmContentTransforms;

#define GMLC_CLASSIC_MAGIC 1234321u
/* Keep direct classic-project reads aligned with the public content-router member limit. */
#define GMLC_CLASSIC_FILE_LIMIT UINT64_C(1073741824)

typedef enum {
  GMLC_CLASSIC_UNKNOWN = 0,
  GMLC_CLASSIC_GM53 = 530,
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
  int start_fullscreen;
  int interpolate;
  int borderless;
  int show_cursor;
  int scaling;
  int resizable;
  int always_on_top;
  uint32_t outside_color;
  int set_resolution;
  int color_depth;
  int resolution;
  int frequency;
  int hide_caption_buttons;
  int synchronize;
  int force_software_vertex_processing;
  int disable_screensaver;
  int f4_fullscreen;
  int f1_help;
  int escape_ends_game;
  int f5_save_f6_load;
  int f9_screenshot;
  int close_as_escape;
  uint32_t priority;
  int freeze_on_focus_loss;
  uint32_t loading_bar;
  int loading_transparent;
  uint32_t loading_alpha;
  int scale_progress_bar;
  int show_errors;
  int log_errors;
  int abort_errors;
  int uninitialized_as_zero;
  int error_on_uninitialized_arguments;
  int swap_creation_events;
} GmlcClassicSettings;

typedef struct {
  GmlcClassicHeader header;
  uint32_t settings_version;
  GmlcClassicSettings settings;
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
  /* Type-specific bytes immediately after the resource-format version. */
  uint8_t *payload;
  size_t payload_size;
  int legacy_layout;
  int executable_layout;
} GmlcClassicResourceSlot;

typedef struct {
  char *name;
  char *value;
} GmlcClassicConstant;

typedef struct {
  uint8_t *data;
  size_t size;
} GmlcClassicBlob;

typedef struct {
  int exists;
  char *name;
  char *condition;
  char *constant_name;
  uint32_t moment;
} GmlcClassicTrigger;

typedef struct {
  char *file_name;
  char *source_path;
  uint8_t *data;
  size_t data_size;
  uint32_t source_length;
  uint32_t export_mode;
  char *custom_folder;
  int data_exists;
  int stored_in_project;
  int overwrite_file;
  int free_memory;
  int remove_at_end;
} GmlcClassicIncludedFile;

enum { GMLC_CLASSIC_EXTENSION_FUNCTION_WORDS = 21 };

typedef struct {
  uint32_t version;
  char *name;
  char *external_name;
  /* Call convention, compiled function id, argument count, 17 argument types and return type. */
  uint32_t signature[GMLC_CLASSIC_EXTENSION_FUNCTION_WORDS];
} GmlcClassicExtensionFunction;

typedef struct {
  uint32_t version;
  char *name;
  char *value;
} GmlcClassicExtensionConstant;

typedef struct {
  uint32_t version;
  char *name;
  uint32_t kind;
  char *initializer;
  char *finalizer;
  GmlcClassicExtensionFunction *functions;
  uint32_t function_count;
  GmlcClassicExtensionConstant *constants;
  uint32_t constant_count;
} GmlcClassicExtensionFile;

typedef struct {
  uint32_t version;
  char *name;
  char *folder;
  GmlcClassicExtensionFile *files;
  uint32_t file_count;
  /* Installed extension data: seed plus encrypted file blobs. */
  uint8_t *data;
  size_t data_size;
} GmlcClassicExtension;

typedef struct {
  GmlcClassicInventory inventory;
  /* True when the manifest uses embedded-content ordering rather than editor-project grouping.
   * Embedded room records preserve their instance chain; editor containers require resource
   * grouping by the Classic importer. */
  int executable_layout;
  GmlcClassicResourceSlot *slots[GMLC_CLASSIC_RESOURCE_TYPES];
  uint32_t existing[GMLC_CLASSIC_RESOURCE_TYPES];
  GmlcClassicConstant *constant_defs;
  uint32_t constant_def_count;
  GmlcClassicTrigger *trigger_defs;
  uint32_t trigger_def_count;
  GmlcClassicIncludedFile *included_files;
  uint32_t included_file_count;
  char **extension_names;
  uint32_t extension_count;
  /* Compiled layouts include complete extension records. Editable projects retain package names
   * and resolve the corresponding .gex/.ged files from the installation. */
  GmlcClassicExtension *extensions;
  uint32_t extension_detail_count;
  /* Raw, versioned game-information payload.  The project converter owns the
   * bytes and decides how much of the legacy rich-text metadata to preserve. */
  GmlcClassicBlob game_information;
  GmlcClassicBlob loading_bar_background;
  GmlcClassicBlob loading_bar_foreground;
  GmlcClassicBlob loading_image;
  char **library_creation_code;
  uint32_t library_creation_code_count;
  uint32_t *room_order;
  uint32_t room_order_count;
} GmlcClassicManifest;

/* Inspect only a normalized common project header. This deliberately does
 * not claim that the rest of the project is loadable. */
int gmlc_classic_probe(const void *data, size_t size, GmlcClassicHeader *out,
                       char *err, size_t errcap);
int gmlc_classic_probe_file(const struct AnygmHostServices *host,const char *path,
                            GmlcClassicHeader *out,
                            char *err, size_t errcap);
/* Complete, side-effect-free structural validator for source candidate selection.
 * Context is the caller's transform registry for nested records, or NULL. */
int gmlc_classic_validate_image(void *context,const void *data,size_t size,char *err,size_t errcap);
/* Parse a normalized project inventory. Memory readers never select the source
 * input adapter; file inventory/manifest entry points prepare that source once.
 * Individual modern resource payloads are not interpreted by this function. */
int gmlc_classic_inventory(const AnygmContentTransforms *transforms,const void *data, size_t size,
                           GmlcClassicInventory *out, char *err, size_t errcap);
int gmlc_classic_inventory_file(const AnygmContentTransforms *transforms,const struct AnygmHostServices *host,const char *path,
                                GmlcClassicInventory *out,
                                char *err, size_t errcap);
/* Inflate enough of each GM8/8.1 resource block to validate its envelope and
 * read its existence flag, name, and resource-format version. */
int gmlc_classic_manifest(const AnygmContentTransforms *transforms,const void *data, size_t size,
                          GmlcClassicManifest *out, char *err, size_t errcap);
int gmlc_classic_manifest_file(const AnygmContentTransforms *transforms,const struct AnygmHostServices *host,const char *path,
                               GmlcClassicManifest *out,
                               char *err, size_t errcap);
/* Extract an exact normalized editor project inside a Classic executable. The returned bytes are
 * owned by the caller. Source preparation belongs to the caller; this entry point only scans
 * ordinary project headers. */
int gmlc_classic_embedded_project(const AnygmContentTransforms *transforms,const void *data,size_t size,
                                  GmlcClassicBlob *project,GmlcClassicVersion *version,
                                  char *err,size_t errcap);
/* Normalize the versioned game-information blob to its bounded, uncompressed
 * record.  Empty projects produce an empty output blob. */
int gmlc_classic_game_information_decode(const GmlcClassicBlob *source,
                                         GmlcClassicBlob *decoded,
                                         char *err, size_t errcap);
void gmlc_classic_manifest_free(GmlcClassicManifest *manifest);
const char *gmlc_classic_version_name(GmlcClassicVersion version);
const char *gmlc_classic_resource_name(GmlcClassicResourceType type);

#endif
