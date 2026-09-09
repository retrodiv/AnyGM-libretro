/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The save root retains persistent content data; the separate cache root
 * holds rebuildable loader data. */
#include "libretro_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
  *engine=(AnygmEngine *)(uintptr_t)1u;
  return ANYGM_OK;
}
void anygm_destroy(AnygmEngine *engine){ (void)engine; }
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
  return ANYGM_OK;
}
AnygmResult anygm_load_start(AnygmEngine *engine){ (void)engine; return ANYGM_OK; }
AnygmResult anygm_load(AnygmEngine *engine,const AnygmContentSource *source,
                       const AnygmLoadConfig *config){
  return anygm_load_prepare(engine,source,config,NULL);
}
void anygm_unload(AnygmEngine *engine){ (void)engine; }
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
bool libretro_hw_render_request(bool needed){ (void)needed; return false; }
void libretro_hw_render_release(void){}
bool libretro_hw_render_requested(void){ return false; }
#endif
void libretro_vfs_request(void){}
void libretro_vfs_services_init(AnygmHostServices *services){ (void)services; }
void libretro_options_register(void){}
void libretro_options_apply(bool all_fields){ (void)all_fields; }
void libretro_options_finalize_graphics(bool available){ (void)available; }
const char *libretro_options_value(const char *key){ (void)key; return NULL; }
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

int main(void){
  cache_lives_under_the_save_root();
  no_save_root_leaves_both_unset();
  an_unfittable_root_reports_no_cache();
  if(failures){
    printf("libretro content directories: %d failure(s)\n",failures);
    return 1;
  }
  puts("libretro content directories: ok");
  return 0;
}
