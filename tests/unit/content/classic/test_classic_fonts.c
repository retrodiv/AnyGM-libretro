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

static int fixture_font_slot(GmlcClassicResourceSlot *slot, const char *name,
                             unsigned size, unsigned bold, unsigned italic,
                             unsigned first, unsigned last){
  slot->exists=1; slot->name=strdup(name);
  Fixture payload={{0},0};
  fixture_string(&payload,"sans"); fixture_u32(&payload,size); fixture_u32(&payload,bold);
  fixture_u32(&payload,italic); fixture_u32(&payload,first); fixture_u32(&payload,last);
  slot->payload=(uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload) return 0;
  memcpy(slot->payload,payload.data,payload.size); slot->payload_size=payload.size;
  return 1;
}

static void free_font_fixture_project(GmlcProject *project){
  for(int i=0;i<project->n_fonts;i++){
    const char *path=project->fonts[i].png_path;
    if(path && strncmp(path,"gmlc-memory://",14)) remove(path);
  }
  gmlc_project_free(project);
}

static int expect_sparse_font_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_FONT]=5;
  manifest.existing[GMLC_CLASSIC_FONT]=2;
  manifest.slots[GMLC_CLASSIC_FONT]=(GmlcClassicResourceSlot*)calloc(5,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_FONT]) return 0;
  if(!fixture_font_slot(&manifest.slots[GMLC_CLASSIC_FONT][1],"font_first",12,0,0,32,127) ||
     !fixture_font_slot(&manifest.slots[GMLC_CLASSIC_FONT][4],"font_second",8,1,1,65,90)){
    gmlc_classic_manifest_free(&manifest); return 0;
  }
  GmlcProject project; fixture_project_clear(&project);
  char err[256],dir[128]="tmp/classic_font_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir,0777);
#endif
  int ok=gmlc_classic_import_fonts(&manifest,&project,dir,err,sizeof(err));
  if(!ok) fprintf(stderr,"font import failed: %s\n",err);
  if(ok) ok=project.n_fonts==2 && project.cap_fonts==2 &&
    !strcmp(project.fonts[0].name,"font_first") && project.fonts[0].n_glyphs==96 &&
    project.fonts[0].glyphs[0].ch==32 && project.fonts[0].em_size==18 &&
    !strcmp(project.fonts[1].name,"font_second") && project.fonts[1].n_glyphs==26 &&
    project.fonts[1].glyphs[0].ch==65 && project.fonts[1].em_size==12 &&
    project.fonts[1].glyphs[0].h==10 && project.fonts[1].glyphs[0].shift==8 &&
    project.fonts[0].png_path;
  free_font_fixture_project(&project); gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_empty_font_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_FONT]=1520;
  manifest.slots[GMLC_CLASSIC_FONT]=(GmlcClassicResourceSlot*)calloc(1520,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_FONT]) return 0;
  GmlcProject project; fixture_project_clear(&project);
  char err[256];
  int ok=gmlc_classic_import_fonts(&manifest,&project,"tmp/classic_empty_font_fixture",err,sizeof(err));
  if(!ok) fprintf(stderr,"empty font import failed: %s\n",err);
  if(ok) ok=project.n_fonts==0 && project.cap_fonts==0 && project.fonts==NULL;
  free_font_fixture_project(&project); gmlc_classic_manifest_free(&manifest);
  return ok;
}

static int expect_gm81_font_metadata(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.header.version=GMLC_CLASSIC_GM81;
  manifest.inventory.resource_slots[GMLC_CLASSIC_FONT]=1;
  manifest.existing[GMLC_CLASSIC_FONT]=1;
  manifest.slots[GMLC_CLASSIC_FONT]=(GmlcClassicResourceSlot*)calloc(1,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_FONT]) return 0;
  unsigned packed_first=(3u<<24)|(177u<<16)|33u;
  if(!fixture_font_slot(&manifest.slots[GMLC_CLASSIC_FONT][0],"font_packed",12,0,0,
                        packed_first,33)){
    gmlc_classic_manifest_free(&manifest);
    return 0;
  }
  GmlcProject project;
  fixture_project_clear(&project);
  project.prefer_memory_files=1;
  char err[256];
  int ok=gmlc_classic_import_fonts(&manifest,&project,"tmp/classic_font_metadata_fixture",
                                   err,sizeof(err));
  if(!ok) fprintf(stderr,"packed font import failed: %s\n",err);
  if(ok) ok=project.n_fonts==1 && project.fonts[0].n_glyphs==1 &&
    project.fonts[0].glyphs[0].ch==33 && project.fonts[0].glyphs[0].h==14 &&
    project.fonts[0].em_size==18 && project.n_memory_files==1 &&
    project.memory_files[0].kind==GMLC_MEMORY_RGBA;
  if(!ok && project.n_fonts>0)
    fprintf(stderr,"packed font assertion: fonts=%d glyphs=%d ch=%d h=%d em=%u files=%d kind=%d\n",
            project.n_fonts,project.fonts[0].n_glyphs,
            project.fonts[0].n_glyphs?project.fonts[0].glyphs[0].ch:-1,
            project.fonts[0].n_glyphs?project.fonts[0].glyphs[0].h:-1,
            project.fonts[0].em_size,project.n_memory_files,
            project.n_memory_files?(int)project.memory_files[0].kind:-1);
  free_font_fixture_project(&project);
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

AnygmTestGroup classic_test_fonts_group(void){
  static const AnygmTestCase cases[]={
    {"sparse-import",expect_sparse_font_import},
    {"empty-import",expect_empty_font_import},
    {"gm81-metadata",expect_gm81_font_metadata},
  };
  const AnygmTestGroup group={
    "classic.fonts",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
