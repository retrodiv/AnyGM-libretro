/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_imported_timelines(GmlcProject *project){
  for(int i=0;i<project->n_timelines;i++){
    free(project->timelines[i].id); free(project->timelines[i].name);
    for(int m=0;m<project->timelines[i].n_moments;m++) free(project->timelines[i].moments[m].source_path);
    free(project->timelines[i].moments);
  }
  free(project->timelines);
  project->timelines=NULL;
  project->n_timelines=project->cap_timelines=0;
}

int gmlc_classic_import_timelines(const GmlcClassicManifest *classic,
                                  GmlcProject *project, const char *cache_dir,
                                  char *err, size_t errcap){
  if(err && errcap) err[0]='\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->timelines || project->n_timelines){
    if(err && errcap) snprintf(err,errcap,"classic import: invalid timeline-import arguments");
    return 0;
  }
  uint32_t count=classic->inventory.resource_slots[GMLC_CLASSIC_TIMELINE];
  if(count>INT32_MAX){ if(err && errcap) snprintf(err,errcap,"classic import: too many timeline slots"); return 0; }
  project->timelines=(GmlcTimeline*)calloc(count?count:1,sizeof(*project->timelines));
  if(!project->timelines){ if(err && errcap) snprintf(err,errcap,"classic import: out of memory allocating timelines"); return 0; }
  project->n_timelines=project->cap_timelines=(int)count;
  const GmlcClassicResourceSlot *slots=classic->slots[GMLC_CLASSIC_TIMELINE];
  for(uint32_t i=0;i<count;i++){
    char fallback[64];
    snprintf(fallback,sizeof(fallback),"__classic_missing_timeline_%u",i);
    const char *name=slots[i].exists && slots[i].name?slots[i].name:fallback;
    GmlcTimeline *timeline=&project->timelines[i];
    timeline->id=copy_string(name); timeline->name=copy_string(name);
    if(!timeline->id || !timeline->name){ free_imported_timelines(project); return 0; }
    if(!slots[i].exists) continue;
    ImportReader r={slots[i].payload,slots[i].payload_size,0,err,errcap};
    uint32_t moments;
    if(!import_u32(&r,&moments,"timeline moment count") || moments>INT32_MAX){
      free_imported_timelines(project); return 0;
    }
    timeline->moments=(GmlcTimelineMoment*)calloc(moments?moments:1,sizeof(*timeline->moments));
    if(!timeline->moments){ free_imported_timelines(project); return 0; }
    timeline->n_moments=timeline->cap_moments=(int)moments;
    for(uint32_t moment=0;moment<moments;moment++){
      uint32_t step;
      ImportText text={0};
      if(!import_u32(&r,&step,"timeline moment step") || !import_actions(&r,&text)){
        free(text.data); free_imported_timelines(project); return 0;
      }
      if(!text.data && !text_append(&text,"exit;\n")){ free_imported_timelines(project); return 0; }
      char leaf[112];
      snprintf(leaf,sizeof(leaf),"classic_timeline_%06u_moment_%06u.gml",i,moment);
      timeline->moments[moment].step=(int32_t)step;
      timeline->moments[moment].source_path=import_source_path(project,cache_dir,leaf,
                                                               text.data,err,errcap);
      if(!timeline->moments[moment].source_path){
        free(text.data); free_imported_timelines(project); return 0;
      }
      free(text.data);
    }
    if(r.pos!=r.size){
      if(err && errcap) snprintf(err,errcap,"classic import: trailing timeline payload");
      free_imported_timelines(project); return 0;
    }
  }
  return 1;
}
