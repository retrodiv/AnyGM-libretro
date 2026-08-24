/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Builtin-owned mutable resource lifetime and canonical VM-state sections. */
#include "gml_builtin_internal.h"
#include "gml_vm_state_codec.h"
#include "gml_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void builtin_state_resource_defaults(GmlBuiltinState *state){
  state->next_buffer_id=1;
  state->async_group_status=1;
  state->next_ds_id=1;
  state->ds_map_last_slot=-1;
  state->next_time_source_id=GML_TIME_SOURCE_ID_BASE;
  state->time_source_game_state=1;
  state->listener_forward_z=-1;
  state->listener_up_y=1;
}

GmlBuiltinState *gml_builtin_state_create(GmlVM *vm){
  GmlBuiltinState *state=calloc(1,sizeof(*state));
  if(!state) return NULL;
  state->vm=vm;
  builtin_state_resource_defaults(state);
  state->log_ds=-1;
  state->log_collision=-1;
  state->log_tile_collision=-1;
  state->gamepad_debug=-1;
  state->collision_grid_mode=-1;
  state->log_stub=-1;
  state->fixed_random_seed=-1;
  state->d3_wall_limit_frame=-1;
  return state;
}

GmlBuiltinState *gml_builtin_state_ensure(GmlVM *vm){
  if(!vm) return NULL;
  if(!vm->builtins) vm->builtins=gml_builtin_state_create(vm);
  return vm->builtins;
}

void gml_builtin_state_rebind(GmlBuiltinState *state,GmlVM *vm){
  if(state) state->vm=vm;
}

void file_find_reset(GmlBuiltinState *state){
  if(!state) return;
  for(int i=0;i<state->file_find_count;i++) free(state->file_find_name[i]);
  memset(state->file_find_name,0,sizeof(state->file_find_name));
  state->file_find_count=0;
  state->file_find_index=0;
}

void builtin_state_ini_reset(GmlBuiltinState *state){
  if(!state) return;
  for(int i=0;i<state->ini_n;i++){
    free(state->ini_kv[i].section);
    free(state->ini_kv[i].key);
    free(state->ini_kv[i].sval);
  }
  memset(state->ini_kv,0,sizeof(state->ini_kv));
  state->ini_n=0;
  state->ini_open=0;
  state->ini_path[0]=0;
}

GmlSaudioEntry *builtin_state_saudio_find(GmlBuiltinState *state,const char *id){
  if(!state || !id) return NULL;
  for(uint32_t i=0;i<state->saudio_count;i++)
    if(state->saudio_entries[i].id && !strcmp(state->saudio_entries[i].id,id))
      return &state->saudio_entries[i];
  return NULL;
}

int builtin_state_saudio_store(GmlBuiltinState *state,const char *id,const char *path,
                               const uint8_t content_sha256[32],int sound,int recording){
  if(!state || !id || !id[0] || strlen(id)>4096u || !path || strlen(path)>4096u ||
     (!recording && (!path[0] || !content_sha256))) return 0;
  char *path_copy=strdup(path);
  if(!path_copy) return 0;
  GmlSaudioEntry *existing=builtin_state_saudio_find(state,id);
  if(existing){
    free(existing->path);
    existing->path=path_copy;
    if(content_sha256) memcpy(existing->content_sha256,content_sha256,32);
    else memset(existing->content_sha256,0,32);
    existing->sound=sound;
    existing->recording=recording!=0;
    existing->record_active=0;
    existing->position_ms=0.0;
    return 1;
  }
  if(state->saudio_count>=GML_SAUDIO_ENTRY_MAX){ free(path_copy); return 0; }
  if(state->saudio_count==state->saudio_capacity){
    uint32_t capacity=state->saudio_capacity?state->saudio_capacity*2u:16u;
    if(capacity<state->saudio_capacity || capacity>GML_SAUDIO_ENTRY_MAX)
      capacity=GML_SAUDIO_ENTRY_MAX;
    GmlSaudioEntry *grown=(GmlSaudioEntry*)realloc(
      state->saudio_entries,(size_t)capacity*sizeof(*grown));
    if(!grown){ free(path_copy); return 0; }
    memset(grown+state->saudio_capacity,0,
           (size_t)(capacity-state->saudio_capacity)*sizeof(*grown));
    state->saudio_entries=grown;
    state->saudio_capacity=capacity;
  }
  char *copy=strdup(id);
  if(!copy){ free(path_copy); return 0; }
  GmlSaudioEntry *entry=&state->saudio_entries[state->saudio_count++];
  entry->id=copy;
  entry->path=path_copy;
  if(content_sha256) memcpy(entry->content_sha256,content_sha256,32);
  entry->sound=sound;
  entry->recording=recording!=0;
  entry->record_active=0;
  entry->position_ms=0.0;
  return 1;
}

void builtin_state_saudio_remove(GmlBuiltinState *state,const char *id){
  if(!state || !id) return;
  for(uint32_t i=0;i<state->saudio_count;i++){
    if(!state->saudio_entries[i].id || strcmp(state->saudio_entries[i].id,id)) continue;
    free(state->saudio_entries[i].id);
    free(state->saudio_entries[i].path);
    if(i+1u<state->saudio_count)
      memmove(&state->saudio_entries[i],&state->saudio_entries[i+1u],
              (size_t)(state->saudio_count-i-1u)*sizeof(*state->saudio_entries));
    state->saudio_count--;
    memset(&state->saudio_entries[state->saudio_count],0,
           sizeof(*state->saudio_entries));
    return;
  }
}

void builtin_state_saudio_clear(GmlBuiltinState *state){
  if(!state) return;
  for(uint32_t i=0;i<state->saudio_count;i++){
    free(state->saudio_entries[i].id);
    free(state->saudio_entries[i].path);
  }
  free(state->saudio_entries);
  state->saudio_entries=NULL;
  state->saudio_count=state->saudio_capacity=0;
}

GmlExternalAudioAsset *builtin_state_external_audio_find(
    GmlBuiltinState *state,int sound){
  if(!state || sound<0) return NULL;
  for(uint32_t i=0;i<state->external_audio_asset_count;i++)
    if(state->external_audio_assets[i].sound==sound)
      return &state->external_audio_assets[i];
  return NULL;
}

int builtin_state_external_audio_store(GmlBuiltinState *state,int sound,
                                       const char *path,
                                       const uint8_t content_sha256[32]){
  if(!state || sound<0 || !path || !path[0] || strlen(path)>4096u ||
     !content_sha256) return 0;
  char *path_copy=strdup(path);
  if(!path_copy) return 0;
  GmlExternalAudioAsset *existing=
    builtin_state_external_audio_find(state,sound);
  if(existing){
    free(existing->path);
    existing->path=path_copy;
    memcpy(existing->content_sha256,content_sha256,32);
    return 1;
  }
  if(state->external_audio_asset_count>=GML_EXTERNAL_AUDIO_ASSET_MAX){
    free(path_copy);
    return 0;
  }
  if(state->external_audio_asset_count==state->external_audio_asset_capacity){
    uint32_t capacity=state->external_audio_asset_capacity
      ? state->external_audio_asset_capacity*2u : 16u;
    if(capacity<state->external_audio_asset_capacity ||
       capacity>GML_EXTERNAL_AUDIO_ASSET_MAX)
      capacity=GML_EXTERNAL_AUDIO_ASSET_MAX;
    GmlExternalAudioAsset *grown=(GmlExternalAudioAsset*)realloc(
      state->external_audio_assets,(size_t)capacity*sizeof(*grown));
    if(!grown){ free(path_copy); return 0; }
    memset(grown+state->external_audio_asset_capacity,0,
           (size_t)(capacity-state->external_audio_asset_capacity)*sizeof(*grown));
    state->external_audio_assets=grown;
    state->external_audio_asset_capacity=capacity;
  }
  GmlExternalAudioAsset *asset=
    &state->external_audio_assets[state->external_audio_asset_count++];
  asset->path=path_copy;
  memcpy(asset->content_sha256,content_sha256,32);
  asset->sound=sound;
  return 1;
}

void builtin_state_external_audio_remove(GmlBuiltinState *state,int sound){
  if(!state || sound<0) return;
  for(uint32_t i=0;i<state->external_audio_asset_count;i++){
    if(state->external_audio_assets[i].sound!=sound) continue;
    free(state->external_audio_assets[i].path);
    if(i+1u<state->external_audio_asset_count)
      memmove(&state->external_audio_assets[i],
              &state->external_audio_assets[i+1u],
              (size_t)(state->external_audio_asset_count-i-1u)*
                sizeof(*state->external_audio_assets));
    state->external_audio_asset_count--;
    memset(&state->external_audio_assets[state->external_audio_asset_count],0,
           sizeof(*state->external_audio_assets));
    return;
  }
}

void builtin_state_external_audio_clear(GmlBuiltinState *state){
  if(!state) return;
  for(uint32_t i=0;i<state->external_audio_asset_count;i++)
    free(state->external_audio_assets[i].path);
  free(state->external_audio_assets);
  state->external_audio_assets=NULL;
  state->external_audio_asset_count=state->external_audio_asset_capacity=0;
}

static void builtin_state_ds_reset(GmlBuiltinState *state){
  for(int i=0;i<GML_DS_MAP_MAX;i++){
    GmlDSMap *map=&state->ds_map[i];
    for(int j=0;j<map->len;j++) free(map->entry[j].key);
    free(map->entry);
    free(map->hidx);
    memset(map,0,sizeof(*map));
    map->last_lookup=-1;
  }
  state->ds_map_last_slot=-1;
  for(int i=0;i<GML_DS_LIST_MAX;i++){
    free(state->ds_list[i].item);
    free(state->ds_list[i].child_kind);
    memset(&state->ds_list[i],0,sizeof(state->ds_list[i]));
  }
  for(int i=0;i<GML_DS_GRID_MAX;i++){
    free(state->ds_grid[i].cell);
    memset(&state->ds_grid[i],0,sizeof(state->ds_grid[i]));
  }
  state->next_ds_id=1;
  state->ds_list_compat_repair=0;
}

void gml_builtin_state_visit_values(const GmlBuiltinState *state,
                                    GmlBuiltinValueVisitor visitor,
                                    void *userdata){
  if(!state || !visitor) return;
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(state->ds_map[i].live){
    const GmlDSMap *map=&state->ds_map[i];
    for(int j=0;j<map->len;j++){
      visitor(userdata,map->entry[j].key_val);
      visitor(userdata,map->entry[j].val);
    }
  }
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(state->ds_list[i].live){
    const GmlDSList *list=&state->ds_list[i];
    for(int j=0;j<list->len;j++) visitor(userdata,list->item[j]);
  }
  for(int i=0;i<GML_DS_GRID_MAX;i++) if(state->ds_grid[i].live){
    const GmlDSGrid *grid=&state->ds_grid[i];
    long long cells=(long long)grid->w*grid->h;
    for(long long j=0;j<cells;j++) visitor(userdata,grid->cell[j]);
  }
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(state->time_source[i].live){
    visitor(userdata,state->time_source[i].callback);
    visitor(userdata,state->time_source[i].args);
  }
}

void gml_builtin_state_take_owned_values(GmlBuiltinState *state,
                                         GmlBuiltinValueVisitor visitor,
                                         void *userdata){
  if(!state) return;
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(state->time_source[i].live){
    GmlVal callback=state->time_source[i].callback;
    GmlVal args=state->time_source[i].args;
    state->time_source[i].callback=vreal(0);
    state->time_source[i].args=vreal(0);
    if(visitor){
      visitor(userdata,callback);
      visitor(userdata,args);
    }
  }
}

static void builtin_state_release_owned_values(GmlBuiltinState *state){
  GmlVal values[GML_TIME_SOURCE_MAX*2];
  size_t count=0;
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(state->time_source[i].live){
    values[count++]=state->time_source[i].callback;
    values[count++]=state->time_source[i].args;
    state->time_source[i].callback=vreal(0);
    state->time_source[i].args=vreal(0);
  }
  gml_values_release(values,count);
}

static void builtin_state_physics_reset(GmlBuiltinState *state){
  if(!state) return;
  memset(state->phys_fixture,0,sizeof(state->phys_fixture));
  memset(state->phys_joint,0,sizeof(state->phys_joint));
  state->phys_next_id=0;
  state->phys_gravity_x=0;
  state->phys_gravity_y=0;
  state->phys_update_speed=0;
  state->phys_update_iterations=0;
  state->phys_paused=0;
  state->phys_debug_draw=0;
}

void gml_builtin_physics_room_reset(GmlVM *vm){
  builtin_state_physics_reset(gml_builtin_state_ensure(vm));
}

void gml_builtin_state_reset(GmlBuiltinState *state){
  if(!state) return;
  builtin_state_release_owned_values(state);
  builtin_io_files_close(state);
  for(int i=0;i<16;i++){
    free(state->buffer[i].data);
    memset(&state->buffer[i],0,sizeof(state->buffer[i]));
  }
  builtin_state_ini_reset(state);
  state->next_buffer_id=1;
  state->n_async_sl=0;
  state->n_async_http=0;
  state->async_group_active=0;
  state->async_group_id=0;
  state->async_group_status=1;
  state->async_group_count=0;
  builtin_state_ds_reset(state);
  memset(state->time_source,0,sizeof(state->time_source));
  state->next_time_source_id=GML_TIME_SOURCE_ID_BASE;
  state->time_source_game_state=1;
  memset(state->emitter_live,0,sizeof(state->emitter_live));
  memset(state->emitter_gain,0,sizeof(state->emitter_gain));
  memset(state->emitter_x,0,sizeof(state->emitter_x));
  memset(state->emitter_y,0,sizeof(state->emitter_y));
  memset(state->emitter_z,0,sizeof(state->emitter_z));
  memset(state->emitter_ref,0,sizeof(state->emitter_ref));
  memset(state->emitter_max,0,sizeof(state->emitter_max));
  memset(state->emitter_factor,0,sizeof(state->emitter_factor));
  state->listener_x=state->listener_y=state->listener_z=0;
  state->listener_forward_x=state->listener_forward_y=0;
  state->listener_forward_z=-1;
  state->listener_up_x=state->listener_up_z=0;
  state->listener_up_y=1;
  state->audio_falloff_model=0;
  builtin_state_saudio_clear(state);
  builtin_state_external_audio_clear(state);
  builtin_wwise_state_free(state);
  builtin_jbfmod_state_free(state);
  builtin_state_physics_reset(state);
  file_find_reset(state);
  memset(state->gamepad_deadzone,0,sizeof(state->gamepad_deadzone));
  memset(state->gamepad_deadzone_set,0,sizeof(state->gamepad_deadzone_set));
  state->string_ring_index=0;
  memset(state->fallback_audio_event,0,sizeof(state->fallback_audio_event));
  memset(state->fallback_audio_global_parameter,0,
         sizeof(state->fallback_audio_global_parameter));
  state->fallback_audio_global_parameter_count=0;
  for(int i=0;i<GML_MP_GRID_MAX;i++){
    free(state->mp_grid[i].cell);
    memset(&state->mp_grid[i],0,sizeof(state->mp_grid[i]));
  }
}

void gml_builtin_state_destroy(GmlBuiltinState *state){
  if(!state) return;
  gml_builtin_state_reset(state);
  free(state);
}

int gml_builtin_ini_entry_count(const GmlVM *vm){
  return vm && vm->builtins ? vm->builtins->ini_n : 0;
}

void gml_builtin_state_profile_ds(const GmlBuiltinState *state,
                                  size_t byte_totals[3],int live_counts[3]){
  if(byte_totals) memset(byte_totals,0,3*sizeof(*byte_totals));
  if(live_counts) memset(live_counts,0,3*sizeof(*live_counts));
  if(!state) return;
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(state->ds_map[i].live){
    const GmlDSMap *map=&state->ds_map[i];
    if(live_counts) live_counts[0]++;
    if(byte_totals){
      byte_totals[0]+=8;
      for(int j=0;j<map->len;j++){
        byte_totals[0]+=gml_vm_state_measure_string(state->vm,map->entry[j].key);
        byte_totals[0]+=gml_vm_state_measure_value(state->vm,map->entry[j].key_val);
        byte_totals[0]+=gml_vm_state_measure_value(state->vm,map->entry[j].val);
      }
    }
  }
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(state->ds_list[i].live){
    const GmlDSList *list=&state->ds_list[i];
    if(live_counts) live_counts[1]++;
    if(byte_totals){
      byte_totals[1]+=8;
      for(int j=0;j<list->len;j++)
        byte_totals[1]+=gml_vm_state_measure_value(state->vm,list->item[j]);
    }
  }
  for(int i=0;i<GML_DS_GRID_MAX;i++) if(state->ds_grid[i].live){
    const GmlDSGrid *grid=&state->ds_grid[i];
    if(live_counts) live_counts[2]++;
    if(byte_totals){
      byte_totals[2]+=12;
      long long cells=(long long)grid->w*grid->h;
      for(long long j=0;j<cells;j++)
        byte_totals[2]+=gml_vm_state_measure_value(state->vm,grid->cell[j]);
    }
  }
}

void gml_builtin_state_write_ini_ds(const GmlBuiltinState *state,
                                    GmlVmStateWriter *writer){
  gml_vm_state_write_i32(writer,state->ini_n);
  gml_vm_state_write_i32(writer,state->ini_open);
  gml_vm_state_write_string(writer,state->ini_path);
  for(int i=0;i<state->ini_n;i++){
    gml_vm_state_write_string(writer,state->ini_kv[i].section);
    gml_vm_state_write_string(writer,state->ini_kv[i].key);
    gml_vm_state_write_i32(writer,state->ini_kv[i].is_str);
    gml_vm_state_write_real(writer,state->ini_kv[i].val);
    gml_vm_state_write_string(writer,state->ini_kv[i].sval);
  }
  gml_vm_state_write_i32(writer,state->next_ds_id);
  int map_live=0;
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(state->ds_map[i].live) map_live++;
  gml_vm_state_write_i32(writer,map_live);
  for(int i=0;i<GML_DS_MAP_MAX;i++) if(state->ds_map[i].live){
    const GmlDSMap *map=&state->ds_map[i];
    gml_vm_state_write_u32(writer,map->id);
    gml_vm_state_write_i32(writer,map->len);
    for(int j=0;j<map->len;j++){
      gml_vm_state_write_string(writer,map->entry[j].key);
      gml_vm_state_write_value(writer,map->entry[j].key_val);
      gml_vm_state_write_value(writer,map->entry[j].val);
      gml_vm_state_write_i32(writer,map->entry[j].child_kind);
    }
  }
  int list_live=0;
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(state->ds_list[i].live) list_live++;
  gml_vm_state_write_i32(writer,list_live);
  for(int i=0;i<GML_DS_LIST_MAX;i++) if(state->ds_list[i].live){
    const GmlDSList *list=&state->ds_list[i];
    gml_vm_state_write_u32(writer,list->id);
    gml_vm_state_write_i32(writer,list->len);
    for(int j=0;j<list->len;j++){
      gml_vm_state_write_value(writer,list->item[j]);
      gml_vm_state_write_i32(writer,list->child_kind?list->child_kind[j]:0);
    }
  }
  int grid_live=0;
  for(int i=0;i<GML_DS_GRID_MAX;i++) if(state->ds_grid[i].live) grid_live++;
  gml_vm_state_write_i32(writer,grid_live);
  for(int i=0;i<GML_DS_GRID_MAX;i++) if(state->ds_grid[i].live){
    const GmlDSGrid *grid=&state->ds_grid[i];
    gml_vm_state_write_u32(writer,grid->id);
    gml_vm_state_write_i32(writer,grid->w);
    gml_vm_state_write_i32(writer,grid->h);
    long long cells=(long long)grid->w*grid->h;
    for(long long j=0;j<cells;j++)
      gml_vm_state_write_value(writer,grid->cell[j]);
  }
}

int gml_builtin_state_read_ini_ds(GmlBuiltinState *state,
                                  GmlVmStateReader *reader){
  if(!state || !gml_vm_state_reader_ok(reader)) return 0;
  state->ini_n=gml_vm_state_read_i32(reader);
  state->ini_open=gml_vm_state_read_i32(reader);
  char *path=gml_vm_state_read_string(reader);
  snprintf(state->ini_path,sizeof(state->ini_path),"%s",path?path:"");
  free(path);
  if(state->ini_n<0 || state->ini_n>GML_INI_MAX){
    gml_vm_state_reader_fail(reader,"bad ini count",(uint32_t)state->ini_n);
  }
  int ini_count=gml_vm_state_reader_ok(reader)?state->ini_n:0;
  state->ini_n=0;
  for(int i=0;i<ini_count && gml_vm_state_reader_ok(reader);i++){
    state->ini_kv[i].section=gml_vm_state_read_string(reader);
    state->ini_kv[i].key=gml_vm_state_read_string(reader);
    state->ini_kv[i].is_str=gml_vm_state_read_i32(reader);
    state->ini_kv[i].val=gml_vm_state_read_real(reader);
    state->ini_kv[i].sval=gml_vm_state_read_string(reader);
    state->ini_n++;
  }
  if(!gml_vm_state_reader_ok(reader)) return 0;
  state->next_ds_id=gml_vm_state_read_i32(reader);
  int map_live=gml_vm_state_read_i32(reader);
  if(map_live<0 || map_live>GML_DS_MAP_MAX){
    gml_vm_state_reader_fail(reader,"bad ds_map count",(uint32_t)map_live);
    map_live=0;
  }
  for(int mi=0;mi<map_live && gml_vm_state_reader_ok(reader);mi++){
    int slot=-1;
    for(int i=0;i<GML_DS_MAP_MAX;i++) if(!state->ds_map[i].live){
      slot=i;
      break;
    }
    if(slot<0){
      gml_vm_state_reader_fail(reader,"no ds_map slot",(uint32_t)mi);
      break;
    }
    GmlDSMap *map=&state->ds_map[slot];
    map->live=1;
    map->id=gml_vm_state_read_u32(reader);
    map->len=gml_vm_state_read_i32(reader);
    map->last_lookup=-1;
    if(map->len<0 || map->len>100000){
      gml_vm_state_reader_fail(reader,"bad ds_map len",(uint32_t)map->len);
      map->len=0;
    }
    map->cap=map->len;
    map->entry=map->cap?calloc((size_t)map->cap,sizeof(*map->entry)):NULL;
    if(map->cap && !map->entry){
      gml_vm_state_reader_fail(reader,"ds_map allocation failed",(uint32_t)map->cap);
      map->len=map->cap=0;
      break;
    }
    for(int j=0;j<map->len && gml_vm_state_reader_ok(reader);j++){
      map->entry[j].key=gml_vm_state_read_string(reader);
      map->entry[j].key_val=gml_vm_state_read_value(reader);
      map->entry[j].val=gml_vm_state_read_value(reader);
      int kind=gml_vm_state_read_i32(reader);
      if(kind<0 || kind>2){
        gml_vm_state_reader_fail(reader,"bad ds_map child kind",(uint32_t)kind);
        kind=0;
      }
      map->entry[j].child_kind=(unsigned char)kind;
      gml_arr_mark_escaped(map->entry[j].key_val);
      gml_arr_mark_escaped(map->entry[j].val);
    }
  }
  if(!gml_vm_state_reader_ok(reader)) return 0;
  int list_live=gml_vm_state_read_i32(reader);
  if(list_live<0 || list_live>GML_DS_LIST_MAX){
    gml_vm_state_reader_fail(reader,"bad ds_list count",(uint32_t)list_live);
    list_live=0;
  }
  for(int li=0;li<list_live && gml_vm_state_reader_ok(reader);li++){
    int slot=-1;
    for(int i=0;i<GML_DS_LIST_MAX;i++) if(!state->ds_list[i].live){
      slot=i;
      break;
    }
    if(slot<0){
      gml_vm_state_reader_fail(reader,"no ds_list slot",(uint32_t)li);
      break;
    }
    GmlDSList *list=&state->ds_list[slot];
    list->live=1;
    list->id=gml_vm_state_read_u32(reader);
    list->len=gml_vm_state_read_i32(reader);
    if(list->len<0 || list->len>100000){
      gml_vm_state_reader_fail(reader,"bad ds_list len",(uint32_t)list->len);
      list->len=0;
    }
    list->cap=list->len;
    list->item=list->cap?calloc((size_t)list->cap,sizeof(*list->item)):NULL;
    list->child_kind=list->cap?calloc((size_t)list->cap,1):NULL;
    if(list->cap && (!list->item || !list->child_kind)){
      uint32_t requested=(uint32_t)list->cap;
      free(list->item);
      free(list->child_kind);
      list->item=NULL;
      list->child_kind=NULL;
      list->len=list->cap=0;
      gml_vm_state_reader_fail(reader,"ds_list allocation failed",requested);
      break;
    }
    for(int j=0;j<list->len && gml_vm_state_reader_ok(reader);j++){
      list->item[j]=gml_vm_state_read_value(reader);
      int kind=gml_vm_state_read_i32(reader);
      if(kind<0 || kind>2){
        gml_vm_state_reader_fail(reader,"bad ds_list child kind",(uint32_t)kind);
        kind=0;
      }
      list->child_kind[j]=(unsigned char)kind;
      gml_arr_mark_escaped(list->item[j]);
    }
  }
  if(!gml_vm_state_reader_ok(reader)) return 0;
  int grid_live=gml_vm_state_read_i32(reader);
  if(grid_live<0 || grid_live>GML_DS_GRID_MAX){
    gml_vm_state_reader_fail(reader,"bad ds_grid count",(uint32_t)grid_live);
    grid_live=0;
  }
  for(int gi=0;gi<grid_live && gml_vm_state_reader_ok(reader);gi++){
    int slot=-1;
    for(int i=0;i<GML_DS_GRID_MAX;i++) if(!state->ds_grid[i].live){
      slot=i;
      break;
    }
    if(slot<0){
      gml_vm_state_reader_fail(reader,"no ds_grid slot",(uint32_t)gi);
      break;
    }
    GmlDSGrid *grid=&state->ds_grid[slot];
    grid->live=1;
    grid->id=gml_vm_state_read_u32(reader);
    grid->w=gml_vm_state_read_i32(reader);
    grid->h=gml_vm_state_read_i32(reader);
    long long cells=(long long)grid->w*grid->h;
    if(grid->w<0 || grid->h<0 || cells>8000000){
      gml_vm_state_reader_fail(reader,"bad ds_grid size",(uint32_t)cells);
      grid->w=grid->h=0;
      cells=0;
    }
    grid->cell=cells?calloc((size_t)cells,sizeof(*grid->cell)):NULL;
    if(cells && !grid->cell){
      grid->w=grid->h=0;
      gml_vm_state_reader_fail(reader,"ds_grid allocation failed",(uint32_t)cells);
      break;
    }
    for(long long j=0;j<cells && gml_vm_state_reader_ok(reader);j++){
      grid->cell[j]=gml_vm_state_read_value(reader);
      gml_arr_mark_escaped(grid->cell[j]);
    }
  }
  return gml_vm_state_reader_ok(reader);
}

/* Preserve live motion-planning grids across state restoration. A grid may have been
 * constructed at runtime and need not be reconstructed after loading a state. */
void gml_builtin_state_write_mp_grids(const GmlBuiltinState *state,
                                      GmlVmStateWriter *writer){
  int live=0;
  for(int i=0;i<GML_MP_GRID_MAX;i++) if(state->mp_grid[i].live) live++;
  gml_vm_state_write_i32(writer,live);
  for(int i=0;i<GML_MP_GRID_MAX;i++){
    const GmlMpGrid *g=&state->mp_grid[i];
    if(!g->live) continue;
    gml_vm_state_write_i32(writer,i);
    gml_vm_state_write_real(writer,g->left);
    gml_vm_state_write_real(writer,g->top);
    gml_vm_state_write_i32(writer,g->hc);
    gml_vm_state_write_i32(writer,g->vc);
    gml_vm_state_write_i32(writer,g->cw);
    gml_vm_state_write_i32(writer,g->ch);
    gml_vm_state_write_raw(writer,g->cell,(size_t)g->hc*(size_t)g->vc);
  }
}

int gml_builtin_state_read_mp_grids(GmlBuiltinState *state,
                                    GmlVmStateReader *reader){
  if(!state || !gml_vm_state_reader_ok(reader)) return 0;
  for(int i=0;i<GML_MP_GRID_MAX;i++){
    free(state->mp_grid[i].cell);
    memset(&state->mp_grid[i],0,sizeof(state->mp_grid[i]));
  }
  int live=gml_vm_state_read_i32(reader);
  if(live<0 || live>GML_MP_GRID_MAX) return 0;
  for(int k=0;k<live;k++){
    int index=gml_vm_state_read_i32(reader);
    if(index<0 || index>=GML_MP_GRID_MAX) return 0;
    GmlMpGrid *g=&state->mp_grid[index];
    if(g->live) return 0;                       /* one record per slot */
    double left=gml_vm_state_read_real(reader);
    double top=gml_vm_state_read_real(reader);
    int hc=gml_vm_state_read_i32(reader);
    int vc=gml_vm_state_read_i32(reader);
    int cw=gml_vm_state_read_i32(reader);
    int ch=gml_vm_state_read_i32(reader);
    /* The same bound mp_grid_create refuses at, so a corrupt count cannot ask for an allocation
     * the creating call would never have made. */
    if(hc<1 || vc<1 || (long)hc*vc>GML_MP_GRID_MAX_CELLS) return 0;
    if(cw<1) cw=1;
    if(ch<1) ch=1;
    uint8_t *cell=calloc((size_t)hc*(size_t)vc,1);
    if(!cell) return 0;
    gml_vm_state_read_raw(reader,cell,(size_t)hc*(size_t)vc);
    if(!gml_vm_state_reader_ok(reader)){ free(cell); return 0; }
    g->live=1; g->left=left; g->top=top;
    g->hc=hc; g->vc=vc; g->cw=cw; g->ch=ch; g->cell=cell;
  }
  return gml_vm_state_reader_ok(reader);
}

void gml_builtin_state_write_physics(const GmlBuiltinState *state,
                                     GmlVmStateWriter *writer){
  gml_vm_state_write_u32(writer,state->phys_next_id);
  gml_vm_state_write_real(writer,state->phys_gravity_x);
  gml_vm_state_write_real(writer,state->phys_gravity_y);
  gml_vm_state_write_real(writer,state->phys_update_speed);
  gml_vm_state_write_i32(writer,state->phys_update_iterations);
  gml_vm_state_write_i32(writer,state->phys_paused);
  gml_vm_state_write_i32(writer,state->phys_debug_draw);
  int fixture_live=0;
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++)
    if(state->phys_fixture[i].live) fixture_live++;
  gml_vm_state_write_i32(writer,fixture_live);
  for(int i=0;i<GML_PHYS_FIXTURE_MAX;i++) if(state->phys_fixture[i].live){
    const GmlPhysicsFixture *fixture=&state->phys_fixture[i];
    gml_vm_state_write_u32(writer,fixture->id);
    gml_vm_state_write_i32(writer,fixture->shape);
    gml_vm_state_write_i32(writer,fixture->bound_inst);
    gml_vm_state_write_i32(writer,fixture->points);
    gml_vm_state_write_real(writer,fixture->density);
    gml_vm_state_write_real(writer,fixture->friction);
    gml_vm_state_write_real(writer,fixture->restitution);
    gml_vm_state_write_real(writer,fixture->lin_damp);
    gml_vm_state_write_real(writer,fixture->ang_damp);
    gml_vm_state_write_real(writer,fixture->awake);
    gml_vm_state_write_real(writer,fixture->radius);
    gml_vm_state_write_real(writer,fixture->w);
    gml_vm_state_write_real(writer,fixture->h);
    gml_vm_state_write_real(writer,fixture->x1);
    gml_vm_state_write_real(writer,fixture->y1);
    gml_vm_state_write_real(writer,fixture->x2);
    gml_vm_state_write_real(writer,fixture->y2);
    for(int p=0;p<GML_PHYS_FIXTURE_POINTS;p++){
      gml_vm_state_write_real(writer,fixture->px[p]);
      gml_vm_state_write_real(writer,fixture->py[p]);
    }
  }
  int joint_live=0;
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++)
    if(state->phys_joint[i].live) joint_live++;
  gml_vm_state_write_i32(writer,joint_live);
  for(int i=0;i<GML_PHYS_JOINT_MAX;i++) if(state->phys_joint[i].live){
    const GmlPhysicsJoint *joint=&state->phys_joint[i];
    gml_vm_state_write_u32(writer,joint->id);
    gml_vm_state_write_i32(writer,joint->type);
    gml_vm_state_write_i32(writer,joint->value_count);
    gml_vm_state_write_real(writer,joint->a);
    gml_vm_state_write_real(writer,joint->b);
    gml_vm_state_write_real(writer,joint->x1);
    gml_vm_state_write_real(writer,joint->y1);
    gml_vm_state_write_real(writer,joint->x2);
    gml_vm_state_write_real(writer,joint->y2);
    for(int p=0;p<24;p++)
      gml_vm_state_write_real(writer,joint->params[p]);
  }
}

int gml_builtin_state_read_physics(GmlBuiltinState *state,
                                   GmlVmStateReader *reader){
  if(!state || !gml_vm_state_reader_ok(reader)) return 0;
  state->phys_next_id=gml_vm_state_read_u32(reader);
  state->phys_gravity_x=gml_vm_state_read_real(reader);
  state->phys_gravity_y=gml_vm_state_read_real(reader);
  state->phys_update_speed=gml_vm_state_read_real(reader);
  state->phys_update_iterations=gml_vm_state_read_i32(reader);
  state->phys_paused=gml_vm_state_read_i32(reader);
  state->phys_debug_draw=gml_vm_state_read_i32(reader);
  int fixture_live=gml_vm_state_read_i32(reader);
  if(fixture_live<0 || fixture_live>GML_PHYS_FIXTURE_MAX){
    gml_vm_state_reader_fail(reader,"bad physics fixture count",
                             (uint32_t)fixture_live);
    fixture_live=0;
  }
  for(int i=0;i<fixture_live && gml_vm_state_reader_ok(reader);i++){
    GmlPhysicsFixture *fixture=&state->phys_fixture[i];
    memset(fixture,0,sizeof(*fixture));
    fixture->live=1;
    fixture->id=gml_vm_state_read_u32(reader);
    fixture->shape=gml_vm_state_read_i32(reader);
    fixture->bound_inst=gml_vm_state_read_i32(reader);
    fixture->points=gml_vm_state_read_i32(reader);
    if(fixture->points<0) fixture->points=0;
    if(fixture->points>GML_PHYS_FIXTURE_POINTS)
      fixture->points=GML_PHYS_FIXTURE_POINTS;
    fixture->density=gml_vm_state_read_real(reader);
    fixture->friction=gml_vm_state_read_real(reader);
    fixture->restitution=gml_vm_state_read_real(reader);
    fixture->lin_damp=gml_vm_state_read_real(reader);
    fixture->ang_damp=gml_vm_state_read_real(reader);
    fixture->awake=gml_vm_state_read_real(reader);
    fixture->radius=gml_vm_state_read_real(reader);
    fixture->w=gml_vm_state_read_real(reader);
    fixture->h=gml_vm_state_read_real(reader);
    fixture->x1=gml_vm_state_read_real(reader);
    fixture->y1=gml_vm_state_read_real(reader);
    fixture->x2=gml_vm_state_read_real(reader);
    fixture->y2=gml_vm_state_read_real(reader);
    for(int p=0;p<GML_PHYS_FIXTURE_POINTS;p++){
      fixture->px[p]=gml_vm_state_read_real(reader);
      fixture->py[p]=gml_vm_state_read_real(reader);
    }
  }
  int joint_live=gml_vm_state_read_i32(reader);
  if(joint_live<0 || joint_live>GML_PHYS_JOINT_MAX){
    gml_vm_state_reader_fail(reader,"bad physics joint count",
                             (uint32_t)joint_live);
    joint_live=0;
  }
  for(int i=0;i<joint_live && gml_vm_state_reader_ok(reader);i++){
    GmlPhysicsJoint *joint=&state->phys_joint[i];
    memset(joint,0,sizeof(*joint));
    joint->live=1;
    joint->id=gml_vm_state_read_u32(reader);
    joint->type=gml_vm_state_read_i32(reader);
    joint->value_count=gml_vm_state_read_i32(reader);
    if(joint->value_count<0) joint->value_count=0;
    if(joint->value_count>24) joint->value_count=24;
    joint->a=gml_vm_state_read_real(reader);
    joint->b=gml_vm_state_read_real(reader);
    joint->x1=gml_vm_state_read_real(reader);
    joint->y1=gml_vm_state_read_real(reader);
    joint->x2=gml_vm_state_read_real(reader);
    joint->y2=gml_vm_state_read_real(reader);
    for(int p=0;p<24;p++)
      joint->params[p]=gml_vm_state_read_real(reader);
  }
  return gml_vm_state_reader_ok(reader);
}

void gml_builtin_state_write_audio(const GmlBuiltinState *state,
                                   GmlVmStateWriter *writer){
  gml_vm_state_write_raw(writer,state->emitter_live,
                         sizeof(state->emitter_live));
  for(int i=0;i<GML_MAX_EMITTERS;i++){
    gml_vm_state_write_real(writer,state->emitter_gain[i]);
    gml_vm_state_write_real(writer,state->emitter_x[i]);
    gml_vm_state_write_real(writer,state->emitter_y[i]);
    gml_vm_state_write_real(writer,state->emitter_z[i]);
    gml_vm_state_write_real(writer,state->emitter_ref[i]);
    gml_vm_state_write_real(writer,state->emitter_max[i]);
    gml_vm_state_write_real(writer,state->emitter_factor[i]);
  }
  gml_vm_state_write_real(writer,state->listener_x);
  gml_vm_state_write_real(writer,state->listener_y);
  gml_vm_state_write_real(writer,state->listener_z);
  gml_vm_state_write_real(writer,state->listener_forward_x);
  gml_vm_state_write_real(writer,state->listener_forward_y);
  gml_vm_state_write_real(writer,state->listener_forward_z);
  gml_vm_state_write_real(writer,state->listener_up_x);
  gml_vm_state_write_real(writer,state->listener_up_y);
  gml_vm_state_write_real(writer,state->listener_up_z);
  gml_vm_state_write_i32(writer,state->audio_falloff_model);
  gml_vm_state_write_u32(writer,state->external_audio_asset_count);
  for(uint32_t i=0;i<state->external_audio_asset_count;i++){
    const GmlExternalAudioAsset *asset=&state->external_audio_assets[i];
    gml_vm_state_write_i32(writer,asset->sound);
    gml_vm_state_write_string(writer,asset->path);
    gml_vm_state_write_raw(writer,asset->content_sha256,
                           sizeof(asset->content_sha256));
  }
  gml_vm_state_write_u32(writer,state->saudio_count);
  for(uint32_t i=0;i<state->saudio_count;i++){
    const GmlSaudioEntry *entry=&state->saudio_entries[i];
    gml_vm_state_write_string(writer,entry->id);
    gml_vm_state_write_string(writer,entry->path);
    gml_vm_state_write_raw(writer,entry->content_sha256,sizeof(entry->content_sha256));
    gml_vm_state_write_i32(writer,entry->sound);
    gml_vm_state_write_i32(writer,entry->recording!=0);
    gml_vm_state_write_i32(writer,entry->record_active!=0);
    gml_vm_state_write_real(writer,entry->position_ms);
  }
  gml_vm_state_write_string(writer,builtin_wwise_base_path(state));
  uint32_t wwise_count=builtin_wwise_bank_count(state);
  gml_vm_state_write_u32(writer,wwise_count);
  for(uint32_t index=0;index<wwise_count;index++){
    gml_vm_state_write_string(writer,builtin_wwise_bank_path(state,index));
    const uint8_t *digest=builtin_wwise_bank_digest(state,index);
    uint8_t zero_digest[32]={0};
    gml_vm_state_write_raw(writer,digest?digest:zero_digest,32u);
  }
}

int gml_builtin_state_read_audio(GmlBuiltinState *state,
                                 GmlVmStateReader *reader){
  if(!state || !gml_vm_state_reader_ok(reader)) return 0;
  gml_vm_state_read_raw(reader,state->emitter_live,sizeof(state->emitter_live));
  for(int i=0;i<GML_MAX_EMITTERS;i++){
    state->emitter_gain[i]=gml_vm_state_read_real(reader);
    state->emitter_x[i]=gml_vm_state_read_real(reader);
    state->emitter_y[i]=gml_vm_state_read_real(reader);
    state->emitter_z[i]=gml_vm_state_read_real(reader);
    state->emitter_ref[i]=gml_vm_state_read_real(reader);
    state->emitter_max[i]=gml_vm_state_read_real(reader);
    state->emitter_factor[i]=gml_vm_state_read_real(reader);
    if(!isfinite(state->emitter_gain[i]) || state->emitter_gain[i]<0 ||
       !isfinite(state->emitter_x[i]) || !isfinite(state->emitter_y[i]) ||
       !isfinite(state->emitter_z[i]) || !isfinite(state->emitter_ref[i]) ||
       !isfinite(state->emitter_max[i]) ||
       !isfinite(state->emitter_factor[i])){
      gml_vm_state_reader_fail(reader,"bad audio emitter",(uint32_t)i);
    }
  }
  state->listener_x=gml_vm_state_read_real(reader);
  state->listener_y=gml_vm_state_read_real(reader);
  state->listener_z=gml_vm_state_read_real(reader);
  state->listener_forward_x=gml_vm_state_read_real(reader);
  state->listener_forward_y=gml_vm_state_read_real(reader);
  state->listener_forward_z=gml_vm_state_read_real(reader);
  state->listener_up_x=gml_vm_state_read_real(reader);
  state->listener_up_y=gml_vm_state_read_real(reader);
  state->listener_up_z=gml_vm_state_read_real(reader);
  state->audio_falloff_model=gml_vm_state_read_i32(reader);
  if(state->audio_falloff_model<0 || state->audio_falloff_model>6)
    gml_vm_state_reader_fail(reader,"bad audio falloff model",
                             (uint32_t)state->audio_falloff_model);
  uint32_t asset_count=gml_vm_state_read_u32(reader);
  if(asset_count>GML_EXTERNAL_AUDIO_ASSET_MAX)
    gml_vm_state_reader_fail(reader,"too many external audio assets",asset_count);
  for(uint32_t i=0;i<asset_count && gml_vm_state_reader_ok(reader);i++){
    int sound=gml_vm_state_read_i32(reader);
    char *path=gml_vm_state_read_string(reader);
    uint8_t digest[32];
    gml_vm_state_read_raw(reader,digest,sizeof(digest));
    if(sound<0 || !path || !path[0] || strlen(path)>4096u ||
       builtin_state_external_audio_find(state,sound) ||
       !builtin_external_audio_restore(state->vm,path,sound,digest) ||
       !builtin_state_external_audio_store(state,sound,path,digest)){
      if(sound>=0) gml_audio_caster_free((GmlAudio*)state->vm->audio,sound);
      free(path);
      gml_vm_state_reader_fail(reader,"bad external audio asset",i);
      break;
    }
    free(path);
  }
  uint32_t saudio_count=gml_vm_state_read_u32(reader);
  if(saudio_count>GML_SAUDIO_ENTRY_MAX)
    gml_vm_state_reader_fail(reader,"too many Saudio entries",saudio_count);
  for(uint32_t i=0;i<saudio_count && gml_vm_state_reader_ok(reader);i++){
    char *id=gml_vm_state_read_string(reader);
    char *path=gml_vm_state_read_string(reader);
    uint8_t digest[32];
    gml_vm_state_read_raw(reader,digest,sizeof(digest));
    int sound=gml_vm_state_read_i32(reader);
    int recording=gml_vm_state_read_i32(reader);
    int record_active=gml_vm_state_read_i32(reader);
    double position_ms=gml_vm_state_read_real(reader);
    GmlExternalAudioAsset *asset=
      builtin_state_external_audio_find(state,sound);
    int duplicate_sound=0;
    if(!recording)
      for(uint32_t previous=0;previous<state->saudio_count;previous++)
        if(!state->saudio_entries[previous].recording &&
           state->saudio_entries[previous].sound==sound){
          duplicate_sound=1;
          break;
        }
    if(!id || !id[0] || strlen(id)>4096u || !path || strlen(path)>4096u ||
       builtin_state_saudio_find(state,id) ||
       duplicate_sound ||
       (recording!=0 && recording!=1) ||
       (record_active!=0 && record_active!=1) ||
       !isfinite(position_ms) || position_ms<0.0 ||
       (!recording && (sound<0 || !path[0] || !asset ||
        strcmp(asset->path,path) ||
        memcmp(asset->content_sha256,digest,sizeof(digest)))) ||
       (recording && sound!=-1)){
      free(path); free(id);
      gml_vm_state_reader_fail(reader,"bad Saudio entry",i);
      break;
    }
    if(!builtin_state_saudio_store(state,id,path,digest,sound,recording)){
      free(path); free(id);
      gml_vm_state_reader_fail(reader,"Saudio entry allocation",i);
      break;
    }
    GmlSaudioEntry *entry=builtin_state_saudio_find(state,id);
    free(path);
    free(id);
    if(!entry){
      gml_vm_state_reader_fail(reader,"missing Saudio entry",i);
      break;
    }
    entry->record_active=record_active;
    entry->position_ms=position_ms;
  }
  if(gml_vm_state_reader_ok(reader)){
    char *base=gml_vm_state_read_string(reader);
    uint32_t count=gml_vm_state_read_u32(reader);
    char *paths[16]={0};
    uint8_t digests[16][32]={{0}};
    if(!base || strlen(base)>=768u || count>16u)
      gml_vm_state_reader_fail(reader,"bad Wwise bank header",count);
    for(uint32_t index=0;index<count && gml_vm_state_reader_ok(reader);index++){
      paths[index]=gml_vm_state_read_string(reader);
      if(!paths[index] || !paths[index][0] || strlen(paths[index])>=1536u)
        gml_vm_state_reader_fail(reader,"bad Wwise bank path",index);
      gml_vm_state_read_raw(reader,digests[index],sizeof(digests[index]));
    }
    if(gml_vm_state_reader_ok(reader) &&
       !builtin_wwise_state_restore(state,base,(const char *const *)paths,digests,count))
      gml_vm_state_reader_fail(reader,"Wwise bank restore",count);
    for(uint32_t index=0;index<16u;index++) free(paths[index]);
    free(base);
  }
  return gml_vm_state_reader_ok(reader);
}

void gml_builtin_state_write_time_sources(const GmlBuiltinState *state,
                                          GmlVmStateWriter *writer){
  gml_vm_state_write_u32(
    writer,state->next_time_source_id>=GML_TIME_SOURCE_ID_BASE
      ? state->next_time_source_id : GML_TIME_SOURCE_ID_BASE);
  gml_vm_state_write_i32(
    writer,state->time_source_game_state>=1 &&
           state->time_source_game_state<=3
      ? state->time_source_game_state : 1);
  int live=0;
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++)
    if(state->time_source[i].live) live++;
  gml_vm_state_write_i32(writer,live);
  for(int i=0;i<GML_TIME_SOURCE_MAX;i++) if(state->time_source[i].live){
    const GmlTimeSource *source=&state->time_source[i];
    gml_vm_state_write_u32(writer,source->id);
    gml_vm_state_write_i32(writer,source->parent);
    gml_vm_state_write_real(writer,source->period);
    gml_vm_state_write_real(writer,source->remaining);
    gml_vm_state_write_i32(writer,source->units);
    gml_vm_state_write_i32(writer,source->state);
    gml_vm_state_write_i32(writer,source->repetitions);
    gml_vm_state_write_i32(writer,source->reps_remaining);
    gml_vm_state_write_i32(writer,source->reps_completed);
    gml_vm_state_write_i32(writer,source->expiry_type);
    gml_vm_state_write_value(writer,source->callback);
    gml_vm_state_write_value(writer,source->args);
  }
}

int gml_builtin_state_read_time_sources(GmlBuiltinState *state,
                                        GmlVmStateReader *reader){
  if(!state || !gml_vm_state_reader_ok(reader)) return 0;
  state->next_time_source_id=gml_vm_state_read_u32(reader);
  state->time_source_game_state=gml_vm_state_read_i32(reader);
  int live=gml_vm_state_read_i32(reader);
  if(state->next_time_source_id<GML_TIME_SOURCE_ID_BASE ||
     live<0 || live>GML_TIME_SOURCE_MAX ||
     state->time_source_game_state<1 ||
     state->time_source_game_state>3){
    gml_vm_state_reader_fail(reader,"bad time source header",(uint32_t)live);
    live=0;
  }
  for(int i=0;i<live && gml_vm_state_reader_ok(reader);i++){
    GmlTimeSource *source=&state->time_source[i];
    source->live=1;
    source->id=gml_vm_state_read_u32(reader);
    source->parent=gml_vm_state_read_i32(reader);
    source->period=gml_vm_state_read_real(reader);
    source->remaining=gml_vm_state_read_real(reader);
    source->units=gml_vm_state_read_i32(reader);
    source->state=gml_vm_state_read_i32(reader);
    source->repetitions=gml_vm_state_read_i32(reader);
    source->reps_remaining=gml_vm_state_read_i32(reader);
    source->reps_completed=gml_vm_state_read_i32(reader);
    source->expiry_type=gml_vm_state_read_i32(reader);
    source->callback=gml_vm_state_read_value(reader);
    source->args=gml_vm_state_read_value(reader);
    gml_arr_mark_escaped(source->callback);
    gml_arr_mark_escaped(source->args);
    if(source->id<GML_TIME_SOURCE_ID_BASE ||
       !isfinite(source->period) || source->period<0 ||
       !isfinite(source->remaining) || source->remaining<0 ||
       source->units<0 || source->units>1 ||
       source->state<0 || source->state>3 ||
       source->repetitions<-1 || source->reps_remaining<-1 ||
       source->reps_completed<0 ||
       source->expiry_type<0 || source->expiry_type>1 ||
       (source->args.t!=V_ARR && source->args.t!=V_UNDEF)){
      gml_vm_state_reader_fail(reader,"bad time source",source->id);
    }
  }
  for(int i=0;i<live && gml_vm_state_reader_ok(reader);i++){
    int parent=state->time_source[i].parent;
    int found=parent==0 || parent==1;
    for(int j=0;j<live && !found;j++)
      found=(int)state->time_source[j].id==parent;
    if(!found)
      gml_vm_state_reader_fail(reader,"bad time source parent",
                               (uint32_t)parent);
  }
  return gml_vm_state_reader_ok(reader);
}
