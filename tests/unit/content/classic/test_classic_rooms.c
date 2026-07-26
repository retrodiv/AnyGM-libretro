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

static int expect_room_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_ROOM] = 1;
  manifest.inventory.last_instance_id = 100001;
  manifest.existing[GMLC_CLASSIC_ROOM] = 1;
  manifest.slots[GMLC_CLASSIC_ROOM] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_ROOM]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_ROOM][0];
  slot->exists = 1; slot->name = strdup("resource_room");
  Fixture payload = {{0}, 0};
  fixture_string(&payload, "caption");
  fixture_u32(&payload, 320); fixture_u32(&payload, 240); fixture_u32(&payload, 16);
  fixture_u32(&payload, 16); fixture_u32(&payload, 0); fixture_u32(&payload, 30);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0x00112233); fixture_u32(&payload, 1);
  fixture_string(&payload, "global.room_ready = 1;");
  fixture_u32(&payload, 1);
  fixture_u32(&payload, 1); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 1);
  fixture_u32(&payload, 1); fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 1);
  for(int field = 0; field < 14; ++field) fixture_u32(&payload, field == 3 ? 320 : field == 4 ? 240 : 0);
  fixture_u32(&payload, 1);
  fixture_u32(&payload, 12); fixture_u32(&payload, 34); fixture_u32(&payload, 0); fixture_u32(&payload, 100001);
  fixture_string(&payload, "x += 1;"); fixture_u32(&payload, 0);
  fixture_u32(&payload, 1);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 2); fixture_u32(&payload, 3); fixture_u32(&payload, 16);
  fixture_u32(&payload, 16); fixture_u32(&payload, 100); fixture_u32(&payload, 1000001); fixture_u32(&payload, 0);
  for(int field = 0; field < 14; ++field) fixture_u32(&payload, 0);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  fixture_project_clear(&project);
  char err[256], dir[128] = "tmp/classic_room_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_rooms(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "room import failed: %s\n", err);
  if(ok) ok = project.n_rooms == 1 && project.rooms[0].width == 320 &&
              project.rooms[0].n_backgrounds == 1 && project.rooms[0].n_instances == 1 &&
              project.rooms[0].instances[0].instance_id == 100001 && project.rooms[0].n_tiles == 1 &&
              project.rooms[0].tiles[0].depth == 100 && project.next_instance_id == 100002;
  if(project.n_rooms){
    GmlcRoom *room = &project.rooms[0];
    if(room->creation_code_path) remove(room->creation_code_path);
    free(room->creation_code_path); free(room->id); free(room->name);
    for(int i = 0; i < room->n_instances; ++i){
      if(room->instances[i].creation_code_path) remove(room->instances[i].creation_code_path);
      free(room->instances[i].creation_code_path); free(room->instances[i].id); free(room->instances[i].name);
    }
    free(room->instances); free(room->backgrounds); free(room->tiles);
  }
  free(project.rooms);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_sparse_room_order(void){
  Fixture plain=legacy_fixture_variant(701,1);
  size_t encoded_size=0;
  unsigned char *encoded=encode_gm7(plain.data,plain.size,&encoded_size);
  if(!encoded) return 0;
  GmlcClassicManifest manifest;
  GmlcProject project;
  memset(&manifest,0,sizeof(manifest));
  fixture_project_clear(&project);
  char err[256]={0};
  int ok=gmlc_classic_manifest(encoded,encoded_size,&manifest,err,sizeof(err));
  if(!ok) fprintf(stderr,"sparse room-order manifest failed: %s\n",err);
  if(ok) ok=manifest.existing[GMLC_CLASSIC_ROOM]==2 && manifest.room_order_count==2 &&
            manifest.room_order[0]==6 && manifest.room_order[1]==2 &&
            manifest.game_information.size>=12 &&
            get_u32le(manifest.game_information.data)==0x00ffffffu &&
            get_u32le(manifest.game_information.data+8)==strlen("Game Information");
  if(ok) ok=gmlc_classic_import_room_order(&manifest,&project,err,sizeof(err));
  if(!ok && err[0]) fprintf(stderr,"sparse room-order import failed: %s\n",err);
  if(ok) ok=project.n_room_order==2 && project.room_order[0]==6 && project.room_order[1]==2;
  free(project.room_order);
  if(manifest.inventory.header.version) gmlc_classic_manifest_free(&manifest);
  free(encoded);
  return ok;
}

AnygmTestGroup classic_test_rooms_group(void){
  static const AnygmTestCase cases[]={
    {"import",expect_room_import},
    {"sparse-order",expect_sparse_room_order},
  };
  const AnygmTestGroup group={
    "classic.rooms",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
