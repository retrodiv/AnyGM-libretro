/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LibretroAdapter g_libretro;

void libretro_log(enum retro_log_level level,const char *format,...){
  char message[2048];
  va_list arguments;
  va_start(arguments,format);
  vsnprintf(message,sizeof message,format,arguments);
  va_end(arguments);
  if(g_libretro.log) g_libretro.log(level,"%s",message);
}

static void update_locale(void){
  unsigned language=RETRO_LANGUAGE_ENGLISH;
  if(g_libretro.environment)
    g_libretro.environment(RETRO_ENVIRONMENT_GET_LANGUAGE,&language);
  const char *iso="en",*region="us",*tag="en-US";
  switch(language){
    case RETRO_LANGUAGE_JAPANESE: iso="ja"; region="jp"; tag="ja"; break;
    case RETRO_LANGUAGE_FRENCH: iso="fr"; region="fr"; tag="fr"; break;
    case RETRO_LANGUAGE_SPANISH: iso="es"; region="es"; tag="es"; break;
    case RETRO_LANGUAGE_GERMAN: iso="de"; region="de"; tag="de"; break;
    case RETRO_LANGUAGE_ITALIAN: iso="it"; region="it"; tag="it"; break;
    case RETRO_LANGUAGE_DUTCH: iso="nl"; region="nl"; tag="nl"; break;
    case RETRO_LANGUAGE_PORTUGUESE_BRAZIL: iso="pt"; region="br"; tag="pt-BR"; break;
    case RETRO_LANGUAGE_PORTUGUESE_PORTUGAL: iso="pt"; region="pt"; tag="pt"; break;
    case RETRO_LANGUAGE_RUSSIAN: iso="ru"; region="ru"; tag="ru"; break;
    case RETRO_LANGUAGE_KOREAN: iso="ko"; region="kr"; tag="ko"; break;
    case RETRO_LANGUAGE_CHINESE_TRADITIONAL: iso="zh"; region="tw"; tag="zh-Hant"; break;
    case RETRO_LANGUAGE_CHINESE_SIMPLIFIED: iso="zh"; region="cn"; tag="zh-Hans"; break;
    case RETRO_LANGUAGE_POLISH: iso="pl"; region="pl"; tag="pl"; break;
    case RETRO_LANGUAGE_VIETNAMESE: iso="vi"; region="vn"; tag="vi"; break;
    case RETRO_LANGUAGE_ARABIC: iso="ar"; region="sa"; tag="ar"; break;
    case RETRO_LANGUAGE_GREEK: iso="el"; region="gr"; tag="el"; break;
    case RETRO_LANGUAGE_TURKISH: iso="tr"; region="tr"; tag="tr"; break;
    case RETRO_LANGUAGE_FINNISH: iso="fi"; region="fi"; tag="fi"; break;
    case RETRO_LANGUAGE_SWEDISH: iso="sv"; region="se"; tag="sv"; break;
    case RETRO_LANGUAGE_UKRAINIAN: iso="uk"; region="ua"; tag="uk"; break;
    case RETRO_LANGUAGE_CZECH: iso="cs"; region="cz"; tag="cs"; break;
    case RETRO_LANGUAGE_CATALAN_VALENCIA:
    case RETRO_LANGUAGE_CATALAN: iso="ca"; region="es"; tag="ca"; break;
    case RETRO_LANGUAGE_BRITISH_ENGLISH: iso="en"; region="gb"; tag="en-GB"; break;
    case RETRO_LANGUAGE_HUNGARIAN: iso="hu"; region="hu"; tag="hu"; break;
    case RETRO_LANGUAGE_NORWEGIAN: iso="no"; region="no"; tag="no"; break;
    case RETRO_LANGUAGE_THAI: iso="th"; region="th"; tag="th"; break;
    default: break;
  }
  snprintf(g_libretro.language,sizeof g_libretro.language,"%s",iso);
  snprintf(g_libretro.region,sizeof g_libretro.region,"%s",region);
  snprintf(g_libretro.language_tag,sizeof g_libretro.language_tag,"%s",tag);
}

static bool create_engine(void){
  if(g_libretro.engine) return true;
  AnygmHostServices services;
  libretro_host_services_init(&services);
  AnygmResult result=anygm_create(&services,&g_libretro.engine);
  if(result!=ANYGM_OK){
    libretro_log(RETRO_LOG_ERROR,"Could not create the AnyGM engine (%d)\n",result);
    return false;
  }
  g_libretro.config.struct_size=sizeof g_libretro.config;
  libretro_options_apply(true);
  return true;
}

static void negotiate_serialization(void){
  uint64_t quirks=RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE;
  g_libretro.variable_state_supported=false;
  if(g_libretro.environment &&
     g_libretro.environment(RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS,&quirks))
    g_libretro.variable_state_supported=
        (quirks&RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE)!=0;
}

static size_t fixed_state_capacity(size_t actual){
  const size_t margin=512u*1024u;
  const size_t floor=actual>1024u*1024u?4u*1024u*1024u:384u*1024u;
  if(actual>(SIZE_MAX-margin)/2u) return SIZE_MAX;
  size_t capacity=actual*2u+margin;
  return capacity<floor?floor:capacity;
}

void retro_set_environment(retro_environment_t callback){
  g_libretro.environment=callback;
  if(!callback) return;
  bool supports_no_content=false;
  callback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME,&supports_no_content);
  libretro_options_register();
  libretro_input_register();
}

void retro_set_video_refresh(retro_video_refresh_t callback){ g_libretro.video=callback; }
void retro_set_audio_sample(retro_audio_sample_t callback){ g_libretro.audio_sample=callback; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t callback){ g_libretro.audio_batch=callback; }
void retro_set_input_poll(retro_input_poll_t callback){ g_libretro.input_poll=callback; }
void retro_set_input_state(retro_input_state_t callback){ g_libretro.input_state=callback; }

void retro_init(void){
  /* The settings are declared when the host hands over its callback, and released when the core is
   * torn down. A host that starts the core again without handing the callback over a second time
   * would otherwise be left with nothing declared, so the declaration is made again here; saying
   * it twice costs nothing. */
  libretro_options_register();
  if(g_libretro.environment){
    struct retro_log_callback log_callback;
    if(g_libretro.environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE,&log_callback))
      g_libretro.log=log_callback.log;
    enum retro_pixel_format format=RETRO_PIXEL_FORMAT_XRGB8888;
    if(!g_libretro.environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT,&format))
      libretro_log(RETRO_LOG_WARN,"The frontend rejected XRGB8888 output\n");
    memset(&g_libretro.rumble,0,sizeof g_libretro.rumble);
    g_libretro.rumble_available=
        g_libretro.environment(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE,&g_libretro.rumble) &&
        g_libretro.rumble.set_rumble_state;
  }
  negotiate_serialization();
  libretro_vfs_request();
  update_locale();
  create_engine();
}

void retro_deinit(void){
  if(g_libretro.engine){
    anygm_destroy(g_libretro.engine);
    g_libretro.engine=NULL;
  }
  libretro_options_release();
  memset(g_libretro.keyboard_events,0,sizeof g_libretro.keyboard_events);
  memset(g_libretro.override_used,0,sizeof g_libretro.override_used);
  g_libretro.loaded=false;
}

unsigned retro_api_version(void){ return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info *info){
  memset(info,0,sizeof *info);
  info->library_name="AnyGM";
  info->library_version="0.1.0";
  info->valid_extensions="win|droid|zip|port|apk|yyp|yyz|gmk|gm6|gm81|exe";
  info->need_fullpath=true;
  info->block_extract=true;
}

static void fill_av_info(struct retro_system_av_info *info){
  memset(info,0,sizeof *info);
  info->geometry.base_width=g_libretro.av.base_width?g_libretro.av.base_width:288;
  info->geometry.base_height=g_libretro.av.base_height?g_libretro.av.base_height:216;
  info->geometry.max_width=g_libretro.av.max_width?g_libretro.av.max_width:3840;
  info->geometry.max_height=g_libretro.av.max_height?g_libretro.av.max_height:2160;
  info->geometry.aspect_ratio=g_libretro.av.aspect_ratio>0?(float)g_libretro.av.aspect_ratio:4.0f/3.0f;
  info->timing.fps=g_libretro.av.frames_per_second>0?g_libretro.av.frames_per_second:60.0;
  info->timing.sample_rate=g_libretro.av.audio_rate?g_libretro.av.audio_rate:44100.0;
}

void libretro_update_av(void){
  if(!g_libretro.engine || !g_libretro.loaded) return;
  memset(&g_libretro.av,0,sizeof g_libretro.av);
  g_libretro.av.struct_size=sizeof g_libretro.av;
  anygm_get_av_info(g_libretro.engine,&g_libretro.av);
}

void retro_get_system_av_info(struct retro_system_av_info *info){ fill_av_info(info); }

void retro_set_controller_port_device(unsigned port,unsigned device){
  if(port<ANYGM_MAX_GAMEPADS) g_libretro.port_device[port]=device;
  libretro_input_register();
}

void retro_reset(void){
  /* Options are otherwise picked up from inside the frame loop, and a player choosing one reaches
   * the restart without the core having run a frame in between: hosts hold the core still while
   * their menu is open. The choice that prompted the restart would then apply to the boot after
   * the next one. Settings read at boot, the start room among them, are read here first. */
  if(g_libretro.loaded){
    libretro_options_apply(true);
    anygm_reset(g_libretro.engine);
  }
  libretro_update_av();
}

static void update_directories(void){
  const char *directory=NULL;
  g_libretro.save_directory[0]=0;
  g_libretro.cache_directory[0]=0;
  if(g_libretro.environment &&
     g_libretro.environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,&directory) &&
     directory && directory[0]){
    snprintf(g_libretro.save_directory,sizeof g_libretro.save_directory,"%s",directory);
    snprintf(g_libretro.cache_directory,sizeof g_libretro.cache_directory,"%s",directory);
  }
}

bool retro_load_game(const struct retro_game_info *info){
  if(!info || !info->path || !create_engine()) return false;
  if(g_libretro.loaded) retro_unload_game();
  memset(g_libretro.setting_cache,0,sizeof g_libretro.setting_cache);
  g_libretro.setting_cache_count=0;
  update_directories();
  update_locale();
  libretro_options_apply(true);
  AnygmContentSource source;
  memset(&source,0,sizeof source);
  source.struct_size=sizeof source;
  source.kind=ANYGM_CONTENT_PATH;
  source.path=info->path;
  source.cache_directory=g_libretro.cache_directory[0]?g_libretro.cache_directory:NULL;
  source.save_directory=g_libretro.save_directory[0]?g_libretro.save_directory:NULL;
  AnygmLoadConfig config;
  memset(&config,0,sizeof config);
  config.struct_size=sizeof config;
  config.language=g_libretro.language;
  config.region=g_libretro.region;
  config.language_tag=g_libretro.language_tag;
  AnygmResult result=anygm_load(g_libretro.engine,&source,&config);
  if(result!=ANYGM_OK){
    char error[512];
    anygm_get_last_error(g_libretro.engine,error,sizeof error);
    libretro_log(RETRO_LOG_ERROR,"Content load failed (%d): %s\n",result,error);
    return false;
  }
  g_libretro.loaded=true;
  g_libretro.fixed_state_capacity=0;
  /* Loading consults dozens of one-shot setting names; clear them out so the
   * per-frame names always find a free slot. */
  memset(g_libretro.setting_cache,0,sizeof g_libretro.setting_cache);
  g_libretro.setting_cache_count=0;
  /* The room names only exist now, and the chooser is worth nothing without them. */
  libretro_options_publish_rooms();
  libretro_update_av();
  return true;
}

bool retro_load_game_special(unsigned type,const struct retro_game_info *info,size_t count){
  (void)type; (void)info; (void)count;
  return false;
}

void retro_unload_game(void){
  if(!g_libretro.loaded) return;
  anygm_unload(g_libretro.engine);
  g_libretro.loaded=false;
  g_libretro.fixed_state_capacity=0;
  memset(g_libretro.override_used,0,sizeof g_libretro.override_used);
  memset(&g_libretro.frame,0,sizeof g_libretro.frame);
  memset(&g_libretro.av,0,sizeof g_libretro.av);
}

unsigned retro_get_region(void){ return RETRO_REGION_NTSC; }

static void apply_live_options(void){
  bool updated=false;
  if(g_libretro.environment &&
     g_libretro.environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,&updated) && updated)
    libretro_options_apply(false);
  bool fast_forward=false;
  if(g_libretro.environment)
    g_libretro.environment(RETRO_ENVIRONMENT_GET_FASTFORWARDING,&fast_forward);
  uint32_t fast_forward_value=fast_forward?1u:0u;
  if(g_libretro.config.fast_forward==fast_forward_value) return;
  AnygmConfigDelta delta;
  memset(&delta,0,sizeof delta);
  delta.struct_size=sizeof delta;
  delta.fields=ANYGM_CONFIG_FAST_FORWARD;
  delta.values=g_libretro.config;
  delta.values.struct_size=sizeof delta.values;
  delta.values.fast_forward=fast_forward_value;
  g_libretro.config.fast_forward=delta.values.fast_forward;
  anygm_set_config(g_libretro.engine,&delta);
}

void retro_run(void){
  if(!g_libretro.loaded){
    if(g_libretro.video) g_libretro.video(NULL,288,216,0);
    return;
  }
  apply_live_options();
  AnygmInputFrame input;
  uint32_t width=g_libretro.frame.width?g_libretro.frame.width:g_libretro.av.base_width;
  uint32_t height=g_libretro.frame.height?g_libretro.frame.height:g_libretro.av.base_height;
  libretro_input_snapshot(&input,width,height);
  memset(&g_libretro.frame,0,sizeof g_libretro.frame);
  g_libretro.frame.struct_size=sizeof g_libretro.frame;
  AnygmResult result=anygm_run_frame(g_libretro.engine,&input,&g_libretro.frame);
  if(result!=ANYGM_OK){
    libretro_log(RETRO_LOG_ERROR,"Frame execution failed (%d)\n",result);
    return;
  }
  if(g_libretro.video)
    g_libretro.video(g_libretro.frame.pixels,g_libretro.frame.width,g_libretro.frame.height,
                     g_libretro.frame.pitch);
  if(g_libretro.frame.audio && g_libretro.frame.audio_frames){
    if(g_libretro.audio_batch)
      g_libretro.audio_batch(g_libretro.frame.audio,g_libretro.frame.audio_frames);
    else if(g_libretro.audio_sample)
      for(size_t i=0;i<g_libretro.frame.audio_frames;i++)
        g_libretro.audio_sample(g_libretro.frame.audio[i*2],g_libretro.frame.audio[i*2+1]);
  }
  if(g_libretro.frame.flags&(ANYGM_FRAME_GEOMETRY_CHANGED|ANYGM_FRAME_TIMING_CHANGED)){
    libretro_update_av();
    struct retro_system_av_info av;
    fill_av_info(&av);
    if(g_libretro.frame.flags&ANYGM_FRAME_TIMING_CHANGED)
      g_libretro.environment(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO,&av);
    else
      g_libretro.environment(RETRO_ENVIRONMENT_SET_GEOMETRY,&av.geometry);
  }
  if((g_libretro.frame.flags&ANYGM_FRAME_SHUTDOWN_REQUESTED) && g_libretro.environment)
    g_libretro.environment(RETRO_ENVIRONMENT_SHUTDOWN,NULL);
}

size_t retro_serialize_size(void){
  if(!g_libretro.loaded) return 0;
  size_t actual=anygm_state_size(g_libretro.engine);
  if(!g_libretro.fixed_state_capacity ||
     (g_libretro.variable_state_supported && actual>g_libretro.fixed_state_capacity))
    g_libretro.fixed_state_capacity=fixed_state_capacity(actual);
  return g_libretro.fixed_state_capacity;
}

bool retro_serialize(void *data,size_t size){
  size_t written=0;
  if(!g_libretro.loaded ||
     anygm_state_save(g_libretro.engine,data,size,&written)!=ANYGM_OK || written>size) return false;
  /* libretro persists the full advertised buffer, while AnyGM records its exact logical size in
   * the state header. Clear the capacity tail so files and rewind deltas never contain stale host
   * memory and remain deterministic for an identical runtime state. */
  if(written<size) memset((uint8_t*)data+written,0,size-written);
  return true;
}

bool retro_unserialize(const void *data,size_t size){
  return g_libretro.loaded && anygm_state_load(g_libretro.engine,data,size)==ANYGM_OK;
}

void retro_cheat_reset(void){
  if(!g_libretro.loaded) return;
  for(uint32_t i=0;i<ANYGM_MAX_RUNTIME_OVERRIDES;i++) if(g_libretro.override_used[i]){
    anygm_set_runtime_override(g_libretro.engine,i,0,NULL);
    g_libretro.override_used[i]=0;
  }
}

void retro_cheat_set(unsigned index,bool enabled,const char *code){
  if(!g_libretro.loaded || index>=ANYGM_MAX_RUNTIME_OVERRIDES) return;
  if(anygm_set_runtime_override(g_libretro.engine,index,enabled?1u:0u,code)==ANYGM_OK)
    g_libretro.override_used[index]=enabled?1u:0u;
}

void *retro_get_memory_data(unsigned id){ (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id){ (void)id; return 0; }
