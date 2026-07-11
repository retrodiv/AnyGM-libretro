/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_classic_project.h"
#include "gmlc_classic.h"
#include "gmlc_classic_import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *classic_stem(const char *path){
  const char *base = strrchr(path, '/');
  const char *backslash = strrchr(path, '\\');
  if(backslash && (!base || backslash > base)) base = backslash;
  base = base ? base + 1 : path;
  const char *dot = strrchr(base, '.');
  size_t length = dot && dot > base ? (size_t)(dot - base) : strlen(base);
  char *stem = (char*)malloc(length + 1);
  if(stem){ memcpy(stem, base, length); stem[length] = '\0'; }
  return stem;
}

static int classic_add_empty_room(GmlcProject *project, const char *cache_dir,
                                  char *err, size_t errcap){
  if(project->n_rooms) return 1;
  GmlcRoom *room=(GmlcRoom*)calloc(1,sizeof(*room));
  int *order=(int*)calloc(1,sizeof(*order));
  char *source_path=gmlc_path_join(cache_dir,"classic_empty_room_create.gml");
  if(!room || !order || !source_path){
    free(room); free(order); free(source_path);
    if(err && errcap) snprintf(err,errcap,"classic project: empty-room allocation failed");
    return 0;
  }
  room->id=gmlc_strdup("__classic_empty_room");
  room->name=gmlc_strdup("__classic_empty_room");
  room->creation_code_path=source_path;
  room->width=640; room->height=480; room->speed=30;
  room->background_color=0xFF000000u; room->draw_background_color=1;
  for(int i=0;i<8;i++){
    room->views[i].wview=room->views[i].wport=room->width;
    room->views[i].hview=room->views[i].hport=room->height;
    room->views[i].hspeed=room->views[i].vspeed=-1;
    room->views[i].object_id=-1;
  }
  FILE *file=fopen(source_path,"wb");
  int wrote=0;
  if(file){
    wrote=fwrite("exit;\n",1,6,file)==6;
    if(fclose(file)!=0) wrote=0;
  }
  if(!room->id || !room->name || !wrote){
    if(err && errcap) snprintf(err,errcap,"classic project: cannot create empty-room source");
    free(room->id); free(room->name); free(room->creation_code_path);
    free(room); free(order);
    return 0;
  }
  project->rooms=room; project->n_rooms=project->cap_rooms=1;
  project->room_order=order; project->n_room_order=1;
  return 1;
}

int gmlc_classic_project_load(GmlcProject *project, const char *project_path,
                              const char *cache_dir, char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!project || !project_path || !*project_path || !cache_dir || !*cache_dir){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid load arguments");
    return 0;
  }
  gmlc_project_init(project);
  GmlcClassicManifest manifest;
  if(!gmlc_classic_manifest_file(project_path, &manifest, err, errcap)) return 0;
  project->classic_version=(int)manifest.inventory.header.version;
  project->classic_scaling=manifest.inventory.settings.scaling;
  project->classic_interpolate=manifest.inventory.settings.interpolate;
  project->name = classic_stem(project_path);
  project->root_dir = gmlc_path_dirname(project_path);
  project->yyp_path = gmlc_strdup(project_path);
  int ok = project->name && project->root_dir && project->yyp_path &&
    gmlc_classic_import_scripts(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_sprites(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_backgrounds(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_sounds(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_paths(&manifest, project, err, errcap) &&
    gmlc_classic_import_fonts(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_timelines(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_objects(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_rooms(&manifest, project, cache_dir, err, errcap);
  if(ok) ok=classic_add_empty_room(project,cache_dir,err,errcap);
  if(ok && manifest.room_order_count){
    project->room_order=(int*)calloc(manifest.room_order_count,sizeof(*project->room_order));
    if(!project->room_order) ok=0;
    else {
      project->n_room_order=(int)manifest.room_order_count;
      for(uint32_t i=0;i<manifest.room_order_count;i++) project->room_order[i]=(int)manifest.room_order[i];
    }
  }
  gmlc_classic_manifest_free(&manifest);
  if(!ok){
    if(err && errcap && !err[0]) snprintf(err, errcap, "classic project: normalization failed");
    gmlc_project_free(project);
    return 0;
  }
  return 1;
}
