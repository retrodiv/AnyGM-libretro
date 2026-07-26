/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import_internal.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_imported_objects(GmlcProject *project){
  for(int i = 0; i < project->n_objects; ++i){
    GmlcObject *object = &project->objects[i];
    free(object->id); free(object->name);
    for(int event = 0; event < object->n_events; ++event){
      free(object->events[event].id); free(object->events[event].collision_id);
      free(object->events[event].source_path);
    }
    free(object->events);
  }
  free(project->objects);
  project->objects = NULL;
  project->n_objects = project->cap_objects = 0;
}

static int append_object_event(GmlcObject *object, GmlcObjectEvent event){
  if(object->n_events >= object->cap_events){
    int capacity = object->cap_events ? object->cap_events * 2 : 4;
    GmlcObjectEvent *events = (GmlcObjectEvent*)realloc(object->events, (size_t)capacity * sizeof(*events));
    if(!events) return 0;
    object->events = events;
    object->cap_events = capacity;
  }
  object->events[object->n_events++] = event;
  return 1;
}

static int classic_sprite_project_index(const GmlcProject *project, int32_t runtime_id){
  if(runtime_id < 0) return -1;
  for(int i = 0; i < project->n_sprites; ++i)
    if(project->sprites[i].runtime_id == runtime_id) return i;
  return -1;
}

int gmlc_classic_import_objects(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->objects || project->n_objects){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid object-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_OBJECT];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many object slots");
    return 0;
  }
  project->objects = (GmlcObject*)calloc(count ? count : 1, sizeof(*project->objects));
  if(!project->objects){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating objects");
    return 0;
  }
  project->n_objects = project->cap_objects = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_OBJECT];
  for(uint32_t i = 0; i < count; ++i){
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_object_%u", i);
    const char *name = slots[i].exists && slots[i].name ? slots[i].name : fallback;
    project->objects[i].id = copy_string(name);
    project->objects[i].name = copy_string(name);
    project->objects[i].sprite_id = project->objects[i].mask_id = -1;
    project->objects[i].parent_id = -100;
    project->objects[i].visible = slots[i].exists ? 1 : 0;
    if(!project->objects[i].id || !project->objects[i].name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory naming object %u", i);
      free_imported_objects(project);
      return 0;
    }
  }
  for(uint32_t i = 0; i < count; ++i){
    const GmlcClassicResourceSlot *source = &slots[i];
    if(!source->exists) continue;
    ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
    uint32_t sprite, solid, visible, depth, persistent, parent, mask, last_event_type;
    if(!import_u32(&r, &sprite, "object sprite") || !import_u32(&r, &solid, "object solid flag") ||
       !import_u32(&r, &visible, "object visible flag") || !import_u32(&r, &depth, "object depth") ||
       !import_u32(&r, &persistent, "object persistent flag") || !import_u32(&r, &parent, "object parent") ||
       !import_u32(&r, &mask, "object mask") || !import_u32(&r, &last_event_type, "object event-type count") ||
       last_event_type > 64){ free_imported_objects(project); return 0; }
    GmlcObject *object = &project->objects[i];
    object->sprite_id = classic_sprite_project_index(project, (int32_t)sprite);
    object->solid = solid != 0; object->visible = visible != 0;
    object->depth = (int32_t)depth; object->persistent = persistent != 0;
    object->parent_id = (int32_t)parent;
    object->mask_id = classic_sprite_project_index(project, (int32_t)mask);
    for(uint32_t event_type = 0; event_type <= last_event_type; ++event_type){
      for(;;){
        uint32_t event_number;
        if(!import_u32(&r, &event_number, "object event number")){ free_imported_objects(project); return 0; }
        if(event_number == UINT32_MAX) break;
        ImportText text = {0};
        if(!import_actions(&r, &text)){
          free(text.data); free_imported_objects(project); return 0;
        }
        if(!text.data && !text_append(&text, "exit;\n")){
          free_imported_objects(project); return 0;
        }
        char leaf[112], event_id[64];
        snprintf(leaf, sizeof(leaf), "classic_object_%06u_event_%02u_%010u.gml", i, event_type, event_number);
        snprintf(event_id, sizeof(event_id), "classic_event_%u_%u_%u", i, event_type, event_number);
        GmlcObjectEvent event;
        memset(&event, 0, sizeof(event));
        event.id = copy_string(event_id);
        event.event_type = (int)event_type;
        event.event_number = (int32_t)event_number;
        event.collision_object_id = event_type == 4 ? (int32_t)event_number : -1;
        event.source_path = import_source_path(project,cache_dir,leaf,text.data,err,errcap);
        if(!event.id || !event.source_path ||
           !append_object_event(object, event)){
          free(event.id); free(event.source_path); free(text.data);
          if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing object event");
          free_imported_objects(project); return 0;
        }
        free(text.data);
      }
    }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing object payload");
      free_imported_objects(project); return 0;
    }
  }
  return 1;
}
