/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_CLASSIC_TEST_FIXTURE_H
#define ANYGM_CLASSIC_TEST_FIXTURE_H

#include "anygm_test_runner.h"
#include "gmlc_classic.h"
#include "gmlc_classic_import.h"
#include "stdio_vfs.h"

typedef struct {
  unsigned char data[16384];
  size_t size;
} Fixture;

/* Shared bounded classic fixture operations. */
void fixture_project_init(GmlcProject *project);
void fixture_project_clear(GmlcProject *project);
void put_u32le(unsigned char *p, unsigned value);
unsigned get_u32le(const unsigned char *p);
unsigned char *encode_gm7(const unsigned char *plain, size_t plain_size,
                                 size_t *encoded_size);
void fixture_u32(Fixture *f, unsigned value);
void fixture_zero(Fixture *f, size_t count);
void fixture_string(Fixture *f, const char *text);
void fixture_double(Fixture *f, double value);
void fixture_compressed(Fixture *f, const unsigned char *raw, int raw_size);
Fixture game_information_fixture(void);
Fixture manifest_fixture(unsigned container_version);
int build_executable_fixture(Fixture *executable);
int build_legacy_executable_fixture(Fixture *executable);
int build_gm53_executable_fixture(Fixture *executable);
void fixture_legacy_room(Fixture *f, const char *name);
Fixture legacy_fixture_variant(unsigned container_version, int sparse_rooms);
Fixture legacy_fixture(unsigned container_version);
int build_project_fixture(unsigned version, Fixture *out);

/* A container carrying one caller-supplied source, run once at startup.
 *
 * A suite that asserts on a language or runtime rule needs content it owns. The smallest fixture
 * is a project with no resources whose startup code is the rule under test. Classic containers
 * keep that code as plain text for compilation during loading, so this embeds the source rather
 * than compiling anything. `gml` may be NULL, which reproduces the plain fixture byte for byte. */
Fixture manifest_fixture_source(unsigned container_version, const char *gml);
Fixture legacy_fixture_source(unsigned container_version, const char *gml);
int build_project_fixture_source(unsigned version, const char *gml, Fixture *out);

/* A container carrying objects placed in a room.
 *
 * Startup code reaches every rule that does not need an instance. Event ordering and destruction
 * order do need one, because an object nobody placed never runs. A program describes the smallest
 * project that can exercise those: one square sprite so that collision has a mask to test, objects
 * whose events carry authored source, and a room holding instances of them. */
typedef struct {
  int event_type;    /* 0 create, 3 step, 4 collision, 8 draw */
  int event_number;  /* collision: the other object's slot; otherwise the event's own number */
  const char *source;
} FixtureEvent;

typedef struct {
  const char *name;
  int sprite;        /* sprite slot, or -1 for an object with no sprite and so no collision */
  const FixtureEvent *events;
  int event_count;
} FixtureObject;

typedef struct {
  int object;        /* object slot */
  int x, y;
} FixtureInstance;

typedef struct {
  const char *startup;             /* library creation code, run once before the room; may be NULL */
  int sprite_size;                 /* edge of the one opaque sprite in slot 0; 0 writes no sprite */
  const FixtureObject *objects;
  int object_count;
  const FixtureInstance *instances;
  int instance_count;
  int room_width, room_height;
  const char *room_caption;   /* the room's authored caption; NULL writes an empty one */
} FixtureProgram;

int build_project_fixture_program(unsigned version, const FixtureProgram *program, Fixture *out);

/* GMLC_CLASSIC_TEST_FIXTURE_OPERATIONS */

void classic_fixture_host_init(void);
AnygmHostServices *classic_fixture_host(void);
unsigned char *classic_fixture_load_png(const char *path, int *width,
                                        int *height, int *components);
void classic_fixture_free_image(void *image);

AnygmTestGroup classic_test_format_group(void);
AnygmTestGroup classic_test_code_group(void);
AnygmTestGroup classic_test_extensions_group(void);
AnygmTestGroup classic_test_images_group(void);
AnygmTestGroup classic_test_fonts_group(void);
AnygmTestGroup classic_test_audio_group(void);
AnygmTestGroup classic_test_legacy_media_group(void);
AnygmTestGroup classic_test_paths_group(void);
AnygmTestGroup classic_test_timelines_group(void);
AnygmTestGroup classic_test_objects_group(void);
AnygmTestGroup classic_test_rooms_group(void);

#endif
