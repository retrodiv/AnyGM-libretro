/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "classic_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect_path_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_PATH] = 1;
  manifest.existing[GMLC_CLASSIC_PATH] = 1;
  manifest.slots[GMLC_CLASSIC_PATH] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_PATH]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_PATH][0];
  slot->exists = 1; slot->name = strdup("resource_path");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 1); fixture_u32(&payload, 1); fixture_u32(&payload, 4);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 16); fixture_u32(&payload, 16);
  fixture_u32(&payload, 2);
  fixture_double(&payload, 1.5); fixture_double(&payload, 2.5); fixture_double(&payload, 100.0);
  fixture_double(&payload, 9.5); fixture_double(&payload, 8.5); fixture_double(&payload, 50.0);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;
  GmlcProject project;
  fixture_project_clear(&project);
  char err[256];
  int ok = gmlc_classic_import_paths(&manifest, &project, err, sizeof(err));
  if(!ok) fprintf(stderr, "path import failed: %s\n", err);
  if(ok) ok = project.n_paths == 1 && project.paths[0].kind == 1 && project.paths[0].closed &&
              project.paths[0].precision == 4 && project.paths[0].n_points == 2 &&
              project.paths[0].points[1].x > 9.49f && project.paths[0].points[1].speed == 50.0f;
  for(int i = 0; i < project.n_paths; ++i){
    free(project.paths[i].id); free(project.paths[i].name); free(project.paths[i].points);
  }
  free(project.paths);
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

AnygmTestGroup classic_test_paths_group(void){
  static const AnygmTestCase cases[]={
    {"import",expect_path_import},
  };
  const AnygmTestGroup group={
    "classic.paths",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
