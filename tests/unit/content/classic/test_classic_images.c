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

static int expect_sprite_import(int executable_layout){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_SPRITE] = 1;
  manifest.existing[GMLC_CLASSIC_SPRITE] = 1;
  manifest.slots[GMLC_CLASSIC_SPRITE] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_SPRITE]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_SPRITE][0];
  slot->exists = 1;
  slot->version = 800;
  slot->executable_layout = executable_layout;
  slot->name = strdup("resource_sprite");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 1); fixture_u32(&payload, 2); fixture_u32(&payload, 1);
  fixture_u32(&payload, 800); fixture_u32(&payload, 2); fixture_u32(&payload, 1);
  const unsigned char bgra[8] = {3, 2, 1, 255, 6, 5, 4, 128};
  fixture_u32(&payload, sizeof(bgra));
  memcpy(payload.data + payload.size, bgra, sizeof(bgra)); payload.size += sizeof(bgra);
  if(executable_layout){
    fixture_u32(&payload, 0);
    fixture_u32(&payload, 800); fixture_u32(&payload, 2); fixture_u32(&payload, 1);
    fixture_u32(&payload, 0); fixture_u32(&payload, 1);
    fixture_u32(&payload, 0); fixture_u32(&payload, 0);
    fixture_u32(&payload, 1); fixture_u32(&payload, 0);
  } else {
    fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
    fixture_u32(&payload, 0); fixture_u32(&payload, 1); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  }
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  fixture_project_clear(&project);
  char err[256], dir[128];
  snprintf(dir,sizeof(dir),"tmp/classic_sprite_%s_fixture",executable_layout?"executable":"project");
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_sprites(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "sprite import failed: %s\n", err);
  if(ok){
    struct stat st;
    int width=0,height=0,components=0;
    unsigned char *rgba=NULL;
    if(project.n_sprites==1 && project.sprites[0].frame_paths)
      rgba=classic_fixture_load_png(project.sprites[0].frame_paths[0],
                                    &width,&height,&components);
    ok = project.n_sprites == 1 && project.sprites[0].frame_paths &&
         project.sprites[0].runtime_id == 0 &&
         project.sprites[0].width == 2 && project.sprites[0].height == 1 &&
         project.sprites[0].xorig == 1 && project.sprites[0].yorig == 2 &&
         project.sprites[0].bbox_right == 1 &&
         (!executable_layout || (project.sprites[0].collision_mask_count==1 &&
           project.sprites[0].collision_mask_stride==1 &&
           project.sprites[0].collision_mask_data &&
           project.sprites[0].collision_mask_data[0]==0x80)) &&
         !stat(project.sprites[0].frame_paths[0], &st) && st.st_size > 0 &&
         rgba && width==2 && height==1 &&
         rgba[0]==1 && rgba[1]==2 && rgba[2]==3 && rgba[3]==255 &&
         rgba[4]==4 && rgba[5]==5 && rgba[6]==6 && rgba[7]==128;
    classic_fixture_free_image(rgba);
    remove(project.sprites[0].frame_paths[0]);
  }
  for(int i = 0; i < project.n_sprites; ++i){
    free(project.sprites[i].id); free(project.sprites[i].name);
    for(int frame = 0; frame < project.sprites[i].n_frames; ++frame) free(project.sprites[i].frame_paths[frame]);
    free(project.sprites[i].frame_paths);
    free(project.sprites[i].collision_mask_data);
  }
  free(project.sprites);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_background_import(int executable_layout){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_BACKGROUND] = 1;
  manifest.existing[GMLC_CLASSIC_BACKGROUND] = 1;
  manifest.slots[GMLC_CLASSIC_BACKGROUND] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_BACKGROUND]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_BACKGROUND][0];
  slot->exists = 1; slot->name = strdup("resource_background");
  slot->version = 800; slot->executable_layout = executable_layout;
  Fixture payload = {{0}, 0};
  if(!executable_layout){
    fixture_u32(&payload, 1); fixture_u32(&payload, 1); fixture_u32(&payload, 1);
    fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  }
  fixture_u32(&payload, 800); fixture_u32(&payload, 1); fixture_u32(&payload, 1);
  fixture_u32(&payload, 4);
  payload.data[payload.size++] = 3; payload.data[payload.size++] = 2;
  payload.data[payload.size++] = 1; payload.data[payload.size++] = 255;
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  fixture_project_clear(&project);
  char err[256], dir[128];
  snprintf(dir,sizeof(dir),"tmp/classic_background_%s_fixture",executable_layout?"executable":"project");
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_backgrounds(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "background import failed: %s\n", err);
  if(ok){
    int width=0,height=0,components=0;
    unsigned char *rgba=NULL;
    if(project.n_sprites==1 && project.sprites[0].frame_paths)
      rgba=classic_fixture_load_png(project.sprites[0].frame_paths[0],
                                    &width,&height,&components);
    ok = project.n_sprites == 1 && project.sprites[0].frame_paths &&
         project.sprites[0].runtime_id == -1 &&
         project.n_tilesets == 1 && project.tilesets[0].sprite_id == 0 &&
         rgba && width==1 && height==1 &&
         rgba[0]==1 && rgba[1]==2 && rgba[2]==3 && rgba[3]==255;
    classic_fixture_free_image(rgba);
  }
  if(project.n_sprites){
    remove(project.sprites[0].frame_paths[0]);
    free(project.sprites[0].frame_paths[0]); free(project.sprites[0].frame_paths);
    free(project.sprites[0].id); free(project.sprites[0].name);
  }
  free(project.sprites);
  if(project.n_tilesets){ free(project.tilesets[0].id); free(project.tilesets[0].name); }
  free(project.tilesets);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}
static int test_sprite_project(void){ return expect_sprite_import(0); }
static int test_sprite_executable(void){ return expect_sprite_import(1); }
static int test_background_project(void){ return expect_background_import(0); }
static int test_background_executable(void){ return expect_background_import(1); }

AnygmTestGroup classic_test_images_group(void){
  static const AnygmTestCase cases[]={
    {"sprite-project",test_sprite_project},
    {"sprite-executable",test_sprite_executable},
    {"background-project",test_background_project},
    {"background-executable",test_background_executable},
  };
  const AnygmTestGroup group={
    "classic.images",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
