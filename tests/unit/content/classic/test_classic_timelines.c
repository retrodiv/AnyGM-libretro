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

static int expect_timeline_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_TIMELINE]=1;
  manifest.existing[GMLC_CLASSIC_TIMELINE]=1;
  manifest.slots[GMLC_CLASSIC_TIMELINE]=(GmlcClassicResourceSlot*)calloc(1,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_TIMELINE]) return 0;
  GmlcClassicResourceSlot *slot=&manifest.slots[GMLC_CLASSIC_TIMELINE][0];
  slot->exists=1; slot->name=strdup("resource_timeline");
  Fixture payload={{0},0};
  fixture_u32(&payload,1);  /* one moment */
  fixture_u32(&payload,12); /* moment position */
  fixture_u32(&payload,400); fixture_u32(&payload,1); /* action-list version/count */
  fixture_u32(&payload,440); fixture_u32(&payload,1); fixture_u32(&payload,603);
  fixture_u32(&payload,7); fixture_u32(&payload,0); fixture_u32(&payload,0);
  fixture_u32(&payload,0); fixture_u32(&payload,2);
  fixture_string(&payload,""); fixture_string(&payload,"");
  fixture_u32(&payload,1); fixture_u32(&payload,8);
  for(int i=0;i<8;i++) fixture_u32(&payload,0);
  fixture_u32(&payload,(unsigned)-1); fixture_u32(&payload,0); fixture_u32(&payload,8);
  fixture_string(&payload,"x += 2;");
  for(int i=1;i<8;i++) fixture_string(&payload,"");
  fixture_u32(&payload,0);
  slot->payload=(uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload,payload.data,payload.size); slot->payload_size=payload.size;

  GmlcProject project; fixture_project_clear(&project);
  char err[256],dir[128]="tmp/classic_timeline_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir,0777);
#endif
  int ok=gmlc_classic_import_timelines(&manifest,&project,dir,err,sizeof(err));
  if(!ok) fprintf(stderr,"timeline import failed: %s\n",err);
  if(ok){
    char source[64]={0};
    FILE *file=fopen(project.timelines[0].moments[0].source_path,"rb");
    size_t got=file?fread(source,1,sizeof(source)-1,file):0;
    if(file) fclose(file);
    ok=project.n_timelines==1 && project.timelines[0].n_moments==1 &&
       project.timelines[0].moments[0].step==12 && got && strstr(source,"x += 2;");
    remove(project.timelines[0].moments[0].source_path);
  }
  for(int i=0;i<project.n_timelines;i++){
    free(project.timelines[i].id); free(project.timelines[i].name);
    for(int m=0;m<project.timelines[i].n_moments;m++) free(project.timelines[i].moments[m].source_path);
    free(project.timelines[i].moments);
  }
  free(project.timelines); gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

AnygmTestGroup classic_test_timelines_group(void){
  static const AnygmTestCase cases[]={
    {"import",expect_timeline_import},
  };
  const AnygmTestGroup group={
    "classic.timelines",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
