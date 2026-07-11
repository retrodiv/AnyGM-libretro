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
  project->name = classic_stem(project_path);
  project->root_dir = gmlc_path_dirname(project_path);
  project->yyp_path = gmlc_strdup(project_path);
  int ok = project->name && project->root_dir && project->yyp_path &&
    gmlc_classic_import_scripts(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_sprites(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_backgrounds(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_sounds(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_paths(&manifest, project, err, errcap) &&
    gmlc_classic_import_objects(&manifest, project, cache_dir, err, errcap) &&
    gmlc_classic_import_rooms(&manifest, project, cache_dir, err, errcap);
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
