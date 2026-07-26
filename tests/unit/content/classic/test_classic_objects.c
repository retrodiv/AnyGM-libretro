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

static int expect_object_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_OBJECT] = 1;
  manifest.existing[GMLC_CLASSIC_OBJECT] = 1;
  manifest.slots[GMLC_CLASSIC_OBJECT] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_OBJECT]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_OBJECT][0];
  slot->exists = 1; slot->name = strdup("resource_object");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); fixture_u32(&payload, 1);
  fixture_u32(&payload, (unsigned)-10); fixture_u32(&payload, 0); fixture_u32(&payload, (unsigned)-100);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); /* fields + final event type */
  fixture_u32(&payload, 0); /* Create subtype */
  fixture_u32(&payload, 400); fixture_u32(&payload, 2);
  fixture_u32(&payload, 440); fixture_u32(&payload, 1); fixture_u32(&payload, 603);
  fixture_u32(&payload, 7); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 2);
  fixture_string(&payload, ""); fixture_string(&payload, "");
  fixture_u32(&payload, 1); fixture_u32(&payload, 8);
  for(int i = 0; i < 8; ++i) fixture_u32(&payload, 0);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); fixture_u32(&payload, 8);
  fixture_string(&payload, "x = 4;");
  for(int i = 1; i < 8; ++i) fixture_string(&payload, "");
  fixture_u32(&payload, 0);
  fixture_u32(&payload, 440); fixture_u32(&payload, 1); fixture_u32(&payload, 603);
  fixture_u32(&payload, 7); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 2);
  fixture_string(&payload, ""); fixture_string(&payload, "");
  fixture_u32(&payload, 1); fixture_u32(&payload, 8);
  for(int i = 0; i < 8; ++i) fixture_u32(&payload, 0);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); fixture_u32(&payload, 8);
  fixture_string(&payload, "/* disabled action");
  for(int i = 1; i < 8; ++i) fixture_string(&payload, "");
  fixture_u32(&payload, 0);
  fixture_u32(&payload, (unsigned)-1); /* end Create event list */
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  fixture_project_clear(&project);
  char err[256], dir[128] = "tmp/classic_object_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_objects(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "object import failed: %s\n", err);
  if(ok){
    char source[256] = {0};
    FILE *file = fopen(project.objects[0].events[0].source_path, "rb");
    size_t got = file ? fread(source, 1, sizeof(source) - 1, file) : 0;
    if(file) fclose(file);
    ok = project.n_objects == 1 && project.objects[0].depth == -10 && project.objects[0].n_events == 1 &&
         project.objects[0].events[0].event_type == 0 && got && strstr(source, "(function(){") &&
         strstr(source, "x = 4;") && strstr(source, "/* disabled action\n*/\n})()") &&
         strstr(source, "})()");
    remove(project.objects[0].events[0].source_path);
  }
  for(int i = 0; i < project.n_objects; ++i){
    free(project.objects[i].id); free(project.objects[i].name);
    for(int event = 0; event < project.objects[i].n_events; ++event){
      free(project.objects[i].events[event].id); free(project.objects[i].events[event].source_path);
    }
    free(project.objects[i].events);
  }
  free(project.objects);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

AnygmTestGroup classic_test_objects_group(void){
  static const AnygmTestCase cases[]={
    {"import",expect_object_import},
  };
  const AnygmTestGroup group={
    "classic.objects",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
