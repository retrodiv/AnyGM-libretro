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
void fixture_legacy_room(Fixture *f, const char *name);
Fixture legacy_fixture_variant(unsigned container_version, int sparse_rooms);
Fixture legacy_fixture(unsigned container_version);
int build_project_fixture(unsigned version, Fixture *out);

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
