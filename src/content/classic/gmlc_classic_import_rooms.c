/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import_internal.h"

#include "anygm_host.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_imported_rooms(GmlcProject *project){
  for(int i = 0; i < project->n_rooms; ++i){
    GmlcRoom *room = &project->rooms[i];
    free(room->id); free(room->name); free(room->caption); free(room->creation_code_path);
    free(room->backgrounds); free(room->tiles);
    for(int instance = 0; instance < room->n_instances; ++instance){
      free(room->instances[instance].id); free(room->instances[instance].name);
      free(room->instances[instance].creation_code_path);
    }
    free(room->instances);
  }
  free(project->rooms);
  project->rooms = NULL;
  project->n_rooms = project->cap_rooms = 0;
}

static int import_room_code(ImportReader *r, GmlcProject *project,
                            const char *cache_dir, const char *leaf,
                            char **out_path, char *err, size_t errcap){
  char *code = NULL;
  *out_path = NULL;
  if(!import_copy_string(r, &code, "room creation code")) return 0;
  if(code[0]){
    *out_path = import_source_path(project,cache_dir,leaf,code,err,errcap);
    if(!*out_path){
      free(code); free(*out_path); *out_path = NULL;
      return 0;
    }
  }
  free(code);
  return 1;
}

int gmlc_classic_import_rooms(const GmlcClassicManifest *classic,
                              GmlcProject *project, const char *cache_dir,
                              char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->rooms || project->n_rooms){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid room-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_ROOM];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many room slots");
    return 0;
  }
  project->rooms = (GmlcRoom*)calloc(count ? count : 1, sizeof(*project->rooms));
  if(!project->rooms){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating rooms");
    return 0;
  }
  project->n_rooms = project->cap_rooms = (int)count;
  project->next_instance_id = classic->inventory.last_instance_id < INT32_MAX
    ? (int)classic->inventory.last_instance_id + 1 : INT32_MAX;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_ROOM];
  for(uint32_t i = 0; i < count; ++i){
    GmlcRoom *room = &project->rooms[i];
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_room_%u", i);
    const char *name = slots[i].exists && slots[i].name ? slots[i].name : fallback;
    room->id = copy_string(name); room->name = copy_string(name);
    room->width = 640; room->height = 480; room->speed = 30;
    room->background_color = 0xFF000000u; room->draw_background_color = 1;
    for(int view = 0; view < 8; ++view){
      room->views[view].wview = room->views[view].wport = room->width;
      room->views[view].hview = room->views[view].hport = room->height;
      room->views[view].hspeed = room->views[view].vspeed = -1;
      room->views[view].object_id = -1;
    }
    if(!room->id || !room->name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory naming room %u", i);
      free_imported_rooms(project); return 0;
    }
    if(!slots[i].exists) continue;
    ImportReader r = {slots[i].payload, slots[i].payload_size, 0, err, errcap};
    const uint8_t *caption = NULL; uint32_t caption_length = 0;
    uint32_t fields[9]={0};
    if(!import_skip_string(&r, &caption, &caption_length, "room caption")){
      free_imported_rooms(project); return 0;
    }
    room->caption = (char*)malloc((size_t)caption_length + 1u);
    if(!room->caption){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory captioning room %u", i);
      free_imported_rooms(project); return 0;
    }
    if(caption_length) memcpy(room->caption, caption, caption_length);
    room->caption[caption_length] = '\0';
    int compact_legacy=slots[i].legacy_layout && slots[i].executable_layout;
    int room_field_count=compact_legacy?6:9;
    for(int field = 0; field < room_field_count; ++field)
      if(!import_u32(&r, &fields[field], "room field")){
        free_imported_rooms(project); return 0;
      }
    room->width = (int32_t)fields[0]; room->height = (int32_t)fields[1];
    int runtime_fields=compact_legacy?2:5;
    room->speed = (int32_t)fields[runtime_fields];
    room->persistent = fields[runtime_fields+1] != 0;
    room->background_color = fields[runtime_fields+2] | 0xFF000000u;
    room->draw_background_color = fields[runtime_fields+3] != 0;
    if(anygm_host_development_setting(project->host,"GMLC_LOG_ROOM"))
      anygm_host_logf(project ? project->host : NULL,ANYGM_LOG_DEBUG,"[classic-room] index=%u size=%dx%d speed=%d persistent=%d colour=%08x clear=%d caption=<%s>\n",
              i,room->width,room->height,room->speed,room->persistent,
              room->background_color,room->draw_background_color,room->caption);
    char leaf[112];
    snprintf(leaf, sizeof(leaf), "classic_room_%06u_create.gml", i);
    if(!import_room_code(&r,project,cache_dir,leaf,&room->creation_code_path,err,errcap)){
      free_imported_rooms(project); return 0;
    }
    uint32_t backgrounds;
    if(!import_u32(&r, &backgrounds, "room background count") || backgrounds > INT32_MAX){
      free_imported_rooms(project); return 0;
    }
    room->backgrounds = (GmlcRoomBackground*)calloc(backgrounds ? backgrounds : 1,
                                                     sizeof(*room->backgrounds));
    if(!room->backgrounds){ free_imported_rooms(project); return 0; }
    room->n_backgrounds = room->cap_backgrounds = (int)backgrounds;
    for(uint32_t background = 0; background < backgrounds; ++background){
      uint32_t value[10];
      for(int field = 0; field < 10; ++field)
        if(!import_u32(&r, &value[field], "room background field")){
          free_imported_rooms(project); return 0;
        }
      GmlcRoomBackground *bg = &room->backgrounds[background];
      bg->visible = value[0] != 0; bg->foreground = value[1] != 0;
      bg->background_id = (int32_t)value[2]; bg->x = (int32_t)value[3]; bg->y = (int32_t)value[4];
      bg->htiled = value[5] != 0; bg->vtiled = value[6] != 0;
      bg->hspeed = (int32_t)value[7]; bg->vspeed = (int32_t)value[8]; bg->stretch = value[9] != 0;
    }
    uint32_t view_enabled, views;
    if(!import_u32(&r, &view_enabled, "room view-enabled flag") ||
       !import_u32(&r, &views, "room view count") || views > 1024){
      free_imported_rooms(project); return 0;
    }
    room->view_enabled = view_enabled != 0; room->n_views = views < 8 ? (int)views : 8;
    int gm53_room=!compact_legacy && slots[i].version==520u;
    for(uint32_t view = 0; view < views; ++view){
      uint32_t value[14]={0};
      int view_fields=gm53_room?12:14;
      for(int field = 0; field < view_fields; ++field)
        if(!import_u32(&r, &value[field], "room view field")){
          free_imported_rooms(project); return 0;
        }
      if(view >= 8) continue;
      GmlcRoomView *target = &room->views[view];
      target->visible = value[0] != 0;
      target->xview = (int32_t)value[1]; target->yview = (int32_t)value[2];
      target->wview = (int32_t)value[3]; target->hview = (int32_t)value[4];
      target->xport = (int32_t)value[5]; target->yport = (int32_t)value[6];
      if(gm53_room){
        target->wport=target->wview; target->hport=target->hview;
        target->hborder=(int32_t)value[7]; target->vborder=(int32_t)value[8];
        target->hspeed=(int32_t)value[9]; target->vspeed=(int32_t)value[10];
        target->object_id=(int32_t)value[11];
      } else {
        target->wport = (int32_t)value[7]; target->hport = (int32_t)value[8];
        target->hborder = (int32_t)value[9]; target->vborder = (int32_t)value[10];
        target->hspeed = (int32_t)value[11]; target->vspeed = (int32_t)value[12];
        target->object_id = (int32_t)value[13];
      }
    }
    if(room->n_views){
      room->view_w = room->views[0].wview; room->view_h = room->views[0].hview;
      room->port_w = room->views[0].wport; room->port_h = room->views[0].hport;
    } else {
      room->view_w = room->port_w = room->width;
      room->view_h = room->port_h = room->height;
    }
    uint32_t instances;
    if(!import_u32(&r, &instances, "room instance count") || instances > INT32_MAX){
      free_imported_rooms(project); return 0;
    }
    room->instances = (GmlcRoomInstance*)calloc(instances ? instances : 1, sizeof(*room->instances));
    if(!room->instances){ free_imported_rooms(project); return 0; }
    room->n_instances = room->cap_instances = (int)instances;
    for(uint32_t instance = 0; instance < instances; ++instance){
      uint32_t x, y, object_id, instance_id, locked=0;
      if(!import_u32(&r, &x, "room instance x") || !import_u32(&r, &y, "room instance y") ||
         !import_u32(&r, &object_id, "room instance object") ||
         !import_u32(&r, &instance_id, "room instance id")){
        free_imported_rooms(project); return 0;
      }
      GmlcRoomInstance *target = &room->instances[instance];
      char instance_name[80], instance_leaf[128];
      snprintf(instance_name, sizeof(instance_name), "classic_instance_%u", instance_id);
      snprintf(instance_leaf, sizeof(instance_leaf), "classic_room_%06u_instance_%010u.gml", i, instance_id);
      target->id = copy_string(instance_name); target->name = copy_string(instance_name);
      target->x = (int32_t)x; target->y = (int32_t)y; target->object_id = (int32_t)object_id;
      target->instance_id = (int32_t)instance_id; target->sx = target->sy = 1.0f;
      target->color = 0xFFFFFFFFu;
      if(!target->id || !target->name ||
         !import_room_code(&r,project,cache_dir,instance_leaf,&target->creation_code_path,err,errcap) ||
         (!compact_legacy &&
          !import_u32(&r, &locked, "room instance locked flag"))){
        free_imported_rooms(project); return 0;
      }
      (void)locked;
    }
    uint32_t tiles;
    if(!import_u32(&r, &tiles, "room tile count") || tiles > INT32_MAX){
      free_imported_rooms(project); return 0;
    }
    room->tiles = (GmlcRoomTile*)calloc(tiles ? tiles : 1, sizeof(*room->tiles));
    if(!room->tiles){ free_imported_rooms(project); return 0; }
    room->n_tiles = room->cap_tiles = (int)tiles;
    for(uint32_t tile = 0; tile < tiles; ++tile){
      uint32_t value[10]={0};
      int tile_field_count=compact_legacy?9:10;
      for(int field = 0; field < tile_field_count; ++field)
        if(!import_u32(&r, &value[field], "room tile field")){
          free_imported_rooms(project); return 0;
        }
      GmlcRoomTile *target = &room->tiles[tile];
      target->x = (int32_t)value[0]; target->y = (int32_t)value[1];
      target->background_id = (int32_t)value[2]; target->source_x = (int32_t)value[3];
      target->source_y = (int32_t)value[4]; target->width = (int32_t)value[5];
      target->height = (int32_t)value[6]; target->depth = (int32_t)value[7];
      target->tile_id = (int32_t)value[8];
    }
    if(!compact_legacy){
      uint32_t editor_field;
      for(int field = 0; field < (gm53_room?20:14); ++field)
        if(!import_u32(&r, &editor_field, "room editor field")){
          free_imported_rooms(project); return 0;
        }
    }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing room payload");
      free_imported_rooms(project); return 0;
    }
  }
  return 1;
}

int gmlc_classic_import_room_order(const GmlcClassicManifest *classic,
                                   GmlcProject *project,
                                   char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid room-order arguments");
    return 0;
  }
  uint32_t slots_count = classic->inventory.resource_slots[GMLC_CLASSIC_ROOM];
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_ROOM];
  if(project->room_order || project->n_room_order){
    /* The synthetic room used by a genuinely roomless project already owns a
     * complete one-entry order. */
    if(!slots_count && project->room_order && project->n_room_order == 1) return 1;
    if(err && errcap) snprintf(err, errcap, "classic import: room order is already populated");
    return 0;
  }
  if(slots_count && !slots){
    if(err && errcap) snprintf(err, errcap, "classic import: room slots are unavailable");
    return 0;
  }

  uint32_t count = classic->room_order_count;
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many ordered rooms");
    return 0;
  }
  if(!count){
    if(classic->existing[GMLC_CLASSIC_ROOM]){
      if(err && errcap) snprintf(err, errcap, "classic import: explicit room order is unavailable");
      return 0;
    }
    return 1;
  }
  if(count != classic->existing[GMLC_CLASSIC_ROOM]){
    if(err && errcap) snprintf(err, errcap, "classic import: room order does not cover every room");
    return 0;
  }

  int *order = (int*)calloc(count, sizeof(*order));
  unsigned char *seen = (unsigned char*)calloc(slots_count ? slots_count : 1, 1);
  if(!order || !seen){
    free(order); free(seen);
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory recording room order");
    return 0;
  }

  uint32_t written = 0;
  for(uint32_t i = 0; i < classic->room_order_count; ++i){
    uint32_t slot = classic->room_order[i];
    if(slot >= slots_count || !slots[slot].exists || seen[slot]){
      free(order); free(seen);
      if(err && errcap) snprintf(err, errcap, "classic import: invalid room order entry %u", slot);
      return 0;
    }
    seen[slot] = 1;
    order[written++] = (int)slot;
  }
  free(seen);
  project->room_order = order;
  project->n_room_order = (int)written;
  return 1;
}
