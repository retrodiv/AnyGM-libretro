/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The save root retains persistent content data; the separate cache root
 * holds rebuildable loader data. */
#include "libretro_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

void retro_set_environment(retro_environment_t callback);
void retro_init(void);
void retro_deinit(void);
bool retro_load_game(const struct retro_game_info *info);
void retro_unload_game(void);

/* What the frontend answers for the save directory, and what the loader was handed for the run. */
static const char *offered_directory;
static char loaded_save_directory[2048];
static char loaded_cache_directory[2048];
static int loaded_save_present;
static int loaded_cache_present;
static int failures;
static AnygmHostServices engine_host;
static const char *current_option;
static const char *all_option;
static int exercise_cache;
static int fail_preparation;
static int delay_start;
static int expect_empty_cache;
static int prepare_calls;
static int teardown_calls;
static void *held_file;
static void *held_mapping;
static const void *mapped_bytes;
static size_t mapped_size;
static char held_path[3072];
static char cache_root[2048];

static int exists(const char *path){ struct stat info; return stat(path,&info)==0; }
static void require(int condition,const char *message){
  if(!condition){ fprintf(stderr,"cache lifecycle: %s\n",message); failures++; }
}
static void write_file(const char *path){
  void *file=engine_host.file_open(NULL,path,ANYGM_FILE_WRITE|ANYGM_FILE_CREATE|ANYGM_FILE_TRUNCATE);
  if(!file) fprintf(stderr,"fixture open failed: %s\n",path);
  require(file!=NULL,"fixture file can be opened");
  if(file){
    require(engine_host.file_write(NULL,file,"cache",5)==5,"fixture bytes written");
    engine_host.file_close(NULL,file);
  }
}
static void make_directory(const char *path){
  require(engine_host.directory_create(NULL,path)==ANYGM_OK,"fixture directory created");
}
static void release_content_files(void){
  if(held_file || held_mapping){
    require(exists(held_path),"cache still exists when engine teardown releases its handles");
    teardown_calls++;
  }
  if(held_file) engine_host.file_close(NULL,held_file);
  if(held_mapping) engine_host.file_unmap(NULL,held_mapping,mapped_bytes,mapped_size);
  held_file=held_mapping=NULL;
}

static bool environment_callback(unsigned command,void *data){
  if(command==RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY){
    if(!offered_directory) return false;
    *(const char **)data=offered_directory;
    return true;
  }
  if(command==RETRO_ENVIRONMENT_SET_PIXEL_FORMAT) return true;
  return false;
}

AnygmResult anygm_create(const AnygmHostServices *services,AnygmEngine **engine){
  if(!services || !engine) return ANYGM_ERROR_INVALID_ARGUMENT;
  engine_host=*services;
  *engine=(AnygmEngine *)(uintptr_t)1u;
  return ANYGM_OK;
}
void anygm_destroy(AnygmEngine *engine){ (void)engine; release_content_files(); }
AnygmResult anygm_load_prepare(AnygmEngine *engine,const AnygmContentSource *source,
                               const AnygmLoadConfig *config,AnygmContentInfo *info){
  (void)engine; (void)config;
  if(info) info->flags=0;
  if(!source) return ANYGM_ERROR_INVALID_ARGUMENT;
  loaded_save_present=source->save_directory!=NULL;
  loaded_cache_present=source->cache_directory!=NULL;
  snprintf(loaded_save_directory,sizeof loaded_save_directory,"%s",
           source->save_directory?source->save_directory:"");
  snprintf(loaded_cache_directory,sizeof loaded_cache_directory,"%s",
           source->cache_directory?source->cache_directory:"");
  prepare_calls++;
  if(exercise_cache){
    char directory[3072],path[4096];
    snprintf(cache_root,sizeof cache_root,"%s",source->cache_directory);
    if(expect_empty_cache){
      snprintf(path,sizeof path,"%s/untouched/.hidden",cache_root);
      require(!exists(path),"global cleanup precedes load preparation");
      snprintf(path,sizeof path,"%s/.old-marker",cache_root);
      require(!exists(path),"global cleanup includes root dotfiles");
    }
    make_directory(cache_root);
    snprintf(directory,sizeof directory,"%s/%s-anygm-archive",cache_root,source->path);
    make_directory(directory);
    snprintf(held_path,sizeof held_path,"%s/%s-anygm-archive/data.win",cache_root,source->path);
    AnygmFileInfo status={0}; status.struct_size=sizeof status;
    /* Warm loads touch existing cache bytes only through the real adapter VFS. */
    if(engine_host.file_stat(NULL,held_path,&status)!=ANYGM_OK) write_file(held_path);
    held_file=engine_host.file_open(NULL,held_path,ANYGM_FILE_READ);
    held_mapping=engine_host.file_map(NULL,held_path,&mapped_bytes,&mapped_size);
    require(held_file && held_mapping,"content holds an open file and mapping until teardown");
    snprintf(path,sizeof path,"%s/.hidden",directory);
    /* Keep warm-cache bytes intact; Windows refuses truncating an existing hidden file. */
    if(engine_host.file_stat(NULL,path,&status)!=ANYGM_OK) write_file(path);
#ifdef _WIN32
    require(SetFileAttributesA(path,FILE_ATTRIBUTE_HIDDEN)!=0,"native hidden cache file created");
#endif
    for(unsigned i=0;i<12;i++){
      size_t length=strlen(directory);
      require(length+7<sizeof directory,"nested fixture path fits");
      if(length+7>=sizeof directory) break;
      memcpy(directory+length,"/level",7);
      make_directory(directory);
    }
    snprintf(path,sizeof path,"%s/leaf",directory); write_file(path);
    snprintf(directory,sizeof directory,"%s/input-derived",cache_root); make_directory(directory);
    snprintf(path,sizeof path,"%s/data.win",directory); write_file(path);
    snprintf(directory,sizeof directory,"%s/compiled.staging",cache_root); make_directory(directory);
    snprintf(path,sizeof path,"%s/data.win",directory); write_file(path);
    char destination[3072];
    snprintf(destination,sizeof destination,"%s/compiled-anygm-classic",cache_root);
    if(!exists(destination))
      require(engine_host.path_rename(NULL,directory,destination)==ANYGM_OK,
              "staging rename publishes a cache directory");
    snprintf(path,sizeof path,"%s/data.win",destination);
    void *compiled_file=engine_host.file_open(NULL,path,ANYGM_FILE_READ);
    require(compiled_file!=NULL,"warm compiled content is read through the host VFS");
    if(compiled_file) engine_host.file_close(NULL,compiled_file);
    if(fail_preparation){ release_content_files(); return ANYGM_ERROR_INVALID_CONTENT; }
  }
  return ANYGM_OK;
}
AnygmResult anygm_load_start(AnygmEngine *engine){ (void)engine; return ANYGM_OK; }
AnygmResult anygm_load(AnygmEngine *engine,const AnygmContentSource *source,
                       const AnygmLoadConfig *config){
  return anygm_load_prepare(engine,source,config,NULL);
}
void anygm_unload(AnygmEngine *engine){ (void)engine; release_content_files(); }
AnygmResult anygm_reset(AnygmEngine *engine){ (void)engine; return ANYGM_OK; }
AnygmResult anygm_get_av_info(const AnygmEngine *engine,AnygmAvInfo *info){
  (void)engine;
  if(info){
    info->base_width=info->max_width=320;
    info->base_height=info->max_height=240;
    info->aspect_ratio=4.0/3.0;
    info->frames_per_second=60.0;
    info->audio_rate=44100u;
  }
  return ANYGM_OK;
}
AnygmResult anygm_run_frame(AnygmEngine *engine,const AnygmInputFrame *input,
                            AnygmFrameOutput *output){
  (void)engine; (void)input; (void)output; return ANYGM_OK;
}
AnygmResult anygm_set_config(AnygmEngine *engine,const AnygmConfigDelta *delta){
  (void)engine; (void)delta; return ANYGM_OK;
}
AnygmResult anygm_set_runtime_override(AnygmEngine *engine,uint32_t slot,uint32_t enabled,
                                       const char *expression){
  (void)engine; (void)slot; (void)enabled; (void)expression; return ANYGM_OK;
}
size_t anygm_state_size(AnygmEngine *engine){ (void)engine; return 0; }
size_t anygm_state_resume_size(AnygmEngine *engine){ (void)engine; return 0; }
size_t anygm_state_capacity_hint(const AnygmEngine *engine){ (void)engine; return 0; }
size_t anygm_state_resume_capacity_hint(const AnygmEngine *engine){ (void)engine; return 0; }
uint32_t anygm_state_capacity_flags(const AnygmEngine *engine){ (void)engine; return 0; }
AnygmResult anygm_state_save(AnygmEngine *engine,void *data,size_t size,size_t *written){
  (void)engine; (void)data; (void)size; (void)written; return ANYGM_ERROR_INVALID_STATE;
}
AnygmResult anygm_state_save_for_resume(AnygmEngine *engine,void *data,size_t capacity,
                                        size_t *written){
  (void)engine; (void)data; (void)capacity; (void)written; return ANYGM_ERROR_INVALID_STATE;
}
AnygmResult anygm_state_load(AnygmEngine *engine,const void *data,size_t size){
  (void)engine; (void)data; (void)size; return ANYGM_ERROR_INVALID_STATE;
}
size_t anygm_get_last_error(const AnygmEngine *engine,char *message,size_t capacity){
  (void)engine;
  if(message && capacity) message[0]=0;
  return 0;
}

/* The rest of the adapter is out of frame; only the directories the loader receives are on test. */
#if ANYGM_HARDWARE_RENDER
bool libretro_hw_render_request(bool needed){ return needed && delay_start; }
void libretro_hw_render_release(void){}
bool libretro_hw_render_requested(void){ return false; }
#endif
void libretro_options_register(void){}
void libretro_options_apply(bool all_fields){
  (void)all_fields; g_libretro.hybrid_gpu_selected=delay_start!=0;
}
void libretro_options_finalize_graphics(bool available){ (void)available; }
const char *libretro_options_value(const char *key){
  if(!strcmp(key,"anygm_clear_game_cache")) return current_option;
  if(!strcmp(key,"anygm_clear_all_caches")) return all_option;
  return NULL;
}
void libretro_options_publish_rooms(void){}
void libretro_options_release(void){}
void libretro_input_register(void){}
void libretro_input_snapshot(AnygmInputFrame *input,uint32_t width,uint32_t height){
  (void)width; (void)height;
  memset(input,0,sizeof *input);
  input->struct_size=sizeof *input;
}

/* One load, from a frontend that answers `directory` for its save root. */
static int load_with_save_directory(const char *directory){
  /* These path-shape probes name no owned filesystem fixture. They must not delete anything. */
  current_option=all_option="Off";
  memset(&g_libretro,0,sizeof g_libretro);
  offered_directory=directory;
  loaded_save_present=loaded_cache_present=0;
  loaded_save_directory[0]=loaded_cache_directory[0]=0;
  retro_set_environment(environment_callback);
  retro_init();
  struct retro_game_info info;
  memset(&info,0,sizeof info);
  info.path="C:/content/Input.bin";
  int ok=retro_load_game(&info);
  if(ok) retro_unload_game();
  retro_deinit();
  return ok;
}

static void expect_text(const char *what,const char *got,const char *want){
  if(got && want && !strcmp(got,want)) return;
  printf("FAIL %s: got %s, expected %s\n",what,got?got:"(none)",want?want:"(none)");
  failures++;
}

static void expect_flag(const char *what,int got,int want){
  if(got==want) return;
  printf("FAIL %s: got %d, expected %d\n",what,got,want);
  failures++;
}

/* An offered save root gets a distinct cache subdirectory. */
static void cache_lives_under_the_save_root(void){
  if(!load_with_save_directory("C:/RetroArch/saves")){
    puts("FAIL ordinary load was refused");
    failures++;
    return;
  }
  expect_flag("save root offered",loaded_save_present,1);
  expect_flag("cache root offered",loaded_cache_present,1);
  expect_text("save root","C:/RetroArch/saves",loaded_save_directory);
  expect_text("cache root","C:/RetroArch/saves/AnyGM-cache",loaded_cache_directory);
  if(!strcmp(loaded_save_directory,loaded_cache_directory)){
    puts("FAIL the cache root is the save root");
    failures++;
  }
}

/* With no save root, leave both fields unset. */
static void no_save_root_leaves_both_unset(void){
  if(!load_with_save_directory(NULL)){
    puts("FAIL load without a save root was refused");
    failures++;
    return;
  }
  expect_flag("save root absent",loaded_save_present,0);
  expect_flag("cache root absent",loaded_cache_present,0);
}

/* An unfittable cache suffix must leave the cache root unset. */
static void an_unfittable_root_reports_no_cache(void){
  char root[1200];
  memset(root,'a',sizeof root-1);
  root[sizeof root-1]=0;
  root[0]='C'; root[1]=':'; root[2]='/';
  if(!load_with_save_directory(root)){
    puts("FAIL load with an overlong save root was refused");
    failures++;
    return;
  }
  expect_flag("cache root withheld",loaded_cache_present,0);
  expect_flag("save root still offered",loaded_save_present,1);
}

static int load_cache_content(const char *name){
  struct retro_game_info info={0}; info.path=name;
  return retro_load_game(&info);
}

static void start_cache_frontend(const char *directory){
  memset(&g_libretro,0,sizeof g_libretro);
  offered_directory=directory;
  retro_set_environment(environment_callback);
  retro_init();
}

static void seed_unrelated_cache(void){
  char path[3072];
  make_directory(cache_root);
  snprintf(path,sizeof path,"%s/untouched",cache_root); make_directory(path);
  snprintf(path,sizeof path,"%s/untouched/.hidden",cache_root); write_file(path);
  snprintf(path,sizeof path,"%s/.old-marker",cache_root); write_file(path);
}

static void cache_lifecycle(void){
  char temporary[1024];
#ifdef _WIN32
  char base[MAX_PATH];
  DWORD base_length=GetTempPathA(sizeof base,base);
  if(!base_length || base_length>=sizeof base){ require(0,"temporary directory available"); return; }
  snprintf(temporary,sizeof temporary,"%sanygm-cache-%lu-%llu",base,
           (unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());
  if(_mkdir(temporary)!=0){ require(0,"private temporary root created"); return; }
#else
  snprintf(temporary,sizeof temporary,"/tmp/anygm-cache-contract-XXXXXX");
  if(!mkdtemp(temporary)){ require(0,"private temporary root created"); return; }
#endif
  start_cache_frontend(temporary);
  snprintf(cache_root,sizeof cache_root,"%s/AnyGM-cache",temporary);
  char saved[2048],original[2048],other[3072],current[3072],transformed[3072],compiled[3072];
  snprintf(saved,sizeof saved,"%s/AnyGM",temporary); make_directory(saved);
  snprintf(saved,sizeof saved,"%s/AnyGM/progress",temporary); write_file(saved);
  snprintf(original,sizeof original,"%s/original.win",temporary); write_file(original);
  snprintf(other,sizeof other,"%s/untouched/.hidden",cache_root);
  snprintf(current,sizeof current,"%s/first-anygm-archive/data.win",cache_root);
  snprintf(transformed,sizeof transformed,"%s/input-derived/data.win",cache_root);
  snprintf(compiled,sizeof compiled,"%s/compiled-anygm-classic/data.win",cache_root);
  seed_unrelated_cache();
  exercise_cache=1;
  all_option=current_option="Off";
  require(load_cache_content("first"),"both cleanup options can be disabled");
  retro_unload_game();
  require(exists(current) && exists(transformed) && exists(compiled) && exists(other),
          "Off preserves current, intermediate and unrelated caches");
  require(g_libretro.cache_path_count==0,"Off releases the tracking list");

  current_option=NULL;
  require(load_cache_content("first"),"warm cache loads with global cleanup Off");
  retro_unload_game();
  require(!exists(current) && !exists(transformed) && !exists(compiled) && exists(other),
          "default unload cleanup removes used warm and intermediate caches only");
  require(teardown_calls==2,"both unloads release handles before deletion");
  retro_unload_game();
  require(exists(other),"repeated unload leaves unrelated caches intact");

  current_option="Off"; all_option=NULL; expect_empty_cache=1;
  require(load_cache_content("first"),"default global cleanup runs before preparation");
  require(!exists(other),"global cleanup removes unrelated caches");
  retro_unload_game();
  require(exists(current),"global On does not override unload Off");

  all_option="Off"; expect_empty_cache=0;
  require(load_cache_content("first"),"load for live unload-option change");
  current_option="On";
  retro_unload_game();
  require(!exists(current),"Off-to-On menu change applies without a frame");
  require(load_cache_content("first"),"load for inverse menu change");
  current_option="Off";
  retro_unload_game();
  require(exists(current),"On-to-Off menu change applies without a frame");

  current_option=NULL;
  require(load_cache_content("first"),"first content loads before replacement");
  require(load_cache_content("second"),"replacement loads after unloading the first");
  require(!exists(current),"content replacement cleans the previous game");
  retro_deinit();
  char second[3072]; snprintf(second,sizeof second,"%s/second-anygm-archive",cache_root);
  require(!exists(second) && !exists(transformed),"direct core shutdown cleans after destruction");

  start_cache_frontend(temporary);
  seed_unrelated_cache();
  fail_preparation=1;
  require(!load_cache_content("first"),"failed preparation is reported");
  require(!exists(current) && !exists(transformed) && exists(other),
          "failed preparation cleans only attempted caches");
  require(!g_libretro.cache_tracking && !g_libretro.cache_path_count,
          "failed preparation leaves no stale tracking");
  fail_preparation=0;
#if ANYGM_HARDWARE_RENDER
  delay_start=1;
  require(load_cache_content("first") && g_libretro.prepared && !g_libretro.loaded,
          "cache can be prepared while a frontend context is pending");
  retro_unload_game();
  require(!exists(current),"abandoning prepared content clears its cache");
  delay_start=0;
#endif
  require(exists(saved) && exists(original),"save data and original content survive every policy");

#ifndef _WIN32
  char link[3072]; snprintf(link,sizeof link,"%s/external-link",cache_root);
  require(symlink(temporary,link)==0,"external directory link created");
  require(libretro_vfs_clear_cache(cache_root,true),"native cache cleanup removes links as leaves");
  require(exists(saved) && exists(original),"linked external data was not traversed");
  require(rmdir(cache_root)==0 && symlink(temporary,cache_root)==0,"cache root replaced by a link");
  all_option=NULL;
  int calls=prepare_calls;
  require(!load_cache_content("first") && prepare_calls==calls,
          "linked cache root refuses global cleanup before preparation");
  require(exists(saved) && exists(original),"linked root target preserved");
  require(unlink(cache_root)==0,"fixture link removed");
#endif
  /* Cleanup only the private fixture files and directories just created by this test. */
  all_option=current_option=NULL;
  require(load_cache_content("first"),"both default-On policies work together");
  retro_deinit();
  require(!exists(current) && !exists(other),"default load and core-close cleanup both completed");
  require(remove(saved)==0 && remove(original)==0,"fixture sentinels removed");
  char saves[2048]; snprintf(saves,sizeof saves,"%s/AnyGM",temporary);
#ifdef _WIN32
  require(_rmdir(saves)==0 && _rmdir(cache_root)==0 && _rmdir(temporary)==0,"fixture roots removed");
#else
  require(rmdir(saves)==0 && rmdir(cache_root)==0 && rmdir(temporary)==0,"fixture roots removed");
#endif
  exercise_cache=0;
}

int main(void){
  cache_lives_under_the_save_root();
  no_save_root_leaves_both_unset();
  an_unfittable_root_reports_no_cache();
  cache_lifecycle();
  if(failures){
    printf("libretro content directories: %d failure(s)\n",failures);
    return 1;
  }
  puts("libretro content directories: ok");
  return 0;
}
