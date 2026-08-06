/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_imported_paths(GmlcProject *project){
  for(int i = 0; i < project->n_paths; ++i){
    free(project->paths[i].id);
    free(project->paths[i].name);
    free(project->paths[i].points);
  }
  free(project->paths);
  project->paths = NULL;
  project->n_paths = project->cap_paths = 0;
}

int gmlc_classic_import_paths(const GmlcClassicManifest *classic,
                              GmlcProject *project, char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || project->paths || project->n_paths){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid path-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_PATH];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many path slots");
    return 0;
  }
  project->paths = (GmlcPath*)calloc(count ? count : 1, sizeof(*project->paths));
  if(!project->paths){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating paths");
    return 0;
  }
  project->n_paths = project->cap_paths = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_PATH];
  for(uint32_t i = 0; i < count; ++i){
    const GmlcClassicResourceSlot *source = &slots[i];
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_path_%u", i);
    const char *name = source->exists && source->name ? source->name : fallback;
    GmlcPath *path = &project->paths[i];
    path->id = copy_string(name);
    path->name = copy_string(name);
    if(!path->id || !path->name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory importing path %u", i);
      free_imported_paths(project);
      return 0;
    }
    if(!source->exists) continue;
    ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
    uint32_t kind, closed, precision, ignored, points;
    /* Every compiled revision omits the three editor-only fields a project carries, so the short
     * form follows the executable layout rather than any one revision. */
    int compact=source->executable_layout;
    if(!import_u32(&r, &kind, "path kind") || !import_u32(&r, &closed, "path closed flag") ||
       !import_u32(&r, &precision, "path precision") ||
       (!compact &&
        (!import_u32(&r, &ignored, "path editor room") ||
         !import_u32(&r, &ignored, "path snap x") ||
         !import_u32(&r, &ignored, "path snap y"))) ||
       !import_u32(&r, &points, "path point count") ||
       points > INT32_MAX || (size_t)points > (r.size - r.pos) / 24u){
      free_imported_paths(project);
      return 0;
    }
    path->kind = (int32_t)kind;
    path->closed = closed != 0;
    path->precision = (int32_t)precision;
    path->n_points = (int)points;
    path->points = (GmlcPathPoint*)calloc(points ? points : 1, sizeof(*path->points));
    if(!path->points){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating path points");
      free_imported_paths(project);
      return 0;
    }
    for(uint32_t point = 0; point < points; ++point){
      double x, y, speed;
      if(!import_double(&r, &x, "path point x") || !import_double(&r, &y, "path point y") ||
         !import_double(&r, &speed, "path point speed")){
        free_imported_paths(project);
        return 0;
      }
      path->points[point].x = (float)x;
      path->points[point].y = (float)y;
      path->points[point].speed = (float)speed;
    }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing path payload");
      free_imported_paths(project);
      return 0;
    }
  }
  return 1;
}
