/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "classic_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static int expect_sound_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_SOUND] = 1;
  manifest.existing[GMLC_CLASSIC_SOUND] = 1;
  manifest.slots[GMLC_CLASSIC_SOUND] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_SOUND]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_SOUND][0];
  slot->exists = 1; slot->name = strdup("resource_sound");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 0); fixture_string(&payload, ".wav"); fixture_string(&payload, "fixture.wav");
  fixture_u32(&payload, 1);
  const unsigned char audio[] = {'R','I','F','F'};
  fixture_u32(&payload, sizeof(audio)); memcpy(payload.data + payload.size, audio, sizeof(audio)); payload.size += sizeof(audio);
  fixture_u32(&payload, 0); fixture_double(&payload, 0.5); fixture_double(&payload, 0.0); fixture_u32(&payload, 1);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  fixture_project_clear(&project);
  char err[256], dir[128] = "tmp/classic_sound_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_sounds(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "sound import failed: %s\n", err);
  if(ok){
    FILE *file = fopen(project.sounds[0].data_path, "rb");
    unsigned char got[4] = {0};
    size_t count = file ? fread(got, 1, sizeof(got), file) : 0;
    if(file) fclose(file);
    ok = project.n_sounds == 1 && count == sizeof(got) && !memcmp(got, audio, sizeof(audio)) &&
         project.sounds[0].volume > 0.49f && project.sounds[0].volume < 0.51f;
    remove(project.sounds[0].data_path);
  }
  for(int i = 0; i < project.n_sounds; ++i){
    free(project.sounds[i].id); free(project.sounds[i].name); free(project.sounds[i].data_path);
  }
  free(project.sounds);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

AnygmTestGroup classic_test_audio_group(void){
  static const AnygmTestCase cases[]={
    {"sound-import",expect_sound_import},
  };
  const AnygmTestGroup group={
    "classic.audio",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
