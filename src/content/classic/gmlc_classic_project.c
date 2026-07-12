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

static int classic_read_file(const char *path, uint8_t **data, size_t *size){
  *data=NULL; *size=0;
  FILE *file=path?fopen(path,"rb"):NULL;
  if(!file) return 0;
  if(fseek(file,0,SEEK_END)!=0){ fclose(file); return 0; }
  long length=ftell(file);
  if(length<0 || fseek(file,0,SEEK_SET)!=0){ fclose(file); return 0; }
  uint8_t *bytes=(uint8_t*)malloc((size_t)length? (size_t)length:1);
  if(!bytes){ fclose(file); return 0; }
  int ok=fread(bytes,1,(size_t)length,file)==(size_t)length;
  if(fclose(file)!=0) ok=0;
  if(!ok){ free(bytes); return 0; }
  *data=bytes; *size=(size_t)length;
  return 1;
}

static char *classic_store_source(GmlcProject *project, const char *cache_dir,
                                  const char *leaf, const char *source){
  if(project->prefer_memory_files)
    return gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_TEXT,source,
                                        strlen(source),0,0);
  char *path=gmlc_path_join(cache_dir,leaf);
  FILE *file=path?fopen(path,"wb"):NULL;
  size_t length=strlen(source);
  int ok=file && (!length || fwrite(source,1,length,file)==length);
  if(file && fclose(file)!=0) ok=0;
  if(!ok){ free(path); return NULL; }
  return path;
}

static int classic_import_metadata(const GmlcClassicManifest *manifest,
                                   GmlcProject *project, const char *cache_dir,
                                   char *err, size_t errcap){
  uint32_t live_triggers=0;
  for(uint32_t i=0;i<manifest->trigger_def_count;i++)
    if(manifest->trigger_defs[i].exists) live_triggers++;
  uint32_t constant_capacity=manifest->constant_def_count+live_triggers;
  if(constant_capacity){
    project->constants=(GmlcProjectConstant*)calloc(constant_capacity,
                                                     sizeof(*project->constants));
    if(!project->constants){
      if(err && errcap) snprintf(err,errcap,"classic project: constant allocation failed");
      return 0;
    }
    project->cap_constants=(int)constant_capacity;
    for(uint32_t i=0;i<manifest->constant_def_count;i++){
      GmlcProjectConstant *constant=&project->constants[project->n_constants++];
      constant->name=gmlc_strdup(manifest->constant_defs[i].name);
      constant->expression=gmlc_strdup(manifest->constant_defs[i].value);
      if(!constant->name || !constant->expression){
        if(err && errcap) snprintf(err,errcap,"classic project: constant copy failed");
        return 0;
      }
    }
  }
  if(live_triggers){
    project->triggers=(GmlcProjectTrigger*)calloc(live_triggers,sizeof(*project->triggers));
    if(!project->triggers){
      if(err && errcap) snprintf(err,errcap,"classic project: trigger allocation failed");
      return 0;
    }
    project->cap_triggers=(int)live_triggers;
    for(uint32_t i=0;i<manifest->trigger_def_count;i++){
      const GmlcClassicTrigger *source=&manifest->trigger_defs[i];
      if(!source->exists) continue;
      GmlcProjectTrigger *trigger=&project->triggers[project->n_triggers++];
      char leaf[64]; snprintf(leaf,sizeof(leaf),"classic_trigger_%06u.gml",i);
      trigger->name=gmlc_strdup(source->name);
      trigger->moment=(int)source->moment;
      trigger->runtime_id=(int)i;
      const char *condition=source->condition?source->condition:"";
      while(*condition==' ' || *condition=='\t' || *condition=='\r' || *condition=='\n') condition++;
      size_t condition_length=strlen(condition);
      const char *prefix="", *suffix="";
      if(!condition_length){
        condition="return false;";
        condition_length=strlen(condition);
        suffix="\n";
      } else if(*condition=='{'){
        suffix="\n";
      } else {
        prefix="return (";
        suffix=");\n";
      }
      size_t normalized_length=strlen(prefix)+condition_length+strlen(suffix);
      char *normalized=(char*)malloc(normalized_length+1);
      if(normalized){
        snprintf(normalized,normalized_length+1,"%s%.*s%s",prefix,(int)condition_length,
                 condition,suffix);
        trigger->condition_path=classic_store_source(project,cache_dir,leaf,normalized);
      }
      free(normalized);
      if(!trigger->name || !trigger->condition_path){
        if(err && errcap) snprintf(err,errcap,"classic project: cannot normalize trigger condition");
        return 0;
      }
      if(source->constant_name && *source->constant_name){
        GmlcProjectConstant *constant=&project->constants[project->n_constants++];
        char value[32]; snprintf(value,sizeof(value),"%u",i);
        constant->name=gmlc_strdup(source->constant_name);
        constant->expression=gmlc_strdup(value);
        if(!constant->name || !constant->expression){
          if(err && errcap) snprintf(err,errcap,"classic project: trigger constant copy failed");
          return 0;
        }
      }
    }
  }
  if(manifest->included_file_count){
    project->included_files=(GmlcProjectIncludedFile*)calloc(manifest->included_file_count,
                                                             sizeof(*project->included_files));
    if(!project->included_files){
      if(err && errcap) snprintf(err,errcap,"classic project: included-file allocation failed");
      return 0;
    }
    project->cap_included_files=(int)manifest->included_file_count;
    for(uint32_t i=0;i<manifest->included_file_count;i++){
      const GmlcClassicIncludedFile *source=&manifest->included_files[i];
      GmlcProjectIncludedFile *included=&project->included_files[project->n_included_files++];
      included->file_name=gmlc_strdup(source->file_name);
      included->custom_folder=gmlc_strdup(source->custom_folder?source->custom_folder:"");
      included->export_mode=(int)source->export_mode;
      included->overwrite_file=source->overwrite_file;
      if(source->data_size){
        included->data=(uint8_t*)malloc(source->data_size);
        if(included->data){
          memcpy(included->data,source->data,source->data_size);
          included->data_size=source->data_size;
        }
      } else if(source->data_exists && source->source_path && *source->source_path){
        char *path=gmlc_path_join(project->root_dir,source->source_path);
        (void)classic_read_file(path,&included->data,&included->data_size);
        free(path);
      }
      if(!included->file_name || !included->custom_folder ||
         (source->data_size && !included->data) ||
         (included->export_mode!=0 && source->data_exists && !included->data)){
        if(err && errcap) snprintf(err,errcap,"classic project: included file data is unavailable");
        return 0;
      }
    }
  }
  if(manifest->library_creation_code_count){
    size_t length=0;
    for(uint32_t i=0;i<manifest->library_creation_code_count;i++)
      length+=strlen(manifest->library_creation_code[i]?manifest->library_creation_code[i]:"")+1;
    char *source=(char*)malloc(length+1);
    size_t at=0;
    if(source) for(uint32_t i=0;i<manifest->library_creation_code_count;i++){
      const char *part=manifest->library_creation_code[i]?manifest->library_creation_code[i]:"";
      size_t part_length=strlen(part);
      memcpy(source+at,part,part_length); at+=part_length;
      source[at++]='\n';
    }
    if(source) source[at]=0;
    char *path=source?classic_store_source(project,cache_dir,"classic_library_startup.gml",source):NULL;
    free(source);
    if(!path){
      if(err && errcap) snprintf(err,errcap,"classic project: cannot write library startup source");
      free(path);
      return 0;
    }
    project->startup_code_path=path;
  }
  return 1;
}

static int classic_add_empty_room(GmlcProject *project, const char *cache_dir,
                                  char *err, size_t errcap){
  if(project->n_rooms) return 1;
  GmlcRoom *room=(GmlcRoom*)calloc(1,sizeof(*room));
  int *order=(int*)calloc(1,sizeof(*order));
  char *source_path=classic_store_source(project,cache_dir,"classic_empty_room_create.gml","exit;\n");
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
  if(!room->id || !room->name){
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
  project->prefer_memory_files=1;
  GmlcClassicManifest manifest;
  if(!gmlc_classic_manifest_file(project_path, &manifest, err, errcap)) return 0;
  project->classic_version=(int)manifest.inventory.header.version;
  project->classic_scaling=manifest.inventory.settings.scaling;
  project->classic_interpolate=manifest.inventory.settings.interpolate;
  project->name = classic_stem(project_path);
  project->root_dir = gmlc_path_dirname(project_path);
  project->yyp_path = gmlc_strdup(project_path);
  int ok = project->name && project->root_dir && project->yyp_path &&
    classic_import_metadata(&manifest, project, cache_dir, err, errcap) &&
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
  if(ok) ok=gmlc_classic_import_room_order(&manifest,project,err,errcap);
  gmlc_classic_manifest_free(&manifest);
  if(!ok){
    if(err && errcap && !err[0]) snprintf(err, errcap, "classic project: normalization failed");
    gmlc_project_free(project);
    return 0;
  }
  return 1;
}
